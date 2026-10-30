#!/bin/zsh
# 07_undo_part_B.sh: the way back from part B: takes MGC.v.0 and SIL.v.0 out of both continuous tables, and so also the
# rows 08 filled for them. Nothing else changes, so the contract rows and the hole fill stay, and part B and 08 can run again.
#
#   zsh 07_undo_part_B.sh production                    new_algo_data (config/defaults.json); asks for the database name
#   SCRATCH_DB=<clone> zsh 07_undo_part_B.sh scratch    a rehearsal clone (CONFIRM=<clone> skips the question)
#
# CHANGES: deletes those two symbols from futures_data.ohlcv_1d and ohlcv_1d_raw, one transaction, after a CSV copy of both
#   in $RUN_DIR/archive/ (bought bars: never commit it). Moves 08's DONE marker aside, so 08 can run again.
# READS: both continuous tables. TAKES: seconds. UNDO: 07 and 08 again.
# THE QT RUNS stay stopped until the configs match what is stored again: with the rows gone, a config with the pairs refuses
#   (README, "The QT runs").
set -u
. "${0:A:h}/lib.zsh"
TARGET="${1:-${TARGET:-}}"
resolve_target
LOG="$RUN_DIR/data/07_undo_${TAG}.log"; mkdir -p "$RUN_DIR/data"
check_db
n1=$(qr "select count(*) from futures_data.ohlcv_1d where symbol in ('MGC.v.0','SIL.v.0')") || die "the count failed"
n2=$(qr "select count(*) from futures_data.ohlcv_1d_raw where symbol in ('MGC.v.0','SIL.v.0')") || die "the count failed"
say "stored: $n1 rows in ohlcv_1d, $n2 in ohlcv_1d_raw"
[ "$n1" != 0 ] || [ "$n2" != 0 ] || { say "nothing to undo"; exit 0 }
if [ "${CONFIRM:-}" != "$DBNAME" ]; then
  print -n "This deletes MGC.v.0 and SIL.v.0 from both continuous tables of $DBNAME. Type the database name to go on: "
  read ans; [ "$ans" = "$DBNAME" ] || die "not confirmed; nothing changed" 5
fi
BK="$RUN_DIR/archive/part_B_${TAG}_$(date -u +%Y%m%dT%H%M%SZ)"; mkdir -p "$BK"
copyout "select * from futures_data.ohlcv_1d where symbol in ('MGC.v.0','SIL.v.0') order by time, symbol" "$BK/ohlcv_1d.csv" || die "the copy failed; nothing deleted"
copyout "select * from futures_data.ohlcv_1d_raw where symbol in ('MGC.v.0','SIL.v.0') order by ts_event, symbol" "$BK/ohlcv_1d_raw.csv" || die "the copy failed; nothing deleted"
[ "$(csvrows "$BK/ohlcv_1d.csv")" = "$n1" ] && [ "$(csvrows "$BK/ohlcv_1d_raw.csv")" = "$n2" ] || die "the copy's counts differ; nothing deleted"
$PGBIN/psql -X -q -v ON_ERROR_STOP=1 <<SQL 2>&1 | tee -a "$LOG"
BEGIN;
DO \$\$ DECLARE n bigint; BEGIN
  WITH d AS (DELETE FROM futures_data.ohlcv_1d WHERE symbol IN ('MGC.v.0','SIL.v.0') RETURNING 1) SELECT count(*) INTO n FROM d;
  IF n <> $n1 THEN RAISE EXCEPTION 'ohlcv_1d: deleted % rows, expected $n1', n; END IF;
  WITH d AS (DELETE FROM futures_data.ohlcv_1d_raw WHERE symbol IN ('MGC.v.0','SIL.v.0') RETURNING 1) SELECT count(*) INTO n FROM d;
  IF n <> $n2 THEN RAISE EXCEPTION 'ohlcv_1d_raw: deleted % rows, expected $n2', n; END IF;
END \$\$;
COMMIT;
SQL
[ ${pipestatus[1]} -eq 0 ] || die "the delete failed and rolled back; nothing changed"
if [ "$TARGET" = production ]; then m="$RUN_DIR/fill_micros/fill_micros_production.DONE"; else m="$RUN_DIR/fill_micros/fill_micros_scratch_$DBNAME.DONE"; fi
[ -e "$m" ] && mv "$m" "$m.undone_$(date -u +%Y%m%dT%H%M%SZ)" && say "08's marker moved aside"
say "copy of the rows: $BK"
say "PART B UNDONE on $DBNAME"
