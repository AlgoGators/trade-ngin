#!/bin/zsh
# 12_package.sh: the hand-back package from the stored rows, in the 2026-10-14 layout (read only on the database).
#
#   TARGET=production REF=<sha> zsh 12_package.sh backtests [END]   $RUN_DIR/backtest_results/conservative_end_<END>/: SUMMARY.md,
#                                                                   every RUN_REPORT.md, and ../CONFIG_AND_KNOBS.md. With no
#                                                                   END, the newest set that has a runs.tsv
#   TARGET=production REF=<sha> zsh 12_package.sh live [END]        $RUN_DIR/live_results/conservative_live_2025-10-05_to_<END>/:
#                                                                   the live_*.csv exports, analysis/, RUN_REPORT.md. With no
#                                                                   END, the chain's own (11_live_chain.sh keeps it)
#   Rehearsal: TARGET=scratch SCRATCH_DB=<clone> REF=<sha> WT=<worktree> END=2026-10-24 zsh 12_package.sh live
#
# CHANGES: no database row. Writes the report files and analysis CSVs in the package folders, and its inputs under
#   $RUN_DIR/tmp/. READS: the book's live and backtest rows, the futures bars, the held contracts' bars, the
#   benchmarks (equities_data SPY, QQQ, AGG, TLT, GLD; backtest.equity_curve run sg_trend), the runs' logs.
# TAKES: about a minute. UNDO: the files can be rebuilt; nothing else changes.
set -u
. "${0:A:h}/lib.zsh"
resolve_target
WHAT="${1:-}"; START=2025-10-05
export PGOPTIONS='-c default_transaction_read_only=on'
case "$WHAT" in
backtests)
  REF="${REF:-x}"; resolve_worktree
  [ -n "${2:-}" ] && export END="$2"
  if [ -z "${END:-}" ] && [ -z "${PIN_END:-}" ]; then   # a set can end a UTC day before this step: never take today's date
    if [ "$TARGET" = production ]; then base="$RUN_DIR/backtest_results"; else base="$RUN_DIR/${TAG}/backtests"; fi
    sets=("$base"/conservative_end_*/runs.tsv(N.om))
    (( ${#sets} )) || die "no finished set under $base: run 09_backtests.sh first" 2
    s="${sets[1]:h:t}"; export END="${s#conservative_end_}"; print -r -- "the newest set: $s"
  fi
  zsh "$PROD/09_backtests.sh" report ;;

live)
  need_report_modules
  ENDFILE="$RUN_DIR/chain_end_${TAG}.txt"
  END="${2:-${END:-}}"; [ -n "$END" ] || { [ -s "$ENDFILE" ] && END=$(cat "$ENDFILE"); }
  [ -n "$END" ] || die "give END, or run 11_live_chain.sh first" 2
  if [ "$TARGET" = production ]; then OUT="$RUN_DIR/live_results/conservative_live_${START}_to_${END}"; else OUT="$RUN_DIR/${TAG}/live"; fi
  [ -d "$OUT/snap" ] || die "run 11_live_chain.sh export first"
  check_db
  INP="$RUN_DIR/tmp/live_inputs_${TAG}"; /bin/rm -rf "$INP"; mkdir -p "$INP"
  copyout "select time, symbol, close from futures_data.ohlcv_1d where symbol like '%.v.0' and time >= '$START'::date - 10 order by symbol, time" "$INP/fut.csv"
  copyout "select instrument_id, date, close from futures_data.contract_ohlcv_1d where instrument_id in (select distinct instrument_id from trading.positions where portfolio_id='$PID' and instrument_id is not null) and date >= '$START'::date - 40 order by instrument_id, date" "$INP/cbar.csv"
  copyout "select * from metadata.contract_metadata order by 1" "$INP/meta.csv"
  copyout "select (time at time zone 'UTC')::date as d, symbol, adjusted_close from equities_data.ohlcv_1d where symbol in ('SPY','QQQ','AGG','TLT','GLD') and time >= '$START'::date - 10 order by symbol, time" "$INP/mkt.csv"
  copyout "select (timestamp at time zone 'UTC')::date as d, equity from backtest.equity_curve where run_id='sg_trend' order by timestamp" "$INP/sg.csv"
  BT2Y="${BT2Y_RUN:-}"
  if [ -n "$BT2Y" ]; then copyout "select (timestamp at time zone 'UTC')::date as d, equity from backtest.equity_curve where split_part(run_id,'|',1)='$BT2Y' order by timestamp" "$INP/bt2y.csv"
  else print -r -- "d,equity" > "$INP/bt2y.csv"; fi
  copyout "select date as d, symbol, roll_bar_move from trading.executions where portfolio_id='$PID' and execution_type='ROLL' and roll_bar_move is not null order by date, symbol" "$INP/roll.csv"
  for f in "$OUT"/checks/logs/*.log.gz; do gzip -dc "$f" | grep -aE "\[RiskManager\] OVERLAY |BOOK raw_target|\] SIZING_CAPITAL date=" | sed -E 's/^[0-9-]+ [0-9:]+ //' | sed "s/^/${${f:t}%.log.gz} /"; done > "$INP/loglines.txt"
  typeset -A NAMES; NAMES=(live_results live_results positions live_positions executions live_executions signals live_signals equity_curve live_equity_curve live_run_metadata live_run_metadata)
  for t n in ${(kv)NAMES}; do cp "$OUT/snap/trading_$t.csv" "$OUT/$n.csv"; done
  ( cd "$OUT" && python3 "$PROD/report/live_report.py" "$OUT" "$INP" ) || die "live report failed"
  for n in ${(v)NAMES}; do [ "$(wc -c < "$OUT/$n.csv")" -gt 500000 ] && gzip -nf "$OUT/$n.csv"; done
  say "package: $OUT" ;;

*) sed -n '2,/^set -u/p' "$0" | sed '$d'; exit 2 ;;
esac
