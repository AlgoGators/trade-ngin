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

# --- no weekday guard -----------------------------------------------------------
# The futures runner runs every calendar day (HD 2026-09-17). There used to be a weekend exit
# here, with a comment that there is no market data on Saturday or Sunday: false for futures.
# Sunday is a Globex session (26 symbols print), and the Saturday run is the one that books and
# trades the Friday session. The runner itself carries the whole book on a day no symbol printed.

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

# --- working directory -------------------------------------------------------
# cron starts jobs from $HOME, but both live binaries resolve their config
# ("./config", see live_portfolio*.cpp ConfigLoader::load) and log directory
# relative to the current directory. Without this cd every scheduled run dies
# on config-not-found -- the same class of silent failure this script exists
# to eliminate.
cd /app || { log "FATAL: cannot cd /app"; exit 1; }

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
