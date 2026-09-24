#!/usr/bin/env python3
"""The seven-day schedule and the watchdog (T-7a commit 4; HD 2026-09-17, T-5 W5 / WATCHDOG-frames).

  * live_portfolio.cron runs the futures runner every calendar day (`30 9 * * *`), and
    scripts/run_live_portfolio.sh no longer exits on a Saturday or Sunday;
  * scripts/check_live_trading.py reads a real write clock (the last completed run's
    live_results row), a book stamp that ignores future-dated rows, and the positions of the
    SYSTEM book only, per portfolio, against a calendar-day threshold; the workflow runs daily.

The database case runs only when TRADE_NGIN_TEST_DSN is set (tests/…/unit.sh points it at the
session clone). It writes inside one transaction and rolls it back.

Run directly (python3 tests/scripts/test_check_live_trading.py) or through ctest.
"""

import importlib.util
import os
import sys
import unittest
from datetime import date, datetime, timedelta

sys.dont_write_bytecode = True  # no scripts/__pycache__ left in the tree

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SCRIPT = os.path.join(REPO_ROOT, "scripts", "check_live_trading.py")


def load_watchdog():
    spec = importlib.util.spec_from_file_location("check_live_trading", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def read(rel):
    with open(os.path.join(REPO_ROOT, rel)) as f:
        return f.read()


class Schedule(unittest.TestCase):
    def test_the_cron_runs_every_calendar_day(self):
        lines = [l for l in read("live_portfolio.cron").splitlines()
                 if l.strip() and not l.lstrip().startswith("#")]
        self.assertEqual(len(lines), 1, lines)
        fields = lines[0].split()
        self.assertEqual(fields[:5], ["30", "9", "*", "*", "*"],
                         "the futures runner must run seven days a week")
        self.assertIn("/app/scripts/run_live_portfolio.sh", lines[0])

    def test_the_wrapper_has_no_weekend_exit(self):
        src = read("scripts/run_live_portfolio.sh")
        code = "\n".join(l for l in src.splitlines() if not l.lstrip().startswith("#"))
        self.assertNotIn("date +%u", code, "the wrapper still reads the day of the week")
        self.assertNotRegex(code, r"skipping: weekend")
        self.assertRegex(code, r'"\$BINARY" "\$DATE" --send-email',
                         "every run is dated with the host's date")

    def test_the_watchdog_workflow_runs_daily(self):
        wf = read(".github/workflows/live-trading-watchdog.yml")
        self.assertIn('cron: "0 15 * * *"', wf)
        self.assertNotIn("1-5", wf)


class Evaluate(unittest.TestCase):
    def setUp(self):
        self.w = load_watchdog()
        self.at = lambda d: datetime(d.year, d.month, d.day, 13, 45)
        self.book = "CONSERVATIVE_PORTFOLIO"

    def ev(self, run_day, book_day, pos_day, active, today, threshold=1):
        run = self.at(run_day) if run_day else None
        return self.w.evaluate(self.book, run, book_day, pos_day, active, today, threshold)

    def test_the_self_test_passes(self):
        self.assertEqual(self.w.self_test(), 0)

    def test_a_weekend_is_not_a_gap(self):
        # Seven-day schedule: Saturday's and Sunday's runs happen; a Sunday check after the
        # Sunday run is healthy.
        stale, _ = self.ev(date(2026, 7, 26), date(2026, 7, 26), date(2026, 7, 26), 12,
                           date(2026, 7, 26))
        self.assertFalse(stale)

    def test_a_friday_stop_alarms_on_sunday_not_thursday(self):
        # The old weekday arithmetic first alarmed on Thursday, five runs later.
        stale, reasons = self.ev(date(2026, 7, 24), date(2026, 7, 24), date(2026, 7, 24), 12,
                                 date(2026, 7, 26))
        self.assertTrue(stale)
        self.assertTrue(any("calendar days ago" in r for r in reasons), reasons)

    def test_a_refused_run_leaves_nothing_the_watchdog_counts(self):
        # A refused run writes no live_results row (every refusal sits above the writes), so
        # the run clock stays at the last completed run.
        stale, _ = self.ev(date(2026, 5, 17), date(2026, 5, 17), date(2026, 5, 17), 13,
                           date(2026, 5, 19))
        self.assertTrue(stale)

    def test_a_partial_write_is_caught_and_a_flat_book_is_not(self):
        stale, reasons = self.ev(date(2026, 7, 27), date(2026, 7, 27), date(2026, 7, 26), 12,
                                 date(2026, 7, 27))
        self.assertTrue(stale)
        self.assertEqual(len(reasons), 1)
        self.assertIn("partial write", reasons[0])
        stale, _ = self.ev(date(2026, 7, 27), date(2026, 7, 27), None, 0, date(2026, 7, 27))
        self.assertFalse(stale)

    def test_the_threshold_is_calendar_days(self):
        stale, _ = self.ev(date(2026, 7, 25), date(2026, 7, 25), date(2026, 7, 25), 12,
                           date(2026, 7, 27), threshold=2)
        self.assertFalse(stale)
        stale, _ = self.ev(date(2026, 7, 25), date(2026, 7, 25), date(2026, 7, 25), 12,
                           date(2026, 7, 27), threshold=1)
        self.assertTrue(stale)

    def test_the_old_upsertable_clock_is_not_read(self):
        src = read("scripts/check_live_trading.py")
        code = "\n".join(l for l in src.splitlines() if not l.lstrip().startswith("#"))
        # T-7b-1 C7b R5 reads live_run_metadata for the day's refusal MARK; the run CLOCK must
        # still never come from that upsertable table.
        self.assertNotRegex(code, r"max\((?:created_at|updated_at)\)[^\n]*FROM trading\.live_run_metadata")
        self.assertNotIn("max(updated_at)", code)
        self.assertIn("portfolio_type = 'system'", code)
        self.assertNotIn("business_days_between", code)


class C7bHeldDay(unittest.TestCase):
    """T-7b-1 C7b R5 (T-7a_CODE_REVIEW R5): a day whose run refused its book (a portfolio risk
    REFUSE or a risk module that could not answer, exit 3) stores live_results and positions, so
    the three write stamps read healthy. The metadata row carries the mark; the watchdog reports it.
    The STRICT assertion's mark (R4) is reported the same way."""

    def setUp(self):
        self.w = load_watchdog()
        self.at = lambda d: datetime(d.year, d.month, d.day, 13, 45)
        self.book = "CONSERVATIVE_PORTFOLIO"

    def test_a_held_day_is_reported_although_every_write_stamp_is_fresh(self):
        today = date(2026, 7, 27)
        stale, reasons = self.w.evaluate(self.book, self.at(today), today, today, 12, today, 1,
                                         latest_mark_date=today, latest_marks=["risk_refusal"])
        self.assertTrue(stale, "a held day reads as a healthy run")
        self.assertEqual(len(reasons), 1, reasons)
        self.assertIn("risk_refusal", reasons[0])
        self.assertIn("2026-07-27", reasons[0])

    def test_the_strict_assertion_mark_is_reported(self):
        today = date(2026, 7, 27)
        stale, reasons = self.w.evaluate(self.book, self.at(today), today, today, 12, today, 1,
                                         latest_mark_date=today, latest_marks=["strict_assertion"])
        self.assertTrue(stale)
        self.assertTrue(any("strict_assertion" in r for r in reasons), reasons)

    def test_an_unmarked_latest_run_is_healthy(self):
        today = date(2026, 7, 27)
        stale, _ = self.w.evaluate(self.book, self.at(today), today, today, 12, today, 1,
                                   latest_mark_date=today, latest_marks=[])
        self.assertFalse(stale)


@unittest.skipUnless(os.environ.get("TRADE_NGIN_TEST_DSN"), "TRADE_NGIN_TEST_DSN not set")
class C7bFetchOnTheClone(unittest.TestCase):
    """R5 and R12 against the real schema, inside one transaction that is rolled back."""

    def test_the_latest_runs_mark_is_read(self):
        import psycopg2

        w = load_watchdog()
        conn = psycopg2.connect(os.environ["TRADE_NGIN_TEST_DSN"])
        book = "C7B_WATCHDOG_PROBE"
        try:
            with conn.cursor() as cur:
                for d, cfg in ((date(2030, 1, 9), '{"total_capital": 1}'),
                               (date(2030, 1, 10), '{"total_capital": 1, "risk_refusal": {"module": "carver"}}'),
                               (date(2030, 2, 20), '{"strict_assertion": {}}')):
                    cur.execute("INSERT INTO trading.live_run_metadata (date, strategy_id, portfolio_id, "
                                "strategy_allocations, portfolio_config) VALUES (%s, 'PROBE', %s, "
                                "'{}'::jsonb, %s::jsonb)", (d, book, cfg))
            mark_date, marks = w._fetch_run_marks(conn, book, date(2030, 1, 10))
            self.assertEqual(mark_date, date(2030, 1, 10), "the future-dated row is ignored")
            self.assertEqual(marks, ["risk_refusal"])
            mark_date, marks = w._fetch_run_marks(conn, book, date(2030, 1, 9))
            self.assertEqual((mark_date, marks), (date(2030, 1, 9), []))
        finally:
            conn.rollback()
            conn.close()

    def test_the_run_clock_is_read_in_utc(self):
        # R12: created_at is a timestamp WITHOUT time zone, written in the writer session's zone.
        # On a session in New York, 21:30 on 2030-01-10 is 02:30 UTC on 2030-01-11, the frame of
        # the watchdog's `today`.
        import psycopg2

        w = load_watchdog()
        conn = psycopg2.connect(os.environ["TRADE_NGIN_TEST_DSN"])
        book = "C7B_WATCHDOG_TZ_PROBE"
        try:
            with conn.cursor() as cur:
                cur.execute("SET LOCAL TimeZone = 'America/New_York'")
                cur.execute("INSERT INTO trading.live_results (strategy_id, portfolio_id, date, "
                            "active_positions, created_at) VALUES ('PROBE', %s, %s, 0, "
                            "'2030-01-10 21:30:00')", (book, date(2030, 1, 10)))
            last_run, _, _, _ = w._fetch(conn, book, date(2030, 1, 11))
            self.assertEqual(last_run.replace(tzinfo=None), datetime(2030, 1, 11, 2, 30),
                             "the run clock is read in the session's zone, not in UTC")
        finally:
            conn.rollback()
            conn.close()


@unittest.skipUnless(os.environ.get("TRADE_NGIN_TEST_DSN"), "TRADE_NGIN_TEST_DSN not set")
class FetchOnTheClone(unittest.TestCase):
    """_fetch against a real schema: a QT-book (desk) position row and a future-dated results row,
    written inside one transaction and rolled back, must not satisfy the watchdog. The schema's
    portfolio_type is 'system' or 'qt' (positions_portfolio_type_check)."""

    def test_qt_rows_and_future_rows_are_ignored(self):
        import psycopg2

        w = load_watchdog()
        conn = psycopg2.connect(os.environ["TRADE_NGIN_TEST_DSN"])
        book = "C4_WATCHDOG_PROBE"
        today = date(2030, 1, 10)
        try:
            with conn.cursor() as cur:
                cur.execute("INSERT INTO trading.live_results (strategy_id, portfolio_id, date, "
                            "active_positions) VALUES ('PROBE', %s, %s, 3)", (book, today))
                cur.execute("INSERT INTO trading.live_results (strategy_id, portfolio_id, date, "
                            "active_positions) VALUES ('PROBE', %s, %s, 3)",
                            (book, today + timedelta(days=40)))
                cur.execute(
                    "INSERT INTO trading.positions (symbol, quantity, average_price, "
                    "daily_unrealized_pnl, daily_realized_pnl, last_update, updated_at, "
                    "strategy_id, strategy_name, date, portfolio_id, portfolio_type) VALUES "
                    "('ZZ.v.0', 1, 1, 0, 0, %s, %s, 'PROBE', 'PROBE', %s, %s, 'qt')",
                    (today, today, today, book))
            last_run, last_book_date, last_pos, last_active = w._fetch(conn, book, today)
            self.assertIsNotNone(last_run, "the run clock is the results row's created_at")
            self.assertEqual(last_book_date, today, "the future-dated row is ignored")
            self.assertEqual(last_active, 3)
            self.assertIsNone(last_pos, "a qt (desk) row never satisfies the system book")
            stale, reasons = w.evaluate(book, last_run, last_book_date, last_pos, last_active,
                                        today, 1)
            self.assertTrue(stale)
            self.assertTrue(any("partial write" in r for r in reasons), reasons)
        finally:
            conn.rollback()
            conn.close()


if __name__ == "__main__":
    unittest.main()
