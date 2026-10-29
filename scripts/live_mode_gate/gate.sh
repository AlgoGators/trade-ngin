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
#      every later one are re-run in date order: every stored table identical to the reference chain.
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
#   usage: GATE_WORKTREE=<built linked worktree> GATE_OUT=<empty dir> GATE_S0_DIR=... GATE_CONFIG_SRC=... \
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
log() { gzip -dc "$1"/logs/"$2".log.gz }

clang -dynamiclib -O1 -o "$OUT/hostclock_shim.dylib" "$HERE/hostclock_shim.c" || { say "FAIL  the clock shim did not build"; exit 1 }
# A driver that stops (a refusal, a failed restore) fails the gate by itself: no check may pass on runs it did not make.
run() {
  [ -n "${GATE_EVALUATE_ONLY:-}" ] && return 0
  local drv="$OUT/proofs/drv_${PF}_${MODE}_$2${3:+_$3}.log" rc
  /bin/zsh "$HERE/chain.sh" "$DB" "$@" > "$drv" 2>&1; rc=$?
  [ $rc -eq 0 ] || { say "FAIL  the driver of $PF $MODE $2 stopped with exit $rc: $(tail -1 "$drv")"; fails=$((fails+1)) }
}

say "live mode gate: database $DB, tree $(git -C "$WT" rev-parse HEAD) ($(git -C "$WT" status --porcelain --untracked-files=no | wc -l | tr -d ' ') modified tracked file(s)), gate script sha256 $(shasum -a 256 "$0" | cut -c1-16), ${GATE_EVALUATE_ONLY:+EVALUATE ONLY, }$(date -u +%FT%TZ)"

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
check "CONSERVATIVE the restart: eight missed dates run in order on a later day, every exit 0" '[ "$(grep -a "host-date 2026-05-04 rc=0" "$C2/progress.txt" | wc -l | tr -d " ")" = 8 ]'
check "CONSERVATIVE after the restart every stored table is identical to the past-date chain" 'same "$C3/snap" "$C2/snap" conservative_restart_vs_past_date'

# ---- 4. the re-run repair (on the conservative book, against the chain of 1) --------------------------
LATE_DATE=2026-04-29 MODE=M3 PF=conservative run $ARM_C latebars;  LB=$(dir "" $ARM_C latebars M3)
check "RE-RUN REPAIR the bar tables are whole again after the late-bar run" 'grep -aq "put back: now dated 2026-04-29: 36 / " "$LB/bars_proof.txt"'
check "RE-RUN REPAIR late bars, then the dates re-run in order: every stored table identical to the clean chain" 'same "$C3/snap" "$LB/snap" rerun_repair_vs_clean'

# ---- 2. BASE ---------------------------------------------------------------------------------------
export RANGE_START=2026-04-24 RANGE_END=2026-04-26
MODE=M3 PF=base run $ARM_B chain;        B3=$(dir base_ $ARM_B chain M3)
MODE=M2 PF=base run $ARM_B prodrestart;  B2=$(dir base_ $ARM_B prodrestart M2)
check "BASE production mode: two complete days, then 2026-04-26 REFUSED" '[[ "$(rcs "$B2")" == "2026-04-24@2026-04-24=0 2026-04-25@2026-04-25=0 2026-04-26@2026-04-26=1 "* ]] && log "$B2" 2026-04-26 | grep -aq "FEED_HOLE_REFUSAL held symbol .* Refusing to run live"'
check "BASE after the restart every stored table is identical to the past-date chain" 'same "$B3/snap" "$B2/snap" base_restart_vs_past_date'

# ---- 3. EQUITY -------------------------------------------------------------------------------------
export RANGE_START=2026-06-15 RANGE_END=2026-06-19
MODE=M3 PF=equity_mr run $ARM_E chain;      E3=$(dir equity_ $ARM_E chain M3)
MODE=M2 PF=equity_mr run $ARM_E chain;      E2=$(dir equity_ $ARM_E chain M2)
check "EQUITY five dates, every exit 0 in both modes" '[ "$(grep -ac " rc=0 " "$E3/progress.txt")$(grep -ac " rc=0 " "$E2/progress.txt")" = 55 ]'
check "EQUITY every stored table identical in both modes" 'same "$E3/snap" "$E2/snap" equity_production_vs_past_date'
REFUSED_DATE=2026-06-24 MODE=M3 PF=equity_mr run $ARM_E refused;  ER=$(dir equity_ $ARM_E refused M3)
check "EQUITY a run whose previous day never ran is refused (exit 1)" 'grep -aq ": 1$" "$ER/exit_code.txt" && log "$ER" 2026-06-24 | grep -aq "A run was missed"'
check "EQUITY the refused run stored nothing, the run-metadata row included" 'same "$ER/snap_before" "$ER/snap" equity_refused_run_stored_nothing'

say "$( [ $fails -eq 0 ] && echo "LIVE MODE GATE PASSED" || echo "LIVE MODE GATE FAILED: $fails check(s)" )"
exit $(( fails > 0 ))
