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

# The optional manifest is a literal two-column TSV: absolute runner path, then
# the validated portfolio config key. Rows run in file order. Blank lines and
# lines beginning with # are ignored. When it is absent, retain the historical
# single conservative-book default.
BINARY="${LIVE_BINARY:-/app/build/bin/Release/live_portfolio_conservative}"
PORTFOLIO="${LIVE_PORTFOLIO:-conservative}"
BOOK_MANIFEST="${LIVE_BOOK_MANIFEST:-}"
APP_ROOT="${APP_ROOT:-/app}"

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
    log "skipping: another cycle already holds $LOCK_DIR"
    exit 0
fi
trap 'rmdir "$LOCK_DIR" 2>/dev/null || true' EXIT

# --- manifest/preflight ------------------------------------------------------
declare -a RUNNERS=()
declare -a PORTFOLIOS=()
declare -A SEEN_PORTFOLIOS=()

valid_portfolio_key() {
    local key="$1"
    [[ ${#key} -ge 1 && ${#key} -le 64 && "$key" =~ ^[a-z0-9][a-z0-9_-]*$ ]]
}

append_book() {
    local runner="$1"
    local portfolio="$2"
    local source="$3"

    if [[ "$runner" != /* || ! -x "$runner" ]]; then
        log "FATAL: $source runner must be an absolute executable path: $runner"
        return 127
    fi
    if ! valid_portfolio_key "$portfolio"; then
        log "FATAL: $source has invalid portfolio key: $portfolio"
        return 64
    fi
    if [[ -n "${SEEN_PORTFOLIOS[$portfolio]:-}" ]]; then
        log "FATAL: $source repeats portfolio key: $portfolio"
        return 64
    fi
    SEEN_PORTFOLIOS[$portfolio]=1
    RUNNERS+=("$runner")
    PORTFOLIOS+=("$portfolio")
}

if [[ -n "$BOOK_MANIFEST" ]]; then
    if [[ ! -f "$BOOK_MANIFEST" ]]; then
        log "FATAL: live-book manifest not found: $BOOK_MANIFEST"
        exit 66
    fi
    line_number=0
    while IFS=$'\t' read -r runner portfolio extra || [[ -n "${runner:-}${portfolio:-}${extra:-}" ]]; do
        line_number=$((line_number + 1))
        [[ -z "${runner:-}${portfolio:-}${extra:-}" ]] && continue
        [[ "${runner:-}" == \#* ]] && continue
        if [[ -z "${runner:-}" || -z "${portfolio:-}" || -n "${extra:-}" ]]; then
            log "FATAL: $BOOK_MANIFEST:$line_number must contain exactly runner<TAB>portfolio"
            exit 64
        fi
        append_book "$runner" "$portfolio" "$BOOK_MANIFEST:$line_number" || exit $?
    done < "$BOOK_MANIFEST"
    if [[ ${#RUNNERS[@]} -eq 0 ]]; then
        log "FATAL: live-book manifest contains no runnable books: $BOOK_MANIFEST"
        exit 64
    fi
else
    append_book "$BINARY" "$PORTFOLIO" "single-book fallback" || exit $?
fi

# --- working directory -------------------------------------------------------
# cron starts jobs from $HOME, but both live binaries resolve their config
# ("./config", see live_portfolio*.cpp ConfigLoader::load) and log directory
# relative to the current directory. Without this cd every scheduled run dies
# on config-not-found -- the same class of silent failure this script exists
# to eliminate.
cd "$APP_ROOT" || { log "FATAL: cannot cd $APP_ROOT"; exit 1; }

# --- run ---------------------------------------------------------------------
DATE="$(date +%Y-%m-%d)"
log "starting ${#RUNNERS[@]} portfolio(s) for $DATE"

for index in "${!RUNNERS[@]}"; do
    runner="${RUNNERS[$index]}"
    portfolio="${PORTFOLIOS[$index]}"
    run_args=("$DATE" --portfolio "$portfolio")
    if [ "${QT_EMAIL_DELIVERY_ENABLED:-}" = "true" ]; then
        run_args+=(--send-email)
    fi

    log "starting portfolio $portfolio with $runner for $DATE"
    "$runner" "${run_args[@]}"
    rc=$?
    if [[ "$rc" -ne 0 ]]; then
        log "FAILED portfolio $portfolio for $DATE (exit $rc); stopping cycle"
        exit "$rc"
    fi
    log "completed portfolio $portfolio for $DATE"
done

log "completed all ${#RUNNERS[@]} portfolio(s) successfully for $DATE"
exit 0
