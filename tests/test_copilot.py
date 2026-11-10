"""Integration checks: real fork/pipe fetching with a stub gh and PTY UI."""
import ctypes
import fcntl
import os
from pathlib import Path
import pty
import select
import signal
import struct
import subprocess
import tempfile
import termios
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]


class CopilotTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.folder = Path(cls.temp.name)
        cls.original_path = os.environ["PATH"]
        cls.original_state = os.environ.get("XDG_STATE_HOME")
        os.environ["XDG_STATE_HOME"] = str(cls.folder / "state")
        os.environ["PATH"] = str(cls.folder) + os.pathsep + cls.original_path
        cls.gh = cls.folder / "gh"
        subprocess.run(["cc", "-Wall", "-Wextra", "-shared", "-fPIC",
                        str(ROOT / "copilot.c"), str(ROOT / "copilot_budget.c"), "-lm", "-o",
                        str(cls.folder / "copilot.so")], check=True)
        cls.lib = ctypes.CDLL(str(cls.folder / "copilot.so"))
        cls.lib.copilot_tick.argtypes = [ctypes.c_bool, ctypes.c_uint]
        cls.lib.copilot_lines.argtypes = [ctypes.c_char_p, ctypes.c_size_t,
                                        ctypes.c_char_p, ctypes.c_size_t, ctypes.c_int]

    @classmethod
    def tearDownClass(cls):
        cls.lib.copilot_cleanup()
        os.environ["PATH"] = cls.original_path
        if cls.original_state is None:
            os.environ.pop("XDG_STATE_HOME", None)
        else:
            os.environ["XDG_STATE_HOME"] = cls.original_state
        cls.temp.cleanup()

    def stub(self, text, delay=0, status=0):
        # Validate that the app uses the expected account host and endpoint.
        self.gh.write_text(
            "#!/usr/bin/python3\nimport sys, time\n"
            "assert sys.argv[1:5] == ['api', '--hostname', 'github.com', "
            "'/copilot_internal/user']\n"
            f"time.sleep({delay})\nprint({text!r})\nsys.exit({status})\n")
        self.gh.chmod(0o755)

    def lines(self, width=44):
        bar, detail = ctypes.create_string_buffer(256), ctypes.create_string_buffer(256)
        self.lib.copilot_lines(bar, 256, detail, 256, width)
        return bar.value.decode(), detail.value.decode()

    def fetch(self):
        # Refresh interval is zero only in this harness, to avoid minute waits.
        self.lib.copilot_tick(True, 0)
        time.sleep(0.1)
        self.lib.copilot_tick(True, 0)

    def test_quota_and_failure_modes(self):
        self.assertIn("loading", self.lines()[0])
        self.stub("quota 69.5 200000 139152.9 60847 0 1 1")
        self.fetch()
        self.assertIn("30.5% used", self.lines()[0])
        self.assertEqual(self.lines()[1], "60,847.1 / 200,000 credits")
        self.assertLessEqual(len(self.lines(32)[0]), 32)

        for invalid in ("", "quota nan 100 10 90 0 1 1", "not JSON", "quota 101 1 1 1 0 0 1"):
            self.stub(invalid)
            self.fetch()
            self.assertIn("stale", self.lines()[0])
            self.assertEqual(self.lines()[1], "60,847.1 / 200,000 credits")

        self.stub("quota 100 0 0 125.5 1 1 1")
        self.fetch()
        self.assertIn("unlimited", self.lines()[0])
        self.assertEqual(self.lines()[1], "125.5 credits used")
        self.stub("quota 0 300 0 -1 0 0 1")
        self.fetch()
        self.assertIn("100.0% used", self.lines()[0])
        self.assertEqual(self.lines()[1], "300 / 300 requests")
        self.stub("quota 100 300 300 -1 0 0 1")
        self.fetch()
        self.assertIn("0.0% used", self.lines()[0])

        self.stub("", delay=30)
        before = time.monotonic()
        self.lib.copilot_tick(True, 0)
        self.assertLess(time.monotonic() - before, 0.2)
        time.sleep(15.1)
        self.lib.copilot_tick(True, 0)
        self.assertIn("stale", self.lines()[0])
        self.lib.copilot_cleanup()

    def ui(self, options, keys=b"q", resize=False, wait=1.5):
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
        process = subprocess.Popen([str(ROOT / "tty-clock"), *options],
                                   stdin=slave, stdout=slave, stderr=slave,
                                   env={**os.environ, "TERM": "xterm-256color"})
        os.close(slave)
        output = bytearray()
        try:
            end = time.monotonic() + wait
            while time.monotonic() < end:
                if select.select([master], [], [], 0.1)[0]:
                    output.extend(os.read(master, 65536))
            if resize:
                for rows, cols in ((5, 15), (12, 40), (24, 80)):
                    fcntl.ioctl(master, termios.TIOCSWINSZ,
                                struct.pack("HHHH", rows, cols, 0, 0))
                    process.send_signal(signal.SIGWINCH)
                    time.sleep(0.15)
            os.write(master, keys)
            process.wait(timeout=3)
            self.assertEqual(process.returncode, 0, output.decode(errors="replace"))
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
        return bytes(output)

    def test_terminal_rendering_and_resize(self):
        self.stub("quota 69.5 200000 139152.9 60847 0 1 1")
        output = self.ui(["-c", "-s", "--copilot"], b"pPsbxctq", resize=True)
        self.assertIn(b"30.5% used", output)
        self.assertIn(b"60,847.1 / 200,000 credits", output)
        self.assertTrue(b"Today: unknown" in output or b"Today UTC:" in output)
        self.assertIn(b"Safe avg:", output)
        self.assertIn(b"Est. limit by", output)
        self.assertTrue(b"of daily avg" in output or b"daily average not available" in output)
        self.assertTrue(b"At this pace:" in output or b"Empty:" in output or
                        b"forecast pending" in output)
        output = self.ui(["-c", "--theme", "nord", "--copilot"])
        self.assertIn(b"30.5% used", output)
        self.assertIn(b"60,847.1 / 200,000 credits", output)
        self.assertIn(b"\x1b[48;5;25m", output)
        self.assertIn(b"\x1b[38;5;17m", output)
        self.ui(["-r", "-D", "--copilot"], resize=True)
        self.stub("", status=1)
        self.assertIn(b"unavailable", self.ui(["--copilot"]))
        self.stub("", delay=30)
        self.ui(["--copilot"], wait=0.2)
        self.ui([], b"psq", wait=0.1)

    def test_refresh_validation(self):
        for value in ("0", "-1", "abc", "86401", "12s"):
            result = subprocess.run([str(ROOT / "tty-clock"), "--copilot-refresh", value],
                                    capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"1..86400", result.stderr)

    def test_default_refresh_updates_display(self):
        log = self.folder / "refresh-times"
        self.gh.write_text(
            "#!/usr/bin/python3\nimport sys, time\n"
            "from pathlib import Path\n"
            "assert 'Cache-Control: no-cache' in sys.argv\n"
            f"log = Path({str(log)!r})\n"
            "with log.open('a') as f: f.write(str(time.monotonic()) + '\\n')\n"
            "count = len(log.read_text().splitlines())\n"
            "print('quota 69.5 200000 139152.9 60847 0 1 1' if count == 1 "
            "else 'quota 50 200000 100000 100000 0 1 1')\n")
        self.gh.chmod(0o755)
        output = self.ui(["--copilot"], wait=64)
        timestamps = [float(t) for t in log.read_text().splitlines()]
        self.assertEqual(len(timestamps), 2)
        self.assertAlmostEqual(timestamps[1] - timestamps[0], 60, delta=1.5)
        self.assertIn(b"30.5% used", output)
        # ncurses updates only changed cells, leaving the suffix on screen.
        self.assertIn(b"50.0", output)
        self.assertIn(b"100,000", output)


if __name__ == "__main__":
    unittest.main()
