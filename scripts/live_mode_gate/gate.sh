#!/bin/zsh
# scripts/live_mode_gate/gate.sh: THE LIVE MODE GATE (HD 2026-10-10).
#
# WHAT IT PROVES. A live runner is run in production with a date equal to the host's date. Every test run
# is made for a past date, because test data ends before today. This gate runs the same dates both ways,
# on the same binary, from the same restored book, and FAILS unless production does what the tests do:
#
#   1. CONSERVATIVE, the ten-day chain: as past dates (the reference), and as production runs it. In
#      production mode the two complete days must be identical to the reference in exit code and report
#      body; the run of the first date with a held feed hole past the tolerance must REFUSE (exit 1,
#      nothing stored); every later date must be refused by the missed-run guard; and THE RESTART (the
#      missed dates run in order, oldest first, on a later day) must end with every stored table
#      identical to the reference.
#   2. BASE, three dates: the same, as a check that the twin runner stayed a twin.
#   3. EQUITY, five dates in both modes: every stored table, the exit codes and the report body
#      identical; and one refused run (a date whose previous day never ran) must store NOTHING, the
#      run-metadata row included.
#   4. THE RE-RUN REPAIR: a day's bars arrive late, two runs are made without them, then those dates and
#      every later one are re-run in date order: every stored table identical to the reference chain. The
#      late day is the bar that books a held roll (issue #188's shape: the roll is legged late by the first
#      pass, and the re-run must not book it twice): 2026-04-27 (MBT confirms its vendor switch on it in the
#      gate's chain) with GATE_SWITCH off, 2026-04-28 (6L's roll bar) with it on.
#   5. GATE_SWITCH=on only (rolls step 4, ROLLS_STEP1_PLAN section 7), on the conservative book's chain: a
#      held roll priced at its two contracts' own closes of one bar (the contract table), an ignored one-bar
#      backward stray and an ignored backward switch (both identical to the reference chain), a run killed
#      while it writes and run again, a run whose ROLL legs fail to store, replayed (both identical to the
#      reference chain), and a crossover day's per-contract rows arriving late with the vendor's bars in,
#      then the dates re-run in order (identical to the reference chain).
#
# THE FEED HOLE. Production mode must refuse a run whose HELD symbol has had no bar past
# live.data_staleness_tolerance_days (4). The 2026-10-21 copy of the data has no such hole in the window, so
# the gate makes one in the database it is given (rolls step 4): 6M.v.0, held by both futures books' S0, loses
# its bars dated 2026-04-21 .. 04-25 in both bar tables for the whole gate and gets them back at the end. Its
# last bar is then 04-20: 4 days old for the 04-25 run (T-1 04-24, not past the tolerance) and 5 for the 04-26
# run (past it). Every chain of the gate runs on the same bars, so the comparisons are unchanged.
#
# GATE_SWITCH=on|off (default off): "on" runs every futures chain with per_contract_rolls true (the
# per-contract bar source) and adds section 5.
#
# HOW PRODUCTION MODE IS RUN. hostclock_shim.c shifts the wall clock of the runner's own process
# (clock_gettime, gettimeofday, time), so the UNPATCHED binary sees the run date as the host's date.
# Nothing in the product reads a test switch: there is no seam to leave on. macOS loads the shim with
# DYLD_INSERT_LIBRARIES; on Linux build it with -shared and load it with LD_PRELOAD (interpose section
# aside, the three functions are the same). The database server's clock is not touched.
#
# NEVER: a run without a date, --send-email, a host other than 127.0.0.1, a database whose name does
# not hold "scratch". The gate writes to the three live books of the database it is given.
#
# THE CHART RULE. A chart process that does not finish in time is killed and that one chart block is left
# out of the report (the run's log says so). Two runs that did the same thing can therefore differ by a
# chart block, so the bodies are compared by compare_bodies.py: byte for byte when neither run's log holds
# a chart timeout line; otherwise with the chart blocks taken out, each side allowed to lack no more charts
# than its own log has timeout lines. Any other difference fails. The verdict of every pair is kept in
# $GATE_OUT/proofs/bodies.txt.
#
#   usage: [GATE_SWITCH=on] GATE_WORKTREE=<built linked worktree> GATE_OUT=<empty dir> GATE_S0_DIR=... GATE_CONFIG_SRC=... \
#          GATE_RUN_LOCK=... GATE_PG_ENV=... \
#          scripts/live_mode_gate/gate.sh <scratch database> [conservative-arm base-arm equity-arm]
#   GATE_WORKTREE is required and may not be the main checkout: the gate writes its run config into that
#   tree and deletes the logs its runs leave there.
#   GATE_EVALUATE_ONLY=1 makes no run: the checks are evaluated on the runs already under $GATE_OUT (how
#   the gate's own failing examples are made: copy a finished gate, plant a difference, evaluate).
#   exit 0 only when every check passed and every driver finished; the checks are printed and kept in
#   $GATE_OUT/GATE.txt. A gate that makes runs refuses an output folder that already holds runs.
set -u
HERE="${0:A:h}"
WT="${GATE_WORKTREE:?set GATE_WORKTREE to the built linked worktree}"; export GATE_WORKTREE="$WT"
DB="${1:?usage: gate.sh <scratch database>}"; ARM_C="${2:-TPL}"; ARM_B="${3:-ABSENT}"; ARM_E="${4:-ABSENT}"
[ "$(cd "$WT" 2>/dev/null && git rev-parse --is-inside-work-tree 2>/dev/null)" = true ] && [ "$(cd "$WT" && git rev-parse --absolute-git-dir 2>/dev/null)" != "$(cd "$WT" && cd "$(git rev-parse --git-common-dir 2>/dev/null)" 2>/dev/null && pwd -P)" ] || { echo "REFUSING worktree $WT: it is the main checkout (or not a git worktree); the gate rewrites config/ and deletes logs/ in its worktree"; exit 2 }
OUT="${GATE_OUT:?set GATE_OUT}"
SW="${GATE_SWITCH:-off}"; case "$SW" in on|off) ;; *) echo "REFUSING GATE_SWITCH $SW"; exit 2;; esac; export GATE_SWITCH="$SW"
case "$DB" in *scratch*) ;; *) echo "REFUSING database $DB"; exit 2;; esac
set -a; . "${GATE_PG_ENV:?set GATE_PG_ENV}" > /dev/null 2>&1; set +a; export PGPASSWORD="$SCRATCH_PGPASSWORD"
PSQL="${GATE_PSQL:-/opt/homebrew/opt/postgresql@16/bin/psql}"
q() { $PSQL -h 127.0.0.1 -p 5433 -U postgres -d "$DB" -At -v ON_ERROR_STOP=1 -c "$1" }
# A gate that makes runs starts from an empty output folder: runs left by an earlier gate would be evaluated as its own.
if [ -z "${GATE_EVALUATE_ONLY:-}" ]; then leftover=("$OUT"/runs/*(N)); [ ${#leftover} -eq 0 ] || { echo "REFUSING output folder $OUT: runs/ is not empty (use an empty folder, or GATE_EVALUATE_ONLY=1 to evaluate the runs already there)"; exit 2 }; fi
mkdir -p "$OUT/runs" "$OUT/proofs"
R="$OUT/runs"; G="$OUT/GATE.txt"; : > "$G"; fails=0
say() { echo "$1" | tee -a "$G" }
check() { if eval "$2"; then say "PASS  $1"; else say "FAIL  $1"; fails=$((fails+1)); fi }
tag="${DB#new_algo_data_scratch_stage3_}"
dir() { echo "$R/live_${1}gate_${tag}_${2}_${3}_${4}${5:+_$5}" }   # <pf prefix> <arm> <unit> <mode> [label]
same() { python3 "$HERE/compare_csv_dirs.py" --tol 0 "$1" "$2" > "$OUT/proofs/$3.txt" 2>&1 }
rcs() { grep -a " rc=" "$1/progress.txt" | sed -E 's/.*run-date ([0-9-]+) host-date ([0-9-]+) rc=([0-9]+).*/\1@\2=\3/' | tr '\n' ' ' }
bodies_same() {   # <run A> <run B> <date ...>: the report bodies of the dates, under the chart rule
  local a="$1" b="$2" d v rc fa fb; shift 2
  for d in "$@"; do
    fa=("$a"/email/$d/*.html(N)); fb=("$b"/email/$d/*.html(N))
    [ ${#fa} -eq 1 ] && [ ${#fb} -eq 1 ] || { echo "   body of $d is missing" >> "$G"; return 1 }
    v=$(python3 "$HERE/compare_bodies.py" "$fa[1]" "$a/logs/$d.log.gz" "$fb[1]" "$b/logs/$d.log.gz"); rc=$?
    echo "${a:t} against ${b:t} $d: $v" >> "$OUT/proofs/bodies.txt"
    [ $rc -eq 0 ] || { echo "   body of $d: $v" >> "$G"; return 1 }
  done
}
log() { gzip -dc "$1"/logs/"$2".log.gz }

clang -dynamiclib -O1 -o "$OUT/hostclock_shim.dylib" "$HERE/hostclock_shim.c" || { say "FAIL  the clock shim did not build"; exit 1 }
# A driver that stops (a refusal, a failed restore) fails the gate by itself: no check may pass on runs it did not make.
run() {
  [ -n "${GATE_EVALUATE_ONLY:-}" ] && return 0
  local drv="$OUT/proofs/drv_${PF}_${MODE}_$2${3:+_$3}.log" rc
  /bin/zsh "$HERE/chain.sh" "$DB" "$@" > "$drv" 2>&1; rc=$?
  [ $rc -eq 0 ] || { say "FAIL  the driver of $PF $MODE $2 stopped with exit $rc: $(tail -1 "$drv")"; fails=$((fails+1)) }
}

say "live mode gate: database $DB, tree $(git -C "$WT" rev-parse HEAD) ($(git -C "$WT" status --porcelain --untracked-files=no | wc -l | tr -d ' ') modified tracked file(s)), gate script sha256 $(shasum -a 256 "$0" | cut -c1-16), GATE_SWITCH $SW, ${GATE_EVALUATE_ONLY:+EVALUATE ONLY, }$(date -u +%FT%TZ)"

# ---- the feed hole (see the header) ----------------------------------------------------------------------
HOLE_B="symbol = '6M.v.0' AND time::date BETWEEN '2026-04-21' AND '2026-04-25'"
HOLE_R="symbol = '6M.v.0' AND ts_event::date BETWEEN '2026-04-21' AND '2026-04-25'"
if [ -z "${GATE_EVALUATE_ONLY:-}" ]; then
  { q "CREATE SCHEMA IF NOT EXISTS gate_hole; DROP TABLE IF EXISTS gate_hole.b, gate_hole.r"
    q "CREATE TABLE gate_hole.b AS SELECT * FROM futures_data.ohlcv_1d WHERE $HOLE_B"
    q "CREATE TABLE gate_hole.r AS SELECT * FROM futures_data.ohlcv_1d_raw WHERE $HOLE_R"
    q "DELETE FROM futures_data.ohlcv_1d WHERE $HOLE_B"; q "DELETE FROM futures_data.ohlcv_1d_raw WHERE $HOLE_R"
    echo "6M.v.0 bars 2026-04-21 .. 04-25 moved aside: $(q "select count(*) from gate_hole.b") / $(q "select count(*) from gate_hole.r"); left $(q "select count(*) from futures_data.ohlcv_1d where $HOLE_B") / $(q "select count(*) from futures_data.ohlcv_1d_raw where $HOLE_R"); last bar before: $(q "select max(time)::date from futures_data.ohlcv_1d where symbol = '6M.v.0' and time < '2026-04-26'")"
  } > "$OUT/proofs/feed_hole.txt" 2>&1 || { say "FAIL  the feed hole could not be made: $(tail -1 "$OUT/proofs/feed_hole.txt")"; exit 1 }
fi
put_hole_back() {
  [ -n "${GATE_EVALUATE_ONLY:-}" ] && return 0
  { q "INSERT INTO futures_data.ohlcv_1d SELECT * FROM gate_hole.b"; q "INSERT INTO futures_data.ohlcv_1d_raw SELECT * FROM gate_hole.r"
    echo "put back: $(q "select count(*) from futures_data.ohlcv_1d where $HOLE_B") / $(q "select count(*) from futures_data.ohlcv_1d_raw where $HOLE_R")"
    q "DROP SCHEMA gate_hole CASCADE"; } >> "$OUT/proofs/feed_hole.txt" 2>&1
}

# ---- 1. CONSERVATIVE -------------------------------------------------------------------------------
export RANGE_START=2026-04-24 RANGE_END=2026-05-03
MODE=M3 PF=conservative run $ARM_C chain;        C3=$(dir "" $ARM_C chain M3)
MODE=M2 PF=conservative run $ARM_C prodrestart;  C2=$(dir "" $ARM_C prodrestart M2)
check "CONSERVATIVE past-date chain: ten dates, every exit 0" '[ "$(grep -ac " rc=0 " "$C3/progress.txt")" = 10 ]'
check "CONSERVATIVE production mode: 2026-04-24 and 04-25 exit 0" '[[ "$(rcs "$C2")" == "2026-04-24@2026-04-24=0 2026-04-25@2026-04-25=0 "* ]]'
check "CONSERVATIVE production mode: 2026-04-26 is REFUSED (exit 1) on a held feed hole" '[[ "$(rcs "$C2")" == *"2026-04-26@2026-04-26=1 "* ]] && log "$C2" 2026-04-26 | grep -aq "FEED_HOLE_REFUSAL held symbol .* Refusing to run live"'
check "CONSERVATIVE production mode: every later date is refused by the missed-run guard" '[ "$(for d in 27 28 29 30; do log "$C2" 2026-04-$d | grep -ac "A run was missed"; done | tr -d "\n")$(for d in 01 02 03; do log "$C2" 2026-05-$d | grep -ac "A run was missed"; done | tr -d "\n")" = 1111111 ] && [[ "$(rcs "$C2")" == *"2026-05-03@2026-05-03=1 "* ]]'
check "CONSERVATIVE production mode: the refused runs stored nothing after 2026-04-25" '[ "$(python3 -c "
import csv,sys
print(max(r[\"date\"][:10] for r in csv.DictReader(open(sys.argv[1]))))" "$C2/snap_before_restart/trading_live_results.csv")" = 2026-04-25 ] && [ "$(python3 -c "
import csv,sys
print(max(r[\"date\"][:10] for r in csv.DictReader(open(sys.argv[1]))))" "$C2/snap_before_restart/trading_positions.csv")" = 2026-04-25 ]'
check "CONSERVATIVE the report bodies of the two complete days are identical in both modes" 'bodies_same "$C3" "$C2" 2026-04-24 2026-04-25'
check "CONSERVATIVE the restart: eight missed dates run in order on a later day, every exit 0" '[ "$(grep -a "host-date 2026-05-04 rc=0" "$C2/progress.txt" | wc -l | tr -d " ")" = 8 ]'
check "CONSERVATIVE after the restart every stored table is identical to the past-date chain" 'same "$C3/snap" "$C2/snap" conservative_restart_vs_past_date'

# ---- 4. the re-run repair (on the conservative book, against the chain of 1) --------------------------
LATE="${GATE_LATE_DATE:-$([ "$SW" = on ] && echo 2026-04-28 || echo 2026-04-27)}"
LATE_DATE=$LATE MODE=M3 PF=conservative run $ARM_C latebars;  LB=$(dir "" $ARM_C latebars M3)
check "RE-RUN REPAIR the late bars ($LATE) book a held roll the first pass legged late, and the bar tables are whole again" 'python3 "$HERE/late_roll_check.py" "$C3/snap" "$LB/snap_late_before_repair" "$LATE" "$LB/bars_proof.txt" > "$OUT/proofs/late_roll.txt" 2>&1'
check "RE-RUN REPAIR late bars, then the dates re-run in order: every stored table identical to the clean chain" 'same "$C3/snap" "$LB/snap" rerun_repair_vs_clean'

# ---- 2. BASE ---------------------------------------------------------------------------------------
export RANGE_START=2026-04-24 RANGE_END=2026-04-26
MODE=M3 PF=base run $ARM_B chain;        B3=$(dir base_ $ARM_B chain M3)
MODE=M2 PF=base run $ARM_B prodrestart;  B2=$(dir base_ $ARM_B prodrestart M2)
check "BASE production mode: two complete days, then 2026-04-26 REFUSED" '[[ "$(rcs "$B2")" == "2026-04-24@2026-04-24=0 2026-04-25@2026-04-25=0 2026-04-26@2026-04-26=1 "* ]] && log "$B2" 2026-04-26 | grep -aq "FEED_HOLE_REFUSAL held symbol .* Refusing to run live"'
check "BASE the report bodies of the two complete days are identical in both modes" 'bodies_same "$B3" "$B2" 2026-04-24 2026-04-25'
check "BASE after the restart every stored table is identical to the past-date chain" 'same "$B3/snap" "$B2/snap" base_restart_vs_past_date'

# ---- 3. EQUITY -------------------------------------------------------------------------------------
export RANGE_START=2026-06-15 RANGE_END=2026-06-19
MODE=M3 PF=equity_mr run $ARM_E chain;      E3=$(dir equity_ $ARM_E chain M3)
MODE=M2 PF=equity_mr run $ARM_E chain;      E2=$(dir equity_ $ARM_E chain M2)
check "EQUITY five dates, every exit 0 in both modes" '[ "$(grep -ac " rc=0 " "$E3/progress.txt")$(grep -ac " rc=0 " "$E2/progress.txt")" = 55 ]'
check "EQUITY every stored table identical in both modes" 'same "$E3/snap" "$E2/snap" equity_production_vs_past_date'
check "EQUITY the report bodies of the five dates are identical in both modes" 'bodies_same "$E3" "$E2" 2026-06-15 2026-06-16 2026-06-17 2026-06-18 2026-06-19'
REFUSED_DATE=2026-06-24 MODE=M3 PF=equity_mr run $ARM_E refused;  ER=$(dir equity_ $ARM_E refused M3)
check "EQUITY a run whose previous day never ran is refused (exit 1)" 'grep -aq ": 1$" "$ER/exit_code.txt" && log "$ER" 2026-06-24 | grep -aq "A run was missed"'
check "EQUITY the refused run stored nothing, the run-metadata row included" 'same "$ER/snap_before" "$ER/snap" equity_refused_run_stored_nothing'

# ---- 5. the per-contract shapes (GATE_SWITCH=on only; rolls step 4, ROLLS_STEP1_PLAN section 7) ------------
if [ "$SW" = on ]; then
  export RANGE_START=2026-04-24 RANGE_END=2026-05-03
  [ -z "${GATE_EVALUATE_ONLY:-}" ] && $PSQL -h 127.0.0.1 -p 5433 -U postgres -d "$DB" -q -c "\\copy (select root, instrument_id, date, close from futures_data.contract_ohlcv_1d where date between '2026-04-01' and '2026-05-03' order by 1, 2, 3) to '$OUT/proofs/contract_closes.csv' with (format csv, header)"
  check "SWITCH ON a held roll: every ROLL pair of the past-date chain is priced at its two contracts' own closes of one bar (the contract table), each roll stored once" 'python3 "$HERE/roll_legs_check.py" "$C3/snap/trading_executions.csv" "$OUT/proofs/contract_closes.csv" > "$OUT/proofs/held_roll_legs.txt" 2>&1'
  # ZS rolls ZSK6 (42009683) -> ZSN6 (194904) on 2026-04-23; afterwards the vendor naming ZSK6 is a switch back.
  STRAY_SHOW="SELECT string_agg(ts_event::date||'='||instrument_id, ' ' ORDER BY ts_event) FROM futures_data.ohlcv_1d_raw WHERE symbol = 'ZS.v.0' AND ts_event BETWEEN '2026-04-29' AND '2026-05-02'"
  PREP_SQL="UPDATE futures_data.ohlcv_1d_raw SET instrument_id = '42009683' WHERE symbol = 'ZS.v.0' AND ts_event = '2026-04-30 00:00:00+00' AND instrument_id = '194904'" \
  UNDO_SQL="UPDATE futures_data.ohlcv_1d_raw SET instrument_id = '194904' WHERE symbol = 'ZS.v.0' AND ts_event = '2026-04-30 00:00:00+00' AND instrument_id = '42009683'" \
  PREP_SHOW="$STRAY_SHOW" MODE=M3 PF=conservative run $ARM_C chain stray;  CS=$(dir "" $ARM_C chain M3 stray)
  check "SWITCH ON an ignored one-bar backward stray (the vendor names ZSK6 on 04-30): every stored table identical to the past-date chain" 'grep -aq "after:  .*2026-04-30=42009683" "$CS/prep_proof.txt" && same "$C3/snap" "$CS/snap" switch_on_stray_vs_clean'
  PREP_SQL="UPDATE futures_data.ohlcv_1d_raw SET instrument_id = '42009683' WHERE symbol = 'ZS.v.0' AND ts_event IN ('2026-04-30 00:00:00+00', '2026-05-01 00:00:00+00') AND instrument_id = '194904'" \
  UNDO_SQL="UPDATE futures_data.ohlcv_1d_raw SET instrument_id = '194904' WHERE symbol = 'ZS.v.0' AND ts_event IN ('2026-04-30 00:00:00+00', '2026-05-01 00:00:00+00') AND instrument_id = '42009683'" \
  PREP_SHOW="$STRAY_SHOW" MODE=M3 PF=conservative run $ARM_C chain backward;  CW=$(dir "" $ARM_C chain M3 backward)
  check "SWITCH ON an ignored backward switch (the vendor names ZSK6 on 04-30 and 05-01): every stored table identical to the past-date chain" 'grep -aq "after:  .*2026-04-30=42009683 2026-05-01=42009683" "$CW/prep_proof.txt" && same "$C3/snap" "$CW/snap" switch_on_backward_vs_clean'
  # R4A fix round 1 (audit B B-1): ZS rolls on the crossover of 04-23. That day's per-contract rows (every root) arrive
  # late, the vendor's bars are in, for the runs of 04-24 and 04-25; then the rows come and the dates re-run in order.
  LATE_DATE=2026-04-23 LATE_TABLES=contract MODE=M3 PF=conservative run $ARM_C latebars contract;  LC=$(dir "" $ARM_C latebars M3 contract)
  check "SWITCH ON a crossover day's per-contract bars (04-23) arrive late with the vendor's bars in: the first pass reads the vendor's 04-23 bar and books no ZS pair (the 04-24 run sells ZS flat, so the 04-25 run sees the roll with a flat book), the rows are put back, and the dates re-run in order end with every stored table identical to the past-date chain" 'python3 "$HERE/late_roll_check.py" "$C3/snap" "$LC/snap_late_before_repair" 2026-04-23 "$LC/bars_proof.txt" > "$OUT/proofs/late_contract_rows.txt" 2>&1 && grep -aq "the vendor.s bars stay: ohlcv_1d [1-9]" "$LC/bars_proof.txt" && same "$C3/snap" "$LC/snap" switch_on_late_contract_rows_vs_clean'
  INTERRUPT_DATE=2026-04-29 INTERRUPT_KIND=kill MODE=M3 PF=conservative run $ARM_C interrupt kill;  IK=$(dir "" $ARM_C interrupt M3 kill)
  check "SWITCH ON a run killed (SIGKILL) while it writes, after storing its ROLL legs, then run again: every stored table identical to the past-date chain" 'grep -aq "parked=1" "$IK/interrupt_proof.txt" && gzip -dc "$IK/logs/2026-04-29_killed.log.gz" | grep -aq "ROLL_LEG store" && same "$C3/snap" "$IK/snap" switch_on_kill_vs_clean'
  INTERRUPT_DATE=2026-04-29 INTERRUPT_KIND=failwrite MODE=M3 PF=conservative run $ARM_C interrupt failwrite;  IF=$(dir "" $ARM_C interrupt M3 failwrite)
  check "SWITCH ON a run whose ROLL legs fail to store refuses (exit 1) and its replay ends identical to the past-date chain" 'grep -aq ": 1$" "$IF/exit_code.txt" && log "$IF" 2026-04-29_failed_write | grep -aq "ROLL_LEG STOP: the day.s executions could not be stored" && same "$C3/snap" "$IF/snap" switch_on_failwrite_vs_clean'
fi

put_hole_back
say "$( [ $fails -eq 0 ] && echo "LIVE MODE GATE PASSED" || echo "LIVE MODE GATE FAILED: $fails check(s)" )"
exit $(( fails > 0 ))
