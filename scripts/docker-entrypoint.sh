#!/usr/bin/env bash
# Container entrypoint: snapshot the environment for cron, then run cron.
#
# WHY THIS EXISTS
# ---------------
# cron does not inherit the environment of the process that starts it. It builds
# a fresh, minimal environment for every job (essentially HOME, LOGNAME, PATH,
# SHELL). So anything passed in via `docker run -e` / compose `environment:` --
# including TRADING_CONFIG_PATH and TRADING_ENCRYPTION_KEY, which is how
# CredentialStore locates and decrypts the DB credentials -- is invisible to the
# scheduled job, even though it is plainly visible to `docker exec`.
#
# That asymmetry is exactly what makes this failure mode so easy to miss: run the
# job by hand and it works; let cron run it and it cannot reach the database.
#
# So: capture the relevant variables at container start into a file the cron
# script sources. Written with printf %q so values containing spaces, quotes or
# newlines survive the round trip.

set -euo pipefail

CRON_ENV=/app/.cron_env

: > "$CRON_ENV"
chmod 600 "$CRON_ENV"

# Only export what the job legitimately needs -- not the whole environment.
while IFS='=' read -r -d '' name value; do
    case "$name" in
        TRADING_* | DB_* | PG* | TZ | LD_LIBRARY_PATH)
            printf 'export %s=%q\n' "$name" "$value" >> "$CRON_ENV"
            ;;
    esac
done < <(env -0)

echo "$(date -Is) [entrypoint] captured $(wc -l < "$CRON_ENV") env var(s) for cron"

# Warn loudly if the credentials the job needs are absent, rather than letting
# the first scheduled run fail silently at 09:30.
if ! grep -q '^export TRADING_ENCRYPTION_KEY=' "$CRON_ENV" \
   && [ ! -f "${TRADING_CONFIG_PATH:-/app/config_template}.key" ]; then
    echo "$(date -Is) [entrypoint] WARNING: neither TRADING_ENCRYPTION_KEY nor a" \
         "key file is present -- the scheduled run will not be able to decrypt" \
         "database credentials." >&2
fi

exec cron -f
