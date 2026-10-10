#!/usr/bin/env bash
# Verifies migration 030 (the twenty statistic and overlay columns of trading.live_results and
# their comments) and its rollback on a real PostgreSQL.
#   1. absent before (an INSERT naming one of the columns fails: the migration is necessary);
#   2. after: the twenty columns with their types, NULL-able, no default;
#   3. NO BACKFILL: every pre-existing row holds NULL in all twenty and is byte-identical in
#      every other column; indexes and constraints as before; an omitting INSERT reads NULL;
#   4. the types hold what the writers send: a date, a symbol, a jsonb object of years, three
#      integers, a number of ten significant digits, a SIGNED net leverage;
#   5. idempotent (a second apply keeps the values) and type-guarded (a column of another type
#      makes 030 refuse);
#   6. the comments: every one of the twenty says rows written before migration 030 hold NULL;
#      total_strategy_fills says it counts the account's fills after netting between sleeves; the
#      overlay columns quote their limits, the coverage, the signed net with the limit on its
#      absolute value, and how each differs from its reporter namesake; var_95_1d its formula;
#   7. the rollback refuses while a cell carries a value and, with migration.force_rollback =
#      'yes', drops the twenty with the original rows byte-identical; with no value it drops
#      without the setting; idempotent.
# DESTRUCTIVE on the target (DROP SCHEMA trading CASCADE): a THROWAWAY database only; production
# and every stage-3 scratch or clone refused by name, any database with the schema refused.
set -euo pipefail
[[ "${MIGRATION_TEST_DB:-}" == "" ]] && { echo "REFUSING: set MIGRATION_TEST_DB and PGDATABASE" >&2; exit 2; }
[[ "${PGDATABASE:-}" != "${MIGRATION_TEST_DB}" ]] && { echo "REFUSING: PGDATABASE != MIGRATION_TEST_DB" >&2; exit 2; }
case "${MIGRATION_TEST_DB}" in new_algo_data|new_algo_data_*|*_new_algo_data|algo_data) echo "REFUSING: production or a clone" >&2; exit 2;; esac
existing=$(psql -X -tAc "SELECT count(*) FROM information_schema.schemata WHERE schema_name IN ('trading','backtest')" 2>/dev/null || echo 0)
[[ "${existing:-0}" != "0" ]] && { echo "REFUSING: the target already has a trading or backtest schema" >&2; exit 2; }
HERE="$(cd "$(dirname "$0")" && pwd)"
PSQL="psql -v ON_ERROR_STOP=1 -q -X"
q() { psql -X -tAc "$1"; }
fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok: $*"; }
apply() { $PSQL -f "$HERE/$1"; }
refuses() { if psql -X -q -v ON_ERROR_STOP=1 -c "$1" >/dev/null 2>&1; then return 1; else return 0; fi; }
COLS="worst_day_date worst_day_symbol max_drawdown_sizing volatility_sizing worst_day_sizing worst_day_sizing_date worst_day_sizing_symbol monthly_skew monthly_tail_ratio calendar_year_returns losing_years total_trades total_strategy_fills total_roll_fills overlay_risk overlay_risk_jump overlay_risk_shock overlay_gross_leverage overlay_net_leverage var_95_1d"
MINUS=""; ALLNULL=""; for c in $COLS; do MINUS="$MINUS - '$c'"; ALLNULL="$ALLNULL AND $c IS NULL"; done
present() { q "select count(*) from information_schema.columns where table_schema='trading' and table_name='live_results' and column_name in ('${COLS// /','}')"; }

$PSQL <<'SQL'
SET TimeZone = 'UTC';
DROP SCHEMA IF EXISTS trading CASCADE;
CREATE SCHEMA trading;
CREATE TABLE trading.live_results (id SERIAL PRIMARY KEY, strategy_id VARCHAR(100) NOT NULL, portfolio_id VARCHAR(100),
    date DATE NOT NULL, daily_pnl NUMERIC, worst_day DOUBLE PRECISION, portfolio_var NUMERIC(8,4), net_leverage NUMERIC(8,4),
    risk_detail JSONB, settled_at TIMESTAMPTZ, config JSONB,
    CONSTRAINT live_results_portfolio_strategy_date_key UNIQUE (portfolio_id, strategy_id, date));
CREATE INDEX idx_live_results_date ON trading.live_results (date);
INSERT INTO trading.live_results (strategy_id, portfolio_id, date, daily_pnl, worst_day, portfolio_var, net_leverage, risk_detail, settled_at, config) VALUES
 ('S','P','2026-04-23',1245,-2.579978,0.0712,0.8121,NULL,'1970-01-01 00:00:00+00','{"a": 1}'),
 ('S','P','2026-04-24',-201.4755,-2.579978,0.0698,0.7934,'{"sizing_capital": 495186.141684}',NULL,NULL),
 ('E','EQ','2026-06-15',454.691463,-0.186556,0.0101,0.1200,NULL,NULL,NULL);
SQL
fp() { q "select count(*)||' '||md5(coalesce(string_agg(x, ';' order by x), '')) from (select (to_jsonb(t) $MINUS)::text x from trading.live_results t) s"; }
B1=$(fp)
I1=$(q "select string_agg(indexdef, ' | ' order by indexdef) from pg_indexes where schemaname = 'trading'")
C1=$(q "select count(*) from pg_constraint c join pg_namespace n on n.oid = c.connamespace where n.nspname = 'trading'")
# 1
[[ "$(present)" == "0" ]] || fail "a 030 column exists before 030"
refuses "INSERT INTO trading.live_results (strategy_id, portfolio_id, date, overlay_risk) VALUES ('S','P',current_date,0.1)" || fail "overlay_risk exists before 030"
refuses "INSERT INTO trading.live_results (strategy_id, portfolio_id, date, total_strategy_fills) VALUES ('S','P',current_date,1)" || fail "total_strategy_fills exists before 030"
pass "1. the twenty columns are absent before the migration"
# 2
apply 030_live_results_statistics.sql
GOT=$(q "select string_agg(column_name||':'||data_type||':'||is_nullable||':'||coalesce(column_default,'-'), ' ' order by ordinal_position) from information_schema.columns where table_schema='trading' and table_name='live_results' and column_name in ('${COLS// /','}')")
WANT="worst_day_date:date:YES:- worst_day_symbol:text:YES:- max_drawdown_sizing:numeric:YES:- volatility_sizing:numeric:YES:- worst_day_sizing:numeric:YES:- worst_day_sizing_date:date:YES:- worst_day_sizing_symbol:text:YES:- monthly_skew:numeric:YES:- monthly_tail_ratio:numeric:YES:- calendar_year_returns:jsonb:YES:- losing_years:integer:YES:- total_trades:integer:YES:- total_strategy_fills:integer:YES:- total_roll_fills:integer:YES:- overlay_risk:numeric:YES:- overlay_risk_jump:numeric:YES:- overlay_risk_shock:numeric:YES:- overlay_gross_leverage:numeric:YES:- overlay_net_leverage:numeric:YES:- var_95_1d:numeric:YES:-"
[[ "$GOT" == "$WANT" ]] || fail "the columns are not the twenty with their types: $GOT"
[[ "$(q "select count(*) from information_schema.columns where table_schema='trading' and table_name='live_results' and column_name in ('${COLS// /','}') and numeric_precision is not null and data_type='numeric'")" == "0" ]] || fail "a numeric column is constrained"
pass "2. twenty columns: 2 date, 2 text, 1 jsonb, 4 integer, 11 unconstrained numeric; NULL-able, no default"
# 3
[[ "$(q "select count(*) from trading.live_results where true $ALLNULL")" == "3" ]] || fail "an existing row holds a value in a 030 column"
[[ "$(fp)" == "$B1" ]] || fail "rows moved"
[[ "$(q "select string_agg(indexdef, ' | ' order by indexdef) from pg_indexes where schemaname = 'trading'")" == "$I1" ]] || fail "indexes changed"
[[ "$(q "select count(*) from pg_constraint c join pg_namespace n on n.oid = c.connamespace where n.nspname = 'trading'")" == "$C1" ]] || fail "constraints changed"
$PSQL -c "INSERT INTO trading.live_results (strategy_id, portfolio_id, date, daily_pnl) VALUES ('E','EQ','2026-06-16',53.357887)"
[[ "$(q "select count(*) from trading.live_results where date='2026-06-16' $ALLNULL")" == "1" ]] || fail "an omitting INSERT is not NULL in all twenty"
pass "3. no backfill: the 3 existing rows hold NULL in all twenty and are byte-identical elsewhere; indexes and constraints unchanged; an omitting INSERT reads NULL"
# 4
$PSQL -c "INSERT INTO trading.live_results (strategy_id, portfolio_id, date, worst_day_date, worst_day_symbol, max_drawdown_sizing, volatility_sizing, worst_day_sizing, worst_day_sizing_date, worst_day_sizing_symbol, calendar_year_returns, losing_years, total_trades, total_strategy_fills, total_roll_fills, overlay_risk, overlay_risk_jump, overlay_risk_shock, overlay_gross_leverage, overlay_net_leverage, var_95_1d) VALUES ('S','P','2026-04-25', '2025-10-10'::date, 'MES.v.0'::text, '1.037548906'::numeric, '13.24771085'::numeric, '-1.027401213'::numeric, '2026-04-30'::date, 'ZN.v.0'::text, '{\"2025\": {\"return\": -4.673357796, \"partial\": true}, \"2026\": {\"return\": 6.0144, \"partial\": true}}'::jsonb, '0'::integer, '71'::integer, '159'::integer, '10'::integer, '0.1234567891'::numeric, '0.2469135782'::numeric, '0.3703703673'::numeric, '2.345678901'::numeric, '-0.4567890123'::numeric, '6338.123456'::numeric)"
GOT=$(q "select worst_day_date||'|'||worst_day_symbol||'|'||max_drawdown_sizing||'|'||worst_day_sizing||'|'||worst_day_sizing_date||'|'||(calendar_year_returns->'2025'->>'return')||'|'||(calendar_year_returns->'2026'->>'partial')||'|'||losing_years||'|'||total_trades||'|'||total_strategy_fills||'|'||total_roll_fills||'|'||overlay_risk||'|'||overlay_net_leverage||'|'||var_95_1d||'|'||coalesce(monthly_skew::text,'NULL')||'|'||coalesce(monthly_tail_ratio::text,'NULL') from trading.live_results where date='2026-04-25'")
[[ "$GOT" == "2025-10-10|MES.v.0|1.037548906|-1.027401213|2026-04-30|-4.673357796|true|0|71|159|10|0.1234567891|-0.4567890123|6338.123456|NULL|NULL" ]] || fail "the typed values did not round trip: $GOT"
refuses "INSERT INTO trading.live_results (strategy_id, portfolio_id, date, losing_years) VALUES ('S','P','2026-04-27','1.5x')" || fail "losing_years took a non-integer"
pass "4. a date, a symbol, the years object, the integers, ten significant digits and a signed net leverage are stored as sent; the two monthly cells left out read NULL"
# 5
apply 030_live_results_statistics.sql
[[ "$(q "select overlay_risk||'|'||total_strategy_fills from trading.live_results where date='2026-04-25'")" == "0.1234567891|159" ]] || fail "a second apply changed a value"
$PSQL -c "ALTER TABLE trading.live_results ALTER COLUMN losing_years TYPE text USING losing_years::text"
if psql -X -q -v ON_ERROR_STOP=1 -f "$HERE/030_live_results_statistics.sql" >/dev/null 2>&1; then fail "030 type guard did not refuse"; fi
$PSQL -c "ALTER TABLE trading.live_results ALTER COLUMN losing_years TYPE integer USING losing_years::integer"
pass "5. a second apply keeps every value; a column of another type makes 030 refuse"
# 6
cmt() { q "select col_description('trading.live_results'::regclass,(select ordinal_position from information_schema.columns where table_schema='trading' and table_name='live_results' and column_name='$1'))"; }
for c in $COLS; do
  [[ "$(cmt $c)" == *"Rows written before migration 030 hold NULL"* ]] || fail "the comment on $c does not say rows written before migration 030 hold NULL"
done
[[ "$(cmt total_strategy_fills)" == *"The ACCOUNT's STRATEGY fills AFTER NETTING BETWEEN SLEEVES, not the sleeve rows of trading.executions"*"a full cross is 0 fills"*"a partial offset is 1"*"two sleeves on the same side are 1"*"a single sleeve is 1"*"a reversal through zero is one fill"*"the close, then the open"*"one order the account sent"*"less one for each such reversal"* ]] || fail "the total_strategy_fills comment"
[[ "$(cmt total_trades)" == *"ROUND TRIPS"*"ROLL and BORROW rows are never a trade"* ]] || fail "the total_trades comment"
[[ "$(cmt worst_day_symbol)" == *"trading.positions.daily_realized_pnl"*"summed over the book's sleeves"* ]] || fail "the worst_day_symbol comment"
[[ "$(cmt max_drawdown_sizing)" == *"Percentage points of the sizing capital, positive"*"running SUM of r^s"*"NULL on equity rows"*"risk_detail.sizing_capital"*"NULL with fewer than 2 such returns"* ]] || fail "the max_drawdown_sizing comment"
[[ "$(cmt monthly_skew)" == *"G1 = n / ((n - 1)(n - 2))"*"NULL under 12 complete months"* ]] || fail "the monthly_skew comment"
[[ "$(cmt monthly_tail_ratio)" == *"95th percentile"*"5th percentile"*"linear interpolation"*"NULL under 12 complete months"* ]] || fail "the monthly_tail_ratio comment"
[[ "$(cmt losing_years)" == *"FULL calendar years"*"never counted"* ]] || fail "the losing_years comment"
[[ "$(cmt overlay_risk)" == *"THE FIGURE TO COMPARE WITH THE RISK TARGET tau"*"R_max = 2.25 x tau"*"0.45 on CONSERVATIVE"*"understates the book on such a day"*"blind window"*"NOT the reporter's portfolio_var"*"HD's decision of 2026-10-10"* ]] || fail "the overlay_risk comment"
[[ "$(cmt overlay_risk_jump)" == *"R_jump_max = 4.5 x tau"*"0.90 on CONSERVATIVE"*"understates the book on such a day"*"NOT the reporter's jump_risk"* ]] || fail "the overlay_risk_jump comment"
[[ "$(cmt overlay_risk_shock)" == *"R_shock_max = 4.0 x tau"*"0.80 on CONSERVATIVE"*"not max_correlation"* ]] || fail "the overlay_risk_shock comment"
[[ "$(cmt overlay_gross_leverage)" == *"max_gross_leverage 8.0"*"Stored on a blind window too"*"NOT the reporter's gross_leverage"* ]] || fail "the overlay_gross_leverage comment"
[[ "$(cmt overlay_net_leverage)" == *"SIGNED"*"max_net_leverage 6.0"*"applied to its ABSOLUTE value"*"NOT net_leverage"* ]] || fail "the overlay_net_leverage comment"
[[ "$(cmt var_95_1d)" == *"Dollars, positive"*"1.645 x overlay_risk / sqrt(the gate window's bars a year) x E_t"*"understates the book on such a day"*"NOT portfolio_var"* ]] || fail "the var_95_1d comment"
pass "6. all twenty comments say rows written before migration 030 hold NULL; the fill counts, the sizing-capital units, the skew formula, the limits, the coverage, the signed net and the namesakes are stated"
# 7
if psql -X -q -v ON_ERROR_STOP=1 -f "$HERE/030_live_results_statistics_rollback.sql" >/dev/null 2>&1; then fail "the rollback did not refuse while a cell carries a value"; fi
[[ "$(present)" == "20" ]] || fail "a refused rollback dropped a column"
psql -X -q -v ON_ERROR_STOP=1 -c "SET migration.force_rollback = 'yes'" -f "$HERE/030_live_results_statistics_rollback.sql"
[[ "$(present)" == "0" ]] || fail "the forced rollback left a column"
$PSQL -c "DELETE FROM trading.live_results WHERE date IN ('2026-06-16','2026-04-25')"
[[ "$(fp)" == "$B1" ]] || fail "rows differ after the rollback"
[[ "$(q "select string_agg(indexdef, ' | ' order by indexdef) from pg_indexes where schemaname = 'trading'")" == "$I1" ]] || fail "indexes changed in the rollback"
apply 030_live_results_statistics.sql
apply 030_live_results_statistics_rollback.sql
[[ "$(present)" == "0" ]] || fail "the rollback of a column set with no value needed the setting"
apply 030_live_results_statistics_rollback.sql
pass "7. the rollback refuses while a cell carries a value; forced, it drops the twenty, rows byte-identical; with no value it drops without the setting; idempotent"
echo "ALL OK"
