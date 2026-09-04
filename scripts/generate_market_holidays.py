#!/usr/bin/env python3
"""Generate the US equity-market holiday calendar (NYSE/NASDAQ full closures).

This is the NYSE market calendar, NOT the federal holiday calendar: it includes
Good Friday (not federal) and excludes Columbus Day and Veterans Day (federal,
but the equity markets trade).

Observance rule (NYSE): a holiday falling on Saturday is observed the preceding
Friday; falling on Sunday it is observed the following Monday. One documented
exception -- when New Year's Day falls on a Saturday the market does NOT close
the preceding December 31, so that year simply has no New Year closure.

Juneteenth became an NYSE holiday in 2022 (first observed 2022-06-20); it is
omitted for earlier years.

Ad-hoc closures (funerals, disasters, 9/11) are NOT rule-derivable and are
listed explicitly in AD_HOC below.

Usage:  python3 scripts/generate_market_holidays.py [--check]
        --check  regenerate and diff against the committed file, exit 1 on drift
"""
import argparse, datetime as dt, json, sys
from pathlib import Path

OUT = Path("include/trade_ngin/core/holidays.json")

# Full-day closures that no rule produces. Sources: NYSE historical closures.
AD_HOC = {
    "2001-09-11": "Closed - September 11 attacks",
    "2001-09-12": "Closed - September 11 attacks",
    "2001-09-13": "Closed - September 11 attacks",
    "2001-09-14": "Closed - September 11 attacks",
    "2004-06-11": "National Day of Mourning (Ronald Reagan)",
    "2007-01-02": "National Day of Mourning (Gerald Ford)",
    "2012-10-29": "Closed - Hurricane Sandy",
    "2012-10-30": "Closed - Hurricane Sandy",
    "2018-12-05": "National Day of Mourning (George H. W. Bush)",
    "2025-01-09": "National Day of Mourning (Jimmy Carter)",
}

DOW = ["Monday","Tuesday","Wednesday","Thursday","Friday","Saturday","Sunday"]


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
    """Anonymous Gregorian computus."""
    a, b, c = year % 19, year // 100, year % 100
    d, e = b // 4, b % 4
    f = (b + 8) // 25
    g = (b - f + 1) // 3
    h = (19 * a + b - d - g + 15) % 30
    i, k = c // 4, c % 4
    l = (32 + 2 * e + 2 * i - h - k) % 7
    m = (a + 11 * h + 22 * l) // 451
    month = (h + l - 7 * m + 114) // 31
    day = ((h + l - 7 * m + 114) % 31) + 1
    return dt.date(year, month, day)


def observed(d):
    """NYSE observance shift. Returns None when the closure is not observed."""
    if d.weekday() == 5:                      # Saturday
        return d - dt.timedelta(days=1)
    if d.weekday() == 6:                      # Sunday
        return d + dt.timedelta(days=1)
    return d


def build_year(year):
    out = []

    def add(date, name, kind, note=""):
        if date is None:
            return
        out.append({"date": date.isoformat(), "name": name,
                    "day_of_week": DOW[date.weekday()], "type": kind,
                    **({"note": note} if note else {})})

    # New Year's Day -- Saturday exception: no closure that year.
    ny = dt.date(year, 1, 1)
    if ny.weekday() == 5:
        pass
    elif ny.weekday() == 6:
        add(dt.date(year, 1, 2), "New Year's Day (Observed)", "fixed",
            "January 1st falls on Sunday, observed Monday")
    else:
        add(ny, "New Year's Day", "fixed")

    add(nth_weekday(year, 1, 0, 3), "Martin Luther King, Jr. Day", "moving")
    add(nth_weekday(year, 2, 0, 3), "Washington's Birthday (Presidents Day)", "moving")
    add(easter(year) - dt.timedelta(days=2), "Good Friday", "moving")
    add(nth_weekday(year, 5, 0, -1), "Memorial Day", "moving")

    if year >= 2022:
        j = dt.date(year, 6, 19)
        o = observed(j)
        add(o, "Juneteenth National Independence Day" + (" (Observed)" if o != j else ""),
            "fixed", f"June 19th falls on {DOW[j.weekday()]}, observed "
                     f"{DOW[o.weekday()][:3]}" if o != j else "")

    ind = dt.date(year, 7, 4)
    o = observed(ind)
    add(o, "Independence Day" + (" (Observed)" if o != ind else ""), "fixed",
        f"July 4th falls on {DOW[ind.weekday()]}, observed {DOW[o.weekday()][:3]}"
        if o != ind else "")

    add(nth_weekday(year, 9, 0, 1), "Labor Day", "moving")
    add(nth_weekday(year, 11, 3, 4), "Thanksgiving Day", "moving")

    xm = dt.date(year, 12, 25)
    o = observed(xm)
    add(o, "Christmas Day" + (" (Observed)" if o != xm else ""), "fixed",
        f"December 25th falls on {DOW[xm.weekday()]}, observed {DOW[o.weekday()][:3]}"
        if o != xm else "")

    for date_s, name in AD_HOC.items():
        d = dt.date.fromisoformat(date_s)
        if d.year == year:
            add(d, name, "ad_hoc", "Unscheduled closure; not rule-derivable")

    out.sort(key=lambda h: h["date"])
    return out


def build(first, last):
    return {str(y): build_year(y) for y in range(first, last + 1)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--first", type=int, default=2000)
    ap.add_argument("--last", type=int, default=2035)
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()

    generated = build(a.first, a.last)

    if a.check:
        current = json.loads(OUT.read_text())
        drift = []
        for year, days in current.items():
            gen = {h["date"]: h["name"] for h in generated.get(year, [])}
            cur = {h["date"]: h["name"] for h in days}
            for d in sorted(set(gen) | set(cur)):
                if d not in cur:
                    drift.append(f"  {year}: generator has {d} ({gen[d]}), file does not")
                elif d not in gen:
                    drift.append(f"  {year}: file has {d} ({cur[d]}), generator does not")
        if drift:
            print("Calendar drift:\n" + "\n".join(drift))
            return 1
        print(f"No drift across {len(current)} committed years.")
        return 0

    OUT.write_text(json.dumps(generated, indent=2) + "\n")
    total = sum(len(v) for v in generated.values())
    print(f"Wrote {OUT}: {a.first}-{a.last}, {total} closures.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
