#!/bin/zsh
# 04_fill_holes.sh: the fill of the missing continuous rows (AlgoGators/algogators#36), by fill/fill_on_production.sh, with the data fingerprints taken before and after it.
#
#   zsh 04_fill_holes.sh production 476         the first run: the dry run runs first, and the insert goes ahead only if it
#                                               found exactly 476 holes per table
#   zsh 04_fill_holes.sh production 476 <count> only after a first run stopped (exit 5) and printed <count>, the holes in all,
#                                               and their list has been read
#   Rehearsal: SCRATCH_DB=<scratch database> zsh 04_fill_holes.sh scratch 476 [<count>]
#
# A hole dated after 2026-10-23 cannot be left behind: every hole found counts, so with such a hole the first run stops at
# the confirmation (exit 5, nothing written) with the list printed, and the second run takes all of them.
#
# CHANGES: only what the fill's own script inserts : the missing rows of each
#   <ROOT>.v.0 in futures_data.ohlcv_1d and ohlcv_1d_raw, insert only, one transaction, every check before COMMIT, no
#   existing row changed (its md5 baseline). Holes dated after 2026-10-23 are listed and inserted by the same rule, with
#   no tested ledger behind them (#87's daily fill would write the same rows within days, and the seed should read the
#   data production keeps). The fill writes its log, ledger and DONE marker in $RUN_DIR/fill/.
# BEFORE AND AFTER: sql/data_fingerprint.sql to the cut 2026-10-23, here in $RUN_DIR/data/. After the fill the
#   contract table (less loaded_at), the metadata and the micros' rows must be unchanged, both continuous tables must
#   have gained exactly 476 rows to the cut, and <count> (or 476) in all.
# ORDER (at the release): after 06 and #84, with #87 merging alongside; before 07. Part A's contract rows do not change
#   its holes (they belong to no stored .v.0 series), rehearsed in that order.
# READS: the three futures tables; the login from config/defaults.json (production).
# TAKES: a few minutes (the dry run, the junk replica, then the insert).
# UNDO: delete the inserted rows: every (symbol, date) of the run's ledger (fill_<target>_<stamp>_ledger.csv
#   in $RUN_DIR/fill/) from both tables. The rows that existed before are unchanged (checked inside the transaction).
set -u
. "${0:A:h}/lib.zsh"
TARGET="${1:-${TARGET:-}}"; ROWS="${2:-}"; ALL="${3:-${2:-}}"
resolve_target
[[ "$ROWS" == <-> ]] && [[ "$ALL" == <-> ]] || die "give 476, the rows of the tested range; a second number only after a first run stopped and printed it" 2
(( ALL >= ROWS )) || die "the holes in all cannot be fewer than the 476 of the tested range" 2
D="$RUN_DIR/data"; mkdir -p "$D"; LOG="$D/04_fill_${TAG}.log"; STAMP=$(date -u +%Y%m%dT%H%M%SZ)
DF="$PROD/fill/fill_on_production.sh"
fp() {   # the cut's fingerprint, then the whole tables' row counts (prefixed "all ")
  { PGOPTIONS='-c default_transaction_read_only=on' $PGBIN/psql -X -At -v ON_ERROR_STOP=1 -v cut=2026-10-23 -f "$PROD/sql/data_fingerprint.sql"
    PGOPTIONS='-c default_transaction_read_only=on' $PGBIN/psql -X -At -v ON_ERROR_STOP=1 -c "select 'all futures_data.ohlcv_1d '||count(*) from futures_data.ohlcv_1d" -c "select 'all futures_data.ohlcv_1d_raw '||count(*) from futures_data.ohlcv_1d_raw"
  } > "$1" || die "fingerprint failed"; }
check_db
say "== 04 hole fill on $DBNAME, $(date -u +%FT%TZ); fill script sha256 $(shasum -a 256 "$DF" | cut -c1-16); expected rows per table: $ROWS to the cut, $ALL in all"
fp "$D/04_${TAG}_${STAMP}_before.txt"; sed 's/^/before: /' "$D/04_${TAG}_${STAMP}_before.txt" | tee -a "$LOG"
if [ "$TARGET" = production ]; then
  print -r -- "$ALL" | zsh "$DF" production 2>&1 | tee -a "$LOG"; rc=${pipestatus[2]}
else
  print -r -- "$ALL" | zsh "$DF" scratch "$DBNAME" 2>&1 | tee -a "$LOG"; rc=${pipestatus[2]}
fi
scrub "$LOG" "$RUN_DIR"/fill/fill_*.log(N)
[ $rc -eq 0 ] || die "the fill exited $rc (its own lines above and its log say what was and was not written; exit 5 with another count: read the holes listed after 2026-10-23, then run again with both numbers)" $rc
fp "$D/04_${TAG}_${STAMP}_after.txt"; sed 's/^/after:  /' "$D/04_${TAG}_${STAMP}_after.txt" | tee -a "$LOG"
python3 - "$D/04_${TAG}_${STAMP}_before.txt" "$D/04_${TAG}_${STAMP}_after.txt" "$ROWS" "$ALL" <<'PY' | tee -a "$LOG"
import sys
n = int(sys.argv[3]); n_all = int(sys.argv[4]); ok = True
def line(name): return [l.split() for l in open(sys.argv[1]) if l.startswith(name + " ")][0], [l.split() for l in open(sys.argv[2]) if l.startswith(name + " ")][0]
for t in ("futures_data.ohlcv_1d", "futures_data.ohlcv_1d_raw"):
    x, y = line(t); grew = int(y[1]) - int(x[1]); good = grew == n
    ok &= good; print("%s: %s rows to the cut before, %s after, +%d (expected +%d) %s" % (t, x[1], y[1], grew, n, "PASS" if good else "FAIL"))
for t in ("all futures_data.ohlcv_1d", "all futures_data.ohlcv_1d_raw"):
    x, y = line(t); grew = int(y[-1]) - int(x[-1]); good = grew == n_all
    ok &= good; print("%s: +%d in all (expected +%d) %s" % (t, grew, n_all, "PASS" if good else "FAIL"))
for t in ("contract_ohlcv_1d_less_loaded_at", "metadata.contract_metadata", "micros ohlcv_1d", "micros ohlcv_1d_raw", "micros contract_ohlcv_1d"):
    x, y = line(t); good = x == y; ok &= good; print("%s: %s %s" % (t, "unchanged" if good else "CHANGED", "PASS" if good else "FAIL"))
print("04 fingerprint check: " + ("PASS" if ok else "FAIL")); sys.exit(0 if ok else 1)
PY
[ ${pipestatus[1]} -eq 0 ] || die "the fingerprints after the fill are not the expected ones (see $LOG)"
say "04 DONE on $DBNAME"
