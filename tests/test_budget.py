"""Deterministic UTC budget/projection checks with isolated on-disk history."""
import ctypes
from datetime import datetime, timezone
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def utc(value):
    return int(datetime.fromisoformat(value).replace(tzinfo=timezone.utc).timestamp())


class Forecast(ctypes.Structure):
    _fields_ = [(name, ctypes.c_bool) for name in
                ("valid", "partial", "storage_error", "has_rate", "monthly_rate", "holds")] + [
                    (name, ctypes.c_double) for name in
                    ("today_used", "daily_budget", "remaining", "daily_rate", "safe_rate",
                     "needed_until_reset")] + [
                    ("reset_at", ctypes.c_long), ("empty_at", ctypes.c_long),
                    ("observed_since", ctypes.c_long)]


class Usage(ctypes.Structure):
    _fields_ = [(name, ctypes.c_bool) for name in
                ("valid", "stale", "unlimited", "credits", "has_quota")] + [
                    (name, ctypes.c_double) for name in ("percent_used", "used", "total")]


class BudgetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.folder = Path(cls.temp.name)
        cls.shared = cls.folder / "budget.so"
        subprocess.run(["cc", "-Wall", "-Wextra", "-shared", "-fPIC",
                        str(ROOT / "copilot_budget.c"), str(ROOT / "copilot.c"),
                        "-lm", "-o", str(cls.shared)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.state = self.folder / self._testMethodName
        self.original_state = os.environ.get("XDG_STATE_HOME")
        os.environ["XDG_STATE_HOME"] = str(self.state)
        self.lib = self.library("first")

    def tearDown(self):
        if self.original_state is None:
            os.environ.pop("XDG_STATE_HOME", None)
        else:
            os.environ["XDG_STATE_HOME"] = self.original_state

    def library(self, suffix):
        # A distinct shared-object path gives each simulated process fresh globals.
        path = self.folder / f"{self._testMethodName}-{suffix}.so"
        shutil.copyfile(self.shared, path)
        lib = ctypes.CDLL(str(path))
        lib.copilot_budget_sample.argtypes = [ctypes.c_long, ctypes.c_double,
                                             ctypes.c_double, ctypes.c_bool]
        lib.copilot_budget_get.argtypes = [ctypes.c_long, ctypes.POINTER(Forecast)]
        lib.copilot_forecast_lines.argtypes = [ctypes.c_long, ctypes.c_void_p, ctypes.c_int]
        return lib

    def sample(self, date, used, total=200000, credits=True, lib=None):
        (lib or self.lib).copilot_budget_sample(utc(date), used, total, credits)
        return self.get(date, lib)

    def get(self, date, lib=None):
        forecast = Forecast()
        (lib or self.lib).copilot_budget_get(utc(date), ctypes.byref(forecast))
        return forecast

    def text(self, date, width=54):
        usage = Usage.in_dll(self.lib, "copilot")
        usage.valid = usage.credits = usage.has_quota = True
        usage.total = 200000
        lines = (ctypes.c_char * 256 * 6)()
        self.lib.copilot_forecast_lines(utc(date), lines, width)
        return [bytes(line).split(b"\0", 1)[0].decode() for line in lines]

    def test_fixed_daily_budget_and_overage(self):
        start = self.sample("2026-11-10T00:00", 30000)
        noon = self.sample("2026-11-10T12:00", 39000)
        self.assertFalse(noon.partial)
        self.assertEqual(noon.today_used, 9000)
        self.assertAlmostEqual(noon.daily_budget, 170000 / 21)
        self.assertEqual(noon.daily_budget, start.daily_budget)
        self.assertGreater(noon.today_used, noon.daily_budget)
        evening = self.sample("2026-11-10T20:00", 45000)
        self.assertEqual(evening.daily_budget, start.daily_budget)

    def test_partial_first_day_and_restart(self):
        start = self.sample("2026-11-10T12:00", 40000)
        self.assertTrue(start.partial)
        self.assertEqual(start.today_used, 0)
        restarted = self.library("restarted")
        later = self.sample("2026-11-10T18:00", 41000, lib=restarted)
        self.assertTrue(later.partial)
        self.assertEqual(later.today_used, 1000)
        self.assertEqual(later.daily_budget, start.daily_budget)
        path = self.state / "tty-clock/copilot-history"
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        self.assertFalse(later.storage_error)

    def test_monthly_forecast_and_zero_usage(self):
        early = self.sample("2026-11-10T12:00", 100000)
        self.assertTrue(early.monthly_rate)
        self.assertFalse(early.holds)
        self.assertEqual(early.empty_at, utc("2026-11-20T00:00"))
        self.assertAlmostEqual(early.safe_rate, 100000 / 20.5)
        self.assertAlmostEqual(early.needed_until_reset, 100000 / 9.5 * 20.5)
        # Lower usage is a correction, so old deltas must be discarded.
        safe = self.sample("2026-11-10T13:00", 10000)
        self.assertTrue(safe.holds)
        zero = self.sample("2026-11-10T14:00", 0)
        self.assertTrue(zero.has_rate)
        self.assertEqual(zero.daily_rate, 0)
        self.assertTrue(zero.holds)
        self.assertEqual(zero.needed_until_reset, 0)

    def test_recent_rate_overrides_monthly_estimate_and_includes_offline_time(self):
        self.sample("2026-11-10T12:00", 100000)
        recent = self.sample("2026-11-13T12:00", 103000)
        self.assertFalse(recent.monthly_rate)
        self.assertEqual(recent.daily_rate, 1000)
        self.assertTrue(recent.holds)
        self.assertTrue(recent.partial)
        self.assertEqual(recent.needed_until_reset, 17500)
        # An old first reading is excluded from the seven-day window.
        recent = self.sample("2026-11-20T12:00", 124000)
        self.assertEqual(recent.daily_rate, 3000)
        self.assertFalse(recent.monthly_rate)

    def test_midnight_and_missing_day(self):
        self.sample("2026-11-10T23:59", 40000)
        next_day = self.sample("2026-11-11T00:01", 40100)
        self.assertFalse(next_day.partial)
        self.assertEqual(next_day.today_used, 100)
        self.assertEqual(next_day.daily_budget, 160000 / 20)
        missing = self.get("2026-11-12T00:01")
        self.assertTrue(missing.partial)
        self.assertEqual(missing.today_used, 0)
        resumed = self.sample("2026-11-12T12:00", 45000)
        self.assertTrue(resumed.partial)
        self.assertEqual(resumed.today_used, 0)

    def test_month_rollover_and_leap_year(self):
        february = self.sample("2028-02-28T00:00", 100000)
        self.assertEqual(february.daily_budget, 50000)
        self.assertEqual(february.reset_at, utc("2028-03-01T00:00"))
        self.assertFalse(self.get("2028-03-01T00:00").valid)
        march = self.sample("2028-03-01T00:01", 100)
        self.assertFalse(march.partial)
        self.assertEqual(march.today_used, 100)
        self.assertAlmostEqual(march.daily_budget, 200000 / 31)
        self.assertFalse(march.has_rate)
        self.assertEqual(march.needed_until_reset, 0)

    def test_exhaustion_corrections_and_plan_changes(self):
        self.sample("2026-11-10T00:00", 30000)
        empty = self.sample("2026-11-10T12:00", 200000)
        self.assertEqual(empty.remaining, 0)
        self.assertEqual(empty.empty_at, utc("2026-11-10T12:00"))
        corrected = self.sample("2026-11-10T13:00", 10000)
        self.assertTrue(corrected.partial)
        self.assertEqual(corrected.today_used, 0)
        changed = self.sample("2026-11-10T14:00", 20000, total=300000)
        self.assertEqual(changed.today_used, 0)
        self.assertAlmostEqual(changed.daily_budget, 280000 / 21)
        legacy = self.sample("2026-11-10T15:00", 21000, total=300000, credits=False)
        self.assertEqual(legacy.today_used, 0)

    def test_invalid_samples_and_clock_rollback(self):
        original = self.sample("2026-11-10T12:00", 40000)
        for used, total in ((float("nan"), 200000), (-1, 200000),
                            (200001, 200000), (0, 0), (0, float("inf"))):
            invalid = self.sample("2026-11-10T13:00", used, total)
            self.assertEqual(invalid.remaining, original.remaining)
        self.assertFalse(self.get("2026-11-10T11:00").valid)
        rollback = self.sample("2026-11-10T10:00", 41000)
        self.assertEqual(rollback.today_used, 0)

    def test_corrupt_history_and_unavailable_storage(self):
        directory = self.state / "tty-clock"
        directory.mkdir(parents=True)
        path = directory / "copilot-history"
        for text in ("bad header", "tty-clock-v1 1793491200 200000 1 999",
                     "tty-clock-v1 1793491200 nan 1 1", "tty-clock-v1 1793491200 200000 1 1\n0"):
            path.write_text(text)
            value = self.sample("2026-11-10T12:00", 40000)
            self.assertTrue(value.valid)
            self.assertFalse(value.storage_error)
        blocked = self.state / "not-a-directory"
        blocked.write_text("blocked")
        os.environ["XDG_STATE_HOME"] = str(blocked)
        value = self.sample("2026-11-10T13:00", 41000)
        self.assertTrue(value.storage_error)
        self.assertEqual(value.today_used, 1000)

    def test_stale_forecast_stays_anchored_to_measurement(self):
        value = self.sample("2026-11-10T12:00", 100000)
        stale = self.get("2026-11-25T12:00")
        self.assertEqual(stale.empty_at, value.empty_at)
        self.assertEqual(stale.needed_until_reset, value.needed_until_reset)
        self.assertFalse(stale.holds)

    def test_forecast_text_and_unlimited_or_missing_quota(self):
        now = utc("2026-11-10T12:00")
        day = now - now % 86400
        self.lib.copilot_budget_sample(day, 0, 200000, True)
        self.lib.copilot_budget_sample(now, 200000, 200000, True)
        usage = Usage.in_dll(self.lib, "copilot")
        usage.valid = usage.credits = usage.has_quota = True
        usage.total = usage.used = 200000
        lines = (ctypes.c_char * 256 * 6)()
        self.lib.copilot_forecast_lines(now, lines, 54)
        text = [bytes(line).split(b"\0", 1)[0].decode() for line in lines]
        self.assertIn("Today UTC: ~200,000 /", text[0])
        forecast = Forecast()
        self.lib.copilot_budget_get(now, ctypes.byref(forecast))
        if forecast.daily_budget < 200000:
            self.assertIn("% over", text[0])
        self.assertIn("Safe avg: 0 credits/day", text[2])
        self.assertIn("Quota empty; reset", text[5])
        self.assertTrue(all(len(line) <= 54 for line in text))
        usage.unlimited = True
        self.lib.copilot_forecast_lines(now, lines, 54)
        self.assertTrue(all(bytes(line).strip(b"\0") == b"" for line in lines))
        usage.unlimited = False
        usage.has_quota = False
        self.lib.copilot_forecast_lines(now, lines, 54)
        self.assertTrue(all(bytes(line).strip(b"\0") == b"" for line in lines))

    def test_partial_day_text_explains_unknown_usage_and_early_exhaustion(self):
        self.sample("2026-11-10T12:00", 100000)
        text = self.text("2026-11-10T12:00")
        self.assertEqual(text[0], "Today: unknown; 0 since 12:00 UTC")
        self.assertIn("Since 12:00 [", text[1])
        self.assertIn("0.0% of daily avg", text[1])
        self.assertIn("Safe avg: 4,878 credits/day", text[2])
        self.assertIn("Month avg: 10,526.3/day (115.8% over safe pace)", text[3])
        self.assertEqual(text[4], "Est. limit by 01 Dec: 315,789.5 credits (+57.9%)")
        self.assertEqual(text[5], "Empty: 20 Nov 00:00 UTC; ~11 days early")
        self.assertTrue(all(len(line) <= 54 for line in text))
        self.sample("2026-11-10T13:00", 101000)
        self.assertEqual(self.text("2026-11-10T13:00")[0],
                         "Today: unknown; 1,000 since 12:00 UTC")

    def test_safe_pace_text_and_daily_headroom(self):
        self.sample("2026-11-10T00:00", 10000)
        self.sample("2026-11-10T12:00", 11000)
        text = self.text("2026-11-10T12:00")
        self.assertIn("Today UTC: ~1,000 / 9,047.6 (8,047.6 left)", text[0])
        self.assertIn("within budget", text[3])
        self.assertEqual(text[4], "Est. limit by 01 Dec: 200,000 credits (no increase)")
        self.assertEqual(text[5], "At this pace: lasts until 01 Dec reset")
        self.assertTrue(all(len(line) <= 54 for line in text))

    def test_limit_adds_only_shortfall_and_stays_anchored_when_stale(self):
        self.sample("2026-11-05T12:00", 40000)
        self.sample("2026-11-06T12:00", 47500)
        expected = "Est. limit by 01 Dec: 231,250 credits (+15.6%)"
        self.assertEqual(self.text("2026-11-06T12:00")[4], expected)
        self.assertEqual(self.text("2026-11-07T12:00")[4], expected)

    def test_forecast_less_than_a_day_early_uses_hours(self):
        self.sample("2026-11-30T12:00", 198000)
        text = self.text("2026-11-30T12:00")
        self.assertIn("over safe pace", text[3])
        self.assertIn("hours early", text[5])

    def test_display_groups_separate_quota_today_and_forecast(self):
        self.sample("2026-11-10T12:00", 100000)
        forecast = self.text("2026-11-10T12:00")
        self.lib.copilot_display_lines.argtypes = [ctypes.c_long, ctypes.c_void_p, ctypes.c_int]
        for width in (54, 32):
            lines = (ctypes.c_char * 256 * 10)()
            self.lib.copilot_display_lines(utc("2026-11-10T12:00"), lines, width)
            text = [bytes(line).split(b"\0", 1)[0].decode() for line in lines]
            self.assertTrue(text[0].startswith("Copilot ["))
            self.assertIn("/ 200,000 credits", text[1])
            self.assertEqual(text[2], "")
            self.assertEqual(text[3], forecast[0])
            self.assertEqual(text[4], self.text("2026-11-10T12:00", width=width)[1])
            self.assertLessEqual(len(text[4]), width)
            self.assertEqual(text[5], "")
            self.assertEqual(text[6:], forecast[2:])

    def test_today_graph_compares_with_daily_average_not_safe_budget(self):
        self.sample("2026-11-09T12:00", 20000)
        self.sample("2026-11-10T00:00", 21000)
        value = self.sample("2026-11-10T12:00", 23000)
        self.assertEqual(value.daily_rate, 3000)
        graph = self.text("2026-11-10T12:00")[1]
        self.assertEqual(graph, "Today [#############-------] 66.7% of daily avg")
        self.assertNotAlmostEqual(value.today_used / value.daily_budget * 100, 66.7, places=1)

    def test_today_graph_over_100_percent_and_narrow_terminal(self):
        self.sample("2026-11-08T00:00", 20000)
        self.sample("2026-11-10T00:00", 22000)
        self.sample("2026-11-10T12:00", 28000)
        graph = self.text("2026-11-10T12:00")[1]
        self.assertEqual(graph, "Today [####################] 187.5% of daily avg")
        narrow = self.text("2026-11-10T12:00", width=32)[1]
        self.assertLessEqual(len(narrow), 32)
        self.assertIn("187.5% of daily avg", narrow)
        self.assertEqual(narrow.count("#"), 4)

    def test_partial_graph_uses_observed_interval_and_no_average_is_explicit(self):
        self.sample("2026-11-10T12:00", 100000)
        self.sample("2026-11-10T13:00", 101000)
        graph = self.text("2026-11-10T13:00")[1]
        self.assertIn("Since 12:00 [", graph)
        self.assertIn("9.4% of daily avg", graph)
        self.sample("2026-11-10T14:00", 0)
        text = self.text("2026-11-10T14:00")
        self.assertEqual(text[1], "Today graph: daily average not available")
        self.assertEqual(text[4], "Est. limit by 01 Dec: 200,000 credits (no increase)")
        self.sample("2026-12-01T00:01", 100)
        text = self.text("2026-12-01T00:01")
        self.assertEqual(text[1], "Today graph: daily average not available")
        self.assertIn("collecting usage data", text[4])


if __name__ == "__main__":
    unittest.main()
