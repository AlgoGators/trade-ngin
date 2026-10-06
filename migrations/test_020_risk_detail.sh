#!/usr/bin/env bash
# Verifies migration 020 (risk_detail jsonb on trading.live_results and backtest.equity_curve, and
# the COMMENT re-pointing trading.live_results.risk_scale) and its rollback on a real PostgreSQL.
#   1. absent before (an INSERT naming the column fails on both tables: the migration is necessary);
#   2. after: type jsonb, NULL-able, no default; every pre-existing row NULL; an omitting INSERT NULL;
#      the nine-key object round-trips on both tables and its keys read back;
#   3. NOTHING ELSE MOVES: pre-existing rows byte-identical apart from the new key (risk_scale's
#      values included); indexes and constraints as before;
#   4. idempotent; type-guarded (a risk_detail of another type makes 020 refuse); refuses when
#      risk_scale is missing;
#   5. the three comments set, risk_scale's naming the delivered scale;
#   6. the rollback refuses while a value exists and, with migration.force_rollback = 'yes', drops
#      the column on both tables with the original rows byte-identical and risk_scale kept.
# DESTRUCTIVE on the target (DROP SCHEMA trading/backtest CASCADE): a THROWAWAY database only;
# production and every stage-3 scratch or clone refused by name, any database with the schemas refused.
set -euo pipefail
[[ "${MIGRATION_TEST_DB:-}" == "" ]] && { echo "REFUSING: set MIGRATION_TEST_DB and PGDATABASE" >&2; exit 2; }
[[ "${PGDATABASE:-}" != "${MIGRATION_TEST_DB}" ]] && { echo "REFUSING: PGDATABASE != MIGRATION_TEST_DB" >&2; exit 2; }
case "${MIGRATION_TEST_DB}" in new_algo_data|new_algo_data_*|*_new_algo_data) echo "REFUSING: production or a clone" >&2; exit 2;; esac
existing=$(psql -X -tAc "SELECT count(*) FROM information_schema.schemata WHERE schema_name IN ('trading','backtest')" 2>/dev/null || echo 0)
[[ "${existing:-0}" != "0" ]] && { echo "REFUSING: the target already has a trading or backtest schema" >&2; exit 2; }
HERE="$(cd "$(dirname "$0")" && pwd)"
PSQL="psql -v ON_ERROR_STOP=1 -q -X"
q() { psql -X -tAc "$1"; }
fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok: $*"; }
apply() { $PSQL -f "$HERE/$1"; }
refuses() { if psql -X -q -v ON_ERROR_STOP=1 -c "$1" >/dev/null 2>&1; then return 1; else return 0; fi; }
DETAIL='{"risk_requested": 0.9959162522254, "binding_term": "R_shock", "over_limit_after_rounding_terms": "R;L_g", "over_limit_after_rounding_excess": 0.25, "over_limit_by_hold_terms": "CAP", "over_limit_by_hold_symbols": "ZN.v.0 ZT.v.0", "overlay_blind": false, "sizing_capital": 487250.5, "account_value": 512300.25}'

$PSQL <<'SQL'
DROP SCHEMA IF EXISTS trading CASCADE; DROP SCHEMA IF EXISTS backtest CASCADE;
CREATE SCHEMA trading; CREATE SCHEMA backtest;
CREATE TABLE trading.live_results (id SERIAL PRIMARY KEY, strategy_id VARCHAR(100) NOT NULL, portfolio_id VARCHAR(100) NOT NULL,
    date DATE NOT NULL, risk_scale NUMERIC, portfolio_var NUMERIC, config JSONB,
    CONSTRAINT live_results_key UNIQUE (portfolio_id, strategy_id, date));
CREATE INDEX idx_live_results_date ON trading.live_results (date);
CREATE TABLE backtest.equity_curve (id SERIAL PRIMARY KEY, run_id TEXT NOT NULL, "timestamp" TIMESTAMPTZ NOT NULL,
    equity DOUBLE PRECISION NOT NULL, portfolio_id VARCHAR(100));
CREATE INDEX idx_equity_curve_run ON backtest.equity_curve (run_id);
INSERT INTO trading.live_results (strategy_id, portfolio_id, date, risk_scale, portfolio_var, config)
VALUES ('S','P','2026-04-24',0.87,0.11,'{"a": 1}'), ('E','EQ','2026-04-24',1.0,0.02,NULL);
INSERT INTO backtest.equity_curve (run_id, "timestamp", equity, portfolio_id)
VALUES ('R1','2024-05-03 04:00:00+00',500000,'P'), ('R1','2024-05-05 10:00:00+00',500012.5,'P');
SQL
fp() { q "select count(*)||' '||md5(coalesce(string_agg(x, ';' order by x), '')) from (select (to_jsonb(t) - 'risk_detail')::text x from $1 t) s"; }
B1=$(fp trading.live_results); B2=$(fp backtest.equity_curve)
I1=$(q "select string_agg(indexdef, ' | ' order by indexdef) from pg_indexes where schemaname in ('trading','backtest')")
C1=$(q "select count(*) from pg_constraint c join pg_namespace n on n.oid = c.connamespace where n.nspname in ('trading','backtest')")
# 1
refuses "INSERT INTO trading.live_results (strategy_id, portfolio_id, date, risk_detail) VALUES ('S','P',current_date,'{}')" || fail "live_results.risk_detail exists before 020"
refuses "INSERT INTO backtest.equity_curve (run_id, \"timestamp\", equity, risk_detail) VALUES ('R1', now(), 1, '{}')" || fail "equity_curve.risk_detail exists before 020"
pass "1. the column is absent on both tables before the migration"
# 2
apply 020_risk_detail.sql
for t in "trading live_results" "backtest equity_curve"; do set -- $t
  [[ "$(q "select data_type||' '||is_nullable||' '||coalesce(column_default,'-') from information_schema.columns where table_schema='$1' and table_name='$2' and column_name='risk_detail'")" == "jsonb YES -" ]] || fail "$1.$2.risk_detail is not jsonb NULL with no default"
  [[ "$(q "select count(*) from $1.$2 where risk_detail is not null")" == "0" ]] || fail "$1.$2 history is not NULL"
done
$PSQL -c "INSERT INTO trading.live_results (strategy_id, portfolio_id, date, risk_scale) VALUES ('S','P','2026-04-25',1.21)"
$PSQL -c "INSERT INTO backtest.equity_curve (run_id, \"timestamp\", equity, portfolio_id) VALUES ('R1','2024-05-06 10:00:00+00',500020,'P')"
[[ "$(q "select count(*) from trading.live_results where date='2026-04-25' and risk_detail is null")" == "1" ]] || fail "an omitting live INSERT is not NULL"
[[ "$(q "select count(*) from backtest.equity_curve where equity=500020 and risk_detail is null")" == "1" ]] || fail "an omitting curve INSERT is not NULL"
$PSQL -c "INSERT INTO trading.live_results (strategy_id, portfolio_id, date, risk_scale, risk_detail) VALUES ('S','P','2026-04-26',1.21,'$DETAIL'::jsonb)"
$PSQL -c "INSERT INTO backtest.equity_curve (run_id, \"timestamp\", equity, portfolio_id, risk_detail) VALUES ('R1','2024-05-07 10:00:00+00',500030,'P','$DETAIL'::jsonb)"
KEYS="account_value,binding_term,over_limit_after_rounding_excess,over_limit_after_rounding_terms,over_limit_by_hold_symbols,over_limit_by_hold_terms,overlay_blind,risk_requested,sizing_capital"
[[ "$(q "select string_agg(k, ',' order by k) from trading.live_results, jsonb_object_keys(risk_detail) k where date='2026-04-26'")" == "$KEYS" ]] || fail "the nine keys do not read back (live)"
[[ "$(q "select string_agg(k, ',' order by k) from backtest.equity_curve, jsonb_object_keys(risk_detail) k where equity=500030")" == "$KEYS" ]] || fail "the nine keys do not read back (curve)"
[[ "$(q "select (risk_detail->>'risk_requested')::numeric||'|'||(risk_detail->>'binding_term')||'|'||(risk_detail->>'overlay_blind')||'|'||(risk_detail->>'sizing_capital')::numeric||'|'||(risk_detail->>'account_value')::numeric from trading.live_results where date='2026-04-26'")" == "0.9959162522254|R_shock|false|487250.5|512300.25" ]] || fail "values do not round-trip (live)"
[[ "$(q "select (risk_detail->>'over_limit_by_hold_symbols')||'|'||(risk_detail->>'over_limit_after_rounding_terms') from backtest.equity_curve where equity=500030")" == "ZN.v.0 ZT.v.0|R;L_g" ]] || fail "values do not round-trip (curve)"
pass "2. jsonb NULL no default on both tables; history NULL; omitting INSERTs NULL; the nine keys and their values round-trip"
# 3
$PSQL -c "DELETE FROM trading.live_results WHERE date >= '2026-04-25'; DELETE FROM backtest.equity_curve WHERE equity >= 500020"
[[ "$(fp trading.live_results)" == "$B1" && "$(fp backtest.equity_curve)" == "$B2" ]] || fail "rows moved"
[[ "$(q "select string_agg(indexdef, ' | ' order by indexdef) from pg_indexes where schemaname in ('trading','backtest')")" == "$I1" ]] || fail "indexes changed"
[[ "$(q "select count(*) from pg_constraint c join pg_namespace n on n.oid = c.connamespace where n.nspname in ('trading','backtest')")" == "$C1" ]] || fail "constraints changed"
[[ "$(q "select string_agg(risk_scale::text, ',' order by id) from trading.live_results")" == "0.87,1.0" ]] || fail "risk_scale values moved"
pass "3. every pre-existing row byte-identical apart from the new key; risk_scale's values, indexes and constraints unchanged"
# 4
apply 020_risk_detail.sql
$PSQL -c "ALTER TABLE backtest.equity_curve ALTER COLUMN risk_detail TYPE text USING NULL"
if psql -X -q -v ON_ERROR_STOP=1 -f "$HERE/020_risk_detail.sql" >/dev/null 2>&1; then fail "020 type guard did not refuse"; fi
$PSQL -c "ALTER TABLE backtest.equity_curve ALTER COLUMN risk_detail TYPE jsonb USING NULL"
$PSQL -c "ALTER TABLE trading.live_results RENAME COLUMN risk_scale TO risk_scale_x"
if psql -X -q -v ON_ERROR_STOP=1 -f "$HERE/020_risk_detail.sql" >/dev/null 2>&1; then fail "020 did not refuse a table without risk_scale"; fi
$PSQL -c "ALTER TABLE trading.live_results RENAME COLUMN risk_scale_x TO risk_scale"
apply 020_risk_detail.sql
pass "4. idempotent; a risk_detail of another type and a missing risk_scale each make 020 refuse"
# 5
cd() { q "select col_description('$1.$2'::regclass,(select ordinal_position from information_schema.columns where table_schema='$1' and table_name='$2' and column_name='$3'))"; }
[[ "$(cd trading live_results risk_detail)" == *"risk_requested"*"account_value"*"migration 020"* ]] || fail "live_results.risk_detail comment"
[[ "$(cd backtest equity_curve risk_detail)" == *"sized rebalance"*"migration 020"* ]] || fail "equity_curve.risk_detail comment"
[[ "$(cd trading live_results risk_scale)" == *"DELIVERED scale"*"risk_detail.risk_requested"*"migration 020"* ]] || fail "risk_scale comment"
pass "5. the three comments set; risk_scale's names the delivered scale"
# 6
$PSQL -c "UPDATE trading.live_results SET risk_detail = '$DETAIL'::jsonb WHERE strategy_id = 'S'"
if psql -X -q -v ON_ERROR_STOP=1 -f "$HERE/020_risk_detail_rollback.sql" >/dev/null 2>&1; then fail "the rollback did not refuse while a value exists"; fi
[[ "$(q "select count(*) from information_schema.columns where column_name='risk_detail' and table_schema in ('trading','backtest')")" == "2" ]] || fail "a refused rollback dropped a column"
psql -X -q -v ON_ERROR_STOP=1 -c "SET migration.force_rollback = 'yes'" -f "$HERE/020_risk_detail_rollback.sql"
[[ "$(q "select count(*) from information_schema.columns where column_name='risk_detail' and table_schema in ('trading','backtest')")" == "0" ]] || fail "the forced rollback left a column"
[[ "$(fp trading.live_results)" == "$B1" && "$(fp backtest.equity_curve)" == "$B2" ]] || fail "rows differ after the rollback"
[[ "$(cd trading live_results risk_scale)" == "" ]] || fail "the rollback left the risk_scale comment"
[[ "$(q "select string_agg(risk_scale::text, ',' order by id) from trading.live_results")" == "0.87,1.0" ]] || fail "risk_scale values moved in the rollback"
apply 020_risk_detail_rollback.sql
pass "6. the rollback refuses while a value exists; forced, it drops both columns, rows byte-identical, risk_scale kept; idempotent"
echo "ALL OK"
