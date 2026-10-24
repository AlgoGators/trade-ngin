"""roll_legs_check.py <trading_executions.csv> <contract_closes.csv>: the live mode gate's held-roll check (rolls step 4,
per_contract_rolls on), read against futures_data.contract_ohlcv_1d (the contract table, exported by the gate; not the
engine's code).

  - Each ROLL pair (EXEC_<symbol>_<YYYYMMDD>_RC and _RO) has exactly one closing and one opening leg.
  - Each roll is stored once: one pair per (symbol, contract rolled out of, contract rolled into). A roll legged on two
    bars (issue #188's double charge) is two pairs of one roll and fails.
  - The pair is priced on the roll's own bar: the closing leg at the old contract's close and the opening leg at the new
    contract's close of the id's date (a roll known on its bar), or, for a vendor switch the next bar confirmed (the id
    then carries the confirming bar), of the one bar before it on which both contracts print. No other bar is accepted.
  - The micros (MES, MNQ, MYM, M2K) stay on the vendor's series under the switch (step 3): their legs are vendor closes,
    not contract-table closes, so they are listed and skipped.
Prints one line per roll; exit 0 when every roll passes and there is at least one."""
import csv, sys, collections
MICROS = {"MES", "MNQ", "MYM", "M2K"}
execs, closes = sys.argv[1:3]
close = {}
for r in csv.DictReader(open(closes)):
    close[(r["root"], r["instrument_id"], r["date"])] = float(r["close"])
pairs = collections.defaultdict(lambda: collections.defaultdict(list))
for r in csv.DictReader(open(execs)):
    if r["execution_type"] != "ROLL":
        continue
    stem, leg = r["exec_id"].rsplit("_", 1)
    pairs[stem][leg].append(r)
ok = bool(pairs)
rolls = collections.defaultdict(list)
for stem, legs in sorted(pairs.items()):
    sym, idd = stem[len("EXEC_"):].rsplit("_", 1)
    root, day = sym.split(".")[0], f"{idd[:4]}-{idd[4:6]}-{idd[6:]}"
    if root in MICROS:
        print(f"skip {stem}: a micro, on the vendor's series")
        continue
    if sorted(legs) != ["RC", "RO"] or any(len(v) != 1 for v in legs.values()):
        print(f"FAIL {stem}: legs {sorted((k, len(v)) for k, v in legs.items())} (one closing and one opening leg)")
        ok = False
        continue
    rc, ro = legs["RC"][0], legs["RO"][0]
    rolls[(sym, rc["instrument_id"], ro["instrument_id"])].append(stem)
    def priced_on(d):
        a, b = close.get((root, rc["instrument_id"], d)), close.get((root, ro["instrument_id"], d))
        return a is not None and b is not None and a == float(rc["price"]) and b == float(ro["price"])
    both = sorted(d for (rt, i, d) in close if rt == root and i == rc["instrument_id"] and d < day
                  and (root, ro["instrument_id"], d) in close)
    if priced_on(day):
        how = f"both closes of the roll bar {day}"
    elif both and priced_on(both[-1]):
        how = f"both closes of the switch bar {both[-1]} (a vendor switch confirmed on {day})"
    else:
        print(f"FAIL {stem}: closing {rc['instrument_id']} at {rc['price']}, opening {ro['instrument_id']} at {ro['price']}: "
              f"not the two closes of {day}, nor of the bar before it where both print")
        ok = False
        continue
    print(f"ok   {stem}: closing {rc['instrument_id']} at {rc['price']} and opening {ro['instrument_id']} at {ro['price']}, "
          f"{how} (stored on the run of {rc['date'][:10]})")
for (sym, out, inc), stems in sorted(rolls.items()):
    if len(stems) != 1:
        print(f"FAIL {sym} {out} -> {inc}: stored {len(stems)} times ({', '.join(stems)})")
        ok = False
print("ROLL LEGS:", f"{len(rolls)} roll(s), each stored once and priced on its own bar from the contract table" if ok else "FAILED")
sys.exit(0 if ok else 1)
