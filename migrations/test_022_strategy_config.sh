#!/usr/bin/env bash
# Verifies migration 022 (trading.strategy_config; settings_used, published_by and published_at on
# trading.live_run_metadata) and its rollback on a real PostgreSQL.
#   1. absent before (the table does not exist; an INSERT naming settings_used fails);
#   2. after: the eight strategy_config columns with their types; the constraints refuse a
#      non-object overrides, an empty or blank reason, a blank author, a version <= 0, a repeated
#      (portfolio, version) and a second active row of one portfolio, and allow one active row per
#      portfolio plus any number of inactive ones; the three live_run_metadata columns are NULL-able
#      with no default, every pre-existing row reads NULL, a settings_used object round-trips;
#   3. NOTHING ELSE MOVES on live_run_metadata: pre-existing rows byte-identical apart from the new
#      keys; its indexes and constraints as before;
#   4. idempotent; type-guarded (#60's integer created_by and a settings_used of another type each
#      make 022 refuse);
#   5. the rollback refuses while a strategy_config row or a settings_used value exists and, with
#      migration.force_rollback = 'yes', drops the table and the three columns, rows byte-identical;
#      idempotent.
# DESTRUCTIVE on the target (DROP SCHEMA trading CASCADE): a THROWAWAY database only; production and
# every clone refused by name, any database with the schema refused.
set -euo pipefail
[[ "${MIGRATION_TEST_DB:-}" == "" ]] && { echo "REFUSING: set MIGRATION_TEST_DB and PGDATABASE" >&2; exit 2; }
[[ "${PGDATABASE:-}" != "${MIGRATION_TEST_DB}" ]] && { echo "REFUSING: PGDATABASE != MIGRATION_TEST_DB" >&2; exit 2; }
case "${MIGRATION_TEST_DB}" in new_algo_data|new_algo_data_*|*_new_algo_data|algo_data|algo_data_*) echo "REFUSING: production or a clone" >&2; exit 2;; esac
existing=$(psql -X -tAc "SELECT count(*) FROM information_schema.schemata WHERE schema_name = 'trading'" 2>/dev/null || echo 0)
[[ "${existing:-0}" != "0" ]] && { echo "REFUSING: the target already has a trading schema" >&2; exit 2; }
HERE="$(cd "$(dirname "$0")" && pwd)"
PSQL="psql -v ON_ERROR_STOP=1 -q -X"
q() { psql -X -tAc "$1"; }
fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok: $*"; }
apply() { $PSQL -f "$HERE/$1"; }
refuses() { if psql -X -q -v ON_ERROR_STOP=1 -c "$1" >/dev/null 2>&1; then return 1; else return 0; fi; }
SC="INSERT INTO trading.strategy_config (portfolio_id, version, overrides, reason, created_by, is_active)"

$PSQL <<'SQL'
DROP SCHEMA IF EXISTS trading CASCADE;
CREATE SCHEMA trading;
CREATE TABLE trading.live_run_metadata (id SERIAL PRIMARY KEY, date DATE NOT NULL, strategy_id VARCHAR(100) NOT NULL,
    portfolio_id VARCHAR(100) NOT NULL, strategy_allocations JSONB, portfolio_config JSONB, strategy_configs JSONB,
    created_at TIMESTAMPTZ DEFAULT now(),
    CONSTRAINT live_run_metadata_key UNIQUE (date, strategy_id, portfolio_id));
CREATE INDEX idx_live_run_metadata_portfolio ON trading.live_run_metadata (portfolio_id, date);
INSERT INTO trading.live_run_metadata (date, strategy_id, portfolio_id, strategy_allocations, portfolio_config, created_at)
VALUES ('2026-10-06','LIVE_TREND_FOLLOWING','BASE_PORTFOLIO','{"TREND_FOLLOWING": 1}','{"total_capital": 500000}','2026-10-06 22:00:00+00'),
       ('2026-10-07','LIVE_TREND_FOLLOWING','BASE_PORTFOLIO','{"TREND_FOLLOWING": 1}','{"risk_refusal": {"module": "carver"}}','2026-10-07 22:00:00+00');
SQL
fp() { q "select count(*)||' '||md5(coalesce(string_agg(x, ';' order by x), '')) from (select (to_jsonb(t) - 'settings_used' - 'published_by' - 'published_at')::text x from trading.live_run_metadata t) s"; }
B1=$(fp)
I1=$(q "select string_agg(indexdef, ' | ' order by indexdef) from pg_indexes where schemaname = 'trading' and tablename = 'live_run_metadata'")
C1=$(q "select string_agg(conname, ',' order by conname) from pg_constraint where conrelid = 'trading.live_run_metadata'::regclass")
# 1
[[ "$(q "select to_regclass('trading.strategy_config') is null")" == "t" ]] || fail "strategy_config exists before 022"
refuses "UPDATE trading.live_run_metadata SET settings_used = '{}'" || fail "settings_used exists before 022"
pass "1. strategy_config and the live_run_metadata columns are absent before the migration"
# 2
apply 022_strategy_config.sql
COLS="created_at:timestamp with time zone:NO,created_by:text:NO,id:bigint:NO,is_active:boolean:NO,overrides:jsonb:NO,portfolio_id:text:NO,reason:text:NO,version:integer:NO"
[[ "$(q "select string_agg(column_name||':'||data_type||':'||is_nullable, ',' order by column_name) from information_schema.columns where table_schema='trading' and table_name='strategy_config'")" == "$COLS" ]] || fail "strategy_config columns"
[[ "$(q "select column_default from information_schema.columns where table_schema='trading' and table_name='strategy_config' and column_name='is_active'")" == "false" ]] || fail "is_active default"
$PSQL -c "$SC VALUES ('BASE_PORTFOLIO', 1, '{\"risk\": {\"max_drawdown\": 0.35}}', 'desk test', 'dom', true)"
$PSQL -c "$SC VALUES ('BASE_PORTFOLIO', 2, '{}', 'a later draft', 'dom', false)"
$PSQL -c "$SC VALUES ('BASE_PORTFOLIO', 3, '{}', 'another draft', 'dom', false)"
$PSQL -c "$SC VALUES ('CONSERVATIVE_PORTFOLIO', 1, '{}', 'its own active row', 'dom', true)"
refuses "$SC VALUES ('BASE_PORTFOLIO', 4, '[]', 'array', 'dom', false)" || fail "an array overrides was accepted"
refuses "$SC VALUES ('BASE_PORTFOLIO', 4, 'null', 'json null', 'dom', false)" || fail "a json null overrides was accepted"
refuses "$SC VALUES ('BASE_PORTFOLIO', 4, '1', 'scalar', 'dom', false)" || fail "a scalar overrides was accepted"
refuses "$SC VALUES ('BASE_PORTFOLIO', 4, NULL, 'sql null', 'dom', false)" || fail "a NULL overrides was accepted"
refuses "$SC VALUES ('BASE_PORTFOLIO', 4, '{}', '', 'dom', false)" || fail "an empty reason was accepted"
refuses "$SC VALUES ('BASE_PORTFOLIO', 4, '{}', '   ', 'dom', false)" || fail "a blank reason was accepted"
refuses "$SC VALUES ('BASE_PORTFOLIO', 4, '{}', 'no author', '  ', false)" || fail "a blank author was accepted"
refuses "$SC VALUES ('BASE_PORTFOLIO', 4, '{}', 'no author', NULL, false)" || fail "a NULL author was accepted"
refuses "$SC VALUES ('BASE_PORTFOLIO', 0, '{}', 'version zero', 'dom', false)" || fail "version 0 was accepted"
refuses "$SC VALUES ('BASE_PORTFOLIO', 1, '{}', 'repeat version', 'dom', false)" || fail "a repeated version was accepted"
refuses "$SC VALUES ('BASE_PORTFOLIO', 4, '{}', 'second active', 'dom', true)" || fail "a second active row was accepted"
refuses "UPDATE trading.strategy_config SET is_active = true WHERE portfolio_id = 'BASE_PORTFOLIO' AND version = 2" || fail "activating a second row was accepted"
# Switching the active version is two statements in one transaction.
$PSQL -c "BEGIN; UPDATE trading.strategy_config SET is_active = false WHERE portfolio_id = 'BASE_PORTFOLIO' AND is_active; UPDATE trading.strategy_config SET is_active = true WHERE portfolio_id = 'BASE_PORTFOLIO' AND version = 2; COMMIT;"
[[ "$(q "select version from trading.strategy_config where portfolio_id = 'BASE_PORTFOLIO' and is_active")" == "2" ]] || fail "the active version did not switch"
[[ "$(q "select (overrides #>> '{risk,max_drawdown}') from trading.strategy_config where portfolio_id = 'BASE_PORTFOLIO' and version = 1")" == "0.35" ]] || fail "overrides do not round-trip"
for c in "settings_used jsonb" "published_by text" "published_at timestamp with time zone"; do set -- $c; col=$1; shift
  [[ "$(q "select data_type||' '||is_nullable||' '||coalesce(column_default,'-') from information_schema.columns where table_schema='trading' and table_name='live_run_metadata' and column_name='$col'")" == "$* YES -" ]] || fail "live_run_metadata.$col is not $* NULL with no default"
  [[ "$(q "select count(*) from trading.live_run_metadata where $col is not null")" == "0" ]] || fail "live_run_metadata.$col history is not NULL"
done
$PSQL -c "INSERT INTO trading.live_run_metadata (date, strategy_id, portfolio_id) VALUES ('2026-10-08','LIVE_TREND_FOLLOWING','BASE_PORTFOLIO')"
[[ "$(q "select count(*) from trading.live_run_metadata where date='2026-10-08' and settings_used is null and published_by is null and published_at is null")" == "1" ]] || fail "an omitting INSERT is not NULL"
$PSQL -c "UPDATE trading.live_run_metadata SET settings_used = '{\"strategy_config_version\": 2, \"config\": {\"portfolio_id\": \"BASE_PORTFOLIO\"}}' WHERE date='2026-10-08'"
[[ "$(q "select (settings_used->>'strategy_config_version')||'|'||(settings_used#>>'{config,portfolio_id}') from trading.live_run_metadata where date='2026-10-08'")" == "2|BASE_PORTFOLIO" ]] || fail "settings_used does not round-trip"
pass "2. strategy_config's columns and constraints (object, reason, author, version, one active per portfolio); the three columns NULL-able, history NULL, settings_used round-trips"
# 3
$PSQL -c "DELETE FROM trading.live_run_metadata WHERE date = '2026-10-08'"
[[ "$(fp)" == "$B1" ]] || fail "live_run_metadata rows moved"
[[ "$(q "select string_agg(indexdef, ' | ' order by indexdef) from pg_indexes where schemaname = 'trading' and tablename = 'live_run_metadata'")" == "$I1" ]] || fail "live_run_metadata indexes changed"
[[ "$(q "select string_agg(conname, ',' order by conname) from pg_constraint where conrelid = 'trading.live_run_metadata'::regclass")" == "$C1" ]] || fail "live_run_metadata constraints changed"
pass "3. every pre-existing live_run_metadata row byte-identical apart from the new keys; its indexes and constraints unchanged"
# 4
apply 022_strategy_config.sql
[[ "$(q "select count(*) from trading.strategy_config")" == "4" ]] || fail "re-applying 022 moved strategy_config rows"
$PSQL -c "ALTER TABLE trading.live_run_metadata ALTER COLUMN settings_used TYPE text USING NULL"
if psql -X -q -v ON_ERROR_STOP=1 -f "$HERE/022_strategy_config.sql" >/dev/null 2>&1; then fail "022 type guard (settings_used) did not refuse"; fi
$PSQL -c "ALTER TABLE trading.live_run_metadata ALTER COLUMN settings_used TYPE jsonb USING NULL"
$PSQL -c "ALTER TABLE trading.strategy_config RENAME TO strategy_config_keep"
$PSQL -c "CREATE TABLE trading.strategy_config (id BIGSERIAL PRIMARY KEY, portfolio_id TEXT NOT NULL, version INTEGER NOT NULL, overrides JSONB NOT NULL, reason TEXT NOT NULL, created_by INTEGER, is_active BOOLEAN NOT NULL DEFAULT FALSE, created_at TIMESTAMPTZ NOT NULL DEFAULT now())"
if psql -X -q -v ON_ERROR_STOP=1 -f "$HERE/022_strategy_config.sql" >/dev/null 2>&1; then fail "022 did not refuse #60's strategy_config (integer created_by)"; fi
$PSQL -c "DROP TABLE trading.strategy_config; ALTER TABLE trading.strategy_config_keep RENAME TO strategy_config"
apply 022_strategy_config.sql
pass "4. idempotent; a settings_used of another type and #60's integer created_by each make 022 refuse"
# 5
$PSQL -c "UPDATE trading.live_run_metadata SET settings_used = '{\"strategy_config_version\": null, \"config\": {}}' WHERE date = '2026-10-06'"
if psql -X -q -v ON_ERROR_STOP=1 -f "$HERE/022_strategy_config_rollback.sql" >/dev/null 2>&1; then fail "the rollback did not refuse while data exists"; fi
[[ "$(q "select to_regclass('trading.strategy_config') is not null")" == "t" ]] || fail "a refused rollback dropped the table"
$PSQL -c "UPDATE trading.live_run_metadata SET settings_used = NULL; DELETE FROM trading.strategy_config WHERE portfolio_id = 'CONSERVATIVE_PORTFOLIO'"
if psql -X -q -v ON_ERROR_STOP=1 -f "$HERE/022_strategy_config_rollback.sql" >/dev/null 2>&1; then fail "the rollback did not refuse while strategy_config rows exist"; fi
psql -X -q -v ON_ERROR_STOP=1 -c "SET migration.force_rollback = 'yes'" -f "$HERE/022_strategy_config_rollback.sql"
[[ "$(q "select to_regclass('trading.strategy_config') is null")" == "t" ]] || fail "the forced rollback left the table"
[[ "$(q "select count(*) from information_schema.columns where table_schema='trading' and table_name='live_run_metadata' and column_name in ('settings_used','published_by','published_at')")" == "0" ]] || fail "the forced rollback left a column"
[[ "$(fp)" == "$B1" ]] || fail "rows differ after the rollback"
[[ "$(q "select string_agg(indexdef, ' | ' order by indexdef) from pg_indexes where schemaname = 'trading' and tablename = 'live_run_metadata'")" == "$I1" ]] || fail "indexes differ after the rollback"
apply 022_strategy_config_rollback.sql
pass "5. the rollback refuses while data exists; forced, it drops the table and the three columns, rows byte-identical; idempotent"
echo "ALL OK"
