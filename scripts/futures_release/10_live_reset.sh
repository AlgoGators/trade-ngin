#!/bin/zsh
# 10_live_reset.sh: clears the CONSERVATIVE live book before the dated chain (11), after an archive copy of everything
# it replaces.
#
#   TARGET=production zsh 10_live_reset.sh show                   read only: what is stored for the book, per table
#   TARGET=production zsh 10_live_reset.sh reset                  archive, then clear (asks you to type the database name)
#   TARGET=production zsh 10_live_reset.sh restore <archive dir>  put the archived book back (the undo)
#   Rehearsal: TARGET=scratch SCRATCH_DB=<scratch database> CONFIRM=<scratch database> zsh 10_live_reset.sh reset
#
# CHANGES (reset): deletes the rows of portfolio_id CONSERVATIVE_PORTFOLIO, and only those, from trading.equity_curve,
#   executions, live_results, live_run_metadata, positions, signals and corp_action_applied (the tables the live runner
#   writes for the book), and sets trading.strategy_trading_days_metadata.live_start_date to 2025-10-05 for the book. One
#   transaction: each table's deleted count must equal its archived count, or everything rolls back. No other book's
#   row is touched (every statement names the book). Never touched: trading.position_overrides (people's actions),
#   strategy_config, strategy_registry and every QT book.
# ARCHIVE FIRST (nothing is deleted until both exist and read back): the whole trading schema, every book, as a
#   pg_dump custom file, and the book's rows of each table as CSV, in $RUN_DIR/archive/<target>_<stamp>/.
# READS: the trading schema; the login from config/defaults.json (production).
# TAKES: about a minute, most of it the schema dump.
# UNDO: restore <archive dir> (the CSVs, one transaction, counts checked); or pg_restore of trading_schema_full.dump.
set -u
. "${0:A:h}/lib.zsh"
resolve_target
START=2025-10-05
TABLES=(equity_curve executions live_results live_run_metadata positions signals corp_action_applied)
OUT="$RUN_DIR/archive"; mkdir -p "$OUT"; LOG="$OUT/reset_${TAG}.log"

show() {
  for t in $TABLES; do
    say "trading.$t: $(qr "select count(*)||' rows, '||coalesce(min(date)::date::text,'-')||' .. '||coalesce(max(date)::date::text,'-') from trading.$t where portfolio_id='$PID'" 2>/dev/null \
      || qr "select count(*)||' rows' from trading.$t where portfolio_id='$PID'")"
  done
  say "live_start_date: $(qr "select coalesce(string_agg(strategy_id||' '||live_start_date, ', '), 'no row') from trading.strategy_trading_days_metadata where portfolio_id='$PID'")"
  say "left alone, for information: position_overrides $(qr "select count(*) from trading.position_overrides where portfolio_id='$PID'"), strategy_config $(qr "select count(*) from trading.strategy_config where portfolio_id='$PID'"), strategy_registry $(qr "select count(*) from trading.strategy_registry where portfolio_id='$PID'") rows of the book"
}

confirm() {
  if [ "${CONFIRM:-}" = "$DBNAME" ]; then return 0; fi
  print -n "$1 Type the database name to go on: "; read ans; [ "$ans" = "$DBNAME" ] || die "not confirmed; nothing changed" 5
}

cmd="${1:-}"; shift 2>/dev/null
case "$cmd" in
show) check_db; show ;;

reset)
  check_db; show
  confirm "This CLEARS every stored day of $PID in $DBNAME (after the archive copy)."
  BK="$OUT/${TAG}_$(date -u +%Y%m%dT%H%M%SZ)"; mkdir -p "$BK"
  $PGBIN/pg_dump -n trading -Fc -f "$BK/trading_schema_full.dump" \
    || die "the full dump of the trading schema failed (a server newer than this pg_dump needs the matching client); nothing deleted"
  $PGBIN/pg_restore -l "$BK/trading_schema_full.dump" > "$BK/trading_schema_full.list" || die "the dump does not read back; nothing deleted"
  say "archive: the whole trading schema, $(du -h "$BK/trading_schema_full.dump" | cut -f1), $(grep -c 'TABLE DATA' "$BK/trading_schema_full.list") tables with data"
  typeset -A N
  for t in $TABLES; do
    copyout "select * from trading.$t where portfolio_id='$PID'" "$BK/$t.csv" || die "archive of $t failed; nothing deleted"
    N[$t]=$(csvrows "$BK/$t.csv"); say "archived trading.$t: ${N[$t]} rows"
  done
  copyout "select * from trading.strategy_trading_days_metadata where portfolio_id='$PID'" "$BK/strategy_trading_days_metadata.txt" || die "archive of the start date failed"
  ( cd "$BK" && shasum -a 256 *.csv *.dump *.txt ) > "$BK/SHA256SUMS"
  # One transaction: each delete's count must equal the archive's, every other book is untouched by construction.
  { print -r -- "BEGIN;"
    for t in $TABLES; do
      print -r -- "DO \$\$ DECLARE n bigint; BEGIN WITH d AS (DELETE FROM trading.$t WHERE portfolio_id='$PID' RETURNING 1) SELECT count(*) INTO n FROM d; IF n <> ${N[$t]} THEN RAISE EXCEPTION 'trading.$t: deleted % rows, the archive holds ${N[$t]}', n; END IF; END \$\$;"
    done
    print -r -- "DO \$\$ BEGIN IF (SELECT count(*) FROM trading.strategy_trading_days_metadata WHERE portfolio_id='$PID') <> 1 THEN RAISE EXCEPTION 'the book has not exactly one strategy_trading_days_metadata row'; END IF; END \$\$;"
    print -r -- "UPDATE trading.strategy_trading_days_metadata SET live_start_date='$START' WHERE portfolio_id='$PID';"
    print -r -- "COMMIT;"; } > "$BK/reset.sql"
  $PGBIN/psql -X -q -v ON_ERROR_STOP=1 -f "$BK/reset.sql" 2>&1 | tee -a "$LOG"; [ ${pipestatus[1]} -eq 0 ] || die "the reset failed and rolled back; nothing changed"
  for t in $TABLES; do say "trading.$t now: $(qr "select count(*) from trading.$t where portfolio_id='$PID'") rows of the book"; done
  say "live_start_date now: $(qr "select live_start_date from trading.strategy_trading_days_metadata where portfolio_id='$PID'")"
  say "archive: $BK"; say "RESET DONE" ;;

restore)
  BK="${1:?give the archive folder}"; check_db
  ( cd "$BK" && shasum -a 256 -c --quiet SHA256SUMS ) || die "the archive does not match its SHA256SUMS"
  confirm "This replaces the current $PID rows with the archive $BK."
  # print -r: zsh's echo reads the "\c" of "\copy" as "stop output". Each table's count is checked inside the
  # transaction, so a short load rolls the whole restore back.
  { print -r -- "BEGIN;"
    for t in $TABLES; do n=$(csvrows "$BK/$t.csv")
      print -r -- "DELETE FROM trading.$t WHERE portfolio_id='$PID';"
      print -r -- "\\copy trading.$t FROM '$BK/$t.csv' WITH (FORMAT csv, HEADER)"
      print -r -- "DO \$\$ BEGIN IF (SELECT count(*) FROM trading.$t WHERE portfolio_id='$PID') <> $n THEN RAISE EXCEPTION 'restore of $t loaded % rows, the archive has $n', (SELECT count(*) FROM trading.$t WHERE portfolio_id='$PID'); END IF; END \$\$;"
    done
    sd=$(python3 -c "import csv,sys;r=list(csv.DictReader(open(sys.argv[1])));print(r[0]['live_start_date'] if r else '')" "$BK/strategy_trading_days_metadata.txt")
    [ -n "$sd" ] && print -r -- "UPDATE trading.strategy_trading_days_metadata SET live_start_date='$sd' WHERE portfolio_id='$PID';"
    print -r -- "COMMIT;"; } > "$BK/restore.sql"
  [ "$(grep -c '^\\copy ' "$BK/restore.sql")" = ${#TABLES} ] || die "restore.sql is incomplete; nothing run"
  $PGBIN/psql -X -q -v ON_ERROR_STOP=1 -f "$BK/restore.sql" 2>&1 | tee -a "$LOG"; [ ${pipestatus[1]} -eq 0 ] || die "the restore failed and rolled back"
  for t in $TABLES; do say "trading.$t: $(qr "select count(*) from trading.$t where portfolio_id='$PID'") rows (archive $(csvrows "$BK/$t.csv"))"; done
  say "RESTORED" ;;

*) sed -n 2,25p "$0"; exit 2 ;;
esac
