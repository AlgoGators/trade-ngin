#!/usr/bin/env bash
# Verifies migration 021 (three books: qt_proposal in the positions/equity_curve CHECK,
# portfolio_type on executions and live_results and in their keys, positions.moved_by) and its
# rollback on a real PostgreSQL, on the stage-3 shape of the four trading tables.
#   1. absent before: a qt_proposal positions row is refused; executions and live_results have no
#      portfolio_type and a second book's row collides on their keys; moved_by is absent;
#   2. after: portfolio_type text NOT NULL DEFAULT 'system' with the three-value CHECK on all four
#      tables; executions_pkey and the live_results key end in portfolio_type, under their old
#      names; rows of three books with the same key coexist in every table; an omitting INSERT
#      reads 'system'; moved_by NULL by default, refused on system and qt_proposal, kept on qt;
#   3. NOTHING ELSE MOVES: pre-existing rows byte-identical apart from the new keys; every index
#      and constraint outside the two rebuilt keys and the CHECKs unchanged;
#   4. idempotent; type-guarded (a portfolio_type or moved_by of another type makes 021 refuse);
#      refuses without migration 001 and without the live_results key;
#   5. the three comments set;
#   6. the rollback refuses while a non-system execution or live_results row, a qt_proposal row or
#      a moved_by exists; once they are gone it restores the keys under their names, drops the
#      columns, narrows the CHECKs, the original rows byte-identical; idempotent.
# DESTRUCTIVE on the target (DROP SCHEMA trading CASCADE): a THROWAWAY database only;
# production and every stage-3 scratch or clone refused by name, any database with the schema refused.
#
#   createdb t021 && PGDATABASE=t021 MIGRATION_TEST_DB=t021 ./test_021_qt_books.sh && dropdb t021
set -euo pipefail
[[ "${MIGRATION_TEST_DB:-}" == "" ]] && { echo "REFUSING: set MIGRATION_TEST_DB and PGDATABASE" >&2; exit 2; }
[[ "${PGDATABASE:-}" != "${MIGRATION_TEST_DB}" ]] && { echo "REFUSING: PGDATABASE != MIGRATION_TEST_DB" >&2; exit 2; }
case "${MIGRATION_TEST_DB}" in new_algo_data|new_algo_data_*|*_new_algo_data|algo_data|algo_data_*) echo "REFUSING: production or a clone" >&2; exit 2;; esac
existing=$(psql -X -tAc "SELECT count(*) FROM information_schema.schemata WHERE schema_name IN ('trading','backtest')" 2>/dev/null || echo 0)
[[ "${existing:-0}" != "0" ]] && { echo "REFUSING: the target already has a trading or backtest schema" >&2; exit 2; }
HERE="$(cd "$(dirname "$0")" && pwd)"
export PGOPTIONS="${PGOPTIONS:-} -c client_min_messages=warning"
PSQL="psql -v ON_ERROR_STOP=1 -q -X"
q() { psql -X -tAc "$1"; }
fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok: $*"; }
apply() { $PSQL -f "$HERE/$1"; }
refuses() { if psql -X -q -v ON_ERROR_STOP=1 -c "$1" >/dev/null 2>&1; then return 1; else return 0; fi; }
refuses_file() { if psql -X -q -v ON_ERROR_STOP=1 -f "$HERE/$1" >/dev/null 2>&1; then return 1; else return 0; fi; }

fixture() {
$PSQL <<'SQL'
DROP SCHEMA IF EXISTS trading CASCADE;
CREATE SCHEMA trading;
CREATE TABLE trading.positions (
    symbol VARCHAR(20) NOT NULL, quantity NUMERIC NOT NULL, average_price NUMERIC NOT NULL,
    daily_unrealized_pnl NUMERIC DEFAULT 0, daily_realized_pnl NUMERIC DEFAULT 0, last_update TIMESTAMPTZ NOT NULL,
    updated_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP, strategy_id VARCHAR(100) NOT NULL, strategy_name VARCHAR(100) NOT NULL,
    date DATE NOT NULL, portfolio_id VARCHAR(100) NOT NULL, portfolio_type TEXT NOT NULL DEFAULT 'system', instrument_id TEXT,
    CONSTRAINT positions_portfolio_type_check CHECK (portfolio_type IN ('system', 'qt')),
    CONSTRAINT positions_pkey PRIMARY KEY (portfolio_id, strategy_id, strategy_name, date, symbol, portfolio_type));
CREATE INDEX idx_trading_positions_portfolio_type ON trading.positions (portfolio_type);
CREATE INDEX idx_positions_strategy_date ON trading.positions (strategy_id, date);
CREATE TABLE trading.equity_curve (id SERIAL PRIMARY KEY, strategy_id VARCHAR(100) NOT NULL, "timestamp" TIMESTAMPTZ NOT NULL,
    equity DOUBLE PRECISION NOT NULL, portfolio_id VARCHAR(100), portfolio_type TEXT NOT NULL DEFAULT 'system',
    CONSTRAINT equity_curve_portfolio_type_check CHECK (portfolio_type IN ('system', 'qt')),
    CONSTRAINT trading_equity_curve_unique UNIQUE (portfolio_id, strategy_id, "timestamp", portfolio_type));
CREATE TABLE trading.executions (
    exec_id VARCHAR(50) NOT NULL, order_id VARCHAR(50) NOT NULL, symbol VARCHAR(20) NOT NULL, side VARCHAR(4) NOT NULL,
    quantity NUMERIC NOT NULL, price NUMERIC NOT NULL, execution_time TIMESTAMPTZ NOT NULL, commissions_fees NUMERIC NOT NULL,
    is_partial BOOLEAN NOT NULL, created_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP, strategy_id VARCHAR(100) NOT NULL,
    strategy_name VARCHAR(100) NOT NULL, date DATE NOT NULL, portfolio_id VARCHAR(100) NOT NULL,
    implicit_price_impact NUMERIC DEFAULT 0.0, slippage_market_impact NUMERIC DEFAULT 0.0, total_transaction_costs NUMERIC DEFAULT 0.0,
    netting_adjustment NUMERIC DEFAULT 0, execution_type TEXT NOT NULL DEFAULT 'STRATEGY', instrument_id TEXT,
    CONSTRAINT executions_pkey PRIMARY KEY (portfolio_id, strategy_id, strategy_name, date, exec_id),
    CONSTRAINT chk_executions_quantity CHECK (quantity > 0::numeric), CONSTRAINT chk_executions_side CHECK (side IN ('BUY','SELL')));
CREATE INDEX idx_executions_order ON trading.executions (order_id);
CREATE TABLE trading.live_results (id SERIAL PRIMARY KEY, strategy_id VARCHAR(100) NOT NULL, portfolio_id VARCHAR(100) NOT NULL,
    date DATE NOT NULL, daily_pnl NUMERIC, total_pnl NUMERIC, current_portfolio_value NUMERIC, risk_scale NUMERIC, config JSONB,
    risk_detail JSONB, created_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
    CONSTRAINT live_results_portfolio_strategy_date_key UNIQUE (portfolio_id, strategy_id, date));
CREATE INDEX idx_live_results_date ON trading.live_results (date);
INSERT INTO trading.positions (symbol, quantity, average_price, last_update, strategy_id, strategy_name, date, portfolio_id, portfolio_type, instrument_id)
VALUES ('ES.v.0', 3, 5120.25, '2026-04-24 21:00:00+00', 'S', 'TREND', '2026-04-24', 'P', 'system', '42140878'),
       ('ES.v.0', 2, 5120.25, '2026-04-24 21:00:00+00', 'S', 'TREND', '2026-04-24', 'P', 'qt', '42140878'),
       ('AAPL', 10, 190.5, '2026-04-24 21:00:00+00', 'E', 'EQ', '2026-04-24', 'EQP', 'system', NULL);
INSERT INTO trading.equity_curve (strategy_id, "timestamp", equity, portfolio_id, portfolio_type)
VALUES ('S', '2026-04-24 00:00:00+00', 500000, 'P', 'system'), ('S', '2026-04-24 00:00:00+00', 500010, 'P', 'qt');
INSERT INTO trading.executions (exec_id, order_id, symbol, side, quantity, price, execution_time, commissions_fees, is_partial,
    strategy_id, strategy_name, date, portfolio_id, total_transaction_costs)
VALUES ('EXEC_ES.v.0_20260424', 'DAILY_ES.v.0_20260424', 'ES.v.0', 'BUY', 1, 5120.25, '2026-04-24 21:00:00+00', 1.5, false, 'S', 'TREND', '2026-04-24', 'P', 6.5);
INSERT INTO trading.live_results (strategy_id, portfolio_id, date, daily_pnl, total_pnl, current_portfolio_value, risk_scale, config)
VALUES ('S', 'P', '2026-04-24', 120.5, 1000.25, 501000.25, 0.87, '{"a": 1}'), ('E', 'EQP', '2026-04-24', 3, 4, 100004, 1.0, NULL);
SQL
}

fixture
fp() { q "select count(*)||' '||md5(coalesce(string_agg(x, ';' order by x), '')) from (select (to_jsonb(t) - 'portfolio_type' - 'moved_by')::text x from trading.$1 t) s"; }
fpall() { echo "$(fp positions) $(fp equity_curve) $(fp executions) $(fp live_results)"; }
BOOKS0=$(q "select string_agg(portfolio_type, ',' order by portfolio_type, symbol) from trading.positions")
B=$(fpall)
# every index and constraint except the two rebuilt keys and the portfolio_type CHECKs
OTHER_IDX="select string_agg(indexdef, ' | ' order by indexdef) from pg_indexes where schemaname='trading' and indexname not in ('executions_pkey','live_results_portfolio_strategy_date_key')"
OTHER_CON="select string_agg(conname||'='||pg_get_constraintdef(c.oid), ' | ' order by conname) from pg_constraint c join pg_namespace n on n.oid = c.connamespace where n.nspname='trading' and conname not in ('executions_pkey','live_results_portfolio_strategy_date_key') and conname not like '%portfolio_type_check' and conname <> 'positions_moved_by_qt_only'"
I1=$(q "$OTHER_IDX"); C1=$(q "$OTHER_CON")
KEYS0=$(q "select string_agg(conname||'='||pg_get_constraintdef(c.oid), ' | ' order by conname) from pg_constraint c where conname in ('executions_pkey','live_results_portfolio_strategy_date_key','positions_portfolio_type_check','equity_curve_portfolio_type_check')")

# 1
refuses "INSERT INTO trading.positions (symbol, quantity, average_price, last_update, strategy_id, strategy_name, date, portfolio_id, portfolio_type) VALUES ('ES.v.0',1,1,now(),'S','TREND','2026-04-24','P','qt_proposal')" || fail "qt_proposal accepted before 021"
refuses "INSERT INTO trading.executions (exec_id, order_id, symbol, side, quantity, price, execution_time, commissions_fees, is_partial, strategy_id, strategy_name, date, portfolio_id, portfolio_type) VALUES ('X','X','ES.v.0','BUY',1,1,now(),0,false,'S','TREND','2026-04-24','P','qt')" || fail "executions.portfolio_type exists before 021"
refuses "INSERT INTO trading.live_results (strategy_id, portfolio_id, date, portfolio_type) VALUES ('S','P','2026-04-25','qt')" || fail "live_results.portfolio_type exists before 021"
refuses "INSERT INTO trading.live_results (strategy_id, portfolio_id, date) VALUES ('S','P','2026-04-24')" || fail "a second live_results row for the key was accepted before 021"
refuses "UPDATE trading.positions SET moved_by = 'x'" || fail "positions.moved_by exists before 021"
pass "1. before 021: qt_proposal refused; no portfolio_type on executions/live_results; a second book collides; no moved_by"

# 2
apply 021_qt_books.sql
for t in positions equity_curve executions live_results; do
  [[ "$(q "select data_type||' '||is_nullable||' '||column_default from information_schema.columns where table_schema='trading' and table_name='$t' and column_name='portfolio_type'")" == "text NO 'system'::text" ]] || fail "$t.portfolio_type is not text NOT NULL DEFAULT 'system'"
  [[ "$(q "select pg_get_constraintdef(oid) from pg_constraint where conname='${t}_portfolio_type_check'")" == *"'system'"*"'qt_proposal'"*"'qt'"* ]] || fail "$t CHECK not widened"
done
[[ "$(q "select pg_get_constraintdef(oid) from pg_constraint where conname='executions_pkey'")" == "PRIMARY KEY (portfolio_id, strategy_id, strategy_name, date, exec_id, portfolio_type)" ]] || fail "executions_pkey"
[[ "$(q "select pg_get_constraintdef(oid) from pg_constraint where conname='live_results_portfolio_strategy_date_key'")" == "UNIQUE (portfolio_id, strategy_id, date, portfolio_type)" ]] || fail "live_results key"
[[ "$(q "select data_type||' '||is_nullable||' '||coalesce(column_default,'-') from information_schema.columns where table_schema='trading' and table_name='positions' and column_name='moved_by'")" == "text YES -" ]] || fail "moved_by is not text NULL with no default"
[[ "$(q "select count(*) from trading.executions where portfolio_type='system'")|$(q "select count(*) from trading.live_results where portfolio_type='system'")|$(q "select count(*) from trading.positions where moved_by is not null")" == "1|2|0" ]] || fail "history is not system / moved_by not NULL"
for bk in qt_proposal qt; do
  $PSQL -c "INSERT INTO trading.positions (symbol, quantity, average_price, last_update, strategy_id, strategy_name, date, portfolio_id, portfolio_type) VALUES ('ES.v.0',1,5120.25,'2026-04-24 21:00:00+00','S','TREND','2026-04-24','P','$bk') ON CONFLICT DO NOTHING"
  $PSQL -c "INSERT INTO trading.equity_curve (strategy_id, \"timestamp\", equity, portfolio_id, portfolio_type) VALUES ('S','2026-04-24 00:00:00+00',499000,'P','$bk') ON CONFLICT DO NOTHING"
  $PSQL -c "INSERT INTO trading.executions (exec_id, order_id, symbol, side, quantity, price, execution_time, commissions_fees, is_partial, strategy_id, strategy_name, date, portfolio_id, portfolio_type) VALUES ('EXEC_ES.v.0_20260424','DAILY_ES.v.0_20260424','ES.v.0','SELL',2,5120.25,'2026-04-24 21:00:00+00',3,false,'S','TREND','2026-04-24','P','$bk')"
  $PSQL -c "INSERT INTO trading.live_results (strategy_id, portfolio_id, date, daily_pnl, portfolio_type) VALUES ('S','P','2026-04-24',7,'$bk')"
done
[[ "$(q "select string_agg(portfolio_type, ',' order by portfolio_type) from trading.positions where symbol='ES.v.0'")" == "qt,qt_proposal,system" ]] || fail "three positions books do not coexist"
[[ "$(q "select string_agg(portfolio_type, ',' order by portfolio_type) from trading.equity_curve")" == "qt,qt_proposal,system" ]] || fail "three equity books do not coexist"
[[ "$(q "select string_agg(portfolio_type, ',' order by portfolio_type) from trading.executions")" == "qt,qt_proposal,system" ]] || fail "three execution books do not coexist"
[[ "$(q "select string_agg(portfolio_type, ',' order by portfolio_type) from trading.live_results where strategy_id='S'")" == "qt,qt_proposal,system" ]] || fail "three live_results books do not coexist"
refuses "INSERT INTO trading.live_results (strategy_id, portfolio_id, date, portfolio_type) VALUES ('S','P','2026-04-24','qt')" || fail "the key no longer refuses a duplicate within a book"
for t in "positions (symbol, quantity, average_price, last_update, strategy_id, strategy_name, date, portfolio_id, portfolio_type) VALUES ('NQ.v.0',1,1,now(),'S','TREND','2026-04-24','P'" \
         "equity_curve (strategy_id, \"timestamp\", equity, portfolio_id, portfolio_type) VALUES ('S',now(),1,'P'" \
         "live_results (strategy_id, portfolio_id, date, portfolio_type) VALUES ('S','P','2026-04-29'"; do
  refuses "INSERT INTO trading.$t,'desk')" || fail "a fourth book accepted: $t"
done
$PSQL -c "INSERT INTO trading.live_results (strategy_id, portfolio_id, date) VALUES ('S','P','2026-04-26')"
[[ "$(q "select portfolio_type from trading.live_results where date='2026-04-26'")" == "system" ]] || fail "an omitting INSERT is not system"
refuses "UPDATE trading.positions SET moved_by='cap' WHERE portfolio_type='system'" || fail "moved_by accepted on system"
refuses "UPDATE trading.positions SET moved_by='cap' WHERE portfolio_type='qt_proposal'" || fail "moved_by accepted on qt_proposal"
$PSQL -c "UPDATE trading.positions SET moved_by='cap_bound' WHERE portfolio_type='qt' AND symbol='ES.v.0'"
[[ "$(q "select moved_by from trading.positions where portfolio_type='qt' and symbol='ES.v.0'")" == "cap_bound" ]] || fail "moved_by does not round-trip on qt"
pass "2. column, CHECK and keys on all four tables; three books coexist per key; a fourth refused; default system; moved_by qt-only"

# 3
$PSQL -c "DELETE FROM trading.positions WHERE portfolio_type='qt_proposal'; UPDATE trading.positions SET moved_by=NULL; DELETE FROM trading.equity_curve WHERE portfolio_type='qt_proposal' OR equity=499000;
          DELETE FROM trading.executions WHERE portfolio_type<>'system'; DELETE FROM trading.live_results WHERE portfolio_type<>'system' OR date='2026-04-26'"
[[ "$(fpall)" == "$B" ]] || fail "pre-existing rows moved"
[[ "$(q "select string_agg(portfolio_type, ',' order by portfolio_type, symbol) from trading.positions")" == "$BOOKS0" ]] || fail "positions books moved"
[[ "$(q "$OTHER_IDX")" == "$I1" ]] || fail "indexes outside the rebuilt keys changed"
[[ "$(q "$OTHER_CON")" == "$C1" ]] || fail "constraints outside the rebuilt keys changed"
pass "3. pre-existing rows byte-identical apart from the new keys; every other index and constraint unchanged"

# 4
apply 021_qt_books.sql
[[ "$(fpall)" == "$B" && "$(q "select pg_get_constraintdef(oid) from pg_constraint where conname='executions_pkey'")" == "PRIMARY KEY (portfolio_id, strategy_id, strategy_name, date, exec_id, portfolio_type)" ]] || fail "second apply moved something"
$PSQL -c "ALTER TABLE trading.positions DROP CONSTRAINT positions_moved_by_qt_only; ALTER TABLE trading.positions ALTER COLUMN moved_by TYPE integer USING NULL"
refuses_file 021_qt_books.sql || fail "021 accepted a moved_by of another type"
$PSQL -c "ALTER TABLE trading.positions ALTER COLUMN moved_by TYPE text USING NULL"
apply 021_qt_books.sql
fixture
$PSQL -c "ALTER TABLE trading.executions ADD COLUMN portfolio_type integer"
refuses_file 021_qt_books.sql || fail "021 accepted an executions.portfolio_type of another type"
[[ "$(q "select count(*) from pg_constraint where conname='executions_portfolio_type_check'")" == "0" ]] || fail "a refused 021 changed something"
fixture
$PSQL -c "ALTER TABLE trading.live_results DROP CONSTRAINT live_results_portfolio_strategy_date_key"
refuses_file 021_qt_books.sql || fail "021 accepted a live_results without its key"
fixture
$PSQL -c "ALTER TABLE trading.equity_curve DROP COLUMN portfolio_type"
refuses_file 021_qt_books.sql || fail "021 accepted a database without migration 001"
fixture
$PSQL -c "ALTER TABLE trading.live_results RENAME CONSTRAINT live_results_portfolio_strategy_date_key TO live_results_key"
apply 021_qt_books.sql
[[ "$(q "select pg_get_constraintdef(oid) from pg_constraint where conname='live_results_key'")" == "UNIQUE (portfolio_id, strategy_id, date, portfolio_type)" ]] || fail "the key is not found by its columns under another name"
fixture
B=$(fpall)  # the fixture's DEFAULT CURRENT_TIMESTAMP columns are re-stamped on each rebuild
apply 021_qt_books.sql
[[ "$(fpall)" == "$B" ]] || fail "021 on a fresh fixture moved rows"
pass "4. idempotent; type-guarded; refuses without 001 or the live_results key; finds the key by its columns, not its name"

# 5
cmt() { q "select col_description('trading.$1'::regclass,(select ordinal_position from information_schema.columns where table_schema='trading' and table_name='$1' and column_name='$2'))"; }
[[ "$(cmt executions portfolio_type)" == *"qt_proposal"*"executions_pkey"* ]] || fail "executions.portfolio_type comment"
[[ "$(cmt live_results portfolio_type)" == *"qt_proposal"*"key"* ]] || fail "live_results.portfolio_type comment"
[[ "$(cmt positions moved_by)" == *"one-pass step"*"migration 021"* ]] || fail "positions.moved_by comment"
pass "5. the three comments set"

# 6
$PSQL -c "INSERT INTO trading.executions (exec_id, order_id, symbol, side, quantity, price, execution_time, commissions_fees, is_partial, strategy_id, strategy_name, date, portfolio_id, portfolio_type) VALUES ('EXEC_ES.v.0_20260424','DAILY_ES.v.0_20260424','ES.v.0','SELL',2,5120.25,'2026-04-24 21:00:00+00',3,false,'S','TREND','2026-04-24','P','qt')"
refuses_file 021_qt_books_rollback.sql || fail "the rollback did not refuse a qt execution"
[[ "$(q "select count(*) from information_schema.columns where table_schema='trading' and column_name in ('portfolio_type','moved_by')")" == "5" ]] || fail "a refused rollback dropped a column"
$PSQL -c "DELETE FROM trading.executions WHERE portfolio_type='qt'"
$PSQL -c "INSERT INTO trading.live_results (strategy_id, portfolio_id, date, portfolio_type) VALUES ('S','P','2026-04-24','qt_proposal')"
refuses_file 021_qt_books_rollback.sql || fail "the rollback did not refuse a qt_proposal live_results row"
$PSQL -c "DELETE FROM trading.live_results WHERE portfolio_type<>'system'"
$PSQL -c "INSERT INTO trading.positions (symbol, quantity, average_price, last_update, strategy_id, strategy_name, date, portfolio_id, portfolio_type) VALUES ('ES.v.0',1,1,now(),'S','TREND','2026-04-24','P','qt_proposal')"
refuses_file 021_qt_books_rollback.sql || fail "the rollback did not refuse a qt_proposal position"
$PSQL -c "DELETE FROM trading.positions WHERE portfolio_type='qt_proposal'; UPDATE trading.positions SET moved_by='trim' WHERE portfolio_type='qt'"
refuses_file 021_qt_books_rollback.sql || fail "the rollback did not refuse a moved_by"
$PSQL -c "UPDATE trading.positions SET moved_by=NULL"
apply 021_qt_books_rollback.sql
[[ "$(q "select count(*) from information_schema.columns where table_schema='trading' and ((table_name in ('executions','live_results') and column_name='portfolio_type') or column_name='moved_by')")" == "0" ]] || fail "the rollback left a column"
[[ "$(q "select string_agg(conname||'='||pg_get_constraintdef(c.oid), ' | ' order by conname) from pg_constraint c where conname in ('executions_pkey','live_results_portfolio_strategy_date_key','positions_portfolio_type_check','equity_curve_portfolio_type_check')")" == "$KEYS0" ]] || fail "the rollback did not restore the keys and CHECKs"
[[ "$(fpall)" == "$B" ]] || fail "rows differ after the rollback"
[[ "$(q "$OTHER_IDX")" == "$I1" && "$(q "$OTHER_CON")" == "$C1" ]] || fail "the rollback changed other indexes or constraints"
apply 021_qt_books_rollback.sql
apply 021_qt_books.sql
[[ "$(fpall)" == "$B" ]] || fail "re-apply after the rollback moved rows"
pass "6. the rollback refuses while a non-system row or a moved_by exists; then restores keys and CHECKs, rows byte-identical; idempotent; 021 re-applies"
echo "ALL OK"
