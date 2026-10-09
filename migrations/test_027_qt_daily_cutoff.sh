#!/usr/bin/env bash
# Verifies migration 027 (the QT daily cutoff: live_run_metadata.publish_source and sent_at, and
# their grants) and its rollback on a real PostgreSQL (16), on the fixture of
# test_025_qt_command_log_hardening.sh (new_algo_data's roles, memberships and grants as of
# 2026-10-09) with 023 and 025 applied first, as on new_algo_data.
#
#   docker run -d --name pg027 -e POSTGRES_PASSWORD=x postgres:16
#   docker cp migrations pg027:/m && docker exec -e QT_MIGRATION_TEST_THROWAWAY=1 -u postgres \
#       pg027 bash /m/test_027_qt_daily_cutoff.sh
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
DB="qt_m027_test_$$"
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

apply 025_qt_command_log_hardening.sql
ok "023 and 025 applied"

as() { local role="$1"; shift; q "SET ROLE $role; $*"; }
as_refused() { refuses_with "SET ROLE $1; $2" "permission denied"; }
META="INSERT INTO trading.live_run_metadata (date, strategy_id, portfolio_id, strategy_allocations"

# Refuses a column of another type.
q "ALTER TABLE trading.live_run_metadata ADD COLUMN sent_at text" >/dev/null
psql -X -q -v ON_ERROR_STOP=1 -d "$DB" -f "$HERE/027_qt_daily_cutoff.sql" >/dev/null 2>&1 \
    && fail "027 applied over a sent_at of another type"
q "ALTER TABLE trading.live_run_metadata DROP COLUMN sent_at" >/dev/null
ok "027 refuses an existing column of another type"

apply 027_qt_daily_cutoff.sql
apply 027_qt_daily_cutoff.sql
[[ "$(q "SELECT string_agg(column_name || ':' || data_type || ':' || is_nullable, ',' ORDER BY column_name) FROM information_schema.columns WHERE table_schema = 'trading' AND table_name = 'live_run_metadata' AND column_name IN ('publish_source', 'sent_at')")" \
    == "publish_source:text:YES,sent_at:timestamp with time zone:YES" ]] || fail "the columns"
[[ "$(q "SELECT count(*) FROM trading.live_run_metadata WHERE publish_source IS NOT NULL OR sent_at IS NOT NULL")" == "0" ]] \
    || fail "existing rows were backfilled"
ok "027 applies, and re-applies; existing rows keep NULL"

# -- the CHECK ----------------------------------------------------------------------------------
for src in desk fallback model-only; do
    q "UPDATE trading.live_run_metadata SET publish_source = '$src' WHERE date = '2026-10-08'" >/dev/null
done
refuses_with "UPDATE trading.live_run_metadata SET publish_source = 'system' WHERE date = '2026-10-08'" "live_run_metadata_publish_source_check"
refuses_with "UPDATE trading.live_run_metadata SET publish_source = '' WHERE date = '2026-10-08'" "live_run_metadata_publish_source_check"
q "UPDATE trading.live_run_metadata SET publish_source = NULL WHERE date = '2026-10-08'" >/dev/null
ok "publish_source is desk, fallback, model-only or NULL"

# -- the engine: the approval and the send, as qt_desk.cpp writes them ---------------------------
as qt_engine_app "$META) VALUES ('2026-10-09', 'S', 'QT_CONSERVATIVE_PORTFOLIO', '{}')" >/dev/null
PUB=$(q "INSERT INTO trading.position_overrides (portfolio_id, date, kind, requested_by) VALUES ('QT_CONSERVATIVE_PORTFOLIO', '2026-10-09', 'publish', 'system:fallback-10am') RETURNING id")
as qt_engine_app "UPDATE trading.position_overrides SET status = 'running', started_at = now() WHERE id = $PUB" >/dev/null
APPROVE="WITH m AS (UPDATE trading.live_run_metadata SET published_by = 'system:fallback-10am', published_at = now(), publish_source = 'fallback' WHERE portfolio_id = 'QT_CONSERVATIVE_PORTFOLIO' AND strategy_id = 'S' AND date = '2026-10-09'::date AND published_at IS NULL AND EXISTS (SELECT 1 FROM trading.position_overrides WHERE id = $PUB AND status IN ('pending', 'running')) RETURNING published_at), r AS (UPDATE trading.position_overrides SET result = COALESCE(result, '{}'::jsonb) || jsonb_build_object('published_at', (SELECT max(published_at) FROM m)::text, 'publish_source', 'fallback') WHERE id = $PUB AND status IN ('pending', 'running') AND EXISTS (SELECT 1 FROM m) RETURNING id) SELECT (SELECT count(*) FROM m)::text || ' ' || (SELECT count(*) FROM r)::text"
[[ "$(as qt_engine_app "$APPROVE")" == "1 1" ]] || fail "the engine's approval"
[[ "$(as qt_engine_app "$APPROVE")" == "0 0" ]] || fail "a second approval changed the day"
SEND="WITH m AS (UPDATE trading.live_run_metadata SET sent_at = now() WHERE portfolio_id = 'QT_CONSERVATIVE_PORTFOLIO' AND date = '2026-10-09' AND sent_at IS NULL RETURNING sent_at), r AS (UPDATE trading.position_overrides SET result = COALESCE(result, '{}'::jsonb) || jsonb_build_object('email_sent_at', now()::text) WHERE id = $PUB AND kind = 'publish' AND status IN ('pending', 'running') RETURNING id) SELECT (SELECT count(*) FROM m)::text"
[[ "$(as qt_engine_app "$SEND")" == "1" ]] || fail "the engine's send record"
[[ "$(as qt_engine_app "$SEND")" == "0" ]] || fail "sent_at moved"
as qt_engine_app "UPDATE trading.position_overrides SET status = 'done', finished_at = now() WHERE id = $PUB AND status IN ('pending', 'running')" >/dev/null
[[ "$(q "SELECT publish_source || ' ' || (sent_at IS NOT NULL)::text || ' ' || (r.result ? 'published_at')::text || ' ' || (r.result ? 'email_sent_at')::text FROM trading.live_run_metadata m, trading.position_overrides r WHERE m.date = '2026-10-09' AND r.id = $PUB")" \
    == "fallback true true true" ]] || fail "the publish record"
ok "svc_trade_ngin (qt_engine_app) approves once and records the send once"

# -- AlgoLens reads the new columns and writes what it wrote before, but not them ----------------
as qt_algolens_app "SELECT publish_source, sent_at, published_at FROM trading.live_run_metadata" >/dev/null \
    || fail "AlgoLens cannot read publish_source and sent_at"
as_refused qt_algolens_app "UPDATE trading.live_run_metadata SET publish_source = 'desk' WHERE date = '2026-10-09'"
as_refused qt_algolens_app "UPDATE trading.live_run_metadata SET sent_at = now() WHERE date = '2026-10-09'"
as_refused qt_algolens_app "$META, sent_at) VALUES ('2026-10-10', 'A', 'P', '{}', now())"
as_refused qt_algolens_app "$META, publish_source) VALUES ('2026-10-10', 'A', 'P', '{}', 'desk')"
as qt_algolens_app "$META) VALUES ('2026-10-10', 'A', 'P', '{}')" >/dev/null || fail "AlgoLens lost INSERT"
as qt_algolens_app "UPDATE trading.live_run_metadata SET portfolio_config = '{}' WHERE strategy_id = 'A'" >/dev/null \
    || fail "AlgoLens lost UPDATE of the other columns"
as qt_algolens_app "DELETE FROM trading.live_run_metadata WHERE strategy_id = 'A'" >/dev/null || fail "AlgoLens lost DELETE"
for role in fund_member quant_dev; do
    as "$role" "SELECT publish_source, sent_at FROM trading.live_run_metadata" >/dev/null || fail "$role cannot read"
    as_refused "$role" "UPDATE trading.live_run_metadata SET sent_at = now()"
done
as quant_dev_rw "UPDATE trading.live_run_metadata SET publish_source = publish_source WHERE date = '2026-10-09'" >/dev/null \
    || fail "quant_dev_rw lost UPDATE"
as_refused svc_airflow "SELECT sent_at FROM trading.live_run_metadata"
ok "svc_algolens reads publish_source and sent_at and writes neither; other roles as 025 left them"

# -- rollback -----------------------------------------------------------------------------------
refuses_with "\i $HERE/027_qt_daily_cutoff_rollback.sql" "027 rollback refused"
PGOPTIONS="-c client_min_messages=warning -c migration.force_rollback=yes" psql -X -q -v ON_ERROR_STOP=1 -d "$DB" \
    -f "$HERE/027_qt_daily_cutoff_rollback.sql" >/dev/null
apply 027_qt_daily_cutoff_rollback.sql
[[ "$(q "SELECT count(*) FROM information_schema.columns WHERE table_schema = 'trading' AND table_name = 'live_run_metadata' AND column_name IN ('publish_source', 'sent_at')")" == "0" ]] \
    || fail "rollback left a column"
[[ "$(q "SELECT count(*) FROM information_schema.role_table_grants WHERE grantee = 'svc_algolens' AND table_name = 'live_run_metadata' AND privilege_type IN ('INSERT', 'UPDATE')")" == "2" ]] \
    || fail "rollback did not restore svc_algolens' table-wide INSERT and UPDATE"
[[ "$(q "SELECT count(*) FROM pg_attribute a WHERE a.attrelid = 'trading.live_run_metadata'::regclass AND a.attacl IS NOT NULL")" == "0" ]] \
    || fail "rollback left column grants"
as qt_algolens_app "UPDATE trading.live_run_metadata SET portfolio_config = '{}' WHERE date = '2026-10-09'" >/dev/null \
    || fail "AlgoLens cannot update after the rollback"
ok "rollback refuses over publish records unless forced, restores svc_algolens' table grants; idempotent"

apply 027_qt_daily_cutoff.sql
[[ "$(q "SELECT count(*) FROM information_schema.role_table_grants WHERE grantee = 'svc_algolens' AND table_name = 'live_run_metadata' AND privilege_type IN ('INSERT', 'UPDATE')")" == "0" ]] \
    || fail "re-apply left svc_algolens table-wide writes"
as_refused qt_algolens_app "UPDATE trading.live_run_metadata SET sent_at = now()"
ok "027 re-applies after its rollback"

echo "PASS: 027 and its rollback"
