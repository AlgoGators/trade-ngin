#!/bin/zsh
# 09_backtests.sh: the stored CONSERVATIVE backtests of the seed: 2y, 5y, 10y and 16y, all ending on the same last bar.
#
#   TARGET=production REF=<release sha> zsh 09_backtests.sh prepare     worktree .worktrees/futures_release at REF, build
#   TARGET=production REF=<release sha> zsh 09_backtests.sh run         the four windows in order: 2y, 5y, 10y, 16y
#   TARGET=production REF=<release sha> zsh 09_backtests.sh run 16y     only the windows named
#   TARGET=production REF=<release sha> zsh 09_backtests.sh report      rebuild the set's reports from the stored runs
#   Rehearsal: TARGET=scratch SCRATCH_DB=<scratch database> REF=<sha> WT=<built worktree>
#              PIN_END=2026-10-23 zsh 09_backtests.sh run 2y           ONE window per clone (a pinned run's id is its
#                                                                       end date alone, so two windows would collide)
#
# CHANGES: adds one run per window to backtest.results, equity_curve, executions, final_positions and run_metadata
#   (insert only, under a new run id). It deletes and updates nothing; no backtest writes signals. It writes the run
#   config into the worktree's config/ (the frozen reference, config_reference/, plus the target's
#   database block) and the set folder: $RUN_DIR/backtest_results/conservative_end_<UTC date>/ (production) or
#   $RUN_DIR/<tag>/backtests/ (scratch).
# READS: futures_data.ohlcv_1d, ohlcv_1d_raw, contract_ohlcv_1d, metadata.contract_metadata (the runner); the target's
#   login from config/defaults.json (production). Sends no e-mail.
# TAKES: on the scratch server about 1, 2, 3 and 5 minutes; on production the 10-14 set took 4, 8, 15 and 21 minutes
#   (about 50 minutes with the build).
# UNDO: nothing to undo for the book: stored backtests are keyed by run id and nothing reads a set until it is handed
#   over. To remove a set's rows: delete from each backtest table where split_part(run_id, '|', 1) = <run id> (the ids
#   are in the set's runs.tsv). Not needed for a re-run: a new run adds a new id.
#
# A set is the UTC day it runs on: before each window the script checks that the UTC date and the last stored bar are
# still the set's, so the four windows end on the same bar; if either moved it stops and the set is run again.
set -u
. "${0:A:h}/lib.zsh"
typeset -A LOOKBACK; LOOKBACK=(2y 3 5y 6 10y 11 16y 16)
ORDER=(2y 5y 10y 16y)
resolve_target; resolve_worktree
BIN="$WT/build/bin/Release/bt_portfolio_conservative"
PIN="${PIN_END:-}"
if [ -n "$PIN" ]; then
  [ "$TARGET" = scratch ] || die "PIN_END is for the rehearsal only; production runs end when they start, as on 2026-10-14" 2
  END="$PIN"; RES="$RUN_DIR/${TAG}/backtests"
elif [ "$TARGET" = production ]; then END="${END:-$(date -u +%F)}"; RES="$RUN_DIR/backtest_results"
else END="${END:-$(date -u +%F)}"; RES="$RUN_DIR/${TAG}/backtests"; fi
SET="$RES/conservative_end_$END"; [ -n "$PIN" ] && SET="$RES/conservative_pinned_$END"
OUT="$SET"; mkdir -p "$OUT"; LOG="$OUT/script.log"
trap 'unlock_run; exit 130' INT TERM HUP

export_run() {   # <run id> <folder>: every backtest table's rows of the run, plain CSV, ordered by every column
  local id="$1" F="$2" t cols
  for t in ${(f)"$(qr "select distinct table_name from information_schema.columns where table_schema='backtest' and column_name='run_id' order by 1")"}; do
    cols=$(qr "select string_agg(quote_ident(column_name), ',' order by ordinal_position) from information_schema.columns where table_schema='backtest' and table_name='$t'")
    copyout "select * from backtest.$t where split_part(run_id, '|', 1) = '$id' order by $cols" "$F/backtest_$t.csv" || return 1
    print -r -- "backtest.$t: $(csvrows "$F/backtest_$t.csv") rows"
  done
  ( cd "$F" && shasum -a 256 backtest_*.csv ) > "$F/exports.sha256"
}

cmd="${1:-}"; shift 2>/dev/null
case "$cmd" in

prepare)
  check_db
  prepare_worktree bt_portfolio_conservative
  { engine_line bt_portfolio_conservative; template_diff } | tee "$OUT/engine.txt" | tee -a "$LOG"
  say "READY" ;;

run)
  [ -x "$BIN" ] || die "run prepare first"
  [ "$(git -C "$WT" rev-parse HEAD)" = "$REF" ] || die "$WT is not at $REF"
  check_db
  labels=("$@"); (( ${#labels} )) || labels=($ORDER)
  for L in $labels; do [ -n "${LOOKBACK[$L]:-}" ] || die "a window is one of $ORDER, not $L" 2; done
  [ -n "$PIN" ] && [ ${#labels} -ne 1 ] && die "PIN_END runs one window per clone" 2
  LASTBAR=$(qr "select max(time)::date from futures_data.ohlcv_1d where symbol like '%.v.0' and symbol <> 'MBT.v.0'")
  [ -f "$SET/prices.txt" ] && [ "$(cat "$SET/prices.txt")" != "$LASTBAR" ] && die "the set ran on bars to $(cat "$SET/prices.txt"); the table now ends $LASTBAR: start a new set"
  print -r -- "$LASTBAR" > "$SET/prices.txt"
  [ -f "$SET/runs.tsv" ] || print -r -- "label	lookback_years	end	run_id	first_bar	first_traded_day	last_day	rc	seconds	engine	database	finished" > "$SET/runs.tsv"
  ENGINE=$(git -C "$WT" log --oneline -1 HEAD)
  say "set $SET; end $END${PIN:+ (pinned)}; bars to $LASTBAR (MBT aside); windows $labels; engine $ENGINE"
  for L in $labels; do
    LY=${LOOKBACK[$L]}; F="$SET/${L}_running"; /bin/rm -rf "$F"; mkdir -p "$F"
    before=$(qr "select coalesce(string_agg(run_id, ',' order by run_id), '') from backtest.results where portfolio_id='$PID'")
    [ -n "$PIN" ] && [ "$(qr "select count(*) from backtest.results where split_part(run_id,'|',1)='TREND_FOLLOWING_${END//-/}_040000_000'")" != 0 ] && die "a run pinned to $END is already stored in $DBNAME"
    say "$L: waiting for the run lock"; lock_run "backtest $L"
    if [ -z "$PIN" ]; then
      [ "$(date -u +%F)" = "$END" ] || { unlock_run; die "the UTC date is past $END; the windows would end a day apart. Run the set again (UTC) into a new folder" }
    fi
    [ "$(qr "select max(time)::date from futures_data.ohlcv_1d where symbol like '%.v.0' and symbol <> 'MBT.v.0'")" = "$LASTBAR" ] || { unlock_run; die "new bars arrived after the set started ($LASTBAR then); move $SET aside and run the set again" }
    write_config $LY $PIN; save_config "$F"
    say "$L: running, $LY years of prices to $END"; t0=$(date +%s)
    # the runner's output is filtered as it streams (a 16-year run prints hundreds of MB)
    ( cd "$WT" && $AWAKE "$BIN" 2>&1 ) | python3 -c '
import sys, re, gzip, collections
keep = re.compile(r"WARN|ERROR|FATAL|Exception|Failed|Config summary|Running conservative|Backtest completed|Saving|saved|FROZEN|run_id|LISTING|PRICE_SOURCE_CHANGE|RELABEL|warm|STATISTICS_CONVENTION|RESOLVED_SLEEVES|sleeve|^Symbols:|Retrieved|Initial capital|Registry contains|email|smtp", re.I)
res = re.compile(r"^(=======|Total Return|Sharpe|Sortino|Max Drawdown|Calmar|Volatility|Trade win|Closing fills|Transaction Costs|Roll Costs|Roll Fills|Backtest failed|Error code|Unexpected error)")
tail = collections.deque(maxlen=300); n = 0
host = re.compile(r"db=[^/\s]+/")
with gzip.open(sys.argv[1], "wt") as log, open(sys.argv[2], "w") as out:
    for line in sys.stdin:
        line = host.sub("db=<host>/", line)   # no server address in any kept file
        n += 1; tail.append(line)
        if res.match(line): out.write(line)
        if keep.search(line) or res.match(line): log.write(line)
with open(sys.argv[3], "w") as t: t.writelines(tail)
print("lines printed by the runner: %d" % n)' "$F/run_log_filtered.txt.gz" "$F/result_block.txt" "$F/run_log_tail.txt"
    rc=${pipestatus[1]}; secs=$(( $(date +%s) - t0 ))
    for f in "$WT"/logs/*.log(N) "$WT"/logs/*/*.log(N) "$WT"/apps/backtest/results/*(N.); do /bin/rm -f "$f"; done
    /bin/rm -f "$WT/config/defaults.json"   # the login leaves the worktree with the run
    unlock_run
    gzip -dc "$F/run_log_filtered.txt.gz" | grep -a "Config summary: db=" | head -1 | grep -q "/$DBNAME, " || { say "$L: the runner did not name $DBNAME"; rc=9; }
    no_email_in_log "$F/run_log_filtered.txt.gz"
    after=$(qr "select coalesce(string_agg(run_id, ',' order by run_id), '') from backtest.results where portfolio_id='$PID'")
    new=(${(s:,:)$(python3 -c "import sys;b=set(filter(None,sys.argv[1].split(',')));print(','.join(sorted(x for x in sys.argv[2].split(',') if x and x not in b)))" "$before" "$after")})
    if [ $rc -eq 0 ] && [ ${#new} -ne 1 ]; then say "$L: expected one new stored run, found ${#new} (${new[*]:-none}); a save that fails still exits 0"; rc=8; fi
    if [ $rc -ne 0 ]; then
      tail -25 "$F/run_log_tail.txt"; print -r -- "$L	$LY	$END	-	-	-	-	$rc	$secs	$ENGINE	$DBNAME	$(date -u +%FT%TZ)" >> "$SET/runs.tsv"
      mv "$F" "$SET/${L}_FAILED_$(date -u +%Y%m%dT%H%M%SZ)"; die "$L exited $rc"
    fi
    ID=${new[1]}; print -r -- "$ID" > "$F/run_id.txt"
    export_run "$ID" "$F" > "$F/export.txt" || die "$L: export of $ID failed"
    span=$(qr "select min(timestamp)::date||' '||(select min(timestamp)::date from backtest.executions where split_part(run_id,'|',1)='$ID')||' '||max(timestamp)::date from backtest.equity_curve where split_part(run_id,'|',1)='$ID'")
    read first_bar first_traded last_day <<< "$span"
    { print -r -- "window: $L = $LY years of prices ending at $END${PIN:+ (pinned)} (stored bars to $LASTBAR)"; print -r -- "first day of the equity curve: $first_bar"
      print -r -- "first traded day (first fill): $first_traded"; print -r -- "last day: $last_day"; print -r -- "run id: $ID"; print -r -- "engine: $ENGINE"
      print -r -- "binary sha256: $(shasum -a 256 "$BIN" | cut -c1-64)"; print -r -- "library sha256: $(shasum -a 256 "$WT/build/bin/Release/libtrade_ngin.dylib" | cut -c1-64)"
      print -r -- "database: $DBNAME"; print -r -- "run took ${secs}s, finished $(date -u +%FT%TZ)"; } > "$F/window.txt"
    print -r -- "$L	$LY	$END	$ID	$first_bar	$first_traded	$last_day	0	$secs	$ENGINE	$DBNAME	$(date -u +%FT%TZ)" >> "$SET/runs.tsv"
    D="$SET/${L}_${first_traded}_to_${last_day}"; [ -e "$D" ] && D="${D}_$(date -u +%H%M%S)"; mv "$F" "$D"
    for c in "$D"/backtest_*.csv; do [ "$(wc -c < "$c")" -gt 200000 ] && gzip -nf "$c"; done
    say "$L: done in ${secs}s, run id $ID, traded $first_traded .. $last_day -> ${D:t}"
    cat "$D/result_block.txt" | tee -a "$LOG"
  done ;;

report)
  check_db
  [ -f "$SET/runs.tsv" ] || die "no runs.tsv in $SET"
  python3 "$PROD/report/backtest_report.py" "$SET" "$PROD/report/backtest_report_knobs.md" || die "report failed"
  ;;

*) sed -n 2,30p "$0"; exit 2 ;;
esac
