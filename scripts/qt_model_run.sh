#!/usr/bin/env bash
# QT model run BY HAND (docs/design/qt-contract.md section 1): runs the conservative runner's
# model run for each portfolio config dir given, for today's date in America/New_York (or
# QT_RUN_DATE), inside the qt-engine (engine-rpc) container. The scheduled runs are the catch-up
# scheduler's (contract C6; scripts/qt_catchup.sh, desk-service.md), which runs exactly this
# command line under the same lock but first checks the day is due: this script does not, so do
# not use it on a day that already has a book unless you mean to re-run it. Each portfolio takes the same lock the desk service
# takes for its commands (QT_LOCK_DIR/<portfolio_id>.lock), so a model run never overlaps a desk
# run of the same portfolio.
#
#   docker exec engine-rpc /app/scripts/qt_model_run.sh qt_conservative qt_conservative_model
#
# The model run never e-mails (ruling 15: publish does). Exit status: the worst of the runs.
set -uo pipefail

BINARY="${QT_ENGINE_BINARY:-/app/build/bin/Release/live_portfolio_conservative}"
CONFIG_DIR="${TRADING_CONFIG_DIR:-/app/config}"
LOCK_DIR="${QT_LOCK_DIR:-/tmp/qt-locks}"
DATE="${QT_RUN_DATE:-$(TZ=America/New_York date +%Y-%m-%d)}"

log() { printf '%s [qt-model-run] %s\n' "$(date -Is)" "$*"; }

if [ "$#" -eq 0 ]; then
    log "usage: $0 <portfolio config dir>..."
    exit 2
fi
mkdir -p "$LOCK_DIR"
cd "${QT_ENGINE_CWD:-/app}" || { log "FATAL: cannot cd"; exit 1; }

worst=0
for dir in "$@"; do
    portfolio_json="$CONFIG_DIR/portfolios/$dir/portfolio.json"
    pid="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["portfolio_id"])' \
        "$portfolio_json" 2>/dev/null)"
    if [ -z "$pid" ]; then
        log "FAILED $dir: cannot read portfolio_id from $portfolio_json"
        worst=1
        continue
    fi
    log "starting model run $pid ($dir) for $DATE"
    flock "$LOCK_DIR/$pid.lock" "$BINARY" --portfolio-config "$dir" --date "$DATE" \
        > "/tmp/qt-model-run-$pid.log" 2>&1
    rc=$?
    tail -n 5 "/tmp/qt-model-run-$pid.log" | cut -c1-300
    if [ "$rc" -eq 0 ]; then
        log "completed $pid for $DATE"
    else
        log "FAILED $pid for $DATE (exit $rc); full log /tmp/qt-model-run-$pid.log"
        [ "$rc" -gt "$worst" ] && worst=$rc
    fi
done
exit "$worst"
