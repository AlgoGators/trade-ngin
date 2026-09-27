#!/usr/bin/env bash

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN_SCRIPT="$REPO_ROOT/scripts/run_live_portfolio.sh"
ENTRYPOINT="$REPO_ROOT/scripts/docker-entrypoint.sh"
TEST_ROOT="$(mktemp -d)"
trap 'rm -rf -- "$TEST_ROOT"' EXIT

FAKE_BIN_DIR="$TEST_ROOT/bin"
FAKE_BINARY="$TEST_ROOT/fake-live-portfolio"
FAKE_CRON_ENV="$TEST_ROOT/cron.env"
ARGS_FILE="$TEST_ROOT/args"
CRON_ARGS_FILE="$TEST_ROOT/cron-args"
mkdir -p "$FAKE_BIN_DIR" "$TEST_ROOT/app"

cat > "$FAKE_BIN_DIR/date" <<'EOF'
#!/usr/bin/env bash
case "${1:-}" in
    -Is) printf '%s\n' '2026-09-22T09:30:00-04:00' ;;
    +%u) printf '%s\n' '2' ;;
    +%Y-%m-%d) printf '%s\n' '2026-09-22' ;;
    *) exit 64 ;;
esac
EOF

cat > "$FAKE_BINARY" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$@" > "$ARGS_FILE"
EOF

cat > "$FAKE_BIN_DIR/cron" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$@" > "$CRON_ARGS_FILE"
EOF

chmod +x "$FAKE_BIN_DIR/date" "$FAKE_BIN_DIR/cron" "$FAKE_BINARY"

fail() {
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

run_scheduler_case() {
    local label="$1"
    local policy_line="$2"
    local expected_flag="$3"
    local lock_dir="$TEST_ROOT/lock-$label"

    : > "$FAKE_CRON_ENV"
    if [[ -n "$policy_line" ]]; then
        printf '%s\n' "$policy_line" > "$FAKE_CRON_ENV"
    fi
    rm -f "$ARGS_FILE"

    env -u QT_EMAIL_DELIVERY_ENABLED \
        PATH="$FAKE_BIN_DIR:$PATH" \
        LIVE_BINARY="$FAKE_BINARY" \
        CRON_ENV="$FAKE_CRON_ENV" \
        LOCK_DIR="$lock_dir" \
        APP_ROOT="$TEST_ROOT/app" \
        ARGS_FILE="$ARGS_FILE" \
        bash "$RUN_SCRIPT" >/dev/null

    mapfile -t args < "$ARGS_FILE"
    [[ "${args[0]:-}" == "2026-09-22" ]] || fail "$label passed wrong fixed date"
    if [[ "$expected_flag" == "yes" ]]; then
        [[ "${#args[@]}" -eq 2 && "${args[1]}" == "--send-email" ]] ||
            fail "$label did not pass exactly one --send-email flag"
    else
        [[ "${#args[@]}" -eq 1 ]] || fail "$label unexpectedly passed --send-email"
    fi
}

run_scheduler_case unset '' no
run_scheduler_case empty 'export QT_EMAIL_DELIVERY_ENABLED=' no
run_scheduler_case false 'export QT_EMAIL_DELIVERY_ENABLED=false' no
run_scheduler_case malformed 'export QT_EMAIL_DELIVERY_ENABLED=TRUE' no
run_scheduler_case runtime-only 'export QT_RUNTIME_CONTROL_ENABLED=true' no
run_scheduler_case true 'export QT_EMAIL_DELIVERY_ENABLED=true' yes

rm -f "$CRON_ARGS_FILE" "$FAKE_CRON_ENV"
env -u QT_EMAIL_DELIVERY_ENABLED \
    QT_RUNTIME_CONTROL_ENABLED=true \
    PATH="$FAKE_BIN_DIR:$PATH" \
    CRON_ENV="$FAKE_CRON_ENV" \
    CRON_ARGS_FILE="$CRON_ARGS_FILE" \
    bash "$ENTRYPOINT" >/dev/null 2>&1
[[ -f "$CRON_ARGS_FILE" ]] || fail 'entrypoint did not execute fake cron'
! grep -q '^export QT_EMAIL_DELIVERY_ENABLED=' "$FAKE_CRON_ENV" ||
    fail 'entrypoint persisted an unset delivery policy'
grep -qx 'export QT_RUNTIME_CONTROL_ENABLED=true' "$FAKE_CRON_ENV" ||
    fail 'entrypoint did not propagate runtime control independently of email'

rm -f "$CRON_ARGS_FILE" "$FAKE_CRON_ENV"
QT_EMAIL_DELIVERY_ENABLED=true \
    PATH="$FAKE_BIN_DIR:$PATH" \
    CRON_ENV="$FAKE_CRON_ENV" \
    CRON_ARGS_FILE="$CRON_ARGS_FILE" \
    bash "$ENTRYPOINT" >/dev/null 2>&1
grep -qx 'export QT_EMAIL_DELIVERY_ENABLED=true' "$FAKE_CRON_ENV" ||
    fail 'entrypoint did not propagate the explicit delivery policy'
mapfile -t cron_args < "$CRON_ARGS_FILE"
[[ "${cron_args[*]}" == '-f' ]] || fail 'entrypoint changed cron invocation'

printf 'PASS: scheduler and cron environment default-deny email delivery\n'
