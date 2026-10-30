"""The load rows for MGC and SIL, in the data owner's shapes (stdlib Python, no engine code).

Usage: python3 -I build_rows.py DATA_DIR TWIN_CONTRACTS_CSV END OUT_DIR [DEFINITIONS...]
  START (environment, optional, YYYY-MM-DD): only bars dated on or after it (the backfill after
         part A, load_micro_metals.sql part A2, takes the days after the bought rows' last day)
  DATA_DIR: a folder holding the purchase decompressed (zstd -d) as <root>_contracts.csv and <root>_v0.csv
            (the pull of PULL_REQUEST.md)
  TWIN_CONTRACTS_CSV: contract_ohlcv_1d export of GC and SI (instrument_id, date, root, raw_symbol, expiration, ...)
  END: the last bar date loaded (YYYY-MM-DD, inclusive)
  DEFINITIONS: optional decompressed Databento definition csv files; where given, each contract's expiration is the
               vendor's (its last definition's expiration, as UTC date). A contract with no definition takes the twin's
               same-month expiration (they agree on 97 of 98 MGC readings), and is listed.
Writes OUT_DIR/contract_ohlcv_1d.csv, ohlcv_1d.csv, ohlcv_1d_raw.csv and expirations.csv (contract, expiration,
source) in the column order of the tables, and prints counts.

Shapes, as production's rows of the 40 roots:
  contract_ohlcv_1d: outrights only (root + month code + one or two digit year, no spread separator), instrument_id
    as text, date = the bar's UTC date, expiration = a date, loaded_at = the load's now().
  ohlcv_1d: time = ts_event (00:00 UTC), symbol "<root>.v.0", volume integer, open/high/low/close.
  ohlcv_1d_raw: ts_event, symbol, open/high/low/close, volume, rtype, publisher_id, instrument_id (text).
"""
import csv, json, os, re, sys

data, twin_csv, end, out = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
defs = sys.argv[5:]
ROOTS = json.loads(os.environ.get("ROOTS", '{"MGC": "GC", "SIL": "SI"}'))  # ROOTS='{"MCL": "CL"}'
# FALLBACK=prev_trading_day:<calendar json> : a contract with no vendor definition takes the twin's
# same-delivery expiration less one trading day of the CME calendar file (MCL ends a trading day before CL from MCLU1).
FALLBACK = os.environ.get("FALLBACK", "")
START = os.environ.get("START", "0000-00-00")
closed = set()
if FALLBACK.startswith("prev_trading_day:"):
    for ex in json.load(open(FALLBACK.split(":", 1)[1]))["exchanges"].values():
        for c in ex["closures"]:
            if "all" in c["closes"] or "energy" in c["closes"]:
                closed.add(c["date"])


def prev_trading_day(s):
    import datetime as dt
    d = dt.date.fromisoformat(s) - dt.timedelta(days=1)
    while d.weekday() >= 5 or d.isoformat() in closed:
        d -= dt.timedelta(days=1)
    return d.isoformat()


def delivery(sym, root, bar_date):
    m = re.fullmatch(re.escape(root) + r"([FGHJKMNQUVXZ])(\d{1,2})", sym)
    if not m:
        return None
    n = len(m.group(2))
    by = int(bar_date[:4])
    y = by - by % 10 ** n + int(m.group(2))
    if y < by - 1:
        y += 10 ** n
    return m.group(1), y


# the twin's expiration by (root, month code, delivery year): the delivery year is the expiration's year for
# these metals (the third-last business day of the contract month)
twin_exp = {}
for r in csv.DictReader(open(twin_csv)):
    if r["root"] in ROOTS.values() and r["expiration"]:
        k = delivery(r["raw_symbol"], r["root"], r["date"])
        if k and (closed or int(r["expiration"][:4]) == k[1]):  # CL's January expires in December
            twin_exp[(r["root"],) + k] = r["expiration"][:10]

vendor_exp = {}
for f in defs:
    for r in csv.DictReader(open(f, errors="replace")):
        sym = r.get("raw_symbol") or ""
        root = next((x for x in ROOTS if re.fullmatch(x + r"[FGHJKMNQUVXZ]\d{1,2}", sym)), None)
        if root and r.get("instrument_class") == "F" and r.get("expiration"):
            vendor_exp[r["instrument_id"]] = (sym, r["expiration"][:10])  # the last definition read wins

os.makedirs(out, exist_ok=True)
crow = open(os.path.join(out, "contract_ohlcv_1d.csv"), "w", newline="")
cw = csv.writer(crow)
cw.writerow(["instrument_id", "date", "root", "raw_symbol", "expiration", "open", "high", "low", "close", "volume"])
exp_rows = {}
counts = {}
for root, twin in ROOTS.items():
    n = spreads = 0
    for r in csv.DictReader(open(os.path.join(data, f"{root}_contracts.csv"))):
        d = r["ts_event"][:10]
        if d > end or d < START:
            continue
        k = delivery(r["symbol"], root, d)
        if not k:
            spreads += 1
            continue
        iid = r["instrument_id"]
        if iid in vendor_exp:
            exp, src = vendor_exp[iid][1], "vendor definition"
        else:
            exp, src = twin_exp.get((twin,) + k), f"{twin} same month"
            if exp and closed:
                exp, src = prev_trading_day(exp), f"{twin} same month less one trading day"
        if exp is None:
            sys.exit(f"no expiration for {r['symbol']} id {iid} on {d}")
        prev = exp_rows.setdefault(iid, (root, r["symbol"], exp, src))
        if prev[2] != exp:
            sys.exit(f"two expirations for id {iid}: {prev} and {exp}")
        cw.writerow([iid, d, root, r["symbol"], exp, r["open"], r["high"], r["low"], r["close"], r["volume"]])
        n += 1
    counts[root] = (n, spreads)
crow.close()

vrow = open(os.path.join(out, "ohlcv_1d.csv"), "w", newline="")
rrow = open(os.path.join(out, "ohlcv_1d_raw.csv"), "w", newline="")
vw, rw = csv.writer(vrow), csv.writer(rrow)
vw.writerow(["time", "symbol", "volume", "open", "high", "low", "close"])
rw.writerow(["ts_event", "symbol", "open", "high", "low", "close", "volume", "rtype", "publisher_id", "instrument_id"])
vcounts = {}
for root in ROOTS:
    n = 0
    for r in csv.DictReader(open(os.path.join(data, f"{root}_v0.csv"))):
        d = r["ts_event"][:10]
        if d > end or d < START:
            continue
        t = d + " 00:00:00+00"
        assert r["ts_event"][10:] in ("T00:00:00.000000000Z",), r["ts_event"]
        vw.writerow([t, r["symbol"], int(r["volume"]), r["open"], r["high"], r["low"], r["close"]])
        rw.writerow([t, r["symbol"], r["open"], r["high"], r["low"], r["close"], r["volume"], r["rtype"],
                     r["publisher_id"], r["instrument_id"]])
        n += 1
    vcounts[root] = n
vrow.close()
rrow.close()
with open(os.path.join(out, "expirations.csv"), "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["instrument_id", "root", "raw_symbol", "expiration", "source"])
    for iid, (root, sym, exp, src) in sorted(exp_rows.items(), key=lambda x: (x[1][0], x[1][2])):
        w.writerow([iid, root, sym, exp, src])
for root in ROOTS:
    srcs = {}
    for v in exp_rows.values():
        if v[0] == root:
            srcs[v[3]] = srcs.get(v[3], 0) + 1
    print(f"{root}: contract rows {counts[root][0]} (spreads left out {counts[root][1]}), .v.0 rows {vcounts[root]}, "
          f"contracts {sum(srcs.values())}, expirations by source {srcs}")
print(f"start {START if START != '0000-00-00' else '-'} end {end}")
