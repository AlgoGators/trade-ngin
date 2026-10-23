#!/usr/bin/env python3
"""Generate the futures exchanges' trading calendars (include/trade_ngin/core/futures_holidays.json).

This is NOT the equity calendar (scripts/generate_market_holidays.py, holidays.json, the NYSE's): the
per-contract roll rule counts the trading days of the exchange that lists the contract, and CME Group
traded on days the NYSE closed (2012-10-29 and 10-30, 2018-12-05, 2025-01-09).

A trading day for a product is a day the exchange publishes a settlement for it (a trade date). On a US
holiday CME Globex may trade until a midday halt, but those trades belong to the next trade date, so
the holiday is not a trading day. Where CME treats its product groups differently (a Good Friday with
the US employment report, a day of mourning), the closure lists only the groups it closes.

One calendar per exchange, so ICE (the softs) can be added as its own. Each exchange has its product
groups and their roots, its coverage years, and its closures: {date, name, closes: [groups] or
["all"], note}.

Usage:  python3 scripts/generate_futures_holidays.py [--check]
        --check  regenerate and diff against the committed file, exit 1 on drift
"""
import argparse
import datetime as dt
import json
import sys
from pathlib import Path

OUT = Path("include/trade_ngin/core/futures_holidays.json")
FIRST, LAST = 2010, 2040

CME_GROUPS = {
    "equity_index": ["ES", "NQ", "YM", "RTY", "MES", "MNQ", "MYM", "M2K"],
    "interest_rates": ["ZT", "ZF", "ZN", "UB"],
    "fx": ["6A", "6B", "6C", "6E", "6J", "6L", "6M", "6N", "6S"],
    "energy": ["CL", "HO", "NG", "RB"],
    "metals": ["GC", "SI", "HG", "PL"],
    "grains": ["ZC", "ZS", "ZW", "KE", "ZL", "ZM", "ZR"],
    "livestock": ["LE", "HE", "GF"],
    "crypto": ["MBT"],
}

# Good Fridays with the US employment report: CME opened these groups for an abbreviated session
# with a settlement (a trade date); every other group published none (equity index settlements were
# carried from Thursday). Sources: CME Good Friday settlement-time PDFs and clearing advisories 2012,
# 2015, 2021, 2023, 2026; 2010 and 2015 FX from the CME floor holiday cards (settlement inferred).
GOOD_FRIDAY_OPEN = {
    "2010-04-02": ["interest_rates", "fx"],
    "2012-04-06": ["interest_rates", "fx"],
    "2015-04-03": ["interest_rates", "fx"],
    "2021-04-02": ["interest_rates", "fx"],
    "2023-04-07": ["interest_rates", "fx", "crypto"],
    "2026-04-03": ["interest_rates", "fx", "crypto"],
}

# Unscheduled closures of some CME groups. The NYSE closed on all four days; CME settled every group
# not listed here. Sources: CFTC filing 12-363 and clearing advisory 12-464 (Sandy), SER-8289 and
# clearing advisory 18-474 (2018), CME's 2025-01-09 press release.
AD_HOC = {
    "2012-10-29": ("Hurricane Sandy", ["equity_index"],
                   "equity index settlements carried forward from Friday 10-26; every other group traded"),
    "2012-10-30": ("Hurricane Sandy", ["equity_index"],
                   "equity index overnight session only, settlements carried forward; every other group traded"),
    "2018-12-05": ("National Day of Mourning (George H. W. Bush)", ["interest_rates", "equity_index"],
                   "interest rates closed for the trade date; equity index overnight session to 08:30 CT only "
                   "(no settlement, inferred); every other group traded"),
    "2025-01-09": ("National Day of Mourning (Jimmy Carter)", ["equity_index"],
                   "equity index overnight session to 08:30 CT, prior settlements carried forward; every other "
                   "group traded and settled (rates and agriculture early at 12:00 CT)"),
}

DOW = ["Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"]


def nth_weekday(year, month, weekday, n):
    """n-th `weekday` (Mon=0) of month; n=-1 means last."""
    if n > 0:
        d = dt.date(year, month, 1)
        d += dt.timedelta(days=(weekday - d.weekday()) % 7)
        return d + dt.timedelta(weeks=n - 1)
    d = dt.date(year, month, 1) + dt.timedelta(days=32)
    d = d.replace(day=1) - dt.timedelta(days=1)
    return d - dt.timedelta(days=(d.weekday() - weekday) % 7)


def easter(year):
    a, b, c = year % 19, year // 100, year % 100
    d, e = b // 4, b % 4
    f = (b + 8) // 25
    g = (b - f + 1) // 3
    h = (19 * a + b - d - g + 15) % 30
    i, k = c // 4, c % 4
    l = (32 + 2 * e + 2 * i - h - k) % 7
    m = (a + 11 * h + 22 * l) // 451
    return dt.date(year, (h + l - 7 * m + 114) // 31, ((h + l - 7 * m + 114) % 31) + 1)


def observed(d):
    """Saturday -> Friday, Sunday -> Monday."""
    if d.weekday() == 5:
        return d - dt.timedelta(days=1)
    if d.weekday() == 6:
        return d + dt.timedelta(days=1)
    return d


def cme_year(year):
    out = []

    def add(date, name, closes=("all",), note=""):
        out.append({"date": date.isoformat(), "name": name, "day_of_week": DOW[date.weekday()],
                    "closes": list(closes), **({"note": note} if note else {})})

    ny = dt.date(year, 1, 1)
    if ny.weekday() == 6:
        add(dt.date(year, 1, 2), "New Year's Day (Observed)")
    elif ny.weekday() != 5:  # on a Saturday CME does not close the Friday before
        add(ny, "New Year's Day")
    add(nth_weekday(year, 1, 0, 3), "Martin Luther King, Jr. Day")
    add(nth_weekday(year, 2, 0, 3), "Washington's Birthday (Presidents Day)")
    gf = easter(year) - dt.timedelta(days=2)
    if gf.isoformat() in GOOD_FRIDAY_OPEN:
        open_groups = GOOD_FRIDAY_OPEN[gf.isoformat()]
        add(gf, "Good Friday", [g for g in CME_GROUPS if g not in open_groups],
            "US employment report: " + ", ".join(open_groups) + " traded an abbreviated session with a settlement")
    else:
        add(gf, "Good Friday")
    add(nth_weekday(year, 5, 0, -1), "Memorial Day")
    if year >= 2022:
        add(observed(dt.date(year, 6, 19)), "Juneteenth National Independence Day")
    add(observed(dt.date(year, 7, 4)), "Independence Day")
    add(nth_weekday(year, 9, 0, 1), "Labor Day")
    add(nth_weekday(year, 11, 3, 4), "Thanksgiving Day")
    add(observed(dt.date(year, 12, 25)), "Christmas Day")
    for date_s, (name, groups, note) in AD_HOC.items():
        d = dt.date.fromisoformat(date_s)
        if d.year == year:
            add(d, name, groups, note)
    out.sort(key=lambda h: h["date"])
    return out


def build():
    return {
        "_description": "Futures exchanges' trading calendars for the per-contract roll rule: a trading day is a "
                        "weekday the exchange publishes a settlement for the product. Generated by "
                        "scripts/generate_futures_holidays.py; do not edit by hand.",
        "exchanges": {
            "CME": {
                "name": "CME Group (CME, CBOT, NYMEX, COMEX)",
                "covers": {"first_year": FIRST, "last_year": LAST},
                "groups": CME_GROUPS,
                "closures": [c for y in range(FIRST, LAST + 1) for c in cme_year(y)],
            }
        },
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    text = json.dumps(build(), indent=2) + "\n"
    if a.check:
        current = OUT.read_text()
        if current != text:
            gen = {(c["date"], tuple(c["closes"])) for c in build()["exchanges"]["CME"]["closures"]}
            cur = {(c["date"], tuple(c["closes"])) for c in json.loads(current)["exchanges"]["CME"]["closures"]}
            for x in sorted(gen - cur):
                print(f"  generator has {x[0]} closing {list(x[1])}, the file does not")
            for x in sorted(cur - gen):
                print(f"  the file has {x[0]} closing {list(x[1])}, the generator does not")
            print("Calendar drift (or a change outside the closures): regenerate")
            return 1
        print("No drift.")
        return 0
    OUT.write_text(text)
    n = len(build()["exchanges"]["CME"]["closures"])
    print(f"Wrote {OUT}: CME {FIRST}-{LAST}, {n} closures.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
