#!/usr/bin/env python3
"""trading_rule_costs.py: which trend speeds each contract of the conservative book may run, by cost.

The rule (Carver, Advanced Futures Trading Strategies, strategy nine, "Removing expensive trading
rules", with the cost definitions of strategy three and the turnovers of table 35): instrument by
instrument, a trading rule is removed when its yearly cost exceeds 0.15 Sharpe-ratio units, where

    yearly cost of a rule on a contract = (turnover of the rule + 2 x rolls a year) x cost per trade
    cost per trade (risk adjusted)      = cost of one contract / (price x multiplier) / sigma

sigma being the contract's annual standard deviation of returns. The rules left weigh equally and
take the multiplier of table 36 for their number (the engine's table).

THE COMMITTED LIST IS THE AUTHORITY. portfolio.json's trading_rule_removals is the owner's decision
(HD, 2026-10-09). This tool shows how the list follows from the recipe below and, with --check,
REPORTS whether today's records still give it; it never writes the list. When the costs are
re-measured (the fee table changes, the cost model changes, the universe or a listing date changes,
or the yearly review on the first run after the turn of the year), a list that comes out different
is a decision for the owner, never an automatic edit. Every cell within FLAG of the limit is
flagged, since such a cell can change sides on a re-measurement.

The recipe (the owner's ruling):
  turnover of a rule    The book's table 35, the same figure for every contract: EWMAC2 98.5,
                        EWMAC4 50.2, EWMAC8 25.4, EWMAC16 13.2, EWMAC32 7.6, EWMAC64 5.2. Our own
                        measured turnovers are printed beside it for the record and decide nothing.
  cost per trade        The MEDIAN, over every signal day of the measuring run after its warm-up
                        (the first day the book is sized, 2011-08-04, onward), of
                        cost / (close x multiplier) / sigma. The two contracts of a listing-date
                        pair each on their own traded days: the contract replaced before the
                        listing date, the listed contract from it.
  rolls a year          The rolls the engine confirmed (and booked) on the contract's consumed
                        series in the run, over that series' span in years.

This tool only does arithmetic. Every input is a record the ENGINE writes about its own run, so the
tool cannot drift from what the engine charges or forecasts:

  estimator_<sleeve>.csv   one row a contract a signal day (strategy/trend_estimator_record.hpp):
                           the sizing volatility `sigma` (trend_estimator::estimate) and, for the
                           measured turnovers, each speed's scaled forecast and the raw close
  onepass_book_<book>.csv  one row a contract a signal day (optimization/one_pass_record.hpp):
                           `cost`, the dollars TransactionCostManager::calculate_costs(symbol, 1,
                           close).total_transaction_costs returns for ONE contract at the signal
                           close: the fee per contract of metadata.contract_metadata plus the
                           spread (SpreadModel) and the impact (ImpactModel) the engine charges a
                           fill; and the `close` and `multiplier` it was priced on
  onepass_days_<book>.csv  one row a signal day: `warmup` is 1 until the book is sized
  series.csv               one row a consumed bar (backtest/consumed_series_record.hpp): `confirm`
                           is 1 on the bar a roll is confirmed on (data/roll_series)

How the records are made (the FIXED measuring run): bt_portfolio_conservative with
TRADE_NGIN_SERIES_DUMP_DIR set, backtest.lookback_years 16 and backtest.frozen_end_date 2026-10-07,
on the fee table in force, with the template's listing_dates and instrument_id_relabels and WITHOUT
trading_rule_removals (records made with removals are refused: a removed speed has no forecast).
The costs read here do not depend on the book held, only on prices, volumes and the fee table. The
record files are read plain or gzipped.

The measured turnovers printed for the record, each the plain mean over the instruments (a
listing-date pair is one instrument) with at least MIN_ROWS signal days, per calendar year:
  forecast   the yearly sum of |change of the speed's scaled forecast| / 10 (the book's appendix B)
  position   the speed alone on the unrounded target at a fixed capital, n = forecast / (price x
             sigma): the yearly sum of |change of n| / mean |n|

Usage:
  trading_rule_costs.py <records dir> --portfolio <portfolio.json> [--out <dir>] [--check]
Writes nothing to any database and never writes portfolio.json. With --out: trading_rule_removals.json
(the block as computed), trading_rule_costs.csv (every contract by every speed, every digit) and
trading_rule_costs.md (the tables). Exit status: 0; 1 when --check finds the committed list is not
the computed one; 2 when one side of a listing-date pair would lose a rule the other keeps.
"""
import argparse
import csv
import datetime
import glob
import gzip
import json
import math
import os
import statistics
import sys

LIMIT = 0.15        # Sharpe-ratio units a year, for one trading rule on one contract
FLAG = 0.02         # a cell within this fraction of the limit is flagged
MIN_ROWS = 512      # signal days an instrument needs to count in a measured turnover
TABLE_35 = {(2, 8): 98.5, (4, 16): 50.2, (8, 32): 25.4, (16, 64): 13.2, (32, 128): 7.6, (64, 256): 5.2}


def open_record(directory, pattern):
    names = sorted(glob.glob(os.path.join(directory, pattern)) +
                   glob.glob(os.path.join(directory, pattern + ".gz")))
    if len(names) != 1:
        sys.exit("expected exactly one %s[.gz] in %s, found %d" % (pattern, directory, len(names)))
    name = names[0]
    return gzip.open(name, "rt", newline="") if name.endswith(".gz") else open(name, newline="")


def root(symbol):
    return symbol.split(".")[0]


def years_between(first, last):
    return (datetime.date.fromisoformat(last) - datetime.date.fromisoformat(first)).days / 365.25


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("records")
    ap.add_argument("--portfolio", required=True)
    ap.add_argument("--out")
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    with open(args.portfolio) as f:
        portfolio = json.load(f)
    sleeves = [v for v in portfolio["strategies"].values()
               if isinstance(v, dict) and (v.get("enabled_backtest") or v.get("enabled_live"))]
    if len(sleeves) != 1 or sleeves[0].get("type") != "TrendFollowingStrategy":
        sys.exit("the rule applies to a book of one trend sleeve")
    pairs = [tuple(p) for p in sleeves[0].get("config", {}).get("ema_windows", [])]
    if not pairs or [p[0] for p in pairs] != sorted(p[0] for p in pairs):
        sys.exit("the sleeve's config.ema_windows is missing or not written fastest first")
    missing = [p for p in pairs if p not in TABLE_35]
    if missing:
        sys.exit("table 35 has no turnover for %s" % missing)
    turnover = [TABLE_35[p] for p in pairs]
    listed = {c["symbol"]: (c["before"], c["listed"])
              for c in portfolio.get("listing_dates", {}).get("contracts", [])}
    before_of = {before: (symbol, date) for symbol, (before, date) in listed.items()}
    slow = portfolio.get("equity_slow_rule", {})
    slow_symbols = set(slow.get("symbols", []))
    slow_pairs = [tuple(p) for p in slow.get("pairs", [])]

    # --- the estimator record: per instrument (a pair joined), date -> sigma, and the two series
    # behind the measured turnovers
    sigma = {}        # instrument -> {date: sigma}
    series = {}       # instrument -> [(date, [scaled per speed], [n per speed])]
    with open_record(args.records, "estimator_*.csv") as f:
        columns = ["scaled_%d_%d" % p for p in pairs]
        for row in csv.DictReader(f):
            contract = root(row["symbol"])
            instrument = before_of[contract][0] if contract in before_of else contract
            s, price = float(row["sigma"]), float(row["price"])
            if not (s > 0.0 and price > 0.0):
                sys.exit("unusable estimator row %s %s" % (row["date"], row["symbol"]))
            scaled = [float(row[c]) for c in columns]
            if not all(math.isfinite(x) for x in scaled):
                sys.exit("%s %s has a speed without a forecast: the measuring run must be made "
                         "WITHOUT trading_rule_removals" % (row["date"], row["symbol"]))
            if row["date"] in sigma.setdefault(instrument, {}):
                sys.exit("two estimator rows for %s on %s" % (instrument, row["date"]))
            sigma[instrument][row["date"]] = s
            series.setdefault(instrument, []).append(
                (row["date"], scaled, [x / (price * s) for x in scaled]))

    measured = {}     # instrument -> (first, last, rows, [forecast turnover], [position turnover])
    for instrument in sorted(series):
        rows = sorted(series[instrument])
        if len(rows) < MIN_ROWS:
            continue
        years = years_between(rows[0][0], rows[-1][0])
        forecast, position = [], []
        for k in range(len(pairs)):
            forecast.append(
                math.fsum(abs(rows[t][1][k] - rows[t - 1][1][k]) for t in range(1, len(rows))) / years / 10.0)
            traded = math.fsum(abs(rows[t][2][k] - rows[t - 1][2][k]) for t in range(1, len(rows)))
            average = math.fsum(abs(r[2][k]) for r in rows) / len(rows)
            position.append(traded / years / average)
        measured[instrument] = (rows[0][0], rows[-1][0], len(rows), forecast, position)
    instruments = sorted(measured)
    our_forecast = [math.fsum(measured[i][3][k] for i in instruments) / len(instruments) for k in range(len(pairs))]
    our_position = [math.fsum(measured[i][4][k] for i in instruments) / len(instruments) for k in range(len(pairs))]

    # --- the first sized signal day
    first_sized = None
    with open_record(args.records, "onepass_days_*.csv") as f:
        for row in csv.DictReader(f):
            if row["warmup"] == "0" and (first_sized is None or row["date"] < first_sized):
                first_sized = row["date"]
    if first_sized is None:
        sys.exit("the record has no sized day")

    # --- cost per trade of each contract, from the engine's own cost of one contract
    book = {}         # contract -> [(date, cost / (close x multiplier))]
    with open_record(args.records, "onepass_book_*.csv") as f:
        for row in csv.DictReader(f):
            cost, close, multiplier = float(row["cost"]), float(row["close"]), float(row["multiplier"])
            if row["date"] >= first_sized and cost > 0.0 and close > 0.0 and multiplier > 0.0:
                book.setdefault(root(row["symbol"]), []).append((row["date"], cost / (close * multiplier)))

    # --- rolls a year of each contract
    span = {}
    with open_record(args.records, "series.csv") as f:
        for row in csv.DictReader(f):
            entry = span.setdefault(root(row["symbol"]), [row["date"], row["date"], 0])
            entry[0], entry[1] = min(entry[0], row["date"]), max(entry[1], row["date"])
            entry[2] += int(row["confirm"])
    rolls = {contract: confirmed / years_between(first, last) for contract, (first, last, confirmed) in span.items()}

    # --- the table
    table = []
    removals = {}
    flags = []
    for contract in sorted(book):
        instrument = before_of[contract][0] if contract in before_of else contract
        if instrument not in sigma or contract not in rolls:
            sys.exit("no estimator or series record for %s" % contract)
        rows = sorted(book[contract])
        if contract in before_of:   # the days it was traded: before its successor's listing date
            rows = [r for r in rows if r[0] < before_of[contract][1]]
            side = "before %s" % before_of[contract][1]
        elif contract in listed:
            rows = [r for r in rows if r[0] >= listed[contract][1]]
            side = "from %s" % listed[contract][1]
        else:
            side = ""
        rows = [(d, c) for d, c in rows if d in sigma[instrument]]
        if not rows:
            sys.exit("no priced signal day for %s" % contract)
        per_trade = statistics.median(c / sigma[instrument][d] for d, c in rows)
        costs = [(turnover[k] + 2.0 * rolls[contract]) * per_trade for k in range(len(pairs))]
        removed = [k for k in range(len(pairs)) if costs[k] > LIMIT]
        if removed != list(range(len(removed))):
            sys.exit("%s: the rules over the limit are not its fastest" % contract)
        if len(removed) == len(pairs):
            sys.exit("%s: no rule is under the limit; it cannot be traded in this strategy" % contract)
        ruled = contract in slow_symbols or instrument in slow_symbols
        if ruled and any(pairs[k] in slow_pairs for k in removed):
            sys.exit("%s: a speed the equity slow rule reads is over the limit" % contract)
        if removed:
            removals[contract] = [list(pairs[k]) for k in removed]
        for k in range(len(pairs)):
            if abs(costs[k] - LIMIT) <= FLAG * LIMIT:
                flags.append((contract, pairs[k], costs[k]))
        table.append(dict(contract=contract, side=side, cost_from=rows[0][0], cost_to=rows[-1][0],
                          cost_rows=len(rows), per_trade=per_trade, rolls=rolls[contract], costs=costs,
                          removed=len(removed)))

    # one side of a listing-date pair losing a rule the other keeps: the sleeve refuses such a list
    split = sorted(symbol for symbol, (before, _) in listed.items()
                   if removals.get(symbol) != removals.get(before))

    # --- output
    def cell(cost):
        mark = " REMOVED" if cost > LIMIT else ""
        if abs(cost - LIMIT) <= FLAG * LIMIT:
            mark += " ON THE LINE"
        return "%.3f%s" % (cost, mark)

    out = []
    out.append("limit %.2f Sharpe-ratio units a year for one rule on one contract; cost per trade: the "
               "median signal day from %s" % (LIMIT, first_sized))
    out.append("")
    out.append("Turnover of each speed, times a year. The list is decided by table 35. Ours, measured "
               "(mean of %d instruments, signal days %s to %s), decide nothing:" % (
                   len(instruments), min(measured[i][0] for i in instruments),
                   max(measured[i][1] for i in instruments)))
    out.append("")
    out.append("| speed | table 35 (used) | our forecast turnover | our position turnover |")
    out.append("|---|---|---|---|")
    for k, (fast, slow_span) in enumerate(pairs):
        out.append("| EWMAC %d/%d | %.1f | %.1f | %.1f |" % (
            fast, slow_span, turnover[k], our_forecast[k], our_position[k]))
    out.append("")
    out.append("Yearly cost of each rule on each contract, Sharpe-ratio units (REMOVED when over %.2f; "
               "ON THE LINE within %.0f per cent of it):" % (LIMIT, 100 * FLAG))
    out.append("")
    out.append("| contract | side | signal days | cost per trade | rolls a year | " +
               " | ".join("%d/%d" % p for p in pairs) + " | rules left |")
    out.append("|---|---|---|---|---|" + "---|" * len(pairs) + "---|")
    for r in table:
        out.append("| %s | %s | %d | %.5f | %.2f | %s | %d |" % (
            r["contract"], r["side"], r["cost_rows"], r["per_trade"], r["rolls"],
            " | ".join(cell(c) for c in r["costs"]), len(pairs) - r["removed"]))
    out.append("")
    out.append("cells on the line: " + ("; ".join("%s %d/%d %.4f (%s)" % (
        c, p[0], p[1], cost, "removed" if cost > LIMIT else "kept") for c, p, cost in flags) or "none"))
    out.append("")
    out.append("trading_rule_removals as computed (%d contracts, %d rules):" % (
        len(removals), sum(len(v) for v in removals.values())))
    block = json.dumps(removals, sort_keys=True)
    out.append(block)
    if split:
        out.append("")
        out.append("STOP: the two contracts of a listing-date pair do not lose the same rules: " + ", ".join(split))
    text = "\n".join(out) + "\n"
    sys.stdout.write(text)

    if args.out:
        os.makedirs(args.out, exist_ok=True)
        with open(os.path.join(args.out, "trading_rule_removals.json"), "w") as f:
            f.write(json.dumps(removals, sort_keys=True, indent=2) + "\n")
        with open(os.path.join(args.out, "trading_rule_costs.md"), "w") as f:
            f.write(text)
        with open(os.path.join(args.out, "trading_rule_costs.csv"), "w", newline="") as f:
            w = csv.writer(f, lineterminator="\n")
            w.writerow(["contract", "side", "cost_from", "cost_to", "cost_rows", "cost_per_trade",
                        "rolls_a_year"] + ["cost_%d_%d" % p for p in pairs] + ["rules_removed"])
            for r in table:
                w.writerow([r["contract"], r["side"], r["cost_from"], r["cost_to"], r["cost_rows"],
                            repr(r["per_trade"]), repr(r["rolls"])] + [repr(c) for c in r["costs"]] +
                           [r["removed"]])
            w.writerow([])
            w.writerow(["instrument", "first_day", "last_day", "signal_days"] +
                       ["forecast_turnover_%d_%d" % p for p in pairs] +
                       ["position_turnover_%d_%d" % p for p in pairs])
            for i in instruments:
                first, last, n, forecast, position = measured[i]
                w.writerow([i, first, last, n] + [repr(t) for t in forecast] + [repr(t) for t in position])
            w.writerow(["MEAN", "", "", len(instruments)] + [repr(t) for t in our_forecast] +
                       [repr(t) for t in our_position])
            w.writerow(["TABLE 35 (used)", "", "", ""] + [repr(t) for t in turnover])

    if split:
        sys.exit(2)
    if args.check:
        committed = {k: v for k, v in portfolio.get("trading_rule_removals", {}).items()
                     if not k.startswith("_")}
        if committed != removals:
            lines = ["CHECK: the committed list (the authority) is NOT what these records give. Nothing "
                     "was rewritten; a changed list is the owner's decision."]
            for contract in sorted(set(committed) | set(removals)):
                if committed.get(contract) != removals.get(contract):
                    row = next((r for r in table if r["contract"] == contract), None)
                    lines.append("  %s: committed %s, computed %s%s" % (
                        contract, json.dumps(committed.get(contract, [])), json.dumps(removals.get(contract, [])),
                        "" if row is None else "; costs " + " ".join("%.4f" % c for c in row["costs"])))
            sys.stderr.write("\n".join(lines) + "\n")
            sys.exit(1)
        sys.stderr.write("check: the committed list equals what these records give\n")


if __name__ == "__main__":
    main()
