#!/usr/bin/env bash
# Verifies migration 025 (C2 indexes, the C4 life-cycle triggers, the grants) and its rollback on
# a real PostgreSQL (16), against a fixture that reproduces new_algo_data's shapes and grants as of
# 2026-10-09: the roles and memberships, USAGE on schema trading, trading.live_run_metadata (with
# PUBLIC holding every privilege) and trading.position_overrides as 023 builds it.
#
#   docker run -d --name pg025 -e POSTGRES_PASSWORD=x postgres:16
#   docker cp migrations pg025:/m && docker exec -e QT_MIGRATION_TEST_THROWAWAY=1 -u postgres \
#       pg025 bash /m/test_025_qt_command_log_hardening.sh
#
# It creates CLUSTER-WIDE ROLES (fund_member, svc_algolens, qt_engine_app, ...), so it runs only
# on a throwaway server: QT_MIGRATION_TEST_THROWAWAY=1 is required, and a server that has a
# new_algo_data or algo_data database, or already has those roles, is refused. It creates and
# drops its own database (qt_m025_test_<pid>).
set -euo pipefail

[[ "${QT_MIGRATION_TEST_THROWAWAY:-}" == "1" ]] || {
    echo "REFUSING: set QT_MIGRATION_TEST_THROWAWAY=1 (this creates cluster roles)" >&2; exit 2; }
if [[ -n "$(psql -X -tAc "SELECT 1 FROM pg_database WHERE datname IN ('new_algo_data', 'algo_data')" -d postgres)" ]]; then
    echo "REFUSING: this server holds a production database" >&2; exit 2
fi
if [[ -n "$(psql -X -tAc "SELECT 1 FROM pg_roles WHERE rolname IN ('svc_algolens', 'qt_engine_app')" -d postgres)" ]]; then
    echo "REFUSING: the roles already exist on this server (not a throwaway)" >&2; exit 2
fi

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DB="qt_m025_test_$$"
fail() { echo "FAIL: $*" >&2; exit 1; }
ok() { echo "ok: $*"; }
q() { psql -X -q -v ON_ERROR_STOP=1 -d "$DB" -tAc "$1"; }
refuses() { ! psql -X -q -v ON_ERROR_STOP=1 -d "$DB" -c "$1" >/dev/null 2>&1; }
# refuses_with "<sql>" "<message part>"
refuses_with() {
    local out
    if out=$(psql -X -q -v ON_ERROR_STOP=1 -d "$DB" -c "$1" 2>&1); then
        fail "accepted: $1"
    fi
    [[ "$out" == *"$2"* ]] || fail "refused for another reason: $1 -> $out"
}
apply() { PGOPTIONS="-c client_min_messages=warning" psql -X -q -v ON_ERROR_STOP=1 -d "$DB" -f "$HERE/$1" >/dev/null; }

psql -X -q -d postgres -c "CREATE DATABASE $DB" >/dev/null
cleanup() {
    psql -X -q -d postgres -c "DROP DATABASE IF EXISTS $DB" >/dev/null 2>&1 || true
    psql -X -q -d postgres >/dev/null 2>&1 <<'SQL' || true
DROP ROLE IF EXISTS qt_algolens_app, qt_engine_app, vault_admin, svc_trade_ngin, svc_algolens,
    svc_airflow, svc_data_ngin, fund_member, quant_dev, quant_dev_rw, quant_research_ro,
    quant_trading_rw, leadership_ro, leadership_rw, investor_relations_ro;
SQL
}
trap cleanup EXIT

# -- the fixture: new_algo_data's roles, memberships and grants (read 2026-10-09) ---------------
psql -X -q -v ON_ERROR_STOP=1 -d "$DB" <<'SQL'
CREATE ROLE fund_member LOGIN;            CREATE ROLE quant_dev LOGIN;
CREATE ROLE quant_research_ro;            CREATE ROLE quant_dev_rw;
CREATE ROLE quant_trading_rw;             CREATE ROLE leadership_ro;
CREATE ROLE leadership_rw;                CREATE ROLE investor_relations_ro;
CREATE ROLE svc_algolens LOGIN;           CREATE ROLE svc_trade_ngin LOGIN;
CREATE ROLE svc_airflow LOGIN;            CREATE ROLE svc_data_ngin LOGIN;
CREATE ROLE qt_algolens_app LOGIN IN ROLE svc_algolens;
CREATE ROLE qt_engine_app LOGIN IN ROLE svc_trade_ngin;
GRANT quant_trading_rw TO svc_trade_ngin;
CREATE ROLE vault_admin LOGIN;
GRANT investor_relations_ro, leadership_ro, leadership_rw, quant_dev_rw, quant_research_ro,
      quant_trading_rw TO vault_admin;

CREATE SCHEMA trading;
GRANT USAGE ON SCHEMA trading TO fund_member, quant_dev, quant_research_ro, quant_dev_rw,
      quant_trading_rw, leadership_ro, investor_relations_ro, leadership_rw, svc_algolens;

-- live_run_metadata as on new_algo_data (022 applied), with its grants: PUBLIC holds everything.
CREATE TABLE trading.live_run_metadata (
    id serial PRIMARY KEY, date date NOT NULL, strategy_id varchar(200) NOT NULL,
    portfolio_id varchar(100) NOT NULL, strategy_allocations jsonb NOT NULL,
    portfolio_config jsonb, strategy_configs jsonb, created_at timestamp DEFAULT now(),
    settings_used jsonb, published_by text, published_at timestamptz,
    CONSTRAINT live_run_metadata_unique UNIQUE (date, strategy_id, portfolio_id));
GRANT ALL ON trading.live_run_metadata TO PUBLIC;
GRANT USAGE, SELECT ON SEQUENCE trading.live_run_metadata_id_seq TO PUBLIC;
GRANT SELECT ON trading.live_run_metadata TO fund_member, quant_dev, quant_research_ro,
      leadership_ro, investor_relations_ro;
GRANT SELECT, INSERT, UPDATE, DELETE ON trading.live_run_metadata TO quant_dev_rw,
      quant_trading_rw, leadership_rw, svc_algolens;
GRANT USAGE, SELECT ON SEQUENCE trading.live_run_metadata_id_seq TO quant_dev_rw,
      quant_trading_rw, leadership_rw, svc_algolens;
INSERT INTO trading.live_run_metadata (date, strategy_id, portfolio_id, strategy_allocations)
    VALUES ('2026-10-08', 'S', 'QT_CONSERVATIVE_PORTFOLIO', '{}');

-- What 023 needs, then 023 itself (below).
CREATE TABLE trading.live_results (id serial PRIMARY KEY, strategy_id varchar(100) NOT NULL,
    portfolio_id varchar(100) NOT NULL, date date NOT NULL);
CREATE TABLE trading.strategy_registry (id text PRIMARY KEY, strategy_type text NOT NULL,
    portfolio_id text NOT NULL, name text, description text, initial_equity numeric,
    managers jsonb, is_active boolean DEFAULT true, sort_order integer DEFAULT 0, lifecycle text);
SQL
apply 023_qt_command_log.sql
psql -X -q -v ON_ERROR_STOP=1 -d "$DB" <<'SQL'
-- position_overrides' grants on new_algo_data (no PUBLIC).
GRANT SELECT ON trading.position_overrides TO fund_member, quant_dev, quant_research_ro,
      investor_relations_ro, leadership_ro;
GRANT SELECT, INSERT, UPDATE, DELETE ON trading.position_overrides TO quant_dev_rw,
      quant_trading_rw, leadership_rw, svc_algolens;
GRANT USAGE, SELECT ON SEQUENCE trading.position_overrides_id_seq TO quant_dev_rw,
      quant_trading_rw, leadership_rw, svc_algolens;
SQL
ok "fixture (new_algo_data roles and grants)"

# Before 025: the hole being closed.
q "SET ROLE fund_member; INSERT INTO trading.live_run_metadata (date, strategy_id, portfolio_id, strategy_allocations) VALUES ('2026-10-01', 'X', 'P', '{}')" >/dev/null \
    || fail "fixture: PUBLIC should let fund_member insert before 025"
q "DELETE FROM trading.live_run_metadata WHERE strategy_id = 'X'" >/dev/null
ok "before 025 fund_member (read-only) can write live_run_metadata through PUBLIC"

INS="INSERT INTO trading.position_overrides (portfolio_id, date, kind, requested_by"
REQ=$(q "$INS, reason) VALUES ('P', '2026-10-08', 'override_request', 'desk@x', 'r') RETURNING id")

# Refuses over rows that break the new indexes.
q "$INS, payload, parent_id, approver_role) VALUES ('P', '2026-10-08', 'override_decision', 'vp@x', '{\"approved\":true}', $REQ, 'vp'),
                                                   ('P', '2026-10-08', 'override_decision', 'pr@x', '{\"approved\":true}', $REQ, 'president')" >/dev/null
psql -X -q -v ON_ERROR_STOP=1 -d "$DB" -f "$HERE/025_qt_command_log_hardening.sql" >/dev/null 2>&1 \
    && fail "025 applied over two live decisions of one request"
q "ALTER TABLE trading.position_overrides DISABLE TRIGGER position_overrides_guard;
   DELETE FROM trading.position_overrides WHERE kind = 'override_decision';
   ALTER TABLE trading.position_overrides ENABLE TRIGGER position_overrides_guard" >/dev/null
ok "025 refuses while rows would break its indexes"

apply 025_qt_command_log_hardening.sql
apply 025_qt_command_log_hardening.sql
ok "025 applies, and re-applies"

# -- C4: inserts --------------------------------------------------------------------------------
for extra in "status) VALUES ('P','2026-10-09','publish','d','done')" \
             "status) VALUES ('P','2026-10-09','publish','d','running')" \
             "result) VALUES ('P','2026-10-09','publish','d','{}')" \
             "message) VALUES ('P','2026-10-09','publish','d','m')" \
             "started_at) VALUES ('P','2026-10-09','publish','d',now())" \
             "finished_at) VALUES ('P','2026-10-09','publish','d',now())" \
             "token_hash) VALUES ('P','2026-10-09','publish','d','h')" \
             "token_expires_at) VALUES ('P','2026-10-09','publish','d',now())"; do
    refuses_with "$INS, $extra" "a new row must be pending"
done
PUB=$(q "$INS) VALUES ('P', '2026-10-09', 'publish', 'd') RETURNING id")
ok "an insert must be pending with the engine columns NULL"

# -- C4: transitions ---------------------------------------------------------------------------
refuses_with "UPDATE trading.position_overrides SET status = 'done' WHERE id = $PUB" "pending -> done is not an allowed"
q "UPDATE trading.position_overrides SET status = 'running', started_at = now() WHERE id = $PUB" >/dev/null
q "UPDATE trading.position_overrides SET result = '{\"email_sent_at\":\"t\"}' WHERE id = $PUB" >/dev/null
refuses_with "UPDATE trading.position_overrides SET status = 'pending', started_at = NULL WHERE id = $PUB" "running -> pending is not an allowed"
q "BEGIN; SET LOCAL algogators.recovery = 'on';
   UPDATE trading.position_overrides SET status = 'pending', started_at = NULL WHERE id = $PUB; COMMIT" >/dev/null
[[ "$(q "SELECT status FROM trading.position_overrides WHERE id = $PUB")" == "pending" ]] || fail "recovery requeue"
[[ "$(q "SELECT coalesce(current_setting('algogators.recovery', true), '')")" == "" ]] || fail "SET LOCAL leaked"
q "UPDATE trading.position_overrides SET status = 'running', started_at = now() WHERE id = $PUB" >/dev/null
q "UPDATE trading.position_overrides SET status = 'done', finished_at = now() WHERE id = $PUB AND status IN ('pending', 'running')" >/dev/null
refuses_with "UPDATE trading.position_overrides SET status = 'running' WHERE id = $PUB" "is done and final"
refuses_with "UPDATE trading.position_overrides SET message = 'x' WHERE id = $PUB" "is done and final"
refuses_with "BEGIN; SET LOCAL algogators.recovery = 'on'; UPDATE trading.position_overrides SET status = 'pending' WHERE id = $PUB; COMMIT" "is done and final"
[[ "$(q "UPDATE trading.position_overrides SET status = 'failed' WHERE id = $PUB AND status IN ('pending', 'running') RETURNING id")" == "" ]] \
    || fail "the engine's guarded finish touched a final row"
refuses_with "UPDATE trading.position_overrides SET reason = 'edited' WHERE id = $REQ" "only status, result"
ok "pending -> running -> done; running -> pending only under algogators.recovery; terminal rows final"

refuses_with "DELETE FROM trading.position_overrides WHERE id = $PUB" "never deleted"
refuses_with "TRUNCATE trading.position_overrides" "never truncated"
ok "DELETE and TRUNCATE refused (also for the superuser)"

# -- C2: indexes -------------------------------------------------------------------------------
q "UPDATE trading.position_overrides SET status = 'running' WHERE id = $REQ;
   UPDATE trading.position_overrides SET status = 'done', token_hash = 'h1', token_expires_at = now() + interval '48 hours' WHERE id = $REQ" >/dev/null
DEC="$INS, payload, parent_id, approver_role) VALUES ('P', '2026-10-08', 'override_decision'"
D1=$(q "$DEC, 'vp@x', '{\"approved\":true}', $REQ, 'vp') RETURNING id")
refuses_with "$DEC, 'pr@x', '{\"approved\":false}', $REQ, 'president')" "position_overrides_one_decision"
q "UPDATE trading.position_overrides SET status = 'running' WHERE id = $D1; UPDATE trading.position_overrides SET status = 'refused' WHERE id = $D1" >/dev/null
D2=$(q "$DEC, 'pr@x', '{\"approved\":true}', $REQ, 'president') RETURNING id")
q "UPDATE trading.position_overrides SET status = 'running' WHERE id = $D2; UPDATE trading.position_overrides SET status = 'done' WHERE id = $D2" >/dev/null
refuses_with "$DEC, 'vp@x', '{\"approved\":true}', $REQ, 'vp')" "position_overrides_one_decision"
ok "one pending/running/done decision per request; a refused one does not count"

P1=$(q "$INS) VALUES ('Q', '2026-10-08', 'publish', 'd') RETURNING id")
refuses_with "$INS) VALUES ('Q', '2026-10-08', 'publish', 'd')" "position_overrides_one_open_publish"
q "UPDATE trading.position_overrides SET status = 'running' WHERE id = $P1; UPDATE trading.position_overrides SET status = 'failed' WHERE id = $P1" >/dev/null
q "$INS) VALUES ('Q', '2026-10-08', 'publish', 'd')" >/dev/null
ok "one open publish per day"

q "$INS, reason) VALUES ('Q', '2026-10-08', 'override_request', 'd', 'r')" >/dev/null
refuses_with "$INS, reason) VALUES ('Q', '2026-10-08', 'override_request', 'd', 'r')" "position_overrides_one_open_request"
q "$INS, reason) VALUES ('Q', '2026-10-09', 'override_request', 'd', 'r')" >/dev/null
ok "one open override request per day"

# -- grants --------------------------------------------------------------------------------------
as() { local role="$1"; shift; q "SET ROLE $role; $*"; }
as_refused() { refuses_with "SET ROLE $1; $2" "permission denied"; }

S=$(as qt_algolens_app "$INS, reason) VALUES ('R', '2026-10-08', 'save', 'desk@x', 'r') RETURNING id")
[[ -n "$S" ]] || fail "AlgoLens cannot insert"
as qt_algolens_app "SELECT id FROM trading.position_overrides WHERE id = $S" >/dev/null \
    || fail "AlgoLens cannot read a command row"
[[ "$(q "SELECT has_sequence_privilege('qt_algolens_app', 'trading.position_overrides_id_seq', 'USAGE')")" == "t" ]] \
    || fail "AlgoLens lost the command log sequence"
as_refused qt_algolens_app "UPDATE trading.position_overrides SET status = 'running' WHERE id = $S"
as_refused qt_algolens_app "UPDATE trading.position_overrides SET result = '{}' WHERE id = $S"
as_refused qt_algolens_app "DELETE FROM trading.position_overrides WHERE id = $S"
as_refused qt_algolens_app "TRUNCATE trading.position_overrides"
as_refused qt_algolens_app "BEGIN; SELECT id FROM trading.position_overrides WHERE id = $S FOR UPDATE; COMMIT"
ok "svc_algolens (qt_algolens_app): SELECT, INSERT and the sequence; no UPDATE/DELETE/TRUNCATE"

as qt_engine_app "INSERT INTO trading.position_overrides (portfolio_id, date, kind, requested_by) VALUES ('E', '2026-10-08', 'publish', 'engine@x')" >/dev/null \
    || fail "the engine role cannot insert a command row"
as qt_engine_app "UPDATE trading.position_overrides SET status = 'running', started_at = now() WHERE id = $S AND status = 'pending'" >/dev/null
as qt_engine_app "BEGIN; SET LOCAL algogators.recovery = 'on'; UPDATE trading.position_overrides SET status = 'pending', started_at = NULL WHERE id = $S; COMMIT" >/dev/null
[[ "$(q "SELECT status FROM trading.position_overrides WHERE id = $S")" == "pending" ]] || fail "engine requeue"
as qt_engine_app "INSERT INTO trading.live_run_metadata (date, strategy_id, portfolio_id, strategy_allocations) VALUES ('2026-10-09', 'S', 'QT_CONSERVATIVE_PORTFOLIO', '{}')" >/dev/null
as qt_engine_app "UPDATE trading.live_run_metadata SET published_by = 'dom', published_at = now() WHERE date = '2026-10-09'" >/dev/null
as qt_algolens_app "SELECT published_at FROM trading.live_run_metadata" >/dev/null
as qt_algolens_app "INSERT INTO trading.live_run_metadata (date, strategy_id, portfolio_id, strategy_allocations) VALUES ('2026-10-10', 'S', 'QT_CONSERVATIVE_PORTFOLIO', '{}')" >/dev/null
ok "svc_trade_ngin (qt_engine_app) moves command rows and writes live_run_metadata; svc_algolens keeps its access"

for role in fund_member quant_dev; do
    as "$role" "SELECT count(*) FROM trading.live_run_metadata" >/dev/null || fail "$role lost SELECT"
    as_refused "$role" "INSERT INTO trading.live_run_metadata (date, strategy_id, portfolio_id, strategy_allocations) VALUES ('2026-10-11', 'X', 'P', '{}')"
    as_refused "$role" "UPDATE trading.live_run_metadata SET published_at = now()"
    as_refused "$role" "DELETE FROM trading.live_run_metadata"
done
for role in quant_dev_rw leadership_rw vault_admin; do
    as "$role" "INSERT INTO trading.live_run_metadata (date, strategy_id, portfolio_id, strategy_allocations) VALUES ('2026-10-12', '$role', 'P', '{}')" >/dev/null \
        || fail "$role lost INSERT"
done
as_refused svc_airflow "SELECT 1 FROM trading.live_run_metadata"
[[ "$(q "SELECT count(*) FROM information_schema.role_table_grants WHERE grantee = 'PUBLIC' AND table_schema = 'trading' AND table_name = 'live_run_metadata'")" == "0" ]] \
    || fail "PUBLIC still holds a privilege on live_run_metadata"
[[ "$(q "SELECT has_sequence_privilege('public', 'trading.live_run_metadata_id_seq', 'USAGE')")" == "f" ]] \
    || fail "PUBLIC still uses the sequence"
for role in quant_dev_rw quant_trading_rw leadership_rw svc_algolens svc_trade_ngin qt_engine_app qt_algolens_app; do
    [[ "$(q "SELECT has_sequence_privilege('$role', 'trading.live_run_metadata_id_seq', 'USAGE')")" == "t" ]] || fail "$role lost the sequence"
done
for role in postgres; do
    [[ "$(q "SELECT has_table_privilege('$role', 'trading.live_run_metadata', 'TRUNCATE')")" == "t" ]] || fail "owner"
done
[[ "$(q "SELECT count(*) FROM pg_roles r WHERE r.rolname NOT IN ('postgres') AND r.rolname !~ '^pg_' AND has_table_privilege(r.oid, 'trading.live_run_metadata', 'TRUNCATE')")" == "0" ]] \
    || fail "a non-owner role can TRUNCATE live_run_metadata"
ok "live_run_metadata: PUBLIC revoked; readers read, writers write, nobody else; svc_airflow unchanged (no schema USAGE)"

# -- rollback -----------------------------------------------------------------------------------
apply 025_qt_command_log_hardening_rollback.sql
apply 025_qt_command_log_hardening_rollback.sql
for idx in position_overrides_one_decision position_overrides_one_open_publish position_overrides_one_open_request; do
    [[ -z "$(q "SELECT to_regclass('trading.$idx')")" ]] || fail "rollback left $idx"
done
[[ "$(q "SELECT count(*) FROM pg_trigger WHERE tgname IN ('position_overrides_insert_guard', 'position_overrides_no_truncate')")" == "0" ]] \
    || fail "rollback left a trigger"
q "$INS, status, message) VALUES ('Z', '2026-10-08', 'publish', 'd', 'done', 'm')" >/dev/null
q "UPDATE trading.position_overrides SET message = 'again' WHERE portfolio_id = 'Z'" >/dev/null
refuses_with "DELETE FROM trading.position_overrides WHERE portfolio_id = 'Z'" "never deleted"
as fund_member "INSERT INTO trading.live_run_metadata (date, strategy_id, portfolio_id, strategy_allocations) VALUES ('2026-10-13', 'X', 'P', '{}')" >/dev/null \
    || fail "rollback did not restore PUBLIC"
as qt_algolens_app "UPDATE trading.position_overrides SET result = '{}' WHERE id = $S" >/dev/null \
    || fail "rollback did not restore svc_algolens UPDATE"
[[ "$(q "SELECT count(*) FROM information_schema.role_table_grants WHERE grantee = 'svc_trade_ngin' AND table_name = 'live_run_metadata'")" == "0" ]] \
    || fail "rollback left svc_trade_ngin's direct grants"
ok "rollback restores 023's guard and the old grants; idempotent"

q "ALTER TABLE trading.position_overrides DISABLE TRIGGER position_overrides_guard; DELETE FROM trading.position_overrides WHERE portfolio_id = 'Z'; ALTER TABLE trading.position_overrides ENABLE TRIGGER position_overrides_guard" >/dev/null
apply 025_qt_command_log_hardening.sql
ok "025 re-applies after its rollback"

echo "PASS: 025 and its rollback"
