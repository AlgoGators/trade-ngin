#!/usr/bin/env bash
# Verifies migration 040 (the roll split: roll_bar_move, roll_gap_points, roll_gap_value on both executions
# tables with their CHECKs, roll_bar_moves and roll_gap_values on backtest.results) and its rollback on a real
# PostgreSQL.
#   1. absent before (an INSERT naming roll_bar_move fails: the migration is necessary);
#   2. after: the types (numeric on trading.executions, double precision on backtest), NULL-able, no default,
#      the last columns; one CHECK per executions table;
#   3. NO BACKFILL: every pre-existing row holds NULL and is byte-identical in every other column; indexes
#      as before; the constraints are the earlier ones plus the two CHECKs; an omitting INSERT reads NULL;
#   4. the columns hold what the writer sends: the worked example's silver roll (closing leg -1075, 0.670,
#      3350; the opening leg NULL) in one statement with a STRATEGY row (NULL) on each table, and the
#      results totals; the CHECK refuses a value on a STRATEGY row and one or two of the three set;
#   5. idempotent (a second apply keeps the values and adds no second CHECK); guarded: a column of another
#      type, and an unexpected cell (a value on a STRATEGY row put there with the CHECK dropped), make 040
#      refuse and change nothing; a missing table or 015's column makes it refuse;
#   6. the comments say the closing leg, NULL where, never P&L for the gap, and name 040;
#   7. the rollback refuses while a cell holds a value and, with migration.force_rollback = 'yes', drops
#      the five columns and two CHECKs with the original rows byte-identical; the column lists, constraints,
#      indexes and every comment of the three tables are then exactly as before 040; with no value it drops
#      without the setting; idempotent; 040 applies again after it.
# Usage: MIGRATION_TEST_DB=<throwaway> PGDATABASE=<same> [PGHOST/PGPORT/PGUSER] bash test_040_roll_split.sh
# DESTRUCTIVE on the target (it creates schemas trading and backtest): a THROWAWAY database only; production
# and every stage-3 scratch or clone refused by name, any database with the schemas refused.
set -euo pipefail
[[ "${MIGRATION_TEST_DB:-}" == "" ]] && { echo "REFUSING: set MIGRATION_TEST_DB and PGDATABASE" >&2; exit 2; }
[[ "${PGDATABASE:-}" != "${MIGRATION_TEST_DB}" ]] && { echo "REFUSING: PGDATABASE != MIGRATION_TEST_DB" >&2; exit 2; }
case "${MIGRATION_TEST_DB}" in new_algo_data|new_algo_data_*|*_new_algo_data|algo_data) echo "REFUSING: production or a clone" >&2; exit 2;; esac
existing=$(psql -X -tAc "SELECT count(*) FROM information_schema.schemata WHERE schema_name IN ('trading','backtest')" 2>/dev/null || echo 0)
[[ "${existing:-0}" != "0" ]] && { echo "REFUSING: the target already has a trading or backtest schema" >&2; exit 2; }
HERE="$(cd "$(dirname "$0")" && pwd)"
PSQL="psql -v ON_ERROR_STOP=1 -q -X"
export PGOPTIONS="${PGOPTIONS:-} -c client_min_messages=warning"   # a second apply's "already exists, skipping" notices
q() { psql -X -tAc "$1"; }
fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok: $*"; }
apply() { $PSQL -f "$HERE/$1"; }
refuses() { if psql -X -q -v ON_ERROR_STOP=1 -c "$1" >/dev/null 2>&1; then return 1; else return 0; fi; }
refuses_file() { if psql -X -q -v ON_ERROR_STOP=1 -f "$HERE/$1" >/dev/null 2>&1; then return 1; else return 0; fi; }
FWD=040_roll_split.sql; RB=040_roll_split_rollback.sql
present() { q "select count(*) from information_schema.columns where (table_schema, table_name) in (('trading','executions'),('backtest','executions')) and column_name in ('roll_bar_move','roll_gap_points','roll_gap_value') or (table_schema, table_name) = ('backtest','results') and column_name in ('roll_bar_moves','roll_gap_values')"; }

# The three tables as the lane leaves them before 040 (production's column lists and constraints, cut to what
# 040 reads and the keys), with rows written before 040: a STRATEGY fill and the two legs of a roll.
$PSQL <<'SQL'
SET TimeZone = 'UTC';
CREATE SCHEMA trading;
CREATE SCHEMA backtest;
CREATE TABLE trading.executions (exec_id VARCHAR(100) NOT NULL, order_id VARCHAR(100) NOT NULL, symbol VARCHAR(50) NOT NULL,
    side VARCHAR(10) NOT NULL, quantity NUMERIC NOT NULL, price NUMERIC NOT NULL, execution_time TIMESTAMPTZ NOT NULL,
    commissions_fees NUMERIC NOT NULL, is_partial BOOLEAN NOT NULL, strategy_id VARCHAR(100) NOT NULL,
    strategy_name VARCHAR(100) NOT NULL, date DATE NOT NULL, portfolio_id VARCHAR(100) NOT NULL,
    total_transaction_costs NUMERIC, netting_adjustment NUMERIC, execution_type TEXT NOT NULL DEFAULT 'STRATEGY',
    instrument_id TEXT, portfolio_type TEXT NOT NULL DEFAULT 'system',
    CONSTRAINT chk_executions_execution_type CHECK (execution_type IN ('STRATEGY', 'ROLL', 'BORROW')),
    CONSTRAINT chk_executions_quantity CHECK (quantity > 0),
    CONSTRAINT executions_pkey PRIMARY KEY (portfolio_id, strategy_id, strategy_name, date, exec_id, portfolio_type));
CREATE TABLE backtest.results (run_id TEXT PRIMARY KEY, total_return DOUBLE PRECISION, portfolio_id VARCHAR(50),
    transaction_costs DOUBLE PRECISION NOT NULL DEFAULT 0, roll_costs DOUBLE PRECISION NOT NULL DEFAULT 0,
    total_roll_fills INTEGER NOT NULL DEFAULT 0);
CREATE TABLE backtest.executions (id SERIAL, run_id TEXT NOT NULL REFERENCES backtest.results(run_id) ON DELETE CASCADE,
    execution_id VARCHAR(100) NOT NULL, order_id VARCHAR(100) NOT NULL, "timestamp" TIMESTAMPTZ NOT NULL,
    symbol VARCHAR(50) NOT NULL, side VARCHAR(10) NOT NULL, quantity DOUBLE PRECISION NOT NULL, price DOUBLE PRECISION NOT NULL,
    commissions_fees DOUBLE PRECISION NOT NULL, is_partial BOOLEAN NOT NULL, strategy_id VARCHAR(100) NOT NULL,
    portfolio_id VARCHAR(100), total_transaction_costs DOUBLE PRECISION, netting_adjustment DOUBLE PRECISION,
    execution_type TEXT NOT NULL DEFAULT 'STRATEGY', instrument_id TEXT,
    CONSTRAINT chk_backtest_executions_execution_type CHECK (execution_type IN ('STRATEGY', 'ROLL', 'BORROW')),
    CONSTRAINT executions_pkey PRIMARY KEY (run_id, strategy_id, execution_id));
CREATE INDEX idx_backtest_executions_run ON backtest.executions (run_id);
INSERT INTO trading.executions (exec_id, order_id, symbol, side, quantity, price, execution_time, commissions_fees, is_partial, strategy_id, strategy_name, date, portfolio_id, total_transaction_costs, netting_adjustment, execution_type, instrument_id) VALUES
    ('EXEC_SI.v.0_20251126', 'O1', 'SI.v.0', 'BUY', 1, 51.63, '2025-11-26 00:00:00+00', 2.51, false, 'C', 'TREND', '2025-11-26', 'CONSERVATIVE_PORTFOLIO', 22.67, 0, 'STRATEGY', NULL),
    ('EXEC_GC.v.0_20240131_RC', 'ROLL_GC.v.0_20240131_RC', 'GC.v.0', 'SELL', 1, 2050.5, '2024-02-01 00:00:00+00', 2.51, false, 'C', 'TREND', '2024-02-01', 'CONSERVATIVE_PORTFOLIO', 12.4, 0, 'ROLL', '41512'),
    ('EXEC_GC.v.0_20240131_RO', 'ROLL_GC.v.0_20240131_RO', 'GC.v.0', 'BUY', 1, 2071.1, '2024-02-01 00:00:00+00', 2.51, false, 'C', 'TREND', '2024-02-01', 'CONSERVATIVE_PORTFOLIO', 12.5, 0, 'ROLL', '44740');
INSERT INTO backtest.results (run_id, total_return, portfolio_id, transaction_costs, roll_costs, total_roll_fills) VALUES ('OLD_RUN', 0.6168, 'CONSERVATIVE_PORTFOLIO', 1200.5, 24.9, 2);
INSERT INTO backtest.executions (run_id, execution_id, order_id, "timestamp", symbol, side, quantity, price, commissions_fees, is_partial, strategy_id, portfolio_id, total_transaction_costs, netting_adjustment, execution_type, instrument_id) VALUES
    ('OLD_RUN', 'RL-TREND-0', 'RL-TREND-0', '2024-02-01 00:00:00+00', 'GC.v.0', 'SELL', 1, 2050.5, 2.51, false, 'TREND', 'CONSERVATIVE_PORTFOLIO', 12.4, 0, 'ROLL', '41512'),
    ('OLD_RUN', 'RL-TREND-1', 'RL-TREND-1', '2024-02-01 00:00:00+00', 'GC.v.0', 'BUY', 1, 2071.1, 2.51, false, 'TREND', 'CONSERVATIVE_PORTFOLIO', 12.5, 0, 'ROLL', '44740'),
    ('OLD_RUN', 'EX-1', 'EX-1', '2024-02-01 00:00:00+00', 'ZN.v.0', 'BUY', 2, 110.25, 5.02, false, 'TREND', 'CONSERVATIVE_PORTFOLIO', 30.1, 0, 'STRATEGY', NULL);
COMMENT ON COLUMN trading.executions.instrument_id IS 'the 015 text';
COMMENT ON COLUMN backtest.executions.instrument_id IS 'the 015 text';
COMMENT ON COLUMN backtest.results.roll_costs IS 'the 018 text';
SQL
fp() { q "select count(*)||' '||md5(coalesce(string_agg(x, ';' order by x), '')) from (select (to_jsonb(t) - 'roll_bar_move' - 'roll_gap_points' - 'roll_gap_value' - 'roll_bar_moves' - 'roll_gap_values')::text x from $1 t) s"; }
comments() { q "select md5(coalesce(string_agg(n.nspname||'.'||c.relname||'|'||d.objsubid||'|'||coalesce(a.attname,'')||'|'||d.description, E'\n' order by n.nspname, c.relname, d.objsubid), '')) || ' ' || count(*) from pg_description d join pg_class c on c.oid = d.objoid and d.classoid = 'pg_class'::regclass join pg_namespace n on n.oid = c.relnamespace left join pg_attribute a on a.attrelid = c.oid and a.attnum = d.objsubid and d.objsubid > 0 where (n.nspname, c.relname) in (('trading','executions'),('backtest','executions'),('backtest','results'))"; }
schema() { q "select md5(string_agg(table_schema||'.'||table_name||'.'||column_name||':'||data_type||':'||is_nullable||':'||coalesce(column_default,'-'), ' ' order by table_schema, table_name, ordinal_position)) from information_schema.columns where table_schema in ('trading','backtest')"; }
constraints() { q "select coalesce(string_agg(c.conrelid::regclass||':'||c.conname||':'||pg_get_constraintdef(c.oid), ' | ' order by c.conrelid::regclass::text, c.conname), '') from pg_constraint c join pg_namespace n on n.oid = c.connamespace where n.nspname in ('trading','backtest') ${1:-}"; }
indexes() { q "select string_agg(indexdef, ' | ' order by indexdef) from pg_indexes where schemaname in ('trading','backtest')"; }
cols() { q "select string_agg(column_name, ' ' order by ordinal_position) from information_schema.columns where table_schema='$1' and table_name='$2'"; }
cmt() { q "select col_description('$1.$2'::regclass,(select ordinal_position from information_schema.columns where table_schema='$1' and table_name='$2' and column_name='$3'))"; }
B1=$(fp trading.executions); B2=$(fp backtest.executions); B3=$(fp backtest.results)
D1=$(comments); S1=$(schema); K1=$(constraints); I1=$(indexes)
C1=$(cols trading executions); C2=$(cols backtest executions); C3=$(cols backtest results)
# 1
[[ "$(present)" == "0" ]] || fail "a roll split column exists before 040"
refuses "UPDATE backtest.executions SET roll_bar_move = 1 WHERE execution_id = 'RL-TREND-0'" || fail "roll_bar_move exists before 040"
pass "1. the five columns are absent before the migration"
# 2
apply $FWD
for c in roll_bar_move roll_gap_points roll_gap_value; do
  [[ "$(q "select data_type||':'||is_nullable||':'||coalesce(column_default,'-') from information_schema.columns where table_schema='trading' and table_name='executions' and column_name='$c'")" == "numeric:YES:-" ]] || fail "trading.executions.$c is not numeric, NULL-able, no default"
  [[ "$(q "select data_type||':'||is_nullable||':'||coalesce(column_default,'-') from information_schema.columns where table_schema='backtest' and table_name='executions' and column_name='$c'")" == "double precision:YES:-" ]] || fail "backtest.executions.$c is not double precision, NULL-able, no default"
done
for c in roll_bar_moves roll_gap_values; do
  [[ "$(q "select data_type||':'||is_nullable||':'||coalesce(column_default,'-') from information_schema.columns where table_schema='backtest' and table_name='results' and column_name='$c'")" == "double precision:YES:-" ]] || fail "backtest.results.$c is not double precision, NULL-able, no default"
done
[[ "$(cols trading executions)" == "$C1 roll_bar_move roll_gap_points roll_gap_value" ]] || fail "trading.executions' new columns are not the last three"
[[ "$(cols backtest executions)" == "$C2 roll_bar_move roll_gap_points roll_gap_value" ]] || fail "backtest.executions' new columns are not the last three"
[[ "$(cols backtest results)" == "$C3 roll_bar_moves roll_gap_values" ]] || fail "backtest.results' new columns are not the last two"
EXPECT_CHECK="CHECK ((((roll_bar_move IS NULL) = (roll_gap_points IS NULL)) AND ((roll_gap_points IS NULL) = (roll_gap_value IS NULL)) AND ((roll_bar_move IS NULL) OR (execution_type = 'ROLL'::text))))"
[[ "$(q "select pg_get_constraintdef(oid) from pg_constraint where conname='chk_executions_roll_split' and conrelid='trading.executions'::regclass")" == "$EXPECT_CHECK" ]] || fail "trading.executions' CHECK"
[[ "$(q "select pg_get_constraintdef(oid) from pg_constraint where conname='chk_backtest_executions_roll_split' and conrelid='backtest.executions'::regclass")" == "$EXPECT_CHECK" ]] || fail "backtest.executions' CHECK"
pass "2. the types, NULL-able with no default, the last columns; one CHECK per executions table"
# 3
[[ "$(q "select count(*) from trading.executions where roll_bar_move is not null or roll_gap_points is not null or roll_gap_value is not null")" == "0" ]] || fail "a trading row holds a value"
[[ "$(q "select count(*) from backtest.executions where roll_bar_move is not null or roll_gap_points is not null or roll_gap_value is not null")" == "0" ]] || fail "a backtest row holds a value"
[[ "$(q "select count(*) from backtest.results where roll_bar_moves is not null or roll_gap_values is not null")" == "0" ]] || fail "a results row holds a value"
[[ "$(fp trading.executions)" == "$B1" && "$(fp backtest.executions)" == "$B2" && "$(fp backtest.results)" == "$B3" ]] || fail "rows moved"
[[ "$(indexes)" == "$I1" ]] || fail "indexes changed"
[[ "$(constraints "and c.conname not in ('chk_executions_roll_split','chk_backtest_executions_roll_split')")" == "$K1" ]] || fail "the constraints are not the earlier ones plus the two CHECKs"
$PSQL -c "INSERT INTO backtest.results (run_id) VALUES ('OFF_RUN'); INSERT INTO backtest.executions (run_id, execution_id, order_id, \"timestamp\", symbol, side, quantity, price, commissions_fees, is_partial, strategy_id, execution_type, instrument_id) VALUES ('OFF_RUN', 'RL-TREND-0', 'RL-TREND-0', '2025-11-26', 'SI.v.0', 'SELL', 1, 50.96, 2.51, false, 'TREND', 'ROLL', '1')"
[[ "$(q "select count(*) from backtest.executions where run_id='OFF_RUN' and roll_bar_move is null and roll_gap_points is null and roll_gap_value is null")" == "1" && "$(q "select count(*) from backtest.results where run_id='OFF_RUN' and roll_bar_moves is null and roll_gap_values is null")" == "1" ]] || fail "an omitting INSERT (a switch-off run) is not NULL"
pass "3. no backfill: existing rows hold NULL and are byte-identical elsewhere; indexes unchanged; two constraints more, the CHECKs; an omitting INSERT reads NULL"
# 4 (the writers' statements: the worked example's silver roll, 2025-11-25, long 1, multiplier 5000)
$PSQL -c "INSERT INTO trading.executions (exec_id, order_id, symbol, side, quantity, price, execution_time, commissions_fees, is_partial, strategy_id, strategy_name, date, portfolio_id, total_transaction_costs, netting_adjustment, execution_type, instrument_id, roll_bar_move, roll_gap_points, roll_gap_value) VALUES
  ('EXEC_SI.v.0_20251125_RC', 'ROLL_SI.v.0_20251125_RC', 'SI.v.0', 'SELL', 1, 50.96, '2025-11-26 00:00:00+00', 2.51, false, 'C', 'TREND', '2025-11-26', 'CONSERVATIVE_PORTFOLIO', 22.62, 0, 'ROLL', '1001', -1075, 0.67, 3350),
  ('EXEC_SI.v.0_20251125_RO', 'ROLL_SI.v.0_20251125_RO', 'SI.v.0', 'BUY', 1, 51.63, '2025-11-26 00:00:00+00', 2.51, false, 'C', 'TREND', '2025-11-26', 'CONSERVATIVE_PORTFOLIO', 22.67, 0, 'ROLL', '1002', NULL, NULL, NULL),
  ('EXEC_ZN.v.0_20251126', 'O2', 'ZN.v.0', 'BUY', 2, 110.25, '2025-11-26 00:00:00+00', 5.02, false, 'C', 'TREND', '2025-11-26', 'CONSERVATIVE_PORTFOLIO', 30.1, 0, 'STRATEGY', NULL, NULL, NULL, NULL)"
$PSQL -c "INSERT INTO backtest.results (run_id, roll_costs, total_roll_fills, roll_bar_moves, roll_gap_values) VALUES ('NEW_RUN', 45.29, 2, -1075, 3350); INSERT INTO backtest.executions (run_id, execution_id, order_id, \"timestamp\", symbol, side, quantity, price, commissions_fees, is_partial, strategy_id, total_transaction_costs, netting_adjustment, execution_type, instrument_id, roll_bar_move, roll_gap_points, roll_gap_value) VALUES
  ('NEW_RUN', 'RL-TREND-0', 'RL-TREND-0', '2025-11-26', 'SI.v.0', 'SELL', 1, 50.96, 2.51, false, 'TREND', 22.62, 0, 'ROLL', '1001', -1075, 0.67, 3350),
  ('NEW_RUN', 'RL-TREND-1', 'RL-TREND-1', '2025-11-26', 'SI.v.0', 'BUY', 1, 51.63, 2.51, false, 'TREND', 22.67, 0, 'ROLL', '1002', NULL, NULL, NULL),
  ('NEW_RUN', 'EX-2', 'EX-2', '2025-11-26', 'ZN.v.0', 'BUY', 2, 110.25, 5.02, false, 'TREND', 30.1, 0, 'STRATEGY', NULL, NULL, NULL, NULL)"
[[ "$(q "select string_agg(exec_id||'='||coalesce(roll_bar_move::text,'NULL')||'/'||coalesce(roll_gap_points::text,'NULL')||'/'||coalesce(roll_gap_value::text,'NULL'), ' ' order by exec_id) from trading.executions where date='2025-11-26'")" == "EXEC_SI.v.0_20251125_RC=-1075/0.67/3350 EXEC_SI.v.0_20251125_RO=NULL/NULL/NULL EXEC_SI.v.0_20251126=NULL/NULL/NULL EXEC_ZN.v.0_20251126=NULL/NULL/NULL" ]] || fail "the trading rows did not round trip"
[[ "$(q "select string_agg(execution_id||'='||coalesce(roll_bar_move::text,'NULL')||'/'||coalesce(roll_gap_points::text,'NULL')||'/'||coalesce(roll_gap_value::text,'NULL'), ' ' order by execution_id) from backtest.executions where run_id='NEW_RUN'")" == "EX-2=NULL/NULL/NULL RL-TREND-0=-1075/0.67/3350 RL-TREND-1=NULL/NULL/NULL" ]] || fail "the backtest rows did not round trip"
[[ "$(q "select sum(e.roll_bar_move)||' '||sum(e.roll_gap_value)||' '||max(r.roll_bar_moves)||' '||max(r.roll_gap_values)||' '||round(sum(e.total_transaction_costs) filter (where e.execution_type='ROLL')::numeric, 2)||' '||max(r.roll_costs) from backtest.executions e join backtest.results r using (run_id) where run_id='NEW_RUN'")" == "-1075 3350 -1075 3350 45.29 45.29" ]] || fail "the results totals do not equal the sums over the rows (one roll counted once)"
for tbl in trading backtest; do
  if [[ $tbl == trading ]]; then key="exec_id = 'EXEC_ZN.v.0_20251126'"; rkey="exec_id = 'EXEC_SI.v.0_20251125_RC'"; else key="run_id = 'NEW_RUN' AND execution_id = 'EX-2'"; rkey="run_id = 'NEW_RUN' AND execution_id = 'RL-TREND-0'"; fi
  refuses "UPDATE $tbl.executions SET roll_bar_move = 1, roll_gap_points = 1, roll_gap_value = 1 WHERE $key" || fail "$tbl: a value on a STRATEGY row passed the CHECK"
  refuses "UPDATE $tbl.executions SET roll_gap_value = NULL WHERE $rkey" || fail "$tbl: two of the three passed the CHECK"
  refuses "UPDATE $tbl.executions SET roll_gap_points = NULL, roll_gap_value = NULL WHERE $rkey" || fail "$tbl: one of the three passed the CHECK"
done
pass "4. the silver roll round trips (closing leg -1075 / 0.67 / 3350, opening leg and STRATEGY rows NULL) on both tables; the results totals equal the sums over the rows; a value on a STRATEGY row, and one or two of the three set, are refused"
# 5
V1=$(fp trading.executions); V2=$(fp backtest.executions)
apply $FWD
[[ "$(q "select count(*) from pg_constraint where conname in ('chk_executions_roll_split','chk_backtest_executions_roll_split')")" == "2" ]] || fail "a second apply added a CHECK"
[[ "$(fp trading.executions)" == "$V1" && "$(fp backtest.executions)" == "$V2" ]] || fail "a second apply changed a row"
[[ "$(q "select roll_bar_move||'/'||roll_gap_points||'/'||roll_gap_value from backtest.executions where run_id='NEW_RUN' and execution_id='RL-TREND-0'")" == "-1075/0.67/3350" ]] || fail "a second apply changed a value"
# the unexpected cell: a value on a STRATEGY row, put there with the CHECK dropped
$PSQL -c "ALTER TABLE backtest.executions DROP CONSTRAINT chk_backtest_executions_roll_split; UPDATE backtest.executions SET roll_bar_move = 5, roll_gap_points = 0.1, roll_gap_value = 10 WHERE run_id = 'NEW_RUN' AND execution_id = 'EX-2'"
refuses_file $FWD || fail "040 did not refuse an unexpected cell (a value on a STRATEGY row)"
[[ "$(q "select count(*) from pg_constraint where conname = 'chk_backtest_executions_roll_split'")" == "0" ]] || fail "the refused apply changed something"
$PSQL -c "UPDATE backtest.executions SET roll_gap_value = NULL, roll_gap_points = NULL, roll_bar_move = NULL WHERE run_id = 'NEW_RUN' AND execution_id = 'EX-2'"
$PSQL -c "ALTER TABLE trading.executions DROP CONSTRAINT chk_executions_roll_split; UPDATE trading.executions SET roll_gap_value = NULL WHERE exec_id = 'EXEC_SI.v.0_20251125_RC'"
refuses_file $FWD || fail "040 did not refuse an unexpected cell (an incomplete split)"
$PSQL -c "UPDATE trading.executions SET roll_gap_value = 3350 WHERE exec_id = 'EXEC_SI.v.0_20251125_RC'"
apply $FWD
[[ "$(q "select count(*) from pg_constraint where conname in ('chk_executions_roll_split','chk_backtest_executions_roll_split')")" == "2" ]] || fail "040 did not put the CHECKs back"
$PSQL -c "ALTER TABLE backtest.executions DROP CONSTRAINT chk_backtest_executions_roll_split; ALTER TABLE backtest.executions ALTER COLUMN roll_gap_points TYPE numeric"
refuses_file $FWD || fail "040 type guard did not refuse a column of another type"
$PSQL -c "ALTER TABLE backtest.executions ALTER COLUMN roll_gap_points TYPE double precision"
apply $FWD
$PSQL -c "ALTER TABLE trading.executions RENAME COLUMN execution_type TO et"
refuses_file $FWD || fail "040 did not refuse without 015's execution_type"
$PSQL -c "ALTER TABLE trading.executions RENAME COLUMN et TO execution_type; ALTER TABLE backtest.results RENAME TO results_x"
refuses_file $FWD || fail "040 did not refuse without backtest.results"
$PSQL -c "ALTER TABLE backtest.results_x RENAME TO results"
apply $FWD
pass "5. a second apply keeps every value and adds no CHECK; an unexpected cell (a value on a STRATEGY row, an incomplete split) refuses and changes nothing; a column of another type, a missing execution_type and a missing table refuse"
# 6
for t in trading backtest; do
  M="$(cmt $t executions roll_bar_move)"; G="$(cmt $t executions roll_gap_points)"; V="$(cmt $t executions roll_gap_value)"
  [[ "$M" == *"CLOSING leg only"*"move booked on the roll bar"*"It is P&L"*"NULL on the opening leg"*"with the switch off"*"micros"*"One roll, one set of values"*"Migration 040." ]] || fail "$t roll_bar_move comment: $M"
  [[ "$G" == *"CLOSING leg only"*"new contract's close minus the old contract's close on the roll bar"*"Migration 040." ]] || fail "$t roll_gap_points comment: $G"
  [[ "$V" == *"CLOSING leg only"*"NEVER profit or loss"*"Migration 040." ]] || fail "$t roll_gap_value comment: $V"
done
[[ "$(cmt backtest results roll_bar_moves)" == *"sum of the run's backtest.executions.roll_bar_move"*"NULL with per_contract_rolls off"*"roll_costs (018)"*"Migration 040." ]] || fail "roll_bar_moves comment"
[[ "$(cmt backtest results roll_gap_values)" == *"sum of the run's backtest.executions.roll_gap_value"*"never profit or loss"*"Migration 040." ]] || fail "roll_gap_values comment"
[[ "$(cmt backtest results roll_costs)" == "the 018 text" && "$(cmt trading executions instrument_id)" == "the 015 text" ]] || fail "040 moved another column's comment"
pass "6. the comments: the closing leg, NULL where, never P&L for the gap, the results totals, 040; no other comment moved"
# 7
refuses_file $RB || fail "the rollback did not refuse while values exist"
[[ "$(present)" == "8" ]] || fail "the refused rollback dropped a column"
PGOPTIONS="$PGOPTIONS -c migration.force_rollback=yes" $PSQL -f "$HERE/$RB"
[[ "$(present)" == "0" ]] || fail "the forced rollback left a column"
$PSQL -c "DELETE FROM backtest.executions WHERE run_id IN ('NEW_RUN','OFF_RUN'); DELETE FROM backtest.results WHERE run_id IN ('NEW_RUN','OFF_RUN'); DELETE FROM trading.executions WHERE date = '2025-11-26' AND exec_id <> 'EXEC_SI.v.0_20251126'"
[[ "$(fp trading.executions)" == "$B1" && "$(fp backtest.executions)" == "$B2" && "$(fp backtest.results)" == "$B3" ]] || fail "the original rows are not byte-identical after the rollback"
[[ "$(schema)" == "$S1" && "$(constraints)" == "$K1" && "$(indexes)" == "$I1" && "$(comments)" == "$D1" ]] || fail "the column lists, constraints, indexes or comments are not exactly as before 040"
apply $FWD
apply $RB
[[ "$(present)" == "0" ]] || fail "the rollback with no value did not drop"
apply $RB
[[ "$(schema)" == "$S1" && "$(constraints)" == "$K1" && "$(comments)" == "$D1" ]] || fail "a second rollback moved something"
apply $FWD
[[ "$(present)" == "8" ]] || fail "040 did not apply again after its rollback"
pass "7. the rollback refuses with values; forced, it drops the five columns and two CHECKs, the original rows byte-identical and the schema, constraints, indexes and comments exactly as before 040; with no value it drops unforced; idempotent; 040 applies again"
echo "ALL PASS: 040"
