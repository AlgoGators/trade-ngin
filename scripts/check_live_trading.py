"""Watchdog: has the live trading engine actually run recently?

Live trading stopped on 2026-05-05 and nothing noticed for 81 days. The engine
had been failing every night -- cron invoked a binary that does not exist -- and
writing the error to a log file inside the container that nothing surfaced.

This is the outcome-based check that would have caught it on day one: rather than
asking "did the process start", it asks "did anything actually reach the
database". That covers every failure mode at once -- container down, cron dead,
binary missing, credentials unavailable, engine crashing mid-run.

What it reads, per book (T-5 W5 / WATCHDOG-frames, fixed in T-7a C4):

  1. the RUN CLOCK: max(trading.live_results.created_at) for the book. A completed
     run deletes and re-inserts its day's live_results row as its LAST write, and
     created_at defaults to now(), so this is a real write clock of the last run
     that finished. (It used to be max(live_run_metadata.created_at): an upserted
     row that a refused run could leave behind, and that replays and future-dated
     orphan rows also satisfy.)
  2. the BOOK STAMP: the latest trading.live_results.date for the book that is not
     in the future. It says which trading date the book has been run through; rows
     dated after today (replays, orphans) are ignored.
  3. the POSITIONS STAMP: the latest trading.positions.date for the book with
     portfolio_type = 'system' (a desk save, portfolio_type 'qt', must never
     satisfy the watchdog), not in the future. Checked only when the book's latest
     results row says it holds positions (a flat book writes no position rows). A
     run that wrote results but not its positions is a partial failure.

Thresholds are CALENDAR days: the futures book runs seven days a week (the cron is
`30 9 * * *`), so a weekend is not a gap. A signal older than
MAX_CALENDAR_DAYS_SILENT days (default 1: today's run may still be running when
this checks, yesterday's may not be missing) is stale.

Read-only. Files or updates a GitHub issue when the answer is no.

Environment:
  DB_HOST / DB_PORT / DB_USER / DB_PASSWORD / DB_NAME   required
  GITHUB_TOKEN / GITHUB_REPO                            optional; issue filing
  LIVE_PORTFOLIOS                                       optional; comma list of
                                                        portfolio_id, default
                                                        CONSERVATIVE_PORTFOLIO
  MAX_CALENDAR_DAYS_SILENT                              optional; default 1

Run with --self-test to exercise the staleness logic without a database.
"""

import os
import sys
from datetime import date, datetime, timedelta, timezone

ISSUE_LABEL = "live-trading-down"
DEFAULT_REPO = "AlgoGators/trade-ngin"

# Calendar days: the futures runner runs every calendar day, so the old weekday
# arithmetic (which made a Friday stop alarm only on the following Thursday) is gone.
MAX_CALENDAR_DAYS_SILENT = int(os.environ.get("MAX_CALENDAR_DAYS_SILENT", "1"))

DEFAULT_PORTFOLIOS = "CONSERVATIVE_PORTFOLIO"


def calendar_days_between(start: date, end: date) -> int:
    """Calendar days from `start` to `end`; 0 when `end` is not after `start`."""
    if end <= start:
        return 0
    return (end - start).days


def _as_date(value):
    if value is None:
        return None
    if isinstance(value, datetime):
        return value.date()
    return value


def evaluate(book, last_run_at, last_book_date, last_position_date, last_active_positions,
             today, threshold):
    """Pure decision function for one book: returns (is_stale, list_of_reasons).

    last_run_at            max(live_results.created_at) -- the run clock
    last_book_date         max(live_results.date) <= today -- the book stamp
    last_position_date     max(positions.date) <= today, portfolio_type 'system'
    last_active_positions  active_positions on the book's latest results row
    """
    reasons = []

    if last_run_at is None:
        reasons.append(f"{book}: trading.live_results has no row -- the engine has never "
                       "completed a run for this book")
    else:
        gap = calendar_days_between(_as_date(last_run_at), today)
        if gap > threshold:
            reasons.append(f"{book}: last completed run wrote its results at "
                           f"{last_run_at:%Y-%m-%d %H:%M} ({gap} calendar days ago, "
                           f"threshold {threshold})")

    if last_book_date is not None:
        gap = calendar_days_between(_as_date(last_book_date), today)
        if gap > threshold:
            reasons.append(f"{book}: the book has been run through {last_book_date} only "
                           f"({gap} calendar days ago, threshold {threshold})")

    holds_positions = last_active_positions is not None and last_active_positions > 0
    if holds_positions and last_book_date is not None:
        if last_position_date is None or _as_date(last_position_date) < _as_date(last_book_date):
            reasons.append(f"{book}: the run for {last_book_date} wrote its results "
                           f"({last_active_positions} active positions) but no system position "
                           f"rows (latest: {last_position_date}) -- a partial write")

    return bool(reasons), reasons


def _fetch(conn, book, today):
    with conn.cursor() as cur:
        cur.execute("SELECT max(created_at) FROM trading.live_results WHERE portfolio_id = %s",
                    (book,))
        last_run = cur.fetchone()[0]
        cur.execute("SELECT date, active_positions FROM trading.live_results "
                    "WHERE portfolio_id = %s AND date <= %s ORDER BY date DESC LIMIT 1",
                    (book, today))
        row = cur.fetchone()
        last_book_date, last_active = (row[0], row[1]) if row else (None, None)
        cur.execute("SELECT max(date) FROM trading.positions WHERE portfolio_id = %s "
                    "AND portfolio_type = 'system' AND date <= %s", (book, today))
        last_pos = cur.fetchone()[0]
    return last_run, last_book_date, last_pos, last_active


def _file_issue(reasons, repo, token):
    import requests

    headers = {
        "Authorization": f"Bearer {token}",
        "Accept": "application/vnd.github+json",
    }
    title = "[live-trading-down] The trading engine has stopped writing"
    body = (
        "The live-trading watchdog found no recent writes to the `trading` schema.\n\n"
        + "\n".join(f"- {r}" for r in reasons)
        + "\n\nThe database can only show that nothing arrived, not why. Check, in order:\n"
        "1. Is the `trade-ngin` container running on the trading host? (`docker ps`)\n"
        "2. Is cron alive inside it? (the image has a HEALTHCHECK for this)\n"
        "3. `docker logs trade-ngin` -- the scheduled run logs there now, timestamped\n"
    )

    query = f"repo:{repo} is:issue is:open label:{ISSUE_LABEL}"
    resp = requests.get(
        "https://api.github.com/search/issues",
        params={"q": query},
        headers=headers,
        timeout=10,
    )
    resp.raise_for_status()
    items = resp.json().get("items", [])

    if items:
        number = items[0]["number"]
        resp = requests.post(
            f"https://api.github.com/repos/{repo}/issues/{number}/comments",
            json={"body": body},
            headers=headers,
            timeout=10,
        )
        resp.raise_for_status()
        print(f"Updated existing issue #{number}")
    else:
        resp = requests.post(
            f"https://api.github.com/repos/{repo}/issues",
            json={"title": title, "body": body, "labels": [ISSUE_LABEL]},
            headers=headers,
            timeout=10,
        )
        resp.raise_for_status()
        print(f"Filed issue #{resp.json()['number']}")


def self_test():
    """Exercise the staleness logic without a database.

    CI runs `--self-test` directly; tests/scripts/test_check_live_trading.py runs the
    same checks and more through ctest.
    """
    failures = []

    def check(name, got, want):
        if got != want:
            failures.append(f"{name}: got {got!r}, wanted {want!r}")

    check("Fri->Mon is 3 calendar days",
          calendar_days_between(date(2026, 7, 24), date(2026, 7, 27)), 3)
    check("same day is 0", calendar_days_between(date(2026, 7, 24), date(2026, 7, 24)), 0)
    check("backwards is 0", calendar_days_between(date(2026, 7, 24), date(2026, 7, 20)), 0)

    at = lambda d: datetime(d.year, d.month, d.day, 13, 45)
    book = "CONSERVATIVE_PORTFOLIO"

    # Today's run done: healthy.
    stale, _ = evaluate(book, at(date(2026, 7, 27)), date(2026, 7, 27), date(2026, 7, 27), 12,
                        date(2026, 7, 27), 1)
    check("today's run done => healthy", stale, False)

    # Yesterday's run done, today's not yet: tolerated.
    stale, _ = evaluate(book, at(date(2026, 7, 26)), date(2026, 7, 26), date(2026, 7, 26), 12,
                        date(2026, 7, 27), 1)
    check("yesterday done, today pending => healthy", stale, False)

    # A stop after Friday's run is caught on Sunday, not on Thursday.
    stale, reasons = evaluate(book, at(date(2026, 7, 24)), date(2026, 7, 24), date(2026, 7, 24),
                              12, date(2026, 7, 26), 1)
    check("friday stop seen on sunday => stale", stale, True)
    check("friday stop names both run signals", len(reasons), 2)

    # The real outage: last run 2026-05-05, checked 2026-07-25.
    stale, _ = evaluate(book, at(date(2026, 5, 5)), date(2026, 5, 5), date(2026, 5, 3), 12,
                        date(2026, 7, 25), 1)
    check("the actual 81-day outage => stale", stale, True)

    # No results row at all.
    stale, _ = evaluate(book, None, None, None, None, date(2026, 7, 27), 1)
    check("never ran => stale", stale, True)

    # Results written, positions not: a partial write.
    stale, reasons = evaluate(book, at(date(2026, 7, 27)), date(2026, 7, 27), date(2026, 7, 26),
                              12, date(2026, 7, 27), 1)
    check("results without positions => stale", stale, True)
    check("partial write names one signal", len(reasons), 1)

    # A flat book writes no position rows: not a partial write.
    stale, _ = evaluate(book, at(date(2026, 7, 27)), date(2026, 7, 27), None, 0,
                        date(2026, 7, 27), 1)
    check("flat book => healthy", stale, False)

    if failures:
        print("SELF-TEST FAILED:")
        for f in failures:
            print(f"  {f}")
        return 1
    print("self-test: all checks passed")
    return 0


def main():
    if "--self-test" in sys.argv:
        return self_test()

    import psycopg2

    books = [b.strip() for b in os.environ.get("LIVE_PORTFOLIOS", DEFAULT_PORTFOLIOS).split(",")
             if b.strip()]
    today = datetime.now(timezone.utc).date()
    conn = psycopg2.connect(
        host=os.environ["DB_HOST"],
        port=os.environ.get("DB_PORT", "5432"),
        user=os.environ["DB_USER"],
        password=os.environ["DB_PASSWORD"],
        dbname=os.environ["DB_NAME"],
        connect_timeout=15,
    )
    reasons = []
    try:
        for book in books:
            last_run, last_book_date, last_pos, last_active = _fetch(conn, book, today)
            print(f"{book}: last completed run {last_run}; book run through {last_book_date}; "
                  f"system positions through {last_pos}; active positions {last_active}")
            _, book_reasons = evaluate(book, last_run, last_book_date, last_pos, last_active,
                                       today, MAX_CALENDAR_DAYS_SILENT)
            reasons.extend(book_reasons)
    finally:
        conn.close()

    if not reasons:
        print("OK: live trading is writing within the expected window.")
        return 0

    print("STALE:")
    for r in reasons:
        print(f"  {r}")

    token = os.environ.get("GITHUB_TOKEN")
    if token:
        _file_issue(reasons, os.environ.get("GITHUB_REPO", DEFAULT_REPO), token)
    else:
        print("GITHUB_TOKEN not set; skipping issue filing.", file=sys.stderr)

    return 1


if __name__ == "__main__":
    sys.exit(main())
