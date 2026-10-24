"""late_roll_check.py <past-date chain snap> <late-bar run snap_late_before_repair> <late date> <bars_proof.txt>: the
gate's proof that its late-bar shape is the #188 shape (rolls step 4; R1 audit B note N6). The past-date chain stores
at least one ROLL pair whose id carries the late date (the late bar books a held roll), and the first pass, made
without the late bars, stored none of those pairs: it legged the roll later. And the bar tables are whole again:
the bars put back are as many as were moved aside, in both bar tables (or in the contract table with
LATE_TABLES=contract). Exit 0 when all hold. With LATE_TABLES=contract the first pass may instead hold the symbol flat
after the late date (the roll it saw a bar late met a flat book, so it had nothing to leg); the re-run must still book
the pair on the late bar."""
import csv, re, sys
chain, before, late, proof = sys.argv[1], sys.argv[2], sys.argv[3].replace("-", ""), sys.argv[4]
def rolls(d):
    return {r["exec_id"] for r in csv.DictReader(open(f"{d}/trading_executions.csv")) if r["execution_type"] == "ROLL"}
on_time = {e for e in rolls(chain) if e.rsplit("_", 2)[1] == late}
first = rolls(before)
syms = {e.rsplit("_", 2)[0] for e in on_time}
later = sorted(e for e in first if e.rsplit("_", 2)[0] in syms and e.rsplit("_", 2)[1] > late)
print("ROLL pairs the clean chain books on the late bar:", sorted(on_time))
print("the first pass without the late bars stored of them:", sorted(on_time & first), "; later legs of the same symbols:", later)
t = open(proof).read()
moved = re.search(r"moved aside: ohlcv_1d (\d+) rows, ohlcv_1d_raw (\d+) rows", t)
back = re.search(r"put back: now dated [0-9-]+: (\d+) / (\d+)", t)
if moved is None:  # LATE_TABLES=contract: the per-contract rows (R4A fix round 1)
    moved = re.search(r"moved aside: contract_ohlcv_1d (\d+) rows", t)
    back = re.search(r"put back: now dated [0-9-]+: (\d+) contract rows", t)
whole = bool(moved and back and moved.groups() == back.groups() and int(moved.group(1)) > 0)
print("bars moved aside:", moved.groups() if moved else None, "put back:", back.groups() if back else None)
legged = {e.rsplit("_", 2)[0] for e in later}
flat = set()
if "contract_ohlcv_1d" in (moved.re.pattern if moved else ""):
    held = {r["symbol"] for r in csv.DictReader(open(f"{before}/trading_positions.csv"))
            if r["date"].replace("-", "") > late and float(r["quantity"]) != 0}
    flat = {s.removeprefix("EXEC_") for s in syms} - held
    print("symbols the first pass holds flat after the late bar (nothing to leg):", sorted(flat))
late_or_flat = bool(syms) and all(s in legged or s.removeprefix("EXEC_") in flat for s in syms)
sys.exit(0 if on_time and not (on_time & first) and late_or_flat and whole else 1)
