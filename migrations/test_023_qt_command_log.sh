#!/usr/bin/env bash
# Verifies migrations 023 (the QT command log, live_results.book_source, the registry columns and QT
# portfolios) and 024 (the ruling-28 drops), and the 023 rollback, on a real PostgreSQL.
#
#   PGHOST=... PGPORT=... PGUSER=... PGPASSWORD=... migrations/test_023_qt_command_log.sh
#
# It creates and drops its own database (qt_m023_test_<pid>); it never touches another database.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DB="qt_m023_test_$$"
fail() { echo "FAIL: $*" >&2; exit 1; }
q() { psql -X -q -v ON_ERROR_STOP=1 -d "$DB" -tAc "$1"; }
refuses() { ! psql -X -q -v ON_ERROR_STOP=1 -d "$DB" -c "$1" >/dev/null 2>&1; }

psql -X -q -d postgres -c "CREATE DATABASE $DB" >/dev/null
trap 'psql -X -q -d postgres -c "DROP DATABASE IF EXISTS $DB" >/dev/null' EXIT

psql -X -q -v ON_ERROR_STOP=1 -d "$DB" <<'SQL'
CREATE SCHEMA trading;
CREATE TABLE trading.live_results (id SERIAL PRIMARY KEY, strategy_id VARCHAR(100) NOT NULL, portfolio_id VARCHAR(100) NOT NULL,
    date DATE NOT NULL, risk_detail jsonb);
CREATE TABLE trading.strategy_registry (id text PRIMARY KEY, strategy_type text NOT NULL, portfolio_id text NOT NULL, name text,
    description text, initial_equity numeric, managers jsonb, is_active boolean DEFAULT true, sort_order integer DEFAULT 0,
    created_at timestamptz DEFAULT now(), updated_at timestamptz DEFAULT now(), lifecycle text,
    incubation_started_at timestamptz, mock_capital numeric);
INSERT INTO trading.strategy_registry (id, strategy_type, portfolio_id, name, lifecycle)
    VALUES ('trendfollowing', 'LIVE_TREND_FOLLOWING', 'CONSERVATIVE_PORTFOLIO', 'Trend Following', 'live');
-- The #55 shape, with its rules.
CREATE TABLE trading.position_overrides (id BIGSERIAL PRIMARY KEY, user_id integer, source_app text, strategy_id text,
    symbol text, before_state jsonb, after_state jsonb, reason text, risk_check_result jsonb, overrode_risk boolean,
    created_at timestamptz DEFAULT now());
CREATE RULE position_overrides_no_update AS ON UPDATE TO trading.position_overrides DO INSTEAD NOTHING;
CREATE RULE position_overrides_no_delete AS ON DELETE TO trading.position_overrides DO INSTEAD NOTHING;
CREATE TABLE trading.risk_limits (id serial PRIMARY KEY);
CREATE TABLE trading.portfolios (id serial PRIMARY KEY);
CREATE TABLE trading.strategy_book_memberships (id serial PRIMARY KEY);
CREATE TABLE trading.portfolio_assignments (id serial PRIMARY KEY);
SQL

# Refuses on a non-empty old log.
q "INSERT INTO trading.position_overrides (reason) VALUES ('x')"
psql -X -q -v ON_ERROR_STOP=1 -d "$DB" -f "$HERE/023_qt_command_log.sql" >/dev/null 2>&1 && fail "023 applied over a non-empty position_overrides"
q "DROP RULE position_overrides_no_delete ON trading.position_overrides; DELETE FROM trading.position_overrides"

psql -X -q -v ON_ERROR_STOP=1 -d "$DB" -f "$HERE/023_qt_command_log.sql" >/dev/null
# Idempotent re-apply of the additive parts must not fail (position_overrides is empty).
psql -X -q -v ON_ERROR_STOP=1 -d "$DB" -f "$HERE/023_qt_command_log.sql" >/dev/null

# Command log shape.
q "INSERT INTO trading.position_overrides (portfolio_id, date, kind, requested_by, reason, payload)
   VALUES ('QT_CONSERVATIVE_PORTFOLIO', '2026-10-08', 'save', 'dom@x', 'trim ES', '{\"changes\":[]}')"
refuses "INSERT INTO trading.position_overrides (portfolio_id, date, kind, requested_by) VALUES ('P','2026-10-08','save','dom@x')" \
    || fail "save without a reason accepted"
refuses "INSERT INTO trading.position_overrides (portfolio_id, date, kind, requested_by, reason) VALUES ('P','2026-10-08','bogus','d','r')" \
    || fail "unknown kind accepted"
refuses "INSERT INTO trading.position_overrides (portfolio_id, date, kind, requested_by) VALUES ('P','2026-10-08','override_decision','d')" \
    || fail "decision without parent and role accepted"
q "UPDATE trading.position_overrides SET status='done', result='{\"ok\":true}', finished_at=now() WHERE id=1"
[ "$(q "SELECT status FROM trading.position_overrides WHERE id=1")" = "done" ] || fail "engine status update did not stick"
refuses "UPDATE trading.position_overrides SET reason='edited' WHERE id=1" || fail "reason edit accepted"
refuses "DELETE FROM trading.position_overrides WHERE id=1" || fail "delete accepted"
q "INSERT INTO trading.position_overrides (portfolio_id, date, kind, requested_by, status) VALUES ('P','2026-10-08','publish','d','done')"
refuses "INSERT INTO trading.position_overrides (portfolio_id, date, kind, requested_by, status) VALUES ('P','2026-10-08','publish','d','done')" \
    || fail "second done publish for the same day accepted"

# book_source, registry, drops.
q "INSERT INTO trading.live_results (strategy_id, portfolio_id, date, book_source) VALUES ('S','P','2026-10-08','desk')"
refuses "INSERT INTO trading.live_results (strategy_id, portfolio_id, date, book_source) VALUES ('S','P','2026-10-09','other')" \
    || fail "bad book_source accepted"
[ "$(q "SELECT count(*) FROM trading.strategy_registry WHERE portfolio_group='qt_conservative'")" = "2" ] || fail "QT portfolios missing"
[ "$(q "SELECT desk_editable FROM trading.strategy_registry WHERE id='trendfollowing'")" = "f" ] || fail "desk_editable default"
psql -X -q -v ON_ERROR_STOP=1 -d "$DB" -f "$HERE/024_drop_retired_qt_tables.sql" >/dev/null
for t in risk_limits portfolios strategy_book_memberships portfolio_assignments; do
    [ -z "$(q "SELECT to_regclass('trading.$t')")" ] || fail "trading.$t not dropped"
done

# Rollback refuses while the log holds rows; succeeds once empty (test-only cleanup bypasses the guard).
psql -X -q -v ON_ERROR_STOP=1 -d "$DB" -f "$HERE/023_qt_command_log_rollback.sql" >/dev/null 2>&1 && fail "rollback ran over a non-empty log"
q "ALTER TABLE trading.position_overrides DISABLE TRIGGER position_overrides_guard; DELETE FROM trading.position_overrides"
psql -X -q -v ON_ERROR_STOP=1 -d "$DB" -f "$HERE/023_qt_command_log_rollback.sql" >/dev/null
[ -z "$(q "SELECT to_regclass('trading.position_overrides')")" ] || fail "rollback left position_overrides"
[ "$(q "SELECT count(*) FROM information_schema.columns WHERE table_schema='trading' AND column_name IN ('book_source','portfolio_group','desk_editable')")" = "0" ] \
    || fail "rollback left columns"

echo "PASS: 023 and its rollback"
