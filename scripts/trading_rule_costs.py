#!/usr/bin/env python3
"""trading_rule_costs.py: which trend speeds each contract of the conservative book may run, by cost.

The rule (Carver, Advanced Futures Trading Strategies, strategy nine, "Removing expensive trading
rules", with the cost definitions of strategy three and the turnovers of table 35): instrument by
instrument, a trading rule is removed when its yearly cost exceeds 0.15 Sharpe-ratio units, where

    yearly cost of a rule on a contract = (turnover of the rule + 2 x rolls a year) x cost per trade
    cost per trade (risk adjusted)      = cost of one contract / (price x multiplier) / sigma

the turnover being the number of times a year the rule trades its own average position, the same
figure for every contract, and sigma the contract's annual standard deviation of returns. The rules
left weigh equally and take the multiplier of table 36 for their number (the engine's table).

This tool only does arithmetic. Every input is a record the ENGINE writes about its own run, so the
tool cannot drift from what the engine charges or forecasts:

  estimator_<sleeve>.csv   one row a contract a signal day (strategy/trend_estimator_record.hpp):
                           each speed's scaled forecast (trend_estimator::estimate), the sizing
                           volatility `sigma` and the raw close `price`
  onepass_book_<book>.csv  one row a contract a signal day (optimization/one_pass_record.hpp):
                           `cost`, the dollars TransactionCostManager::calculate_costs(symbol, 1,
                           close).total_transaction_costs returns for ONE contract at the signal
                           close: the fee per contract of metadata.contract_metadata plus the
                           spread (SpreadModel) and the impact (ImpactModel) the engine charges a
                           fill; and the `close` and `multiplier` it was priced on
  series.csv               one row a consumed bar (backtest/consumed_series_record.hpp): `confirm`
                           is 1 on the bar a roll is confirmed on (data/roll_series)

How the records are made (the FIXED measuring run): bt_portfolio_conservative with
TRADE_NGIN_SERIES_DUMP_DIR set, backtest.lookback_years 16 and backtest.frozen_end_date 2026-10-07,
on the fee table in force, with the template's listing_dates and instrument_id_relabels and WITHOUT
trading_rule_removals (the turnovers and costs read here do not depend on the book held, only on
prices, volumes and the fee table). The tool reads the record files plain or gzipped.

What is computed, and on what window:
  turnover of a speed   For each instrument (a listing-date pair is one instrument: one price
                        history), the speed alone on the unrounded target at a fixed capital:
                        n(t) = scaled forecast(t) / (price(t) x sigma(t)), which is the position
                        up to a constant; turnover = (sum of |n(t) - n(t-1)| a year) / mean |n(t)|,
                        over EVERY signal day of the run. The speed's turnover is the plain mean
                        over the instruments with at least MIN_ROWS signal days.
  cost per trade        The mean, over the contract's last COST_ROWS signal days of the run, of
                        cost / (close x multiplier) / sigma. For the contract a listed contract
                        replaced (ES before MES), the last COST_ROWS signal days BEFORE the
                        listing date: the days it was traded.
  rolls a year          The confirmed rolls of the contract's whole consumed series over its span
                        in years.

When the list is recomputed: whenever metadata.contract_metadata's fees change, whenever the cost
model changes (transaction_cost/), whenever the universe or the listing dates change, and otherwise
once a year on the first run after the turn of the year with the measuring run's end date moved to
that day. Between recomputations the list committed in portfolio.json is the authority; it is this
tool's output and is never edited by hand. `--check` fails when the two differ.

Usage:
  trading_rule_costs.py <records dir> --portfolio <portfolio.json> [--out <dir>] [--check]
Writes nothing to any database. With --out: trading_rule_removals.json (the block),
trading_rule_costs.csv (every contract by every speed) and trading_rule_costs.md (the tables).
"""
import argparse
import csv
import datetime
import glob
import gzip
import json
import math
import os
import sys

LIMIT = 0.15        # Sharpe-ratio units a year, for one trading rule on one contract
COST_ROWS = 256     # signal days behind a contract's cost per trade: about a year
MIN_ROWS = 512      # signal days an instrument needs to count in a speed's turnover
CARVER_TURNOVER = {2: 98.5, 4: 50.2, 8: 25.4, 16: 13.2, 32: 7.6, 64: 5.2}  # table 35, for comparison


def open_record(directory, pattern):
    names = sorted(glob.glob(os.path.join(directory, pattern)) +
                   glob.glob(os.path.join(directory, pattern + ".gz")))
    if len(names) != 1:
        sys.exit("expected exactly one %s[.gz] in %s, found %d" % (pattern, directory, len(names)))
    name = names[0]
    return gzip.open(name, "rt", newline="") if name.endswith(".gz") else open(name, newline="")


def root(symbol):
    return symbol.split(".")[0]


def fsum(values):
    return math.fsum(values)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("records")
    ap.add_argument("--portfolio", required=True)
    ap.add_argument("--out")
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    portfolio = json.load(open(args.portfolio))
    sleeves = [v for v in portfolio["strategies"].values() if isinstance(v, dict)]
    if len(sleeves) != 1:
        sys.exit("the rule applies to a book of one trend sleeve")
    pairs = [tuple(p) for p in sleeves[0]["config"]["ema_windows"]]
    speeds = [fast for fast, _ in pairs]
    if speeds != sorted(speeds):
        sys.exit("the sleeve's pairs are not written fastest first")
    listed = {c["symbol"]: (c["before"], c["listed"])
              for c in portfolio.get("listing_dates", {}).get("contracts", [])}
    before_of = {before: (symbol, date) for symbol, (before, date) in listed.items()}
    slow_symbols = set(portfolio["equity_slow_rule"]["symbols"])
    slow_pairs = [tuple(p) for p in portfolio["equity_slow_rule"]["pairs"]]

    # --- the estimator record: per instrument (a pair joined), date -> sigma and each speed's n
    sigma = {}        # instrument -> {date: sigma}
    position = {}     # instrument -> [(date, [n per speed])]
    with open_record(args.records, "estimator_*.csv") as f:
        reader = csv.DictReader(f)
        columns = ["scaled_%d_%d" % p for p in pairs]
        for row in reader:
            contract = root(row["symbol"])
            instrument = before_of[contract][0] if contract in before_of else contract
            s, price = float(row["sigma"]), float(row["price"])
            if not (s > 0.0 and price > 0.0):
                sys.exit("unusable estimator row %s %s" % (row["date"], row["symbol"]))
            if row["date"] in sigma.setdefault(instrument, {}):
                sys.exit("two estimator rows for %s on %s" % (instrument, row["date"]))
            sigma[instrument][row["date"]] = s
            position.setdefault(instrument, []).append(
                (row["date"], [float(row[c]) / (price * s) for c in columns]))

    # --- turnover of each speed: per instrument, then the plain mean over instruments
    turnover_by_instrument = {}
    for instrument in sorted(position):
        rows = sorted(position[instrument])
        if len(rows) < MIN_ROWS:
            continue
        first = datetime.date.fromisoformat(rows[0][0])
        last = datetime.date.fromisoformat(rows[-1][0])
        years = (last - first).days / 365.25
        per_speed = []
        for k in range(len(pairs)):
            traded = fsum(abs(rows[t][1][k] - rows[t - 1][1][k]) for t in range(1, len(rows)))
            average = fsum(abs(r[1][k]) for r in rows) / len(rows)
            per_speed.append(traded / years / average)
        turnover_by_instrument[instrument] = (rows[0][0], rows[-1][0], len(rows), per_speed)
    instruments = sorted(turnover_by_instrument)
    turnover = [fsum(turnover_by_instrument[i][3][k] for i in instruments) / len(instruments)
                for k in range(len(pairs))]

    # --- cost per trade of each contract, from the engine's own cost of one contract
    book = {}         # contract -> [(date, cost / (close x multiplier))]
    with open_record(args.records, "onepass_book_*.csv") as f:
        for row in csv.DictReader(f):
            cost, close, multiplier = float(row["cost"]), float(row["close"]), float(row["multiplier"])
            if cost > 0.0 and close > 0.0 and multiplier > 0.0:
                book.setdefault(root(row["symbol"]), []).append((row["date"], cost / (close * multiplier)))

    # --- rolls a year of each contract
    rolls = {}
    series = {}
    with open_record(args.records, "series.csv") as f:
        for row in csv.DictReader(f):
            entry = series.setdefault(root(row["symbol"]), [row["date"], row["date"], 0])
            entry[0], entry[1] = min(entry[0], row["date"]), max(entry[1], row["date"])
            entry[2] += int(row["confirm"])
    for contract, (first, last, confirmed) in series.items():
        years = (datetime.date.fromisoformat(last) - datetime.date.fromisoformat(first)).days / 365.25
        rolls[contract] = confirmed / years

    # --- the table
    table = []
    removals = {}
    for contract in sorted(book):
        instrument = before_of[contract][0] if contract in before_of else contract
        if instrument not in sigma or contract not in rolls:
            sys.exit("no estimator or series record for %s" % contract)
        rows = sorted(book[contract])
        if contract in before_of:   # the days it was traded: before its successor's listing date
            rows = [r for r in rows if r[0] < before_of[contract][1]]
            side = "before %s" % before_of[contract][1]
        elif contract in listed:
            side = "from %s" % listed[contract][1]
        else:
            side = ""
        rows = [(d, c) for d, c in rows if d in sigma[instrument]][-COST_ROWS:]
        if not rows:
            sys.exit("no priced signal day for %s" % contract)
        per_trade = fsum(c / sigma[instrument][d] for d, c in rows) / len(rows)
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
        table.append(dict(contract=contract, side=side, cost_from=rows[0][0], cost_to=rows[-1][0],
                          cost_rows=len(rows), per_trade=per_trade, rolls=rolls[contract], costs=costs,
                          removed=len(removed)))

    pair_sides = sorted(c for c in removals if c in listed or c in before_of)

    # --- output
    out = []
    out.append("limit %.2f Sharpe-ratio units a year for one rule on one contract" % LIMIT)
    out.append("")
    out.append("Turnover of each speed, times a year (mean of %d instruments, signal days %s to %s):" % (
        len(instruments), min(turnover_by_instrument[i][0] for i in instruments),
        max(turnover_by_instrument[i][1] for i in instruments)))
    out.append("")
    out.append("| speed | ours | lowest instrument | highest instrument | Carver, table 35 | our share | Carver's share |")
    out.append("|---|---|---|---|---|---|---|")
    ours_total = fsum(turnover)
    carver_total = fsum(CARVER_TURNOVER.get(s, 0.0) for s in speeds)
    for k, (fast, slow) in enumerate(pairs):
        each = [turnover_by_instrument[i][3][k] for i in instruments]
        carver = CARVER_TURNOVER.get(fast)
        out.append("| EWMAC %d/%d | %.1f | %.1f | %.1f | %s | %.0f%% | %s |" % (
            fast, slow, turnover[k], min(each), max(each),
            "%.1f" % carver if carver else "", 100.0 * turnover[k] / ours_total,
            "%.0f%%" % (100.0 * carver / carver_total) if carver else ""))
    out.append("")
    out.append("Yearly cost of each rule on each contract, Sharpe-ratio units (REMOVED when over %.2f):" % LIMIT)
    out.append("")
    out.append("| contract | side | cost per trade | rolls a year | " +
               " | ".join("%d/%d" % p for p in pairs) + " | rules left |")
    out.append("|---|---|---|---|" + "---|" * len(pairs) + "---|")
    for r in table:
        cells = ["%.3f%s" % (c, " REMOVED" if c > LIMIT else "") for c in r["costs"]]
        out.append("| %s | %s | %.5f | %.2f | %s | %d |" % (
            r["contract"], r["side"], r["per_trade"], r["rolls"], " | ".join(cells),
            len(pairs) - r["removed"]))
    out.append("")
    out.append("trading_rule_removals:")
    block = json.dumps(removals, sort_keys=True)
    out.append(block)
    if pair_sides:
        out.append("")
        out.append("STOP: a contract of a listing-date pair loses a rule: " + ", ".join(pair_sides))
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
                       ["turnover_%d_%d" % p for p in pairs])
            for i in instruments:
                first, last, n, each = turnover_by_instrument[i]
                w.writerow([i, first, last, n] + [repr(t) for t in each])
            w.writerow(["MEAN", "", "", len(instruments)] + [repr(t) for t in turnover])

    if args.check:
        committed = {k: v for k, v in portfolio.get("trading_rule_removals", {}).items()
                     if not k.startswith("_")}
        if committed != removals:
            sys.stderr.write("CHECK FAILED: portfolio.json's trading_rule_removals is not this output\n"
                             "committed: %s\ncomputed:  %s\n" % (json.dumps(committed, sort_keys=True), block))
            sys.exit(1)
        sys.stderr.write("check: portfolio.json's trading_rule_removals equals this output\n")
    if pair_sides:
        sys.exit(2)


if __name__ == "__main__":
    main()
