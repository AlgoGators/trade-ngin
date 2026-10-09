#!/usr/bin/env sh
# QT catch-up scheduler, by hand or from the host watchdog cron (docs/design/desk-service.md,
# "Catch-up scheduler"; contract C6). The scheduler itself runs inside the rpc server
# (background task desk.catchup); this runs the same code once, in the engine-rpc container:
#
#   docker exec engine-rpc /app/scripts/qt_catchup.sh watchdog   # a pass only if none ran lately
#   docker exec engine-rpc /app/scripts/qt_catchup.sh run-once   # one pass now, any hour
#
# Both take the scheduler's lock (QT_LOCK_DIR/qt-catchup.lock), so they never overlap the
# in-process pass, and each model run takes QT_LOCK_DIR/<portfolio_id>.lock like desk jobs.
# Exit 0, or 1 when a pass raised an alert, 2 on bad usage or configuration.
set -eu
cd /opt/rpc/app
exec /opt/rpc/venv/bin/python -m algogators_rpc.services.desk.catchup "$@"
