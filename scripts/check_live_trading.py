"""Watchdog: has the live trading engine actually run recently?

Live trading stopped on 2026-05-05 and nothing noticed for 81 days. The engine
had been failing every night -- cron invoked a binary that does not exist -- and
writing the error to a log file inside the container that nothing surfaced.

This is the outcome-based check that would have caught it on day one: rather than
asking "did the process start", it asks "did anything actually reach the
database". That covers every failure mode at once -- container down, cron dead,
binary missing, credentials unavailable, engine crashing mid-run.

Read-only. Files or updates a GitHub issue when the answer is no.

Environment:
  DB_HOST / DB_PORT / DB_USER / DB_PASSWORD / DB_NAME   required
  GITHUB_TOKEN / GITHUB_REPO                            optional; issue filing
  MAX_BUSINESS_DAYS_SILENT                              optional; default 3

Run with --self-test to exercise the staleness logic without a database.
"""

import os
import sys
from datetime import date, datetime, timedelta, timezone

ISSUE_LABEL = "live-trading-down"
DEFAULT_REPO = "AlgoGators/trade-ngin"

# Trading runs Mon-Fri, so calendar-day thresholds produce weekend false alarms.
# Three business days tolerates a public holiday next to a weekend while still
# surfacing a genuine outage inside the same week.
MAX_BUSINESS_DAYS_SILENT = int(os.environ.get("MAX_BUSINESS_DAYS_SILENT", "3"))


def business_days_between(start: date, end: date) -> int:
    """Weekdays strictly after `start`, up to and including `end`.

    Holidays are deliberately not modelled: a market holiday makes this
    over-count by one, which the threshold absorbs. Under-counting would be the
    dangerous direction, and this never does that.
    """
    if end <= start:
        return 0
    days = 0
    cursor = start + timedelta(days=1)
    while cursor <= end:
        if cursor.weekday() < 5:  # Mon-Fri
            days += 1
        cursor += timedelta(days=1)
    return days


def evaluate(last_run_at, last_position_at, today, threshold):
    """Pure decision function: returns (is_stale, list_of_reasons).

    Takes the two independent signals -- when the engine last recorded a run, and
    when a position row was last physically written -- because they fail
    differently. A run that starts and dies mid-way updates one but not the other.
    """
    reasons = []

    if last_run_at is None:
        reasons.append(
            "trading.live_run_metadata is empty -- the engine has never recorded a run"
        )
    else:
        gap = business_days_between(last_run_at.date(), today)
        if gap > threshold:
            reasons.append(
                f"last engine run was {last_run_at:%Y-%m-%d %H:%M} "
                f"({gap} business days ago, threshold {threshold})"
            )

    if last_position_at is None:
        reasons.append(
            "trading.positions is empty -- no positions have ever been written"
        )
    else:
        gap = business_days_between(last_position_at.date(), today)
        if gap > threshold:
            reasons.append(
                f"last position write was {last_position_at:%Y-%m-%d %H:%M} "
                f"({gap} business days ago, threshold {threshold})"
            )

    return bool(reasons), reasons


def _fetch(conn):
    with conn.cursor() as cur:
        cur.execute("SELECT max(created_at) FROM trading.live_run_metadata")
        last_run = cur.fetchone()[0]
        cur.execute("SELECT max(updated_at) FROM trading.positions")
        last_pos = cur.fetchone()[0]
    return last_run, last_pos


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

    Kept in-process rather than as a pytest suite because this is a C++ repo with
    no Python test infrastructure; CI runs `--self-test` directly.
    """
    failures = []

    def check(name, got, want):
        if got != want:
            failures.append(f"{name}: got {got!r}, wanted {want!r}")

    # business_days_between
    check(
        "Fri->Mon is 1 business day",
        business_days_between(date(2026, 7, 24), date(2026, 7, 27)),
        1,
    )
    check(
        "Fri->Sat is 0", business_days_between(date(2026, 7, 24), date(2026, 7, 25)), 0
    )
    check(
        "Mon->Fri is 4", business_days_between(date(2026, 7, 20), date(2026, 7, 24)), 4
    )
    check(
        "same day is 0", business_days_between(date(2026, 7, 24), date(2026, 7, 24)), 0
    )
    check(
        "backwards is 0", business_days_between(date(2026, 7, 24), date(2026, 7, 20)), 0
    )

    monday = date(2026, 7, 27)
    dt = lambda d: datetime(d.year, d.month, d.day, 9, 30, tzinfo=timezone.utc)

    # A Friday run seen on Monday is healthy -- the weekend must not alarm.
    stale, _ = evaluate(dt(date(2026, 7, 24)), dt(date(2026, 7, 24)), monday, 3)
    check("friday run, monday check => healthy", stale, False)

    # The real outage: last run 2026-05-05, checked 2026-07-25.
    stale, reasons = evaluate(
        dt(date(2026, 5, 5)), dt(date(2026, 5, 3)), date(2026, 7, 25), 3
    )
    check("the actual 81-day outage => stale", stale, True)
    check("outage reports both signals", len(reasons), 2)

    # Empty tables are stale, not silently healthy.
    stale, reasons = evaluate(None, None, monday, 3)
    check("empty tables => stale", stale, True)
    check("empty tables report both", len(reasons), 2)

    # Engine recorded a run but wrote no positions -- a partial failure.
    stale, reasons = evaluate(dt(monday), dt(date(2026, 5, 3)), monday, 3)
    check("run ok but positions stale => stale", stale, True)
    check("partial failure names one signal", len(reasons), 1)

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

    conn = psycopg2.connect(
        host=os.environ["DB_HOST"],
        port=os.environ.get("DB_PORT", "5432"),
        user=os.environ["DB_USER"],
        password=os.environ["DB_PASSWORD"],
        dbname=os.environ["DB_NAME"],
        connect_timeout=15,
    )
    try:
        last_run, last_pos = _fetch(conn)
    finally:
        conn.close()

    today = datetime.now(timezone.utc).date()
    print(f"last engine run:     {last_run}")
    print(f"last position write: {last_pos}")

    stale, reasons = evaluate(last_run, last_pos, today, MAX_BUSINESS_DAYS_SILENT)

    if not stale:
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
