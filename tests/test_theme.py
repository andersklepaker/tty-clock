"""Nord CLI and real ncurses rendering checks in pseudo-terminals."""
import fcntl
import os
from pathlib import Path
import pty
import select
import signal
import struct
import subprocess
import termios
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ThemeTests(unittest.TestCase):
    maxDiff = 1000

    def ui(self, options, term="xterm-256color", resize=False):
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
        process = subprocess.Popen(
            [str(ROOT / "tty-clock"), "-d", "0", "-a", "10000000", *options],
            stdin=slave, stdout=slave, stderr=slave,
            env={**os.environ, "TERM": term})
        os.close(slave)
        output = bytearray()

        def drain(seconds):
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                if select.select([master], [], [], 0.02)[0]:
                    try:
                        output.extend(os.read(master, 65536))
                    except OSError:
                        break

        try:
            drain(0.2)
            if resize:
                for rows, cols in ((5, 15), (24, 80)):
                    fcntl.ioctl(master, termios.TIOCSWINSZ,
                                struct.pack("HHHH", rows, cols, 0, 0))
                    process.send_signal(signal.SIGWINCH)
                    drain(0.2)
                os.write(master, b"1")
                drain(0.1)
                self.assertIn(b"\x1b[48;5;20m", output)  # Nord red accent
            os.write(master, b"q")
            drain(0.1)
            process.wait(timeout=3)
            self.assertEqual(process.returncode, 0, output.decode(errors="replace"))
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
        return bytes(output)

    def test_cli(self):
        help_result = subprocess.run([str(ROOT / "tty-clock"), "-h"], capture_output=True)
        self.assertIn(b"--theme", help_result.stdout)
        for name in ("unknown", "", "Nord"):
            result = subprocess.run([str(ROOT / "tty-clock"), "--theme", name],
                                    capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"unknown theme", result.stderr)

    def test_nord_palette_and_resize(self):
        output = self.ui(["--theme", "nord", "-c", "-s", "-x"], resize=True)
        # xterm palette definitions: background, text, and default Frost accent.
        self.assertIn(b"\x1b]4;16;rgb:2E/34/40", output)
        self.assertIn(b"\x1b]4;17;rgb:D8/DE/E9", output)
        self.assertIn(b"\x1b]4;25;rgb:88/C0/D0", output)
        self.assertIn(b"\x1b[48;5;16m", output)
        self.assertIn(b"\x1b[48;5;25m", output)
        self.assertIn(b"\x1b[38;5;17m", output)

    def test_explicit_accent_and_default_theme(self):
        for options in (["-C", "1", "--theme", "nord"],
                        ["--theme", "nord", "-C", "1"]):
            output = self.ui(options)
            self.assertIn(b"\x1b[48;5;20m", output)
            self.assertNotIn(b"\x1b[48;5;25m", output)
        output = self.ui(["--theme", "nord", "--theme", "default"])
        self.assertNotIn(b"\x1b]4;", output)
        self.assertIn(b"\x1b[42m", output)  # Original green clock

    def test_limited_color_fallback(self):
        output = self.ui(["--theme", "nord"], term="xterm")
        self.assertNotIn(b"\x1b]4;", output)
        self.assertNotIn(b";5;", output)
        self.assertIn(b"\x1b[40m", output)  # Nearest background is ANSI black


if __name__ == "__main__":
    unittest.main()
