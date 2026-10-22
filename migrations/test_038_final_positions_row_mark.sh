#!/usr/bin/env bash
# This tests 038_final_positions_row_mark.sql, numbered 032 until 2026-10-21. The text below keeps the numbers of that time
# (032 and its neighbours 028 to 033): the SQL is unchanged and the comments it stores name them.
# Verifies migration 032 (backtest.final_positions.row_mark with its CHECK and comment, the five
# ratio comments it restates and the table comment of backtest.signals) and its rollback on a
# real PostgreSQL.
#   1. absent before (an INSERT naming row_mark fails: the migration is necessary);
#   2. after: row_mark is text, NULL-able, no default, the last column; one CHECK
#      (row_mark = 'NO_BAR');
#   3. NO BACKFILL: every pre-existing row holds NULL and is byte-identical in every other
#      column (the other tables too); indexes as before; one constraint more, the CHECK; an
#      omitting INSERT reads NULL;
#   4. the column holds what the writer sends: 'NO_BAR' on a no-bar row beside realized_pnl 0,
#      NULL on a bar row, both in one multi-row INSERT; any other value is refused by the CHECK
#      (another spelling, an empty string, another word);
#   5. idempotent (a second apply keeps the values and adds no second CHECK) and guarded (a
#      row_mark of another type, a CHECK of that name with another definition, a missing
#      commented column or table: 032 refuses);
#   6. the comments: row_mark's says what the mark means, where it is NULL and that there is no
#      backfill; the five ratio comments say NULL where there is no denominator, name what rows
#      before 032 hold, and are otherwise the earlier text; backtest.signals says no writer;
#      no other comment moved;
#   7. the rollback refuses while a row carries a mark and, with migration.force_rollback =
#      'yes', drops the column and its CHECK with the original rows byte-identical; EVERY
#      comment of the four tables (pg_description), the column lists and the constraints are
#      then exactly as before 032; with no mark it drops without the setting; idempotent.
# DESTRUCTIVE on the target (DROP SCHEMA trading, backtest CASCADE): a THROWAWAY database only;
# production and every stage-3 scratch or clone refused by name, any database with the schemas
# refused.
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
FWD=038_final_positions_row_mark.sql; RB=038_final_positions_row_mark_rollback.sql
present() { q "select count(*) from information_schema.columns where table_schema='backtest' and table_name='final_positions' and column_name='row_mark'"; }
checkdef() { q "select coalesce((select pg_get_constraintdef(oid) from pg_constraint where conrelid='backtest.final_positions'::regclass and conname='final_positions_row_mark_check'), 'none')"; }

# The four tables in the shape the lane leaves them (the clone's column lists, cut to what 032
# reads), with rows written before 032.
$PSQL <<'SQL'
SET TimeZone = 'UTC';
CREATE SCHEMA trading;
CREATE SCHEMA backtest;
CREATE TABLE trading.live_results (id SERIAL PRIMARY KEY, strategy_id VARCHAR(100) NOT NULL, portfolio_id VARCHAR(100),
    date DATE NOT NULL, profit_factor DOUBLE PRECISION, gross_profit DOUBLE PRECISION, gross_loss DOUBLE PRECISION);
CREATE TABLE backtest.results (run_id TEXT PRIMARY KEY, total_return DOUBLE PRECISION, sharpe_ratio DOUBLE PRECISION,
    sortino_ratio DOUBLE PRECISION, max_drawdown DOUBLE PRECISION, calmar_ratio DOUBLE PRECISION,
    volatility DOUBLE PRECISION, profit_factor DOUBLE PRECISION, total_trades INTEGER, portfolio_id VARCHAR(50));
CREATE TABLE backtest.final_positions (run_id TEXT NOT NULL, symbol TEXT NOT NULL, quantity DOUBLE PRECISION,
    average_price DOUBLE PRECISION, unrealized_pnl DOUBLE PRECISION, realized_pnl DOUBLE PRECISION,
    strategy_id VARCHAR(100) NOT NULL, last_update TIMESTAMP NOT NULL, updated_at TIMESTAMP NOT NULL,
    date DATE NOT NULL, portfolio_id VARCHAR(100), instrument_id TEXT,
    CONSTRAINT final_positions_pkey PRIMARY KEY (run_id, strategy_id, date, symbol));
CREATE INDEX idx_backtest_final_positions_run_date ON backtest.final_positions (run_id, date);
CREATE TABLE backtest.signals (id BIGSERIAL PRIMARY KEY, run_id TEXT, symbol TEXT, signal_value DOUBLE PRECISION,
    "timestamp" TIMESTAMPTZ);
INSERT INTO trading.live_results (strategy_id, portfolio_id, date, profit_factor, gross_profit, gross_loss) VALUES
    ('C', 'CONSERVATIVE_PORTFOLIO', '2025-10-06', 999.99, 1180.25, 0), ('C', 'CONSERVATIVE_PORTFOLIO', '2025-10-05', 0, 0, 0);
INSERT INTO backtest.results (run_id, total_return, sharpe_ratio, sortino_ratio, max_drawdown, calmar_ratio, volatility, profit_factor, total_trades, portfolio_id)
    VALUES ('OLD_RUN', 0.616818, 1.20523, 999, 0, 999, 0.200073, 999, 300, 'CONSERVATIVE_PORTFOLIO');
-- a bar row and a no-bar row (a Sunday KE has no session on) as a binary before 032 wrote them
INSERT INTO backtest.final_positions (run_id, symbol, quantity, average_price, unrealized_pnl, realized_pnl, strategy_id, last_update, updated_at, date, portfolio_id, instrument_id) VALUES
    ('OLD_RUN', 'KE.v.0', 3, 541.25, 0, -412.5, 'TREND', '2025-05-16 00:00:00', '2025-05-16 00:00:00', '2025-05-16', 'CONSERVATIVE_PORTFOLIO', '42001'),
    ('OLD_RUN', 'KE.v.0', 3, 541.25, 0, 0, 'TREND', '2025-05-18 00:00:00', '2025-05-18 00:00:00', '2025-05-18', 'CONSERVATIVE_PORTFOLIO', '42001');
COMMENT ON TABLE backtest.signals IS 'Strategy signals generated during backtest';
COMMENT ON COLUMN backtest.final_positions.instrument_id IS 'the 016 text';
COMMENT ON COLUMN backtest.results.volatility IS 'the 031 text of another column';
SQL
# the texts the lane leaves on the five ratio columns before 032: 028's and 031's own statements
for c in 'trading.live_results."profit_factor"' 'backtest.results."profit_factor"'; do grep -F "COMMENT ON COLUMN $c IS " "$HERE/034_column_comments.sql" | $PSQL; done
for c in sharpe_ratio sortino_ratio calmar_ratio; do grep -F "COMMENT ON COLUMN backtest.results.\"$c\" IS " "$HERE/037_backtest_statistics.sql" | $PSQL; done
fp() { q "select count(*)||' '||md5(coalesce(string_agg(x, ';' order by x), '')) from (select (to_jsonb(t) - 'row_mark')::text x from $1 t) s"; }
# every comment of the four tables, their column lists with type, null-ness and default, their constraints
comments() { q "select md5(coalesce(string_agg(n.nspname||'.'||c.relname||'|'||d.objsubid||'|'||coalesce(a.attname,'')||'|'||d.description, E'\n' order by n.nspname, c.relname, d.objsubid), '')) || ' ' || count(*) from pg_description d join pg_class c on c.oid = d.objoid and d.classoid = 'pg_class'::regclass join pg_namespace n on n.oid = c.relnamespace left join pg_attribute a on a.attrelid = c.oid and a.attnum = d.objsubid and d.objsubid > 0 where (n.nspname, c.relname) in (('trading','live_results'),('backtest','results'),('backtest','final_positions'),('backtest','signals'))"; }
schema() { q "select md5(string_agg(table_schema||'.'||table_name||'.'||column_name||':'||data_type||':'||is_nullable||':'||coalesce(column_default,'-'), ' ' order by table_schema, table_name, ordinal_position)) from information_schema.columns where table_schema in ('trading','backtest')"; }
constraints() { q "select coalesce(string_agg(c.conrelid::regclass||':'||c.conname||':'||pg_get_constraintdef(c.oid), ' | ' order by c.conrelid::regclass::text, c.conname), '') from pg_constraint c join pg_namespace n on n.oid = c.connamespace where n.nspname in ('trading','backtest') ${1:-}"; }
indexes() { q "select string_agg(indexdef, ' | ' order by indexdef) from pg_indexes where schemaname in ('trading','backtest')"; }
cmt() { q "select col_description('$1.$2'::regclass,(select ordinal_position from information_schema.columns where table_schema='$1' and table_name='$2' and column_name='$3'))"; }
B1=$(fp backtest.final_positions); B2=$(fp backtest.results); B3=$(fp trading.live_results); B4=$(fp backtest.signals)
D1=$(comments); S1=$(schema); K1=$(constraints); I1=$(indexes)
[[ "${D1##* }" == "8" ]] || fail "the fixture does not carry the eight comments of the lane before 032: $D1"
LPF=$(cmt trading live_results profit_factor); BPF=$(cmt backtest results profit_factor)
SHA=$(cmt backtest results sharpe_ratio); SOR=$(cmt backtest results sortino_ratio); CAL=$(cmt backtest results calmar_ratio)
[[ "$LPF" == *"999.99 when there is profit and no loss; 0 with neither."* && "$BPF" == *"999 when there is profit and no loss."* && "$SHA" == *"0 when volatility is 0."* && "$SOR" == *"999 when there is no downside"* && "$CAL" == *"999 when there is no drawdown"* ]] || fail "the fixture's five ratio comments are not 028's and 031's"
# 1
[[ "$(present)" == "0" ]] || fail "row_mark exists before 032"
refuses "INSERT INTO backtest.final_positions (run_id, symbol, strategy_id, last_update, updated_at, date, row_mark) VALUES ('X', 'KE.v.0', 'TREND', now(), now(), '2025-05-25', 'NO_BAR')" || fail "row_mark exists before 032"
pass "1. row_mark is absent before the migration"
# 2
apply $FWD
[[ "$(q "select data_type||':'||is_nullable||':'||coalesce(column_default,'-') from information_schema.columns where table_schema='backtest' and table_name='final_positions' and column_name='row_mark'")" == "text:YES:-" ]] || fail "row_mark is not text, NULL-able, no default"
[[ "$(q "select string_agg(column_name, ' ' order by ordinal_position) from information_schema.columns where table_schema='backtest' and table_name='final_positions'")" == "run_id symbol quantity average_price unrealized_pnl realized_pnl strategy_id last_update updated_at date portfolio_id instrument_id row_mark" ]] || fail "row_mark is not the last column"
[[ "$(checkdef)" == "CHECK ((row_mark = 'NO_BAR'::text))" ]] || fail "the CHECK is not (row_mark = 'NO_BAR'): $(checkdef)"
pass "2. row_mark text, NULL-able, no default, the last column; CHECK (row_mark = 'NO_BAR')"
# 3
[[ "$(q "select count(*) from backtest.final_positions where row_mark is null")" == "2" ]] || fail "an existing row holds a mark"
[[ "$(fp backtest.final_positions)" == "$B1" && "$(fp backtest.results)" == "$B2" && "$(fp trading.live_results)" == "$B3" && "$(fp backtest.signals)" == "$B4" ]] || fail "rows moved"
[[ "$(indexes)" == "$I1" ]] || fail "indexes changed"
[[ "$(constraints "and c.conname <> 'final_positions_row_mark_check'")" == "$K1" && "$(q "select count(*) from pg_constraint where conrelid='backtest.final_positions'::regclass and contype='c'")" == "1" ]] || fail "the constraints are not the earlier ones plus the one CHECK: $(constraints)"
$PSQL -c "INSERT INTO backtest.final_positions (run_id, symbol, quantity, average_price, unrealized_pnl, realized_pnl, strategy_id, last_update, updated_at, date, portfolio_id) VALUES ('EQ_RUN', 'AAPL', 10, 228.5, 12.5, 0, 'MR', '2026-08-06', '2026-08-06', '2026-08-06', 'EQUITY_MR_PORTFOLIO')"
[[ "$(q "select count(*) from backtest.final_positions where run_id='EQ_RUN' and row_mark is null")" == "1" ]] || fail "an omitting INSERT is not NULL"
pass "3. no backfill: the existing rows hold NULL and are byte-identical elsewhere (the other three tables too); indexes unchanged; one constraint more, the CHECK; an omitting INSERT reads NULL"
# 4 (the writer's statement: every row names the column, NULL or the mark)
$PSQL -c "INSERT INTO backtest.final_positions (run_id, portfolio_id, strategy_id, date, symbol, quantity, average_price, unrealized_pnl, realized_pnl, last_update, updated_at, instrument_id, row_mark) VALUES ('NEW_RUN', 'CONSERVATIVE_PORTFOLIO', 'TREND', '2026-06-13', 'KE.v.0', 3.000000, 541.250000, 0.000000, 0.000000, '2026-06-13 00:00:00', '2026-06-13 00:00:00', '42001', 'NO_BAR'), ('NEW_RUN', 'CONSERVATIVE_PORTFOLIO', 'TREND', '2026-06-13', 'MBT.v.0', 1.000000, 104250.000000, 0.000000, 152.500000, '2026-06-13 00:00:00', '2026-06-13 00:00:00', '42777', NULL)"
[[ "$(q "select string_agg(symbol||'='||coalesce(row_mark,'NULL')||'/'||realized_pnl||'/'||quantity||'/'||average_price, ' ' order by symbol) from backtest.final_positions where run_id='NEW_RUN'")" == "KE.v.0=NO_BAR/0/3/541.25 MBT.v.0=NULL/152.5/1/104250" ]] || fail "the mark and the NULL did not round trip"
for bad in "'no_bar'" "''" "'JUNK'" "'NO_BAR '" "'HELD'"; do
  refuses "INSERT INTO backtest.final_positions (run_id, symbol, strategy_id, last_update, updated_at, date, row_mark) VALUES ('BAD', 'KE.v.0', 'TREND', now(), now(), '2026-06-14', $bad)" || fail "row_mark took $bad"
done
refuses "UPDATE backtest.final_positions SET row_mark = 'CLOSED' WHERE run_id = 'NEW_RUN'" || fail "an UPDATE to another value passed the CHECK"
pass "4. 'NO_BAR' beside realized_pnl 0 and NULL beside a booked figure are stored as sent in one statement; another spelling, an empty string, a trailing space and other words are refused"
# 5
apply $FWD
[[ "$(q "select string_agg(symbol||'='||coalesce(row_mark,'NULL'), ' ' order by symbol) from backtest.final_positions where run_id='NEW_RUN'")" == "KE.v.0=NO_BAR MBT.v.0=NULL" ]] || fail "a second apply changed a value"
[[ "$(q "select count(*) from pg_constraint where conrelid='backtest.final_positions'::regclass and contype='c'")" == "1" ]] || fail "a second apply added a second CHECK"
$PSQL -c "ALTER TABLE backtest.final_positions DROP CONSTRAINT final_positions_row_mark_check; ALTER TABLE backtest.final_positions ALTER COLUMN row_mark TYPE varchar(16)"
refuses_file $FWD || fail "032 type guard did not refuse a row_mark of another type"
$PSQL -c "ALTER TABLE backtest.final_positions ALTER COLUMN row_mark TYPE text; ALTER TABLE backtest.final_positions ADD CONSTRAINT final_positions_row_mark_check CHECK (row_mark IN ('NO_BAR', 'JUNK'))"
refuses_file $FWD || fail "032 did not refuse a CHECK of its name with another definition"
$PSQL -c "ALTER TABLE backtest.final_positions DROP CONSTRAINT final_positions_row_mark_check"
apply $FWD
[[ "$(checkdef)" == "CHECK ((row_mark = 'NO_BAR'::text))" ]] || fail "032 did not add the CHECK to a column that had none"
$PSQL -c "ALTER TABLE backtest.results RENAME COLUMN calmar_ratio TO cr"
refuses_file $FWD || fail "032 did not refuse without a column it comments"
$PSQL -c "ALTER TABLE backtest.results RENAME COLUMN cr TO calmar_ratio; ALTER TABLE backtest.signals RENAME TO signals_x"
refuses_file $FWD || fail "032 did not refuse without backtest.signals"
$PSQL -c "ALTER TABLE backtest.signals_x RENAME TO signals"
pass "5. a second apply keeps every value and adds no second CHECK; a row_mark of another type, a CHECK of its name with another definition, a missing commented column and a missing backtest.signals make 032 refuse; a column without its CHECK gets it"
# 6
RM="$(cmt backtest final_positions row_mark)"
[[ "$RM" == "'NO_BAR' or NULL; the CHECK admits no other value. 'NO_BAR' means: no bar, held, marked at last close."*"FUTURES row whose symbol printed no bar on the row's date"*"quantity and average_price are written as on any other row"*"realized_pnl is 0"*"the equity curve books nothing for the symbol that date"*"NULL on every other row"*"the row is NOT marked"*"every row of an equity (cash) book"*"every row written before migration 032 (no backfill)"*"T-8D-2 R50"*"migration 032." ]] || fail "the row_mark comment: $RM"
NEW_LPF=$(cmt trading live_results profit_factor); NEW_BPF=$(cmt backtest results profit_factor)
NEW_SHA=$(cmt backtest results sharpe_ratio); NEW_SOR=$(cmt backtest results sortino_ratio); NEW_CAL=$(cmt backtest results calmar_ratio)
for t in "$NEW_LPF" "$NEW_BPF" "$NEW_SHA" "$NEW_SOR" "$NEW_CAL"; do
  [[ "$t" == *"NULL when "*"the ratio has no denominator, so it has no value, and NULL is not a measured 0. Rows written before migration 032 hold "* ]] || fail "a ratio comment does not say NULL with no denominator and what rows before 032 hold: $t"
done
[[ "$NEW_LPF" == "gross_profit / gross_loss. NULL when gross_loss is 0 (no losing session on the grid; an empty series included)"*"999.99 with a profit and no loss, 0 with neither. The grid, K, the base and the settled-row rule are in the comment on the table. Rows written before migration 028 keep the earlier definition." ]] || fail "the live profit_factor comment"
[[ "$NEW_BPF" == "The summed P&L of the winning trades over the summed loss of the others. NULL when that loss is 0 (no losing trade; a run with no trade included)"*"999 with a profit and no loss, 0 with neither. A trade and its P&L in dollars are defined on total_trades. Rows written before migration 028 keep the earlier definition." ]] || fail "the backtest profit_factor comment"
[[ "$NEW_SHA" == "The mean grid return x K over volatility, risk-free rate 0. NULL when volatility is 0 or the series has no return"*"hold 0 there instead. Taken on the run's statistics grid"*"Rows written before migration 031 keep the earlier definition." ]] || fail "the sharpe_ratio comment"
[[ "$NEW_SOR" == "The mean grid return x K over downside_volatility. NULL when downside_volatility is 0 (no grid return below 0) or the series has no return"*"999 when the mean is not negative, 0 when it is. Taken on the run's statistics grid"*"Rows written before migration 031 keep the earlier definition." ]] || fail "the sortino_ratio comment"
[[ "$NEW_CAL" == "The annualised return on the grid, the CAGR (1 + R)^(K / n) - 1"*"over max_drawdown. NULL when max_drawdown is 0"*"999 when the CAGR is not negative, 0 when it is. Taken on the run's statistics grid"*"Rows written before migration 031 keep the earlier definition." ]] || fail "the calmar_ratio comment"
# each restated text is the earlier one with exactly its one clause replaced: the text before the clause and after it are kept
[[ "${NEW_LPF#*neither. }" == "${LPF#*neither. }" && "${NEW_BPF#*neither. }" == "${BPF#*no loss. }" && "${NEW_SHA#*instead. }" == "${SHA#*is 0. }" && "${NEW_SOR#*when it is. }" == "${SOR#*not negative. }" && "${NEW_CAL#*when it is. }" == "${CAL#*not negative. }" ]] || fail "a restated comment's tail differs from the earlier text's"
[[ "${NEW_CAL%%over max_drawdown*}" == "${CAL%%over max_drawdown*}" ]] || fail "the calmar comment's definition moved"
[[ "$(q "select obj_description('backtest.signals'::regclass)")" == "No writer; reserved. No engine path writes this table"*"whoever adds one restates this comment and the columns' meaning." ]] || fail "the backtest.signals comment"
[[ "$(cmt backtest final_positions instrument_id)" == "the 016 text" && "$(cmt backtest results volatility)" == "the 031 text of another column" ]] || fail "032 moved a comment it does not own"
[[ "$(comments)" == *" 9" ]] || fail "the four tables do not carry 9 comments after 032 (8 before + row_mark's): $(comments)"
pass "6. the row_mark comment says what the mark means, where it is NULL and that there is no backfill; the five ratio comments say NULL with no denominator and what rows before 032 hold, the rest of each text kept; backtest.signals says no writer; no other comment moved"
# 7
refuses_file $RB || fail "the rollback did not refuse while a row carries a mark"
[[ "$(present)" == "1" && "$(checkdef)" != "none" ]] || fail "a refused rollback dropped the column or its CHECK"
[[ "$(cmt backtest results sortino_ratio)" == "$NEW_SOR" ]] || fail "a refused rollback restored a comment"
psql -X -q -v ON_ERROR_STOP=1 -c "SET migration.force_rollback = 'yes'" -f "$HERE/$RB"
[[ "$(present)" == "0" && "$(checkdef)" == "none" ]] || fail "the forced rollback left the column or its CHECK"
$PSQL -c "DELETE FROM backtest.final_positions WHERE run_id IN ('EQ_RUN','NEW_RUN')"
[[ "$(fp backtest.final_positions)" == "$B1" && "$(fp backtest.results)" == "$B2" && "$(fp trading.live_results)" == "$B3" && "$(fp backtest.signals)" == "$B4" ]] || fail "rows differ after the rollback"
[[ "$(comments)" == "$D1" ]] || fail "the comments of the four tables are not exactly as before 032: $(comments) against $D1"
[[ "$(schema)" == "$S1" && "$(constraints)" == "$K1" && "$(indexes)" == "$I1" ]] || fail "the column lists, constraints or indexes are not exactly as before 032"
[[ "$(cmt trading live_results profit_factor)" == "$LPF" && "$(cmt backtest results profit_factor)" == "$BPF" && "$(cmt backtest results sharpe_ratio)" == "$SHA" && "$(cmt backtest results sortino_ratio)" == "$SOR" && "$(cmt backtest results calmar_ratio)" == "$CAL" ]] || fail "a ratio comment is not the earlier text byte for byte"
[[ "$(q "select obj_description('backtest.signals'::regclass)")" == "Strategy signals generated during backtest" ]] || fail "the backtest.signals comment is not the earlier text"
apply $FWD
apply $RB
[[ "$(present)" == "0" ]] || fail "the rollback of a column with no mark needed the setting"
apply $RB
[[ "$(comments)" == "$D1" && "$(schema)" == "$S1" && "$(constraints)" == "$K1" ]] || fail "a second rollback changed the comments, the columns or the constraints"
pass "7. the rollback refuses while a row carries a mark; forced, it drops the column and its CHECK, rows byte-identical, every comment of the four tables (pg_description: $D1), the column lists, constraints and indexes exactly as before 032; with no mark it drops without the setting; idempotent"
echo "ALL OK"
