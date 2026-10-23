"""roll_legs_check.py <trading_executions.csv> <contract_closes.csv>: the live mode gate's held-roll check (rolls step 4,
per_contract_rolls on). Every ROLL pair stored (EXEC_<symbol>_<YYYYMMDD>_RC and _RO) must be priced at the two
contracts' own closes of ONE bar on or before the id's date, read here from futures_data.contract_ohlcv_1d (the
contract table, exported by the gate; not the engine's code): the closing leg at the contract rolled out of, the
opening leg at the contract rolled into. Each roll is stored once. Prints one line per roll; exit 0 when every roll
passes and there is at least one."""
import csv, sys, collections
execs, closes = sys.argv[1:3]
close = {}
for r in csv.DictReader(open(closes)):
    close[(r["root"], r["instrument_id"], r["date"])] = float(r["close"])
pairs = collections.defaultdict(dict)
for r in csv.DictReader(open(execs)):
    if r["execution_type"] != "ROLL":
        continue
    stem, leg = r["exec_id"].rsplit("_", 1)
    pairs[stem].setdefault(leg, []).append(r)
ok = bool(pairs)
for stem, legs in sorted(pairs.items()):
    sym, idd = stem[len("EXEC_"):].rsplit("_", 1)
    root, day = sym.split(".")[0], f"{idd[:4]}-{idd[4:6]}-{idd[6:]}"
    if sorted(legs) != ["RC", "RO"] or any(len(v) != 1 for v in legs.values()):
        print(f"FAIL {stem}: legs {sorted((k, len(v)) for k, v in legs.items())} (each roll must be stored once)")
        ok = False
        continue
    rc, ro = legs["RC"][0], legs["RO"][0]
    dates = sorted({d for (rt, i, d) in close if rt == root and d <= day}, reverse=True)
    hit = None
    for d in dates[:6]:
        a, b = close.get((root, rc["instrument_id"], d)), close.get((root, ro["instrument_id"], d))
        if a is not None and b is not None and a == float(rc["price"]) and b == float(ro["price"]):
            hit = d
            break
    if hit is None:
        print(f"FAIL {stem}: closing {rc['instrument_id']} at {rc['price']}, opening {ro['instrument_id']} at {ro['price']}: no bar on or before {day} prices both")
        ok = False
    else:
        print(f"ok   {stem}: closing {rc['instrument_id']} at {rc['price']} and opening {ro['instrument_id']} at {ro['price']}, both closes of {hit} (stored on the run of {rc['date'][:10]})")
print("ROLL LEGS:", "every roll priced on one bar from the contract table, stored once" if ok else "FAILED")
sys.exit(0 if ok else 1)
