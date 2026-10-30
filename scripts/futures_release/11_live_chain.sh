#!/bin/zsh
# 11_live_chain.sh: the CONSERVATIVE live history of the seed: one dated live run per calendar date from 2025-10-05 to
# END, in date order, never with --send-email. Run 10_live_reset.sh reset first.
#
#   TARGET=production REF=<release sha> zsh 11_live_chain.sh prepare   worktree .worktrees/futures_release at REF, build,
#                                                                      and END fixed: it prints END=<date>
#   TARGET=production REF=<release sha> zsh 11_live_chain.sh run       2025-10-05 .. END
#   TARGET=production REF=<release sha> FROM=<date> zsh 11_live_chain.sh run    resume at FROM after a stop
#   TARGET=production REF=<release sha> zsh 11_live_chain.sh check     read only: the book's identity checks
#   TARGET=production REF=<release sha> zsh 11_live_chain.sh export    read only: the book's tables as CSV (the package)
#   Rehearsal: TARGET=scratch SCRATCH_DB=<clone> REF=<sha> WT=<built worktree> END=2026-10-24 zsh 11_live_chain.sh run
#
# END is fixed once. The first prepare or run takes END= from the environment, or by default the last stored weekday bar
# plus one day (the run that settles the last complete bar), and keeps it in $RUN_DIR/chain_end_<target>.txt. Every later
# call, run, check, export and 12_package.sh live, reads that file, so a daily bar landing during the chain moves nothing.
# An END= that differs refuses. To start a new chain, remove the file.
#
# CHANGES: each run writes the book's day: trading.live_results, positions, executions, signals, equity_curve,
#   live_run_metadata (portfolio_id CONSERVATIVE_PORTFOLIO only), as the production runner does. It writes the run
#   config into the worktree's config/ (the frozen reference plus the target's database block; removed after each
#   run) and the run folder: $RUN_DIR/live_results/conservative_live_2025-10-05_to_<END>/ (production) or
#   $RUN_DIR/<tag>/live/ (scratch). Each run's report body is written to bodies/ (the runner's
#   TRADE_NGIN_EMAIL_BODY_DIR); nothing is mailed, and every log is checked for an e-mail line.
# READS: the futures tables, the book's own earlier days, the login from config/defaults.json (production).
# TAKES: on production the 10-14 chain took about 28 s a run (375 runs, about 3 hours); the scratch server is faster.
# UNDO: 10_live_reset.sh restore <archive> puts the book back as it was before the reset.
#
# BEFORE ANY RUN: the data to 2026-10-23 must equal sql/expected_fingerprint.txt (lib.zsh, check_fingerprints), or the run
# refuses (exit 3). check adds the live reference: the rows to 2026-10-23 must equal the rehearsal's chain. No day after
# 2026-10-23 has a reference (README, "Dates after 2026-10-23").
#
# The run dated END reads the bars through END - 1, so it settles the last complete bar, and the last run may be the
# host's own date, as on 2026-10-14. Every run before it is a past-date run, the mode every rehearsal and gate ran.
set -u
. "${0:A:h}/lib.zsh"
case "${1:-}" in prepare|run|check|export) ;; *) sed -n '2,/^set -u/p' "$0" | sed '$d'; exit 2 ;; esac
resolve_target; resolve_worktree
BIN="$WT/build/bin/Release/live_portfolio_conservative"
START=2025-10-05
# The chain ends on the run that settles the last complete bar: the last stored weekday bar (the .v.0 rows less MBT, as 09
# takes it) plus one day, never after the host's own date (every run is then a past-date run, but at most the last one).
ENDFILE="$RUN_DIR/chain_end_${TAG}.txt"
if [ -s "$ENDFILE" ]; then
  [ -z "${END:-}" ] || [ "$END" = "$(cat "$ENDFILE")" ] || die "END=$END, but this chain's END is $(cat "$ENDFILE") ($ENDFILE); remove that file only to start a new chain" 2
  END=$(cat "$ENDFILE")
elif [ "${1:-}" = prepare ] || [ "${1:-}" = run ]; then
  if [ -z "${END:-}" ]; then
    lb=$(qr "select max(time)::date from futures_data.ohlcv_1d where symbol like '%.v.0' and symbol <> 'MBT.v.0'") || die "cannot read the last bar"
    END=$(python3 -c "import datetime as t,sys;print((t.date.fromisoformat(sys.argv[1])+t.timedelta(1)).isoformat())" "$lb")
  fi
  [[ "$END" > "$(date +%F)" ]] && die "END $END is after the host's own date $(date +%F); give END= or run later" 2
  print -r -- "$END" > "$ENDFILE"
else
  die "this chain has no END yet: run prepare first ($ENDFILE)" 2
fi
print -r -- "END=$END"
if [ "$TARGET" = production ]; then OUT="$RUN_DIR/live_results/conservative_live_${START}_to_${END}"
else OUT="$RUN_DIR/${TAG}/live"; fi
mkdir -p "$OUT/checks/logs" "$OUT/bodies"; LOG="$OUT/checks/script.log"
trap '/bin/rm -f "$WT/config/defaults.json"; unlock_run; exit 130' INT TERM HUP
days() { python3 -c "import datetime as t,sys;s=t.date.fromisoformat(sys.argv[1]);e=t.date.fromisoformat(sys.argv[2]);print(*[(s+t.timedelta(i)).isoformat() for i in range((e-s).days+1)])" "$1" "$2" }
addd() { python3 -c "import datetime as t,sys;print((t.date.fromisoformat(sys.argv[1])+t.timedelta(int(sys.argv[2]))).isoformat())" "$1" "$2" }

# The rehearsal's chain, to the cut: rows, then the md5 of date, book value, daily P&L and total costs, each to the cent.
LIVE_REFERENCE="384 519541.86 1524f1a6a50a38b17d06f49fdbd6f7d1"
live_reference() {
  qr "select count(*)||' '||coalesce(round(max(current_portfolio_value) filter (where date='2026-10-23'),2)::text,'none')||' '||md5(string_agg(concat_ws(',',date,round(current_portfolio_value,2),round(daily_pnl,2),round(total_transaction_costs,2)), E'\n' order by date)) from trading.live_results where portfolio_id='$PID' and date<='2026-10-23'"
}

others() {   # <cutoff>: every other book's rows in the live tables dated before the cutoff: count and md5 per table and book
  local t dc
  for t dc in live_results "date" positions "date" executions "date" live_run_metadata "date" equity_curve "(timestamp at time zone 'UTC')::date" signals "(timestamp at time zone 'UTC')::date"; do
    qr "select '$t', portfolio_id, count(*), md5(string_agg(x::text, E'\n' order by x::text)) from trading.$t x where portfolio_id <> '$PID' and $dc < '$1' group by 2 order by 2"
  done
}

cmd="${1:-}"; shift 2>/dev/null
case "$cmd" in

prepare)
  check_db
  prepare_worktree live_portfolio_conservative
  { engine_line live_portfolio_conservative; template_diff } | tee "$OUT/checks/engine.txt" | tee -a "$LOG"
  say "READY" ;;

run)
  [ -x "$BIN" ] || die "run prepare first"
  [ "$(git -C "$WT" rev-parse HEAD)" = "$REF" ] || die "$WT is not at $REF"
  check_db
  check_fingerprints
  s="${FROM:-$START}"; [[ "$s" > "$END" ]] && die "nothing to run ($s after $END)"
  if [ "$s" = "$START" ]; then
    [ "$(qr "select count(*) from trading.live_results where portfolio_id='$PID'")" = 0 ] || die "the book is not empty: run 10_live_reset.sh reset first"
    [ "$(qr "select live_start_date from trading.strategy_trading_days_metadata where portfolio_id='$PID'")" = "$START" ] || die "live_start_date is not $START: run 10_live_reset.sh reset first"
    # Other books (the QT desk writes its own day every day) are compared on rows dated two days before the chain began:
    # a run of day T also settles day T-1 and writes T-1's positions, so only older rows are final.
    addd "$(date -u +%F)" -2 > "$OUT/checks/other_books_cutoff.txt"
    others "$(cat "$OUT/checks/other_books_cutoff.txt")" > "$OUT/checks/other_books_before.txt"
  else
    [ "$(qr "select count(*) from trading.live_results where portfolio_id='$PID' and date::date='$(addd $s -1)'")" -ge 1 ] || die "no stored row for $(addd $s -1): run the earlier dates first"
  fi
  [ "$(qr "select count(*) from trading.live_results where portfolio_id='$PID' and date::date between '$s' and '$END'")" = 0 ] || die "dates $s .. $END already have rows; resume with FROM= after the last good date"
  lastbar=$(qr "select max(time)::date from futures_data.ohlcv_1d")
  [[ "$lastbar" < "$(addd $END -1)" ]] && die "bars end $lastbar; a run dated $END needs bars to $(addd $END -1)"
  say "chain $s .. $END; bars to $lastbar; $(head -1 "$OUT/checks/engine.txt" 2>/dev/null || git -C "$WT" log --oneline -1)" | tee -a "$OUT/checks/progress.txt"
  for d in $(days $s $END); do
    lock_run "live $d"; t0=$(date +%s)
    write_config -
    [ -d "$OUT/config_used" ] || save_config "$OUT"
    ( cd "$WT" && TRADE_NGIN_EMAIL_BODY_DIR="$OUT/bodies" $AWAKE "$BIN" "$d" ) > "$OUT/checks/logs/$d.log" 2>&1; rc=$?
    /bin/rm -f "$WT/config/defaults.json"
    unlock_run
    sed -E 's#db=[^/[:space:]]+/#db=<host>/#g' "$OUT/checks/logs/$d.log" | grep -av "password" | gzip -nc > "$OUT/checks/logs/$d.log.gz"; /bin/rm -f "$OUT/checks/logs/$d.log"
    gzip -dc "$OUT/checks/logs/$d.log.gz" | grep -a "Config summary: db=" | head -1 | grep -q "/$DBNAME, " || { say "$d: the run did not name $DBNAME"; rc=9; }
    no_email_in_log "$OUT/checks/logs/$d.log.gz"
    print -r -- "$d rc=$rc $(( $(date +%s) - t0 ))s" | tee -a "$OUT/checks/progress.txt"
    if [ $rc -ne 0 ]; then
      gzip -dc "$OUT/checks/logs/$d.log.gz" | grep -aE "ERROR|REFUS|STOP" | tail -8
      die "$d exited $rc. Read $OUT/checks/logs/$d.log.gz. To resume after fixing: FROM=$d zsh $0 run"
    fi
    [ "$(qr "select count(*) from trading.live_results where portfolio_id='$PID' and date::date='$d'")" = 1 ] || die "$d: no stored row for the date after the run"
  done
  say "CHAIN DONE: $s .. $END" ;;

check)
  check_db
  e="${END}"; R="$OUT/checks/check.txt"
  { print -r -- "CHECK $START .. $e; $DBNAME; $(date -u +%FT%TZ)"
    $PGBIN/psql -X -At -F' | ' -v ON_ERROR_STOP=1 -f "$PROD/sql/live_identity_checks.sql" -v pid="$PID" -v s="$START" -v e="$e" \
      | sed 's/^/  /'
    print -r -- "runs: $(grep -cE '^[0-9-]+ rc=' "$OUT/checks/progress.txt") in progress.txt, non-zero exits: $(grep -cE '^[0-9-]+ rc=[1-9]' "$OUT/checks/progress.txt")"
    if [[ "$e" > "2026-10-23" ]]; then
      lr=$(live_reference); [ "$lr" = "$LIVE_REFERENCE" ] && print -r -- "  live reference to 2026-10-23 | PASS | $lr" || print -r -- "  live reference to 2026-10-23 | FAIL | read $lr, the rehearsal's chain $LIVE_REFERENCE"
    else print -r -- "  live reference to 2026-10-23: not reached, END is $e"; fi
    if [ -s "$OUT/checks/other_books_before.txt" ]; then
      others "$(cat "$OUT/checks/other_books_cutoff.txt")" > "$OUT/checks/other_books_after.txt"
      if cmp -s "$OUT/checks/other_books_before.txt" "$OUT/checks/other_books_after.txt"; then print -r -- "other books' rows dated before the chain began: UNCHANGED ($(wc -l < "$OUT/checks/other_books_before.txt" | tr -d ' ') table-book lines, dated before $(cat "$OUT/checks/other_books_cutoff.txt"))"
      else print -r -- "other books' rows dated before the chain began: CHANGED"; diff "$OUT/checks/other_books_before.txt" "$OUT/checks/other_books_after.txt"; fi
    fi; } 2>&1 | tee "$R"
  grep -qE "\| FAIL \||: CHANGED$" "$R" && die "a check failed, see $R"
  say "checks: $R" ;;

export)
  check_db
  mkdir -p "$OUT/snap"
  for t in ${(f)"$(qr "select table_name from information_schema.columns where table_schema='trading' and column_name='portfolio_id' order by 1")"}; do
    cols=$(qr "select string_agg(quote_ident(column_name), ',' order by ordinal_position) from information_schema.columns where table_schema='trading' and table_name='$t'")
    copyout "select * from trading.$t where portfolio_id='$PID' order by $cols" "$OUT/snap/trading_$t.csv" || die "export of $t failed"
    say "trading.$t: $(csvrows "$OUT/snap/trading_$t.csv") rows"
  done
  ( cd "$OUT/snap" && shasum -a 256 *.csv ) > "$OUT/snap/exports.sha256" ;;

*) sed -n 2,27p "$0"; exit 2 ;;
esac
