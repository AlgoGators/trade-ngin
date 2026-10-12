#!/usr/bin/env bash
# Verifies migration 028 (ops.data_ready and its grants) and its rollback on a real PostgreSQL 16.
#
#   docker run -d --name pg028 -e POSTGRES_PASSWORD=x postgres:16
#   docker cp migrations pg028:/m && docker exec -e QT_MIGRATION_TEST_THROWAWAY=1 -u postgres \
#       pg028 bash /m/test_028_ops_data_ready.sh
#
# It creates CLUSTER-WIDE ROLES (qt_engine_app, svc_trade_ngin), so it runs only on a throwaway
# server: QT_MIGRATION_TEST_THROWAWAY=1 is required, and a server that has a new_algo_data or
# algo_data database, or already has those roles, is refused. It creates and drops its own
# database (qt_m028_test_<pid>).
set -euo pipefail

[[ "${QT_MIGRATION_TEST_THROWAWAY:-}" == "1" ]] || {
    echo "REFUSING: set QT_MIGRATION_TEST_THROWAWAY=1 (this creates cluster roles)" >&2; exit 2; }
if [[ -n "$(psql -X -tAc "SELECT 1 FROM pg_database WHERE datname IN ('new_algo_data', 'algo_data')" -d postgres)" ]]; then
    echo "REFUSING: this server holds a production database" >&2; exit 2
fi
if [[ -n "$(psql -X -tAc "SELECT 1 FROM pg_roles WHERE rolname IN ('qt_engine_app', 'svc_trade_ngin')" -d postgres)" ]]; then
    echo "REFUSING: the roles already exist on this server (not a throwaway)" >&2; exit 2
fi

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DB="qt_m028_test_$$"
fail() { echo "FAIL: $*" >&2; exit 1; }
ok() { echo "ok: $*"; }
q() { psql -X -q -v ON_ERROR_STOP=1 -d "$DB" -tAc "$1"; }
as() { psql -X -q -v ON_ERROR_STOP=1 -d "$DB" -tAc "SET ROLE $1; $2"; }
refuses() { ! psql -X -q -v ON_ERROR_STOP=1 -d "$DB" -c "$1" >/dev/null 2>&1; }
apply() { PGOPTIONS="-c client_min_messages=warning" psql -X -q -v ON_ERROR_STOP=1 -d "$DB" -f "$HERE/$1" >/dev/null; }

psql -X -q -d postgres -c "CREATE DATABASE $DB" >/dev/null
cleanup() {
    psql -X -q -d postgres -c "DROP DATABASE IF EXISTS $DB" >/dev/null 2>&1 || true
    psql -X -q -d postgres -c "DROP ROLE IF EXISTS qt_engine_app, svc_trade_ngin" >/dev/null 2>&1 || true
}
trap cleanup EXIT

q "CREATE ROLE svc_trade_ngin LOGIN; CREATE ROLE qt_engine_app LOGIN NOINHERIT;"

apply 028_ops_data_ready.sql
apply 028_ops_data_ready.sql
ok "applies, and re-applies as a no-op"

q "INSERT INTO ops.data_ready (dataset, bar_date, expected, loaded, status, run_id)
   VALUES ('futures_ohlcv_1d', '2026-10-09', 36, 36, 'complete', 'r1')"
[[ "$(q "SELECT missing = '{}' AND finished_at IS NOT NULL FROM ops.data_ready")" == "t" ]] \
    || fail "defaults"
refuses "INSERT INTO ops.data_ready (dataset, bar_date, expected, loaded, status)
         VALUES ('futures_ohlcv_1d', '2026-10-09', 36, 36, 'complete')" || fail "duplicate key"
refuses "INSERT INTO ops.data_ready (dataset, bar_date, expected, loaded, status)
         VALUES ('x', '2026-10-09', 1, 1, 'done')" || fail "bad status accepted"
refuses "INSERT INTO ops.data_ready (dataset, bar_date, loaded, status)
         VALUES ('x', '2026-10-10', 1, 'complete')" || fail "expected may be NULL"
q "INSERT INTO ops.data_ready (dataset, bar_date, expected, loaded, missing, status)
   VALUES ('futures_ohlcv_1d', '2026-10-09', 36, 35, '{ZN}', 'partial')
   ON CONFLICT (dataset, bar_date) DO UPDATE SET loaded = EXCLUDED.loaded,
       missing = EXCLUDED.missing, status = EXCLUDED.status, finished_at = now()"
[[ "$(q "SELECT status || ':' || array_to_string(missing, ',') FROM ops.data_ready")" == "partial:ZN" ]] \
    || fail "upsert"
ok "constraints and the data-ngin upsert"

for r in qt_engine_app svc_trade_ngin; do
    [[ "$(as $r "SELECT count(*) FROM ops.data_ready")" == "1" ]] || fail "$r cannot read"
    ! psql -X -q -d "$DB" -c "SET ROLE $r; INSERT INTO ops.data_ready (dataset, bar_date, expected, loaded, status) VALUES ('y', '2026-10-09', 1, 1, 'complete')" >/dev/null 2>&1 \
        || fail "$r can write"
    ! psql -X -q -d "$DB" -c "SET ROLE $r; UPDATE ops.data_ready SET loaded = 0" >/dev/null 2>&1 \
        || fail "$r can update"
done
[[ "$(q "SELECT has_table_privilege('postgres', 'ops.data_ready', 'INSERT, UPDATE, SELECT')")" == "t" ]] \
    || fail "postgres grants"
ok "grants: the engine roles read only"

refuses "\\i $HERE/028_ops_data_ready_rollback.sql" || true
if PGOPTIONS="-c client_min_messages=warning" psql -X -q -v ON_ERROR_STOP=1 -d "$DB" \
        -f "$HERE/028_ops_data_ready_rollback.sql" >/dev/null 2>&1; then
    fail "the rollback dropped rows without migration.force_rollback"
fi
PGOPTIONS="-c migration.force_rollback=yes -c client_min_messages=warning" \
    psql -X -q -v ON_ERROR_STOP=1 -d "$DB" -f "$HERE/028_ops_data_ready_rollback.sql" >/dev/null
[[ -z "$(q "SELECT 1 FROM pg_namespace WHERE nspname = 'ops'")" ]] || fail "schema ops left"
apply 028_ops_data_ready_rollback.sql
apply 028_ops_data_ready.sql
ok "rollback refuses rows, drops with force, re-runs, and 028 re-applies after it"
echo "PASS 028"
