#!/bin/zsh
# pull_micro_metals.sh <END>: the Databento pull of micro gold (MGC) and micro silver (SIL) (PULL_REQUEST.md).
#   START (environment, default 2026-10-27): the first day. END is exclusive, as Databento reads it: the merge day
#   loads through the day before. Per-contract daily bars (parent symbology), both continuous .v.0 series and the
#   contract definitions of those days.
#
# Reads the key from config/databento.env and never prints it or puts it in the process list. Writes only into
# $RUN_DIR/pull_<today UTC>/ and refuses to overwrite a file. CHANGES: nothing in any database. TAKES: seconds; a few
# cents (START=2010-06-06: about two dollars). UNDO: remove the new folder.
set -e
REPO="${0:A:h:h:h}"; RUN_DIR="${RUN_DIR:-$REPO/futures_release_run}"
END="${1:?give END (exclusive), e.g. the seed day}"
[[ "$END" == <->-<->-<-> ]] || { echo "END must be YYYY-MM-DD"; exit 2 }
OUT="$RUN_DIR/pull_$(date -u +%F)"
KEY=$(sed -n 's/^DATABENTO_API_KEY=//p' "$REPO/config/databento.env")
[ -n "$KEY" ] || { echo "no key in config/databento.env"; exit 1 }
mkdir -p "$OUT"
START="${START:-2026-10-27}"

pull() {  # dataset symbol stype schema file
  local f="$OUT/$5.csv.zst"
  [ -e "$f" ] && { echo "exists, skipped: $5"; return 0 }
  printf 'user = "%s:"\n' "$KEY" | curl -sS -f -K - -o "$f.part" \
    -X POST https://hist.databento.com/v0/timeseries.get_range \
    -d dataset="$1" -d symbols="$2" -d stype_in="$3" -d schema="$4" -d start="$START" -d end="$END" \
    -d encoding=csv -d compression=zstd -d map_symbols=true -d pretty_px=true -d pretty_ts=true
  mv "$f.part" "$f"
  echo "$5  $(du -h "$f" | cut -f1)"
}

for r in MGC SIL; do
  pull GLBX.MDP3 $r.FUT parent ohlcv-1d ${r}_contracts_ohlcv-1d
  pull GLBX.MDP3 $r.v.0 continuous ohlcv-1d ${r}_v0_ohlcv-1d
  START=2026-10-27 pull GLBX.MDP3 $r.FUT parent definition ${r}_definition   # the new days' contracts only: the full history is the slow pull
done
print -r -- "start $START end $END (exclusive) pulled $(date -u +%FT%TZ)" > "$OUT/PULL.txt"
echo "done: $OUT"
