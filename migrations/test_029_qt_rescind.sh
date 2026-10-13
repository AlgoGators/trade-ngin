#!/usr/bin/env bash
# Verifies migration 029 (Rescind, contract C8: the 'rescind' kind, the dropped one-done-publish
# index, one open rescind per day) and its rollback on a real PostgreSQL (16), on the fixture of
# test_025_qt_command_log_hardening.sh (new_algo_data's roles, memberships and grants as of
# 2026-10-09) with 023, 025 and 027 applied first, as on new_algo_data.
#
#   docker run -d --name pg029 -e POSTGRES_PASSWORD=x postgres:16
#   docker cp migrations pg029:/m && docker exec -e QT_MIGRATION_TEST_THROWAWAY=1 -u postgres #       pg029 bash /m/test_029_qt_rescind.sh
#
# It creates CLUSTER-WIDE ROLES (fund_member, svc_algolens, qt_engine_app, ...), so it runs only
# on a throwaway server: QT_MIGRATION_TEST_THROWAWAY=1 is required, and a server that has a
# new_algo_data or algo_data database, or already has those roles, is refused. It creates and
# drops its own database (qt_m029_test_<pid>).
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
DB="qt_m029_test_$$"
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

apply 027_qt_daily_cutoff.sql
ok "027 applied"

INS="INSERT INTO trading.position_overrides (portfolio_id, date, kind, requested_by"
P="'QT_CONSERVATIVE_PORTFOLIO', '2026-10-13'"
finish() { as qt_engine_app "UPDATE trading.position_overrides SET status = 'running', started_at = now() WHERE id = $1" >/dev/null
           as qt_engine_app "UPDATE trading.position_overrides SET status = '$2', finished_at = now() WHERE id = $1" >/dev/null; }

# Before 029: no rescind, and a second done publish for a day is refused (023).
refuses_with "SET ROLE qt_algolens_app; $INS) VALUES ($P, 'rescind', 'd@x.com')" "check"
A=$(as qt_algolens_app "$INS) VALUES ($P, 'publish', 'd@x.com') RETURNING id"); finish "$A" done
B=$(as qt_algolens_app "$INS) VALUES ($P, 'publish', 'd@x.com') RETURNING id")
as qt_engine_app "UPDATE trading.position_overrides SET status = 'running', started_at = now() WHERE id = $B" >/dev/null
refuses_with "SET ROLE qt_engine_app; UPDATE trading.position_overrides SET status = 'done', finished_at = now() WHERE id = $B" "position_overrides_one_publish"
as qt_engine_app "UPDATE trading.position_overrides SET status = 'failed', finished_at = now() WHERE id = $B" >/dev/null
ok "before 029: no rescind kind; one done publish per day"

apply 029_qt_rescind.sql
apply 029_qt_rescind.sql
[[ "$(q "SELECT count(*) FROM pg_constraint WHERE conrelid = 'trading.position_overrides'::regclass AND contype = 'c' AND pg_get_constraintdef(oid) LIKE '%kind = ANY%'")" == "1" ]]     || fail "exactly one kind CHECK after 029"
[[ -z "$(q "SELECT to_regclass('trading.position_overrides_one_publish')")" ]] || fail "one_publish still there"
ok "029 applies, and re-applies"

# The desk rescinds, then approves again: a second done publish row for the day.
R=$(as qt_algolens_app "$INS) VALUES ($P, 'rescind', 'd@x.com') RETURNING id")
refuses_with "SET ROLE qt_algolens_app; $INS) VALUES ($P, 'rescind', 'd@x.com')" "position_overrides_one_open_rescind"
finish "$R" done
R2=$(as qt_algolens_app "$INS) VALUES ($P, 'rescind', 'd@x.com') RETURNING id"); finish "$R2" refused
C=$(as qt_algolens_app "$INS) VALUES ($P, 'publish', 'd@x.com') RETURNING id"); finish "$C" done
[[ "$(q "SELECT count(*) FROM trading.position_overrides WHERE kind = 'publish' AND status = 'done' AND date = '2026-10-13'")" == "2" ]]     || fail "the day's second approval"
refuses_with "$INS) VALUES ($P, 'unpublish', 'd@x.com')" "position_overrides_kind"
refuses_with "UPDATE trading.position_overrides SET status = 'pending' WHERE id = $R" "final"
refuses "DELETE FROM trading.position_overrides WHERE id = $R" || fail "a rescind row was deleted"
ok "svc_algolens inserts rescind rows, one open per day; a rescinded day is approved again; rows stay final"

# -- rollback -----------------------------------------------------------------------------------
psql -X -q -v ON_ERROR_STOP=1 -d "$DB" -f "$HERE/029_qt_rescind_rollback.sql" >/dev/null 2>&1     && fail "rollback over rescind rows"
ok "rollback refuses while rescind rows exist"

DB2="qt_m029_rb_$$"
PGOPTIONS="-c client_min_messages=warning" psql -X -q -d postgres -c "DROP DATABASE IF EXISTS $DB2" >/dev/null
psql -X -q -d postgres -c "CREATE DATABASE $DB2 TEMPLATE $DB" >/dev/null
# A copy without the rescind rows, where the day was never approved twice: the rows are
# removed with the triggers off (the log is never pruned in production).
psql -X -q -v ON_ERROR_STOP=1 -d "$DB2" >/dev/null <<'SQL'
SET session_replication_role = replica;
DELETE FROM trading.position_overrides WHERE kind = 'rescind' OR (kind = 'publish' AND status = 'done' AND id = (SELECT max(id) FROM trading.position_overrides WHERE kind = 'publish' AND status = 'done'));
SQL
PGOPTIONS="-c client_min_messages=warning" psql -X -q -v ON_ERROR_STOP=1 -d "$DB2" -f "$HERE/029_qt_rescind_rollback.sql" >/dev/null
PGOPTIONS="-c client_min_messages=warning" psql -X -q -v ON_ERROR_STOP=1 -d "$DB2" -f "$HERE/029_qt_rescind_rollback.sql" >/dev/null
[[ -n "$(psql -X -tAc "SELECT to_regclass('trading.position_overrides_one_publish')" -d "$DB2")" ]] || fail "rollback: one_publish"
! psql -X -q -d "$DB2" -c "INSERT INTO trading.position_overrides (portfolio_id, date, kind, requested_by) VALUES ('Q', '2026-10-14', 'rescind', 'd')" >/dev/null 2>&1     || fail "rollback: rescind still accepted"
PGOPTIONS="-c client_min_messages=warning" psql -X -q -v ON_ERROR_STOP=1 -d "$DB2" -f "$HERE/029_qt_rescind.sql" >/dev/null
psql -X -q -d postgres -c "DROP DATABASE IF EXISTS $DB2" >/dev/null
ok "rollback restores 023's CHECK and index, is idempotent, and 029 re-applies"

echo "PASS: 029 and its rollback"
