#!/usr/bin/env bash
# Daily live-portfolio run, invoked by cron inside the container.
#
# Replaces scripts/run_live_trend.sh, which invoked
# /app/build/bin/Release/live_trend -- a binary this project does not build. The
# CMake targets are `live_portfolio` and `live_portfolio_conservative`; there has
# never been a `live_trend` target. Every scheduled run therefore failed
# instantly with "not found", into a log file inside the container that nothing
# surfaced.
#
# Everything below is written so that a failure is loud rather than silent:
# output goes to container stdout (visible in `docker logs`), every line is
# timestamped, and the exit code is both logged and propagated.

set -uo pipefail  # deliberately NOT -e: we capture the binary's exit code ourselves

# Which portfolio to run. Defaults to the conservative binary because that is
# what the live database shows was actually being run -- CONSERVATIVE_PORTFOLIO
# has data through 2026-05-03, while BASE_PORTFOLIO stops in December 2025.
# Override with -e LIVE_BINARY=... to run a different one.
BINARY="${LIVE_BINARY:-/app/build/bin/Release/live_portfolio_conservative}"

CRON_ENV="${CRON_ENV:-/app/.cron_env}"
LOCK_DIR="${LOCK_DIR:-/tmp/live_portfolio.lock}"

log() { printf '%s [live-portfolio] %s\n' "$(date -Is)" "$*"; }

# --- environment -------------------------------------------------------------
# cron builds a fresh minimal environment, so the credentials passed into the
# container are not visible here. docker-entrypoint.sh snapshots them at startup.
if [ -f "$CRON_ENV" ]; then
    # shellcheck disable=SC1090
    . "$CRON_ENV"
    log "sourced environment from $CRON_ENV"
else
    log "WARNING: $CRON_ENV missing -- database credentials are probably unavailable."
    log "         (is the container running scripts/docker-entrypoint.sh?)"
fi

# --- weekday guard -----------------------------------------------------------
# There is no market data for Saturday or Sunday, so a weekend run can only fail
# or no-op. The cron schedule also restricts to Mon-Fri; this is the second belt.
DOW="$(date +%u)"  # 1=Monday .. 7=Sunday
if [ "$DOW" -ge 6 ]; then
    log "skipping: weekend (day-of-week $DOW)"
    exit 0
fi

# --- single instance ---------------------------------------------------------
# mkdir is atomic, so this is a safe lock without extra tooling. A catch-up run
# started by hand should not collide with the scheduled one.
if ! mkdir "$LOCK_DIR" 2>/dev/null; then
    log "skipping: another run already holds $LOCK_DIR"
    exit 0
fi
trap 'rmdir "$LOCK_DIR" 2>/dev/null || true' EXIT

# --- preflight ---------------------------------------------------------------
if [ ! -x "$BINARY" ]; then
    log "FATAL: binary not found or not executable: $BINARY"
    log "       built targets are live_portfolio and live_portfolio_conservative"
    exit 127
fi

# --- run ---------------------------------------------------------------------
DATE="$(date +%Y-%m-%d)"
log "starting $BINARY for $DATE"

"$BINARY" "$DATE" --send-email
rc=$?

if [ "$rc" -eq 0 ]; then
    log "completed successfully for $DATE"
else
    log "FAILED for $DATE (exit $rc)"
fi

# Propagate the real exit code so cron -- and anything reading container logs --
# sees the failure.
exit "$rc"
