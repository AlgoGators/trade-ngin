#!/usr/bin/env bash

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN_SCRIPT="$REPO_ROOT/scripts/run_live_portfolio.sh"
TEST_ROOT="$(mktemp -d)"
trap 'rm -rf -- "$TEST_ROOT"' EXIT

FAKE_BIN_DIR="$TEST_ROOT/bin"
MANIFEST="$TEST_ROOT/live-books.tsv"
CALLS="$TEST_ROOT/calls"
mkdir -p "$FAKE_BIN_DIR" "$TEST_ROOT/app"

cat > "$FAKE_BIN_DIR/date" <<'EOF'
#!/usr/bin/env bash
case "${1:-}" in
    -Is) printf '%s\n' '2026-10-01T09:30:00-04:00' ;;
    +%u) printf '%s\n' '4' ;;
    +%Y-%m-%d) printf '%s\n' '2026-10-01' ;;
    *) exit 64 ;;
esac
EOF

cat > "$TEST_ROOT/runner-a" <<'EOF'
#!/usr/bin/env bash
printf 'a\t%s\t%s\n' "$1" "$3" >> "$CALLS"
EOF

cat > "$TEST_ROOT/runner-b" <<'EOF'
#!/usr/bin/env bash
printf 'b\t%s\t%s\n' "$1" "$3" >> "$CALLS"
exit "${RUNNER_B_EXIT:-0}"
EOF

cat > "$TEST_ROOT/runner-c" <<'EOF'
#!/usr/bin/env bash
printf 'c\t%s\t%s\n' "$1" "$3" >> "$CALLS"
EOF

chmod +x "$FAKE_BIN_DIR/date" "$TEST_ROOT/runner-a" "$TEST_ROOT/runner-b" "$TEST_ROOT/runner-c"

fail() {
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

run_cycle() {
    local lock="$1"
    shift
    env PATH="$FAKE_BIN_DIR:$PATH" \
        LIVE_BOOK_MANIFEST="$MANIFEST" \
        CRON_ENV="$TEST_ROOT/missing-cron-env" \
        LOCK_DIR="$lock" \
        APP_ROOT="$TEST_ROOT/app" \
        CALLS="$CALLS" \
        "$@" bash "$RUN_SCRIPT"
}

printf '# runner<TAB>portfolio\n%s\talpha_book\n%s\tbeta-book\n%s\tgamma3\n' \
    "$TEST_ROOT/runner-a" "$TEST_ROOT/runner-b" "$TEST_ROOT/runner-c" > "$MANIFEST"

: > "$CALLS"
run_cycle "$TEST_ROOT/lock-success" >/dev/null
mapfile -t calls < "$CALLS"
[[ "${calls[*]}" == \
    $'a\t2026-10-01\talpha_book b\t2026-10-01\tbeta-book c\t2026-10-01\tgamma3' ]] ||
    fail "manifest order, fixed date, or --portfolio forwarding changed: ${calls[*]}"

: > "$CALLS"
set +e
run_cycle "$TEST_ROOT/lock-failure" RUNNER_B_EXIT=23 >"$TEST_ROOT/failure.log" 2>&1
rc=$?
set -e
[[ "$rc" -eq 23 ]] || fail "scheduler did not propagate first failing runner exit (got $rc)"
mapfile -t calls < "$CALLS"
[[ "${calls[*]}" == $'a\t2026-10-01\talpha_book b\t2026-10-01\tbeta-book' ]] ||
    fail "scheduler did not stop immediately after beta-book failed: ${calls[*]}"
grep -q 'FAILED portfolio beta-book' "$TEST_ROOT/failure.log" ||
    fail "scheduler did not name the failed portfolio"

mkdir "$TEST_ROOT/held-lock"
: > "$CALLS"
run_cycle "$TEST_ROOT/held-lock" >"$TEST_ROOT/locked.log" 2>&1
[[ ! -s "$CALLS" ]] || fail "scheduler ran a book while the global lock was held"
grep -q 'another cycle already holds' "$TEST_ROOT/locked.log" ||
    fail "scheduler did not report the global lock"

printf '%s\t../escape\n' "$TEST_ROOT/runner-a" > "$MANIFEST"
set +e
run_cycle "$TEST_ROOT/lock-invalid" >"$TEST_ROOT/invalid.log" 2>&1
rc=$?
set -e
[[ "$rc" -eq 64 ]] || fail "invalid portfolio key was not rejected (got $rc)"
[[ ! -s "$CALLS" ]] || fail "invalid manifest invoked a runner"

printf '%s\talpha\n%s\talpha\n' "$TEST_ROOT/runner-a" "$TEST_ROOT/runner-b" > "$MANIFEST"
set +e
run_cycle "$TEST_ROOT/lock-duplicate" >"$TEST_ROOT/duplicate.log" 2>&1
rc=$?
set -e
[[ "$rc" -eq 64 ]] || fail "duplicate portfolio was not rejected (got $rc)"

printf 'PASS: ordered multi-book scheduler is locked, date-stable, and fail-fast\n'
