#!/usr/bin/env bash
# Verifies migration 031 (the thirteen statistic columns of backtest.results, the six overlay
# columns of backtest.equity_curve, their comments and the fourteen comments it restates) and its
# rollback on a real PostgreSQL.
#   1. absent before (an INSERT naming one of the columns fails: the migration is necessary);
#   2. after: the nineteen columns with their types, NULL-able, no default;
#   3. NO BACKFILL: every pre-existing row holds NULL in all nineteen and is byte-identical in
#      every other column; indexes and constraints as before; an omitting INSERT reads NULL;
#   4. the types hold what the writers send: a date, a symbol, a jsonb object of years, two
#      integers, numbers of ten significant digits as text cast through numeric, a SIGNED net
#      leverage;
#   5. idempotent (a second apply keeps the values) and type-guarded (a column of another type
#      makes 031 refuse); refuses without the tables it comments;
#   6. the comments: every one of the nineteen says rows written before migration 031 hold NULL;
#      the units are fractions; the sizing-capital, month, year and fill-count rules, the limits,
#      the coverage and the signed net are stated; the fourteen restated texts say what the binary
#      does (the grid, K, the sample deviation, the CAGR, the VaR observation, ROUND TRIPS) and
#      total_dividend_income is migration 028's text plus the one sentence, nothing else changed;
#   7. the rollback refuses while a cell carries a value and, with migration.force_rollback =
#      'yes', drops the nineteen with the original rows byte-identical; EVERY comment of the three
#      tables (pg_description) and the column list are then exactly as before 031; with no value
#      it drops without the setting; idempotent.
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
RCOLS="worst_day worst_day_date worst_day_symbol max_drawdown_sizing volatility_sizing worst_day_sizing worst_day_sizing_date worst_day_sizing_symbol monthly_skew monthly_tail_ratio calendar_year_returns losing_years total_strategy_fills"
CCOLS="overlay_risk overlay_risk_jump overlay_risk_shock overlay_gross_leverage overlay_net_leverage var_95_1d"
RESTATED="total_return volatility sharpe_ratio downside_volatility sortino_ratio max_drawdown calmar_ratio var_95 cvar_95 beta correlation total_trades"
RMINUS=""; RNULL=""; for c in $RCOLS; do RMINUS="$RMINUS - '$c'"; RNULL="$RNULL AND $c IS NULL"; done
CMINUS=""; CNULL=""; for c in $CCOLS; do CMINUS="$CMINUS - '$c'"; CNULL="$CNULL AND $c IS NULL"; done
present() { q "select (select count(*) from information_schema.columns where table_schema='backtest' and table_name='results' and column_name in ('${RCOLS// /','}')) + (select count(*) from information_schema.columns where table_schema='backtest' and table_name='equity_curve' and column_name in ('${CCOLS// /','}'))"; }

$PSQL <<'SQL'
SET TimeZone = 'UTC';
CREATE SCHEMA trading;
CREATE SCHEMA backtest;
CREATE TABLE trading.live_results (id SERIAL PRIMARY KEY, strategy_id VARCHAR(100) NOT NULL, portfolio_id VARCHAR(100),
    date DATE NOT NULL, total_dividend_income NUMERIC DEFAULT 0, overlay_risk NUMERIC);
CREATE TABLE backtest.results (run_id TEXT PRIMARY KEY, start_date TIMESTAMPTZ, end_date TIMESTAMPTZ,
    total_return DOUBLE PRECISION, sharpe_ratio DOUBLE PRECISION, sortino_ratio DOUBLE PRECISION,
    max_drawdown DOUBLE PRECISION, calmar_ratio DOUBLE PRECISION, volatility DOUBLE PRECISION, total_trades INTEGER,
    var_95 DOUBLE PRECISION, cvar_95 DOUBLE PRECISION, beta DOUBLE PRECISION, correlation DOUBLE PRECISION,
    downside_volatility DOUBLE PRECISION, portfolio_id VARCHAR(50), transaction_costs DOUBLE PRECISION DEFAULT 0,
    roll_costs DOUBLE PRECISION DEFAULT 0, total_roll_fills INTEGER DEFAULT 0);
CREATE TABLE backtest.equity_curve (run_id TEXT NOT NULL REFERENCES backtest.results(run_id) ON DELETE CASCADE,
    "timestamp" TIMESTAMPTZ NOT NULL, equity DOUBLE PRECISION, portfolio_id VARCHAR(50), risk_detail JSONB);
CREATE INDEX idx_bt_equity_curve_run ON backtest.equity_curve (run_id, "timestamp");
INSERT INTO trading.live_results (strategy_id, portfolio_id, date, total_dividend_income) VALUES ('E','EQ','2026-06-15',3.752281);
INSERT INTO backtest.results (run_id, start_date, end_date, total_return, sharpe_ratio, volatility, total_trades, portfolio_id, total_roll_fills)
    VALUES ('OLD_RUN', '2023-10-07', '2026-10-07', 0.616818, 1.20523, 0.200073, 312, 'CONSERVATIVE_PORTFOLIO', 420);
INSERT INTO backtest.equity_curve (run_id, "timestamp", equity, portfolio_id, risk_detail) VALUES
    ('OLD_RUN', '2024-08-04', 500000, 'CONSERVATIVE_PORTFOLIO', NULL),
    ('OLD_RUN', '2024-08-05', 498120.5, 'CONSERVATIVE_PORTFOLIO', '{"sizing_capital": 498120.5, "risk_requested": 1.0}');
SQL
# the texts the lane leaves on the fourteen objects before 031: migration 028's own statements
for obj in "TABLE backtest.results" "COLUMN trading.live_results.\"total_dividend_income\""; do
  grep -F "COMMENT ON $obj IS " "$HERE/034_column_comments.sql" | $PSQL
done
for c in $RESTATED; do grep -F "COMMENT ON COLUMN backtest.results.\"$c\" IS " "$HERE/034_column_comments.sql" | $PSQL; done
$PSQL -c "COMMENT ON COLUMN backtest.equity_curve.risk_detail IS 'the 020 text'; COMMENT ON COLUMN backtest.results.total_roll_fills IS 'the 018 text'"
fp_r() { q "select count(*)||' '||md5(coalesce(string_agg(x, ';' order by x), '')) from (select (to_jsonb(t) $RMINUS)::text x from backtest.results t) s"; }
fp_c() { q "select count(*)||' '||md5(coalesce(string_agg(x, ';' order by x), '')) from (select (to_jsonb(t) $CMINUS)::text x from backtest.equity_curve t) s"; }
fp_l() { q "select count(*)||' '||md5(coalesce(string_agg(x, ';' order by x), '')) from (select to_jsonb(t)::text x from trading.live_results t) s"; }
# every comment of the three tables, and their column lists with type, null-ness and default
comments() { q "select md5(coalesce(string_agg(c.relname||'|'||d.objsubid||'|'||coalesce(a.attname,'')||'|'||d.description, E'\n' order by c.relname, d.objsubid), '')) || ' ' || count(*) from pg_description d join pg_class c on c.oid = d.objoid and d.classoid = 'pg_class'::regclass join pg_namespace n on n.oid = c.relnamespace left join pg_attribute a on a.attrelid = c.oid and a.attnum = d.objsubid where (n.nspname, c.relname) in (('backtest','results'),('backtest','equity_curve'),('trading','live_results'))"; }
schema() { q "select md5(string_agg(table_schema||'.'||table_name||'.'||column_name||':'||data_type||':'||is_nullable||':'||coalesce(column_default,'-'), ' ' order by table_schema, table_name, ordinal_position)) from information_schema.columns where (table_schema, table_name) in (('backtest','results'),('backtest','equity_curve'),('trading','live_results'))"; }
cmt() { q "select col_description('$1.$2'::regclass,(select ordinal_position from information_schema.columns where table_schema='$1' and table_name='$2' and column_name='$3'))"; }
B1=$(fp_r); B2=$(fp_c); B3=$(fp_l); D1=$(comments); S1=$(schema)
I1=$(q "select string_agg(indexdef, ' | ' order by indexdef) from pg_indexes where schemaname in ('trading','backtest')")
C1=$(q "select count(*) from pg_constraint c join pg_namespace n on n.oid = c.connamespace where n.nspname in ('trading','backtest')")
DIV028=$(cmt trading live_results total_dividend_income)
[[ "${D1##* }" == "16" ]] || fail "the fixture does not carry the sixteen comments of the lane before 031: $D1"
# 1
[[ "$(present)" == "0" ]] || fail "a 031 column exists before 031"
refuses "INSERT INTO backtest.results (run_id, total_strategy_fills) VALUES ('X', 1)" || fail "total_strategy_fills exists before 031"
refuses "INSERT INTO backtest.equity_curve (run_id, \"timestamp\", overlay_risk) VALUES ('OLD_RUN', now(), 0.1)" || fail "overlay_risk exists before 031"
pass "1. the nineteen columns are absent before the migration"
# 2
apply 037_backtest_statistics.sql
GOT=$(q "select string_agg(column_name||':'||data_type||':'||is_nullable||':'||coalesce(column_default,'-'), ' ' order by ordinal_position) from information_schema.columns where table_schema='backtest' and table_name='results' and column_name in ('${RCOLS// /','}')")
WANT="worst_day:double precision:YES:- worst_day_date:date:YES:- worst_day_symbol:text:YES:- max_drawdown_sizing:double precision:YES:- volatility_sizing:double precision:YES:- worst_day_sizing:double precision:YES:- worst_day_sizing_date:date:YES:- worst_day_sizing_symbol:text:YES:- monthly_skew:double precision:YES:- monthly_tail_ratio:double precision:YES:- calendar_year_returns:jsonb:YES:- losing_years:integer:YES:- total_strategy_fills:integer:YES:-"
[[ "$GOT" == "$WANT" ]] || fail "backtest.results does not carry the thirteen with their types: $GOT"
GOT=$(q "select string_agg(column_name||':'||data_type||':'||is_nullable||':'||coalesce(column_default,'-'), ' ' order by ordinal_position) from information_schema.columns where table_schema='backtest' and table_name='equity_curve' and column_name in ('${CCOLS// /','}')")
WANT="overlay_risk:double precision:YES:- overlay_risk_jump:double precision:YES:- overlay_risk_shock:double precision:YES:- overlay_gross_leverage:double precision:YES:- overlay_net_leverage:double precision:YES:- var_95_1d:double precision:YES:-"
[[ "$GOT" == "$WANT" ]] || fail "backtest.equity_curve does not carry the six with their types: $GOT"
[[ "$(q "select string_agg(column_name, ' ' order by ordinal_position) from information_schema.columns where table_schema='backtest' and table_name='equity_curve'")" == "run_id timestamp equity portfolio_id risk_detail $CCOLS" ]] || fail "the six are not beside risk_detail"
pass "2. nineteen columns: backtest.results 7 double precision, 2 date, 2 text, 1 jsonb, 2 integer; backtest.equity_curve 6 double precision after risk_detail; NULL-able, no default"
# 3
[[ "$(q "select count(*) from backtest.results where true $RNULL")" == "1" ]] || fail "an existing results row holds a value in a 031 column"
[[ "$(q "select count(*) from backtest.equity_curve where true $CNULL")" == "2" ]] || fail "an existing curve row holds a value in a 031 column"
[[ "$(fp_r)" == "$B1" && "$(fp_c)" == "$B2" && "$(fp_l)" == "$B3" ]] || fail "rows moved"
[[ "$(q "select string_agg(indexdef, ' | ' order by indexdef) from pg_indexes where schemaname in ('trading','backtest')")" == "$I1" ]] || fail "indexes changed"
[[ "$(q "select count(*) from pg_constraint c join pg_namespace n on n.oid = c.connamespace where n.nspname in ('trading','backtest')")" == "$C1" ]] || fail "constraints changed"
$PSQL -c "INSERT INTO backtest.results (run_id, total_return, portfolio_id) VALUES ('EQ_RUN', -0.011095, 'EQUITY_MR_PORTFOLIO'); INSERT INTO backtest.equity_curve (run_id, \"timestamp\", equity, portfolio_id) VALUES ('EQ_RUN', '2026-08-06', 98890.5, 'EQUITY_MR_PORTFOLIO')"
[[ "$(q "select count(*) from backtest.results where run_id='EQ_RUN' $RNULL")" == "1" ]] || fail "an omitting results INSERT is not NULL in all thirteen"
[[ "$(q "select count(*) from backtest.equity_curve where run_id='EQ_RUN' $CNULL")" == "1" ]] || fail "an omitting curve INSERT is not NULL in all six"
pass "3. no backfill: the existing rows hold NULL in all nineteen and are byte-identical elsewhere (trading.live_results too); indexes and constraints unchanged; an omitting INSERT reads NULL"
# 4
$PSQL -c "INSERT INTO backtest.results (run_id, portfolio_id, worst_day, worst_day_date, worst_day_symbol, max_drawdown_sizing, volatility_sizing, worst_day_sizing, worst_day_sizing_date, worst_day_sizing_symbol, calendar_year_returns, losing_years, total_strategy_fills) VALUES ('NEW_RUN', 'CONSERVATIVE_PORTFOLIO', '-0.03812345678'::numeric, '2025-04-06'::date, 'MNQ.v.0'::text, '0.1037548906'::numeric, '0.2024771085'::numeric, '-0.04127401213'::numeric, '2025-04-06'::date, 'ZN.v.0'::text, '{\"2024\": {\"return\": 0.04673357796, \"partial\": true}, \"2025\": {\"return\": -0.060144, \"partial\": false}}'::jsonb, '1'::integer, '668'::integer)"
$PSQL -c "INSERT INTO backtest.equity_curve (run_id, \"timestamp\", equity, portfolio_id, risk_detail, overlay_risk, overlay_risk_jump, overlay_risk_shock, overlay_gross_leverage, overlay_net_leverage, var_95_1d) VALUES ('NEW_RUN', '2025-04-06', 512345.678901, 'CONSERVATIVE_PORTFOLIO', '{\"sizing_capital\": 500000}'::jsonb, '0.1234567891'::numeric, '0.2469135782'::numeric, '0.3703703673'::numeric, '2.345678901'::numeric, '-0.4567890123'::numeric, '6338.123456'::numeric), ('NEW_RUN', '2025-04-07', 512000, 'CONSERVATIVE_PORTFOLIO', '{\"sizing_capital\": 500000, \"overlay_blind\": true}'::jsonb, NULL, NULL, NULL, '1.5'::numeric, '-1.5'::numeric, NULL)"
GOT=$(q "select worst_day||'|'||worst_day_date||'|'||worst_day_symbol||'|'||max_drawdown_sizing||'|'||worst_day_sizing||'|'||worst_day_sizing_date||'|'||(calendar_year_returns->'2024'->>'return')||'|'||(calendar_year_returns->'2025'->>'partial')||'|'||losing_years||'|'||total_strategy_fills||'|'||coalesce(monthly_skew::text,'NULL')||'|'||coalesce(monthly_tail_ratio::text,'NULL') from backtest.results where run_id='NEW_RUN'")
[[ "$GOT" == "-0.03812345678|2025-04-06|MNQ.v.0|0.1037548906|-0.04127401213|2025-04-06|0.04673357796|false|1|668|NULL|NULL" ]] || fail "the typed results values did not round trip: $GOT"
GOT=$(q "select overlay_risk||'|'||overlay_risk_jump||'|'||overlay_risk_shock||'|'||overlay_gross_leverage||'|'||overlay_net_leverage||'|'||var_95_1d from backtest.equity_curve where run_id='NEW_RUN' and \"timestamp\"='2025-04-06'")
[[ "$GOT" == "0.1234567891|0.2469135782|0.3703703673|2.345678901|-0.4567890123|6338.123456" ]] || fail "the typed curve values did not round trip: $GOT"
[[ "$(q "select coalesce(overlay_risk::text,'NULL')||'|'||overlay_gross_leverage||'|'||overlay_net_leverage||'|'||coalesce(var_95_1d::text,'NULL') from backtest.equity_curve where \"timestamp\"='2025-04-07'")" == "NULL|1.5|-1.5|NULL" ]] || fail "a blind row does not keep its two leverage readings alone"
refuses "INSERT INTO backtest.results (run_id, losing_years) VALUES ('BAD', '1.5x')" || fail "losing_years took a non-integer"
pass "4. a date, a symbol, the years object, the integers, ten significant digits and a signed net leverage are stored as sent; the two monthly cells left out read NULL; a blind row keeps its two leverage readings alone"
# 5
apply 037_backtest_statistics.sql
[[ "$(q "select worst_day||'|'||total_strategy_fills from backtest.results where run_id='NEW_RUN'")" == "-0.03812345678|668" ]] || fail "a second apply changed a results value"
[[ "$(q "select overlay_risk from backtest.equity_curve where \"timestamp\"='2025-04-06'")" == "0.1234567891" ]] || fail "a second apply changed a curve value"
$PSQL -c "ALTER TABLE backtest.results ALTER COLUMN losing_years TYPE text USING losing_years::text"
if psql -X -q -v ON_ERROR_STOP=1 -f "$HERE/037_backtest_statistics.sql" >/dev/null 2>&1; then fail "031 type guard did not refuse on backtest.results"; fi
$PSQL -c "ALTER TABLE backtest.results ALTER COLUMN losing_years TYPE integer USING losing_years::integer"
$PSQL -c "ALTER TABLE backtest.equity_curve ALTER COLUMN var_95_1d TYPE numeric"
if psql -X -q -v ON_ERROR_STOP=1 -f "$HERE/037_backtest_statistics.sql" >/dev/null 2>&1; then fail "031 type guard did not refuse on backtest.equity_curve"; fi
$PSQL -c "ALTER TABLE backtest.equity_curve ALTER COLUMN var_95_1d TYPE double precision"
$PSQL -c "ALTER TABLE trading.live_results RENAME COLUMN total_dividend_income TO tdi"
if psql -X -q -v ON_ERROR_STOP=1 -f "$HERE/037_backtest_statistics.sql" >/dev/null 2>&1; then fail "031 did not refuse without a column it comments"; fi
$PSQL -c "ALTER TABLE trading.live_results RENAME COLUMN tdi TO total_dividend_income"
pass "5. a second apply keeps every value; a column of another type on either table makes 031 refuse; a missing commented column makes it refuse"
# 6
for c in $RCOLS; do
  [[ "$(cmt backtest results $c)" == *"Rows written before migration 031 hold NULL"* ]] || fail "the comment on backtest.results.$c does not say rows written before migration 031 hold NULL"
done
for c in $CCOLS; do
  t="$(cmt backtest equity_curve $c)"
  [[ "$t" == *"Rows written before migration 031 hold NULL"*"The twin of trading.live_results.$c (migration 030)"* ]] || fail "the comment on backtest.equity_curve.$c: before 031, the twin"
  [[ "$t" == *"FUTURES runs only, on the rows that carry risk_detail"*"NULL on every row of an equity run, on the seed row"*"on the warm-up rows and on a row with no risk_detail"*"overlay::readings, no second implementation"*"backtest.final_positions of the date, summed over the sleeves"* ]] || fail "the comment on backtest.equity_curve.$c: the rows and the book"
done
[[ "$(cmt backtest results worst_day)" == "Fraction: the smallest grid return of the account"*"at least 9 stored symbols"*"K = 311.0574"*"K = 252"*"the first row after the warm-up is the base and never a return"*"which is in PERCENT"* ]] || fail "the worst_day comment"
[[ "$(cmt backtest results worst_day_symbol)" == *"NULL when no symbol lost over those rows"*"after the base row's date"*"backtest.final_positions.realized_pnl"*"summed over the run's sleeves"* ]] || fail "the worst_day_symbol comment"
[[ "$(cmt backtest results max_drawdown_sizing)" == "Fraction of the sizing capital, positive"*"running SUM of r^s"*"NULL on an equity run's row"*"risk_detail.sizing_capital of the last backtest.equity_curve row on or before the grid date"*"a row with no capital of its own"*"NULL with fewer than 2 such returns"* ]] || fail "the max_drawdown_sizing comment"
[[ "$(cmt backtest results volatility_sizing)" == "Fraction, annualised, REALISED, on the sizing capital: the sample standard deviation (n - 1) of r^s x sqrt(K)"*"backtest.equity_curve.overlay_risk"* ]] || fail "the volatility_sizing comment"
[[ "$(cmt backtest results monthly_skew)" == *"G1 = n / ((n - 1)(n - 2))"*"after the base row's date"*"NULL under 12 complete months"* ]] || fail "the monthly_skew comment"
[[ "$(cmt backtest results monthly_tail_ratio)" == *"95th percentile"*"5th percentile"*"linear interpolation"*"NULL under 12 complete months"* ]] || fail "the monthly_tail_ratio comment"
[[ "$(cmt backtest results calendar_year_returns)" == *'"return": fraction'*"a partial first or last year is shown and flagged"* ]] || fail "the calendar_year_returns comment"
[[ "$(cmt backtest results losing_years)" == *"FULL calendar years"*"never counted"* ]] || fail "the losing_years comment"
[[ "$(cmt backtest results total_strategy_fills)" == *"The run's count of the ACCOUNT's STRATEGY fills AFTER NETTING BETWEEN SLEEVES, not the sleeve rows of backtest.executions"*"a full cross is 0 fills"*"a partial offset is 1"*"two sleeves on the same side are 1"*"a single sleeve is 1"*"a reversal through zero is one fill"*"less one for each such reversal"*"BacktestMetricsCalculator::account_fills"* ]] || fail "the total_strategy_fills comment"
[[ "$(cmt backtest equity_curve overlay_risk)" == *"THE FIGURE TO COMPARE WITH THE RISK TARGET tau"*"R_max = 2.25 x tau"*"0.45 on CONSERVATIVE"*"understates the book on a day when a held contract is left out of it"*"blind window"*"HD's decision of 2026-10-10"* ]] || fail "the overlay_risk comment"
[[ "$(cmt backtest equity_curve overlay_risk_jump)" == *"R_jump_max = 4.5 x tau"*"0.90 on CONSERVATIVE"*"is not in the pass and is in no reading"* ]] || fail "the overlay_risk_jump comment"
[[ "$(cmt backtest equity_curve overlay_risk_shock)" == *"R_shock_max = 4.0 x tau"*"0.80 on CONSERVATIVE"*"covers more of the book than overlay_risk"* ]] || fail "the overlay_risk_shock comment"
[[ "$(cmt backtest equity_curve overlay_gross_leverage)" == *"max_gross_leverage 8.0"*"Stored on a blind window too"* ]] || fail "the overlay_gross_leverage comment"
[[ "$(cmt backtest equity_curve overlay_net_leverage)" == *"SIGNED"*"max_net_leverage 6.0"*"applied to its ABSOLUTE value"* ]] || fail "the overlay_net_leverage comment"
[[ "$(cmt backtest equity_curve var_95_1d)" == "Dollars, positive"*"1.645 x overlay_risk / sqrt(the gate window's bars a year) x E_t"*"NOT backtest.results.var_95"* ]] || fail "the var_95_1d comment"
# the restated fourteen
[[ "$(cmt backtest results total_trades)" == "ROUND TRIPS (LOOP_SPEC v6.2 sections 7.4 and 10): the count of trades"*"total_strategy_fills"*"total_roll_fills"*"The engine's rule, unchanged by migration 031. A trade is an ACCOUNT fill that reduces, closes or flips a non-zero net position in a contract."*"Rows written before migration 028 keep the earlier definition." ]] || fail "the total_trades comment"
[[ "$(cmt backtest results volatility)" == *"the sample standard deviation (n - 1) of the grid returns x sqrt(K)"*"Rows written before migration 031 keep the earlier definition." ]] || fail "the volatility comment"
[[ "$(cmt backtest results calmar_ratio)" == *"the CAGR (1 + R)^(K / n) - 1"*"over max_drawdown"* ]] || fail "the calmar_ratio comment"
[[ "$(cmt backtest results cvar_95)" == *"floor(0.05 x n) + 1 lowest grid returns, the var_95 observation included"* ]] || fail "the cvar_95 comment"
[[ "$(cmt backtest results total_return)" == *"every row counted"*"not folded onto the statistics grid"* ]] || fail "the total_return comment"
for c in sharpe_ratio downside_volatility sortino_ratio max_drawdown var_95 beta correlation; do
  t="$(cmt backtest results $c)"
  [[ "$t" == *"grid"*"Rows written before migration 031 keep the earlier definition." && "$t" != *"252"* && "$t" != *"not on the live statistics grid"* ]] || fail "the restated comment on $c"
done
T="$(q "select obj_description('backtest.results'::regclass)")"
[[ "$T" == *"FRACTIONS"*"THE STATISTICS GRID"*"at least 9 distinct stored symbols"*"statistics_K in backtest.run_metadata.portfolio_config"*"Rows written before migration 031 keep the earlier definitions: every row after the warm-up, 252, the population standard deviation"*"a fill through zero (a reversal) is ONE account fill with one cost"*"THE THREE COUNTS"* && "$T" != *"that alignment belongs to a later migration"* ]] || fail "the table comment of backtest.results"
DIV031=$(cmt trading live_results total_dividend_income)
ADDED=" With nothing to carry (no Day T-1 row, a NULL cell on it, or the read of it failing too) the run's own cell takes the column default, 0."
[[ "$DIV031" == *"its own row carries the figure of the Day T-1 row and the Day T-1 row is left as it is.$ADDED THIS COLUMN IS THE DIVIDEND INCOME"* ]] || fail "the total_dividend_income comment does not carry the sentence in its place"
[[ "${DIV031/"$ADDED"/}" == "$DIV028" ]] || fail "the total_dividend_income comment differs from 028's by more than the one sentence"
[[ "$(cmt backtest equity_curve risk_detail)" == "the 020 text" && "$(cmt backtest results total_roll_fills)" == "the 018 text" ]] || fail "031 moved a comment it does not own"
[[ "$(comments)" == *" 35" ]] || fail "the three tables do not carry 35 comments after 031 (16 before + 19 new): $(comments)"
pass "6. all nineteen comments say rows written before migration 031 hold NULL; fractions, the sizing-capital, month, year and fill rules, the limits, the coverage and the signed net are stated; the fourteen restated texts say the grid, K, the sample deviation, the CAGR, the VaR observation and ROUND TRIPS; total_dividend_income is 028's text plus the one sentence; risk_detail's and total_roll_fills' comments untouched"
# 7
if psql -X -q -v ON_ERROR_STOP=1 -f "$HERE/037_backtest_statistics_rollback.sql" >/dev/null 2>&1; then fail "the rollback did not refuse while a cell carries a value"; fi
[[ "$(present)" == "19" ]] || fail "a refused rollback dropped a column"
[[ "$(cmt backtest results volatility)" == *"sample standard deviation"* ]] || fail "a refused rollback restored a comment"
psql -X -q -v ON_ERROR_STOP=1 -c "SET migration.force_rollback = 'yes'" -f "$HERE/037_backtest_statistics_rollback.sql"
[[ "$(present)" == "0" ]] || fail "the forced rollback left a column"
$PSQL -c "DELETE FROM backtest.results WHERE run_id IN ('EQ_RUN','NEW_RUN')"
[[ "$(fp_r)" == "$B1" && "$(fp_c)" == "$B2" && "$(fp_l)" == "$B3" ]] || fail "rows differ after the rollback"
[[ "$(comments)" == "$D1" ]] || fail "the comments of the three tables are not exactly as before 031: $(comments) against $D1"
[[ "$(schema)" == "$S1" ]] || fail "the column lists of the three tables are not exactly as before 031"
[[ "$(cmt trading live_results total_dividend_income)" == "$DIV028" ]] || fail "total_dividend_income's comment is not 028's byte for byte"
[[ "$(q "select string_agg(indexdef, ' | ' order by indexdef) from pg_indexes where schemaname in ('trading','backtest')")" == "$I1" ]] || fail "indexes changed in the rollback"
apply 037_backtest_statistics.sql
apply 037_backtest_statistics_rollback.sql
[[ "$(present)" == "0" ]] || fail "the rollback of a column set with no value needed the setting"
apply 037_backtest_statistics_rollback.sql
[[ "$(comments)" == "$D1" && "$(schema)" == "$S1" ]] || fail "a second rollback changed the comments or the columns"
pass "7. the rollback refuses while a cell carries a value; forced, it drops the nineteen, rows byte-identical, every comment of the three tables (pg_description: $D1) and the column lists exactly as before 031; with no value it drops without the setting; idempotent"
echo "ALL OK"
