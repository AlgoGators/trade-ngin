"""roll_legs_check.py <trading_executions.csv> <contract_closes.csv> <vendor_ids.csv> [ROOT,ROOT...]: the live mode gate's held-roll
check (rolls step 4, per_contract_rolls on), read against futures_data.contract_ohlcv_1d (the contract table) and the
vendor's own contract id of each bar (futures_data.ohlcv_1d_raw), both exported by the gate; not the engine's code.

  - Each ROLL pair (EXEC_<symbol>_<YYYYMMDD>_RC and _RO) has exactly one closing and one opening leg.
  - Each roll is stored once: one pair per (symbol, contract rolled out of, contract rolled into). A roll legged on two
    bars (issue #188's double charge) is two pairs of one roll and fails.
  - The pair is priced on the roll's own bar, the closing leg at the old contract's close and the opening leg at the new
    contract's close, and the roll's kind decides which bar that is (R4B, R4A audit A recheck note 2):
      a vendor switch the next bar confirmed: the id carries the confirming bar D; the switch bar S is the vendor's bar
        before D, a weekday on which the vendor named another contract than the old one after naming the old one on the
        bar before S, named that same contract again on D, and on which both contracts print: the legs are S's closes;
      every other roll (a crossover, a deadline, a vendor switch known on its bar): the id's own bar D's closes.
    A roll legged on the other bar fails (a crossover legged at the previous bar's closes, a vendor-confirmed roll at the
    confirming bar's).
  - The micros (MES, MNQ, MYM, M2K) are checked like every root (R6): before the vendor's 2026-02-22 change a leg is
    the full-size contract's (ES, NQ, YM, RTY), so a micro's leg is looked up under the micro's root, then its
    full-size root's.
  - With the optional fourth argument, at least one roll of one of those roots must be among them.
Prints one line per roll; exit 0 when every roll passes and there is at least one."""
import csv, sys, collections, datetime as dt
FULL_SIZE = {"MES": "ES", "MNQ": "NQ", "MYM": "YM", "M2K": "RTY"}
execs, closes, vendor_ids = sys.argv[1:4]
required = set(sys.argv[4].split(",")) if len(sys.argv) > 4 and sys.argv[4] else set()
close = {}
for r in csv.DictReader(open(closes)):
    close[(r["root"], r["instrument_id"], r["date"])] = float(r["close"])
vendor = collections.defaultdict(dict)  # symbol -> date -> the vendor's contract id of that bar
for r in csv.DictReader(open(vendor_ids)):
    vendor[r["symbol"]][r["date"]] = r["instrument_id"]
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
    if sorted(legs) != ["RC", "RO"] or any(len(v) != 1 for v in legs.values()):
        print(f"FAIL {stem}: legs {sorted((k, len(v)) for k, v in legs.items())} (one closing and one opening leg)")
        ok = False
        continue
    rc, ro = legs["RC"][0], legs["RO"][0]
    rolls[(sym, rc["instrument_id"], ro["instrument_id"])].append(stem)
    def close_of(cid, d):
        c = close.get((root, cid, d))
        return close.get((FULL_SIZE[root], cid, d)) if c is None and root in FULL_SIZE else c
    def priced_on(d):
        a, b = close_of(rc["instrument_id"], d), close_of(ro["instrument_id"], d)
        return a is not None and b is not None and a == float(rc["price"]) and b == float(ro["price"])
    bars = sorted(vendor[sym])
    before = [d for d in bars if d < day]
    switch = before[-1] if before else None
    prior = [d for d in bars if switch and d < switch]
    confirmed_switch = (
        switch is not None and dt.date.fromisoformat(switch).weekday() < 5
        and vendor[sym].get(switch) not in (None, rc["instrument_id"])
        and vendor[sym].get(day) == vendor[sym].get(switch)
        and bool(prior) and vendor[sym].get(prior[-1]) == rc["instrument_id"]
        and close_of(rc["instrument_id"], switch) is not None and close_of(ro["instrument_id"], switch) is not None)
    bar = switch if confirmed_switch else day
    kind = (f"a vendor switch on {switch} the bar {day} confirmed: the switch bar's closes" if confirmed_switch
            else f"a roll known on its bar {day}: its own closes")
    if priced_on(bar):
        how = f"both closes of {bar} ({kind})"
    else:
        other = day if confirmed_switch else switch
        print(f"FAIL {stem}: closing {rc['instrument_id']} at {rc['price']}, opening {ro['instrument_id']} at {ro['price']}: "
              f"not the two closes of {bar} ({kind})" + (f"; they are {other}'s" if other and priced_on(other) else ""))
        ok = False
        continue
    print(f"ok   {stem}: closing {rc['instrument_id']} at {rc['price']} and opening {ro['instrument_id']} at {ro['price']}, "
          f"{how} (stored on the run of {rc['date'][:10]})")
for (sym, out, inc), stems in sorted(rolls.items()):
    if len(stems) != 1:
        print(f"FAIL {sym} {out} -> {inc}: stored {len(stems)} times ({', '.join(stems)})")
        ok = False
if required:
    found = sorted({sym for (sym, _, _) in rolls if sym.split(".")[0] in required})
    print(f"rolls of {','.join(sorted(required))}: {', '.join(found) if found else 'none'}")
    if not found:
        ok = False
print("ROLL LEGS:", f"{len(rolls)} roll(s), each stored once and priced on its own bar from the contract table" if ok else "FAILED")
sys.exit(0 if ok else 1)
