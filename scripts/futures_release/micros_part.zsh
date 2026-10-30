# micros_part.zsh: sourced by 05, 06 and 07. Runs one part of the load (load/apply_micro_metals_on_production.sh) with the data fingerprints before and after it, and checks that the
# part changed only what it names. Called as: . micros_part.zsh <part> <target>; BACKFILL as the load script reads it.
set -u
. "${0:A:h}/lib.zsh"
PART="$1"; TARGET="${2:-${TARGET:-}}"
resolve_target
D="$RUN_DIR/data"; mkdir -p "$D"; LOG="$D/micros_part${PART}_${TAG}.log"
MMS="$PROD/load/apply_micro_metals_on_production.sh"
fp() { PGOPTIONS='-c default_transaction_read_only=on' $PGBIN/psql -X -At -v ON_ERROR_STOP=1 -v cut=2099-12-31 -f "$PROD/sql/data_fingerprint.sql" > "$1" || die "fingerprint failed"; }
check_db
if [ "$PART" = B ]; then   # part B after the hole fill only: 166,611 rows to the cut before it, 476 more after it, micros aside
  n=$(qr "select (select count(*) from futures_data.ohlcv_1d where time < '2026-10-24' and symbol not in ('MGC.v.0','SIL.v.0')) || ' ' || (select count(*) from futures_data.ohlcv_1d_raw where ts_event < '2026-10-24' and symbol not in ('MGC.v.0','SIL.v.0'))") || die "the row count to the cut failed"
  [ "$n" = "167087 167087" ] || die "the continuous tables hold $n rows to 2026-10-23, less the micros, not 167087 167087: run the hole fill (04) first; nothing written" 8
fi
say "== part $PART on $DBNAME, $(date -u +%FT%TZ); load script sha256 $(shasum -a 256 "$MMS" | cut -c1-16)${BACKFILL:+; BACKFILL $BACKFILL}"
STAMP=$(date -u +%Y%m%dT%H%M%SZ); B="$D/mm${PART}_${TAG}_${STAMP}_before.txt"; A="$D/mm${PART}_${TAG}_${STAMP}_after.txt"
fp "$B"; sed 's/^/before: /' "$B" | tee -a "$LOG"
if [ "$TARGET" = production ]; then zsh "$MMS" "$PART" production 2>&1 | tee -a "$LOG"; rc=${pipestatus[1]}
else zsh "$MMS" "$PART" scratch "$DBNAME" 2>&1 | tee -a "$LOG"; rc=${pipestatus[1]}; fi
scrub "$LOG" "$RUN_DIR"/load/apply_micro_metals_*_part${PART}_*.log(N)
[ $rc -eq 0 ] || die "part $PART exited $rc (refused or failed, rolled back, nothing written)" $rc
fp "$A"; sed 's/^/after:  /' "$A" | tee -a "$LOG"
python3 - "$B" "$A" "$PART" <<'PY' | tee -a "$LOG"
import sys
def rows(p):
    out = {}
    for l in open(p):
        w = l.split()
        if not w: continue
        k = " ".join(w[:2]) if w[0] in ("micros", "v0") else w[0]
        out[k] = w
    return out
b, a, part = rows(sys.argv[1]), rows(sys.argv[2]), sys.argv[3]
def cnt(d, k): return int(d[k][2] if k.startswith("micros") else d[k][1])
ok = True
def same(k):
    global ok
    g = b[k] == a[k]; ok &= g
    print("%s: %s" % (k, "unchanged PASS" if g else "CHANGED FAIL"))
def grew_by_micros(table, mk):
    global ok
    d = cnt(a, table) - cnt(b, table); m = cnt(a, mk) - cnt(b, mk); g = d == m and m > 0; ok &= g
    print("%s: +%d rows, all of them %s (+%d) %s" % (table, d, mk, m, "PASS" if g else "FAIL"))
if part in ("A", "A2"):
    for k in ("futures_data.ohlcv_1d", "futures_data.ohlcv_1d_raw", "micros ohlcv_1d", "micros ohlcv_1d_raw"): same(k)
    grew_by_micros("futures_data.contract_ohlcv_1d", "micros contract_ohlcv_1d")
    if part == "A":
        m = cnt(a, "metadata.contract_metadata") - cnt(b, "metadata.contract_metadata"); g = m == 2; ok &= g
        print("metadata.contract_metadata: +%d rows (expected +2) %s" % (m, "PASS" if g else "FAIL"))
    else:
        same("metadata.contract_metadata")
else:
    for k in ("futures_data.contract_ohlcv_1d", "metadata.contract_metadata", "micros contract_ohlcv_1d"): same(k)
    grew_by_micros("futures_data.ohlcv_1d", "micros ohlcv_1d"); grew_by_micros("futures_data.ohlcv_1d_raw", "micros ohlcv_1d_raw")
    print("v0 symbols before: %s; after: %s" % (b["v0 symbols"][2], a["v0 symbols"][2]))
print("part %s fingerprint check: %s" % (part, "PASS" if ok else "FAIL"))
sys.exit(0 if ok else 1)
PY
[ ${pipestatus[1]} -eq 0 ] || die "the fingerprints after part $PART are not the expected ones (see $LOG)"
say "part $PART DONE on $DBNAME"
