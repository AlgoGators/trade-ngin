#!/usr/bin/env bash
# Verifies migrations 015 (executions.execution_type + instrument_id, both tables), 017 (live_results
# roll costs) and 018 (backtest.results costs) and their rollbacks on a real PostgreSQL, on the
# shape of the four tables as the stage-3 scratch has them (T-ROLLX commit 3).
#   1. absent before (an INSERT naming each column fails: every migration is necessary);
#   2. after: types, nullability, defaults, the CHECK (a fourth class is refused), every
#      pre-existing row at the default, an omitting INSERT at the default, values round-trip;
#   3. NOTHING ELSE MOVES: pre-existing rows byte-identical apart from the new keys; indexes
#      and constraints as before (plus the two new CHECKs);
#   4. idempotent; type-guarded;
#   5. comments set;
#   6. the rollbacks refuse while a value exists and, with migration.force_rollback = 'yes', drop
#      the columns with the original rows byte-identical.
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

$PSQL <<'SQL'
DROP SCHEMA IF EXISTS trading CASCADE; DROP SCHEMA IF EXISTS backtest CASCADE;
CREATE SCHEMA trading; CREATE SCHEMA backtest;
CREATE TABLE trading.executions (
    exec_id VARCHAR(50) NOT NULL, order_id VARCHAR(50) NOT NULL, symbol VARCHAR(20) NOT NULL, side VARCHAR(4) NOT NULL,
    quantity NUMERIC NOT NULL, price NUMERIC NOT NULL, execution_time TIMESTAMPTZ NOT NULL, commissions_fees NUMERIC NOT NULL,
    is_partial BOOLEAN NOT NULL, created_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP, strategy_id VARCHAR(100) NOT NULL,
    strategy_name VARCHAR(100) NOT NULL, date DATE NOT NULL, portfolio_id VARCHAR(100) NOT NULL,
    implicit_price_impact NUMERIC DEFAULT 0.0, slippage_market_impact NUMERIC DEFAULT 0.0, total_transaction_costs NUMERIC DEFAULT 0.0,
    netting_adjustment NUMERIC DEFAULT 0,
    CONSTRAINT executions_pkey PRIMARY KEY (portfolio_id, strategy_id, strategy_name, date, exec_id),
    CONSTRAINT chk_executions_quantity CHECK (quantity > 0::numeric), CONSTRAINT chk_executions_side CHECK (side IN ('BUY','SELL')));
CREATE INDEX idx_executions_symbol ON trading.executions (symbol);
CREATE TABLE backtest.results (run_id TEXT PRIMARY KEY, total_return DOUBLE PRECISION, total_trades INTEGER);
CREATE TABLE backtest.executions (
    id SERIAL, run_id TEXT NOT NULL REFERENCES backtest.results(run_id) ON DELETE CASCADE, execution_id VARCHAR(255) NOT NULL,
    order_id VARCHAR(255) NOT NULL, "timestamp" TIMESTAMPTZ NOT NULL, symbol VARCHAR(50) NOT NULL, side VARCHAR(10) NOT NULL,
    quantity DOUBLE PRECISION NOT NULL, price DOUBLE PRECISION NOT NULL, commissions_fees DOUBLE PRECISION NOT NULL,
    is_partial BOOLEAN NOT NULL DEFAULT false, strategy_id VARCHAR(100) NOT NULL, portfolio_id VARCHAR(100),
    implicit_price_impact DOUBLE PRECISION DEFAULT 0.0, slippage_market_impact DOUBLE PRECISION DEFAULT 0.0,
    total_transaction_costs DOUBLE PRECISION DEFAULT 0.0, netting_adjustment DOUBLE PRECISION DEFAULT 0,
    CONSTRAINT executions_pkey PRIMARY KEY (run_id, strategy_id, execution_id));
CREATE TABLE trading.live_results (id SERIAL PRIMARY KEY, strategy_id VARCHAR(100) NOT NULL, portfolio_id VARCHAR(100) NOT NULL,
    date DATE NOT NULL, daily_transaction_costs NUMERIC, total_transaction_costs NUMERIC(20,8));
INSERT INTO trading.executions (exec_id, order_id, symbol, side, quantity, price, execution_time, commissions_fees, is_partial,
    strategy_id, strategy_name, date, portfolio_id, total_transaction_costs)
VALUES ('EXEC_NG.v.0_20251028','DAILY_NG.v.0_20251028','NG.v.0','BUY',1,3.822,'2025-10-28 05:00:00+00',1.5,false,'S','TREND','2025-10-28','P',6.5);
INSERT INTO backtest.results (run_id, total_return, total_trades) VALUES ('R1', 0.1, 3);
INSERT INTO backtest.executions (run_id, execution_id, order_id, "timestamp", symbol, side, quantity, price, commissions_fees, strategy_id, portfolio_id)
VALUES ('R1','EX-S-0','PM-S-0','2025-10-28 00:00:00+00','NG.v.0','BUY',1,3.822,1.5,'S','P');
INSERT INTO trading.live_results (strategy_id, portfolio_id, date, daily_transaction_costs, total_transaction_costs) VALUES ('S','P','2025-10-28',6.5,100.25);
SQL
fp() { q "select count(*)||' '||md5(coalesce(string_agg(x, ';' order by x), '')) from (select (to_jsonb(t) - 'execution_type' - 'instrument_id' - 'daily_roll_costs' - 'total_roll_costs' - 'transaction_costs' - 'roll_costs' - 'total_roll_fills')::text x from $1 t) s"; }
B1=$(fp trading.executions); B2=$(fp backtest.executions); B3=$(fp trading.live_results); B4=$(fp backtest.results)
I1=$(q "select string_agg(indexdef, ' | ' order by indexdef) from pg_indexes where schemaname='trading' and tablename='executions'")
# 1
refuses "INSERT INTO trading.executions (exec_id, order_id, symbol, side, quantity, price, execution_time, commissions_fees, is_partial, strategy_id, strategy_name, date, portfolio_id, execution_type) VALUES ('x','x','x','BUY',1,1,now(),1,false,'S','T',current_date,'P','ROLL')" || fail "execution_type exists before 015"
refuses "INSERT INTO trading.live_results (strategy_id, portfolio_id, date, daily_roll_costs) VALUES ('S','P',current_date,1)" || fail "daily_roll_costs exists before 017"
refuses "UPDATE backtest.results SET roll_costs = 1" || fail "roll_costs exists before 018"
pass "1. every column absent before its migration"
# 2
apply 015_executions_execution_type.sql; apply 017_live_results_roll_costs.sql; apply 018_backtest_results_costs.sql
[[ "$(q "select data_type||' '||is_nullable||' '||column_default from information_schema.columns where table_schema='trading' and table_name='executions' and column_name='execution_type'")" == "text NO 'STRATEGY'::text" ]] || fail "trading execution_type"
[[ "$(q "select data_type||' '||is_nullable||' '||coalesce(column_default,'-') from information_schema.columns where table_schema='backtest' and table_name='executions' and column_name='instrument_id'")" == "text YES -" ]] || fail "backtest instrument_id"
[[ "$(q "select data_type||' '||is_nullable||' '||column_default from information_schema.columns where table_schema='trading' and table_name='live_results' and column_name='daily_roll_costs'")" == "numeric NO 0" ]] || fail "daily_roll_costs"
[[ "$(q "select numeric_precision||','||numeric_scale from information_schema.columns where table_schema='trading' and table_name='live_results' and column_name='total_roll_costs'")" == "20,8" ]] || fail "total_roll_costs numeric(20,8)"
[[ "$(q "select data_type||' '||is_nullable||' '||column_default from information_schema.columns where table_schema='backtest' and table_name='results' and column_name='total_roll_fills'")" == "integer NO 0" ]] || fail "total_roll_fills"
[[ "$(q "select execution_type||'|'||coalesce(instrument_id,'NULL') from trading.executions")" == "STRATEGY|NULL" ]] || fail "pre-existing row not at the default"
[[ "$(q "select daily_roll_costs||'|'||total_roll_costs from trading.live_results")" == "0|0.00000000" ]] || fail "live_results history not 0"
[[ "$(q "select transaction_costs||'|'||roll_costs||'|'||total_roll_fills from backtest.results")" == "0|0|0" ]] || fail "results history not 0"
refuses "INSERT INTO trading.executions (exec_id, order_id, symbol, side, quantity, price, execution_time, commissions_fees, is_partial, strategy_id, strategy_name, date, portfolio_id, execution_type) VALUES ('x','x','x','BUY',1,1,now(),1,false,'S','T',current_date,'P','SPREAD')" || fail "a fourth class was accepted"
$PSQL -c "INSERT INTO trading.executions (exec_id, order_id, symbol, side, quantity, price, execution_time, commissions_fees, is_partial, strategy_id, strategy_name, date, portfolio_id, execution_type, instrument_id) VALUES ('EXEC_NG.v.0_20251028_RC','ROLL_NG.v.0_20251028_RC','NG.v.0','SELL',1,3.376,'2025-10-28 05:00:00+00',1.5,false,'S','TREND','2025-10-28','P','ROLL','864'), ('EXEC_NG.v.0_20251028_RO','ROLL_NG.v.0_20251028_RO','NG.v.0','BUY',1,3.965,'2025-10-28 05:00:00+00',1.5,false,'S','TREND','2025-10-28','P','ROLL','863')"
[[ "$(q "select count(*) from trading.executions where date='2025-10-28' and symbol='NG.v.0'")" == "3" ]] || fail "the two legs and the fill do not share the day's key"
$PSQL -c "INSERT INTO backtest.executions (run_id, execution_id, order_id, \"timestamp\", symbol, side, quantity, price, commissions_fees, strategy_id, portfolio_id, execution_type) VALUES ('R1','BORROW_S_X','BORROW_S_X','2025-10-28 00:00:00+00','X','SELL',0,1,0.1,'S','P','BORROW')"
$PSQL -c "UPDATE trading.live_results SET daily_roll_costs = 13.0, total_roll_costs = 26.00000001; UPDATE backtest.results SET transaction_costs = 1692.9, roll_costs = 2367.25, total_roll_fills = 420"
[[ "$(q "select total_roll_costs from trading.live_results")" == "26.00000001" ]] || fail "total_roll_costs round trip"
[[ "$(q "select total_roll_fills from backtest.results")" == "420" ]] || fail "total_roll_fills round trip"
pass "2. types, defaults, the CHECK, history at the default, omitting INSERTs, values round-trip, the legs share the day's key"
# 3
$PSQL -c "DELETE FROM trading.executions WHERE execution_type='ROLL'; DELETE FROM backtest.executions WHERE execution_type='BORROW'; UPDATE trading.live_results SET daily_roll_costs=0, total_roll_costs=0; UPDATE backtest.results SET transaction_costs=0, roll_costs=0, total_roll_fills=0"
[[ "$(fp trading.executions)" == "$B1" && "$(fp backtest.executions)" == "$B2" && "$(fp trading.live_results)" == "$B3" && "$(fp backtest.results)" == "$B4" ]] || fail "rows moved"
[[ "$(q "select string_agg(indexdef, ' | ' order by indexdef) from pg_indexes where schemaname='trading' and tablename='executions'")" == "$I1" ]] || fail "indexes changed"
[[ "$(q "select count(*) from pg_constraint where conname like 'chk_%executions_execution_type'")" == "2" ]] || fail "the two CHECKs"
pass "3. every pre-existing row byte-identical apart from the new keys; indexes unchanged; the two new CHECKs present"
# 4
apply 015_executions_execution_type.sql; apply 017_live_results_roll_costs.sql; apply 018_backtest_results_costs.sql
$PSQL -c "ALTER TABLE trading.executions ALTER COLUMN instrument_id TYPE bigint USING NULL"
refuses "" 2>/dev/null || true
if psql -X -q -v ON_ERROR_STOP=1 -f "$HERE/015_executions_execution_type.sql" >/dev/null 2>&1; then fail "015 type guard did not refuse"; fi
$PSQL -c "ALTER TABLE trading.executions ALTER COLUMN instrument_id TYPE text USING NULL"
pass "4. idempotent; a column of another type makes 015 refuse"
# 5
[[ "$(q "select col_description('trading.executions'::regclass,(select ordinal_position from information_schema.columns where table_schema='trading' and table_name='executions' and column_name='execution_type')::int)")" == *"migration 015"* ]] || fail "015 comment"
[[ "$(q "select col_description('trading.live_results'::regclass,(select ordinal_position from information_schema.columns where table_schema='trading' and table_name='live_results' and column_name='total_roll_costs')::int)")" == *"Migration 017"* ]] || fail "017 comment"
[[ "$(q "select col_description('backtest.results'::regclass,(select ordinal_position from information_schema.columns where table_schema='backtest' and table_name='results' and column_name='roll_costs')::int)")" == *"Migration 018"* ]] || fail "018 comment"
pass "5. comments set"
# 6
$PSQL -c "INSERT INTO trading.executions (exec_id, order_id, symbol, side, quantity, price, execution_time, commissions_fees, is_partial, strategy_id, strategy_name, date, portfolio_id, execution_type, instrument_id) VALUES ('EXEC_NG.v.0_20251028_RC','ROLL_NG.v.0_20251028_RC','NG.v.0','SELL',1,3.376,'2025-10-28 05:00:00+00',1.5,false,'S','TREND','2025-10-28','P','ROLL','864'); UPDATE trading.live_results SET daily_roll_costs = 1; UPDATE backtest.results SET roll_costs = 1"
for rb in 015_executions_execution_type_rollback.sql 017_live_results_roll_costs_rollback.sql 018_backtest_results_costs_rollback.sql; do
  if psql -X -q -v ON_ERROR_STOP=1 -f "$HERE/$rb" >/dev/null 2>&1; then fail "$rb did not refuse"; fi
done
$PSQL -c "DELETE FROM trading.executions WHERE execution_type='ROLL'; UPDATE trading.live_results SET daily_roll_costs=0; UPDATE backtest.results SET roll_costs=0"
for rb in 015_executions_execution_type_rollback.sql 017_live_results_roll_costs_rollback.sql 018_backtest_results_costs_rollback.sql; do apply "$rb"; done
[[ "$(q "select count(*) from information_schema.columns where column_name in ('execution_type','instrument_id','daily_roll_costs','total_roll_costs','transaction_costs','roll_costs','total_roll_fills') and table_schema in ('trading','backtest')")" == "0" ]] || fail "a rollback left a column"
[[ "$(fp trading.executions)" == "$B1" && "$(fp backtest.executions)" == "$B2" && "$(fp trading.live_results)" == "$B3" && "$(fp backtest.results)" == "$B4" ]] || fail "rows differ after the rollbacks"
pass "6. the rollbacks refuse while a value exists and drop cleanly after, rows byte-identical"
echo "ALL OK"
