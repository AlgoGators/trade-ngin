#!/bin/zsh
# 06_build_backfill.sh <pull folder>: builds the MGC and SIL backfill rows (the BACKFILL of 06 part A2 and 07 part B)
# from the pull of the days after 2026-10-26 (PULL_REQUEST.md), with load/build_rows.py
# (load/build_rows.py, START=2026-10-27).
#
#   zsh 06_build_backfill.sh production <pull folder>                 the twin rows read (read only) from new_algo_data
#   SCRATCH_DB=<clone> zsh 06_build_backfill.sh scratch <pull folder>  the twin rows read from a clone
#
# CHANGES: no database. Writes $RUN_DIR/backfill/<pull folder name>_<target>/ (contract_ohlcv_1d.csv, ohlcv_1d.csv,
#   ohlcv_1d_raw.csv, expirations.csv, build.txt, SHA256SUMS); the decompressed files go to $RUN_DIR/tmp/.
# READS: the pull folder; futures_data.contract_ohlcv_1d of GC and SI (the twin's expirations), read only.
# TAKES: seconds. UNDO: remove the folder it wrote.
set -u
. "${0:A:h}/lib.zsh"
TARGET="${1:-${TARGET:-}}"; PULL="${2:-}"
resolve_target
[ -d "$PULL" ] || die "give the folder the pull printed" 2
PULL="${PULL:A}"
for r in MGC SIL; do for f in ${r}_contracts_ohlcv-1d ${r}_v0_ohlcv-1d; do [ -s "$PULL/$f.csv.zst" ] || die "missing $PULL/$f.csv.zst" 2; done; done
OUTB="$RUN_DIR/backfill/${PULL:t}_${TAG}"; [ -e "$OUTB" ] && die "$OUTB exists; move it aside to rebuild" 2
W="$RUN_DIR/tmp/backfill_${PULL:t}_${TAG}"; /bin/rm -rf "$W"; mkdir -p "$W/data" "$W/defs" "$OUTB"
for r in MGC SIL; do
  zstd -q -d -c "$PULL/${r}_contracts_ohlcv-1d.csv.zst" > "$W/data/${r}_contracts.csv" || die "decompress ${r} contracts"
  zstd -q -d -c "$PULL/${r}_v0_ohlcv-1d.csv.zst" > "$W/data/${r}_v0.csv" || die "decompress ${r} v0"
  [ -s "$PULL/${r}_definition.csv.zst" ] && { zstd -q -d -c "$PULL/${r}_definition.csv.zst" > "$W/defs/${r}_definition.csv" || die "decompress ${r} definitions"; }
done
check_db
copyout "select * from futures_data.contract_ohlcv_1d where root in ('GC','SI') order by instrument_id, date" "$W/twin.csv" || die "twin export"
END=$(python3 -c "
import csv, sys
d = [r['ts_event'][:10] for f in sys.argv[1:] for r in csv.DictReader(open(f))]
print(max(d) if d else '')" "$W"/data/*_contracts.csv "$W"/data/*_v0.csv)
[ -n "$END" ] || die "the pull holds no bars"
defs=("$W"/defs/*.csv(N))
{ print -r -- "pull $PULL ($(cat "$PULL/PULL.txt" 2>/dev/null || print -r -- 'no PULL.txt')); START 2026-10-27; END $END (the last bar in the files)"
  print -r -- "definitions: ${#defs} file(s); twin rows from $DBNAME: $(csvrows "$W/twin.csv")"
  START=2026-10-27 python3 -I "$PROD/load/build_rows.py" "$W/data" "$W/twin.csv" "$END" "$OUTB" "${defs[@]}"
} 2>&1 | tee "$OUTB/build.txt"
[ ${pipestatus[1]} -eq 0 ] || die "build_rows failed (see $OUTB/build.txt)"
( cd "$OUTB" && shasum -a 256 *.csv ) > "$OUTB/SHA256SUMS"
for f in contract_ohlcv_1d ohlcv_1d ohlcv_1d_raw; do say "$f: $(csvrows "$OUTB/$f.csv") rows"; done
say "BACKFILL=$OUTB"
# A pull from the start of the history (START=2010-06-06) also gives part B's rows to 2026-10-26, under rows/.
FIRST=$(python3 -c "
import csv, sys
print(min(r['ts_event'][:10] for f in sys.argv[1:] for r in csv.DictReader(open(f))))" "$W"/data/*_v0.csv)
if [[ "$FIRST" < "2026-10-27" ]]; then
  mkdir -p "$OUTB/rows"
  python3 -I "$PROD/load/build_rows.py" "$W/data" "$W/twin.csv" 2026-10-26 "$OUTB/rows" "${defs[@]}" 2>&1 | tee -a "$OUTB/build.txt"
  [ ${pipestatus[1]} -eq 0 ] || die "build_rows failed for rows/ (see $OUTB/build.txt)"
  ( cd "$OUTB/rows" && shasum -a 256 *.csv ) > "$OUTB/rows/SHA256SUMS"
  say "ROWS=$OUTB/rows"
fi
