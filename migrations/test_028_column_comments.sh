#!/usr/bin/env bash
# Verifies migration 028 (COMMENT ON only: 83 column comments and 4 table comments on trading.* and
# backtest.*) and its rollback on a real PostgreSQL.
#   1. the fixture carries the comments the objects had before 028 (the earlier migrations' texts and
#      the texts the tables were created with; risk_scale's and risk_detail's are set by the real
#      020 file); 73 of the 87 objects have none;
#   2. after 028: every one of the 87 objects has a comment and it differs from the one before;
#      the comments 028 does not own (risk_detail's among them) are byte-identical;
#   3. risk_scale: 020's text, restated in full, plus the one sentence on equity rows; the texts the
#      rulings fix (the ratio, portfolio_var, the grid and K, the first-date sentence on every
#      redefined statistic, beta / correlation, signal_value, the equity_curve table) are present;
#      no comment names a migration other than its own or a later one of the lane;
#   4. NOTHING ELSE MOVES: rows byte-identical; columns, types, defaults, indexes and constraints as
#      before;
#   5. idempotent; refuses, changing nothing, when a commented column or table is missing;
#   6. the rollback restores every comment byte for byte (the whole comment set of both schemas
#      equals the set before 028; risk_scale's equals the text 020 set); idempotent; 028 applies
#      again after it.
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
# the comment of schema.table (2 arguments) or schema.table.column (3)
cmt() { if [[ $# -eq 2 ]]; then q "select coalesce(obj_description('$1.$2'::regclass, 'pg_class'), '')";
        else q "select coalesce(col_description('$1.$2'::regclass, (select attnum from pg_attribute where attrelid = '$1.$2'::regclass and attname = '$3')), '')"; fi; }
# every table and column of both schemas with its comment (NULL kept apart from the empty string)
SNAP_SQL="select n.nspname, c.relname, a.attnum, a.attname, d.description is null as none, coalesce(d.description, '') as description
  from pg_class c join pg_namespace n on n.oid = c.relnamespace
  join (select attrelid, attnum, attname from pg_attribute where attnum > 0 and not attisdropped
        union all select oid, 0, '' from pg_class) a on a.attrelid = c.oid
  left join pg_description d on d.objoid = c.oid and d.objsubid = a.attnum and d.classoid = 'pg_class'::regclass
  where n.nspname in ('trading','backtest') and c.relkind = 'r'"
snap() { q "select count(*) || ' ' || count(*) filter (where not none) || ' ' || md5(string_agg(nspname || '|' || relname || '|' || attname || '|' || none || '|' || description, E'\n' order by nspname, relname, attnum)) from ($SNAP_SQL) s"; }
TARGETS="trading.live_results trading.live_results.date trading.live_results.created_at trading.live_results.total_days trading.live_results.total_annualized_return trading.live_results.volatility trading.live_results.sharpe_ratio trading.live_results.downside_deviation trading.live_results.sortino_ratio trading.live_results.max_drawdown trading.live_results.win_rate trading.live_results.winning_days trading.live_results.losing_days trading.live_results.avg_win trading.live_results.avg_loss trading.live_results.best_day trading.live_results.worst_day trading.live_results.gross_profit trading.live_results.gross_loss trading.live_results.profit_factor trading.live_results.total_cumulative_return trading.live_results.current_portfolio_value trading.live_results.total_pnl trading.live_results.daily_pnl trading.live_results.total_realized_pnl trading.live_results.daily_realized_pnl trading.live_results.total_unrealized_pnl trading.live_results.daily_unrealized_pnl trading.live_results.daily_transaction_costs trading.live_results.total_transaction_costs trading.live_results.active_positions trading.live_results.margin_posted trading.live_results.cash_available trading.live_results.equity_to_margin_ratio trading.live_results.margin_cushion trading.live_results.portfolio_leverage trading.live_results.net_leverage trading.live_results.gross_leverage trading.live_results.total_dividend_income trading.live_results.portfolio_var trading.live_results.max_correlation trading.live_results.jump_risk trading.live_results.risk_scale trading.positions.average_price trading.positions.daily_unrealized_pnl trading.positions.last_update trading.positions.updated_at trading.executions.execution_time trading.executions.implicit_price_impact trading.executions.total_transaction_costs trading.signals.signal_value trading.signals.timestamp trading.signals.created_at trading.equity_curve trading.equity_curve.timestamp trading.live_run_metadata.created_at trading.live_run_metadata.strategy_configs trading.corp_action_applied.qty_held backtest.results backtest.results.start_date backtest.results.end_date backtest.results.total_return backtest.results.volatility backtest.results.sharpe_ratio backtest.results.downside_volatility backtest.results.sortino_ratio backtest.results.max_drawdown backtest.results.calmar_ratio backtest.results.var_95 backtest.results.cvar_95 backtest.results.beta backtest.results.correlation backtest.results.total_trades backtest.results.win_rate backtest.results.profit_factor backtest.results.avg_win backtest.results.avg_loss backtest.results.max_win backtest.results.max_loss backtest.results.avg_holding_period backtest.run_metadata.start_date backtest.run_metadata.end_date backtest.final_positions backtest.final_positions.average_price backtest.final_positions.last_update backtest.final_positions.updated_at backtest.executions.implicit_price_impact"
REDEFINED="trading.live_results.total_days trading.live_results.total_annualized_return trading.live_results.volatility trading.live_results.sharpe_ratio trading.live_results.downside_deviation trading.live_results.sortino_ratio trading.live_results.max_drawdown trading.live_results.win_rate trading.live_results.winning_days trading.live_results.losing_days trading.live_results.avg_win trading.live_results.avg_loss trading.live_results.best_day trading.live_results.worst_day trading.live_results.gross_profit trading.live_results.gross_loss trading.live_results.profit_factor trading.live_results.margin_posted trading.live_results.cash_available trading.live_results.equity_to_margin_ratio trading.live_results.margin_cushion trading.live_results.portfolio_leverage trading.live_results.net_leverage trading.live_results.gross_leverage backtest.results.downside_volatility backtest.results.sortino_ratio backtest.results.total_trades backtest.results.win_rate backtest.results.profit_factor backtest.results.avg_win backtest.results.avg_loss backtest.results.max_win backtest.results.max_loss backtest.results.avg_holding_period"

$PSQL <<'SQL'
DROP SCHEMA IF EXISTS trading CASCADE; DROP SCHEMA IF EXISTS backtest CASCADE;
CREATE SCHEMA trading; CREATE SCHEMA backtest;
CREATE TABLE trading.live_results ("id" serial PRIMARY KEY, "strategy_id" varchar(100) NOT NULL, "portfolio_id" varchar(100) NOT NULL, "gross_notional" numeric, "net_notional" numeric, "config" jsonb, "date" date, "created_at" timestamp without time zone DEFAULT now(), "total_days" integer, "total_annualized_return" numeric(15,4), "volatility" numeric(8,4), "sharpe_ratio" double precision, "downside_deviation" double precision, "sortino_ratio" double precision, "max_drawdown" double precision, "win_rate" double precision, "winning_days" integer, "losing_days" integer, "avg_win" double precision, "avg_loss" double precision, "best_day" double precision, "worst_day" double precision, "gross_profit" double precision, "gross_loss" double precision, "profit_factor" double precision, "total_cumulative_return" double precision, "current_portfolio_value" numeric(15,4), "total_pnl" numeric(15,4), "daily_pnl" numeric(20,8), "total_realized_pnl" numeric(15,4), "daily_realized_pnl" numeric, "total_unrealized_pnl" numeric(15,4), "daily_unrealized_pnl" numeric, "daily_transaction_costs" numeric, "total_transaction_costs" numeric(20,8), "active_positions" integer, "margin_posted" double precision, "cash_available" double precision, "equity_to_margin_ratio" double precision, "margin_cushion" double precision, "portfolio_leverage" numeric(8,4), "net_leverage" numeric(8,4), "gross_leverage" numeric(8,4), "total_dividend_income" numeric(18,8), "portfolio_var" numeric(8,4), "max_correlation" numeric(8,4), "jump_risk" numeric(8,4), "risk_scale" numeric(8,4));
CREATE TABLE trading.positions ("symbol" varchar(20), "quantity" numeric(20,6), "average_price" numeric(20,6), "daily_unrealized_pnl" numeric(20,6), "last_update" timestamp with time zone, "updated_at" timestamp with time zone);
CREATE TABLE trading.executions ("exec_id" varchar(50), "netting_adjustment" numeric, "execution_time" timestamp with time zone, "implicit_price_impact" numeric, "total_transaction_costs" numeric);
CREATE TABLE trading.signals ("id" serial PRIMARY KEY, "symbol" varchar(20), "signal_value" numeric, "timestamp" timestamp with time zone, "created_at" timestamp with time zone DEFAULT now());
CREATE TABLE trading.equity_curve ("id" serial PRIMARY KEY, "equity" double precision, "timestamp" timestamp with time zone);
CREATE TABLE trading.live_run_metadata ("id" serial PRIMARY KEY, "date" date, "created_at" timestamp without time zone DEFAULT now(), "strategy_configs" jsonb);
CREATE TABLE trading.corp_action_applied ("symbol" text, "total_cash" double precision, "qty_held" double precision);
CREATE TABLE backtest.results ("run_id" text PRIMARY KEY, "transaction_costs" double precision, "start_date" timestamp with time zone, "end_date" timestamp with time zone, "total_return" double precision, "volatility" double precision, "sharpe_ratio" double precision, "downside_volatility" double precision, "sortino_ratio" double precision, "max_drawdown" double precision, "calmar_ratio" double precision, "var_95" double precision, "cvar_95" double precision, "beta" double precision, "correlation" double precision, "total_trades" integer, "win_rate" double precision, "profit_factor" double precision, "avg_win" double precision, "avg_loss" double precision, "max_win" double precision, "max_loss" double precision, "avg_holding_period" double precision);
CREATE TABLE backtest.run_metadata ("run_id" text PRIMARY KEY, "start_date" date, "end_date" date);
CREATE TABLE backtest.final_positions ("run_id" text, "symbol" text, "average_price" double precision, "last_update" timestamp without time zone, "updated_at" timestamp without time zone);
CREATE TABLE backtest.executions ("id" serial PRIMARY KEY, "run_id" text, "implicit_price_impact" double precision);
CREATE TABLE backtest.equity_curve (run_id text NOT NULL, "timestamp" timestamptz NOT NULL, equity double precision NOT NULL, portfolio_id varchar(100));
CREATE UNIQUE INDEX live_results_key ON trading.live_results (portfolio_id, strategy_id, date);
INSERT INTO trading.live_results (strategy_id, portfolio_id, date, risk_scale, portfolio_var, volatility, gross_leverage, margin_cushion, equity_to_margin_ratio, total_days)
VALUES ('S','P','2026-04-24',0.87,0.11,9.5,NULL,0.93,6.2,140), ('E','EQ','2026-06-15',1.0,0.02,3.1,0.1593,NULL,NULL,12);
INSERT INTO backtest.results (run_id, start_date, end_date, total_return, beta, correlation, total_trades, win_rate)
VALUES ('R1','2024-05-03 04:00:00+00','2026-05-01 04:00:00+00',0.21,-0.03,-0.03,300,0.42);
INSERT INTO trading.signals (symbol, signal_value, "timestamp") VALUES ('ES.v.0', 7.25, '2026-04-24 05:00:00+00');
SQL
apply 020_risk_detail.sql
T020=$(cmt trading live_results risk_scale); D020=$(cmt trading live_results risk_detail)
RS_MD5="select md5(col_description('trading.live_results'::regclass, (select attnum from pg_attribute where attrelid = 'trading.live_results'::regclass and attname = 'risk_scale'))) || ' ' || octet_length(col_description('trading.live_results'::regclass, (select attnum from pg_attribute where attrelid = 'trading.live_results'::regclass and attname = 'risk_scale')))"
M020=$(q "$RS_MD5")
[[ "$T020" == "The DELIVERED scale"*"migration 020." ]] || fail "020 did not set the risk_scale comment the fixture relies on"
$PSQL <<'SQL'
COMMENT ON COLUMN trading.live_results."total_annualized_return" IS $c028$Annualized return using formula: ((current_value/initial_value)^(252/days) - 1) * 100$c028$;
COMMENT ON COLUMN trading.live_results."total_cumulative_return" IS $c028$Total cumulative return since inception: ((current_value/initial_value) - 1) * 100$c028$;
COMMENT ON COLUMN trading.live_results."margin_posted" IS $c028$Sum of initial margin per contract × contracts across all open positions (daily posted margin).$c028$;
COMMENT ON COLUMN trading.live_results."cash_available" IS $c028$Current portfolio value minus margin_posted (cash on hand).$c028$;
COMMENT ON COLUMN trading.live_results."equity_to_margin_ratio" IS $c028$Current portfolio value divided by total posted margin, computed at the book level: one portfolio value over the sum of every position's posted margin, so a mixed futures-plus-equity book sums both margin kinds under one portfolio value (HD 2026-09-10). The futures runners compute this; the equity runner writes gross notional over posted margin until T-8 moves it to this definition (migration 013).$c028$;
COMMENT ON COLUMN trading.live_results."margin_cushion" IS $c028$(Current portfolio value – today's maintenance margin requirement) ÷ current portfolio value.$c028$;
COMMENT ON COLUMN trading.live_results."portfolio_leverage" IS $c028$Gross notional divided by current portfolio value (live-time portfolio leverage).$c028$;
COMMENT ON COLUMN trading.live_results."net_leverage" IS $c028$Absolute net notional divided by configured capital (risk manager).$c028$;
COMMENT ON COLUMN trading.live_results."gross_leverage" IS $c028$Sum of absolute notionals divided by configured capital (risk manager).$c028$;
COMMENT ON COLUMN trading.live_results."total_dividend_income" IS $c028$Cumulative dividend cash income for (strategy, portfolio) as of this date. Informational ONLY -- NOT added to total_pnl. closeadj captures dividend total-return via price continuity (Phase 4 avg_price frame-alignment fix); adding dividend cash on top would double-count. Source: in-process sum of CorporateActionsAuditLog dividend_events at daily finalization. Decomposition view: total_return = capital_appreciation + total_dividend_income.$c028$;
COMMENT ON TABLE trading.equity_curve IS $c028$Daily equity curve tracking for live strategies$c028$;
COMMENT ON TABLE backtest.results IS $c028$Main backtest performance metrics and results$c028$;
COMMENT ON TABLE backtest.final_positions IS $c028$Final positions at end of backtest$c028$;
COMMENT ON COLUMN trading.live_results."gross_notional" IS $c028$fixture: a comment 028 does not own$c028$;
COMMENT ON COLUMN backtest.results."transaction_costs" IS $c028$The run's transaction costs: every fill's total_transaction_costs (STRATEGY + ROLL + BORROW), the sum the equity curve charged. Migration 018.$c028$;
SQL
fp() { q "select count(*)||' '||md5(coalesce(string_agg(x, ';' order by x), '')) from (select to_jsonb(t)::text x from $1 t) s"; }
B1=$(fp trading.live_results); B2=$(fp backtest.results); B3=$(fp trading.signals)
COLS_SQL="select md5(string_agg(table_schema||'.'||table_name||'.'||column_name||' '||data_type||' '||is_nullable||' '||coalesce(column_default,'-'), ';' order by table_schema, table_name, ordinal_position)) from information_schema.columns where table_schema in ('trading','backtest')"
IDX_SQL="select coalesce(string_agg(indexdef, ' | ' order by indexdef), '') from pg_indexes where schemaname in ('trading','backtest')"
CON_SQL="select count(*) from pg_constraint c join pg_namespace n on n.oid = c.connamespace where n.nspname in ('trading','backtest')"
C1=$(q "$COLS_SQL"); I1=$(q "$IDX_SQL"); K1=$(q "$CON_SQL")
S0=$(snap)
declare -a BEFORE=()
nulls=0
for o in $TARGETS; do IFS=. read -r s t c <<<"$o"
  if [[ -z "$c" ]]; then b=$(cmt "$s" "$t"); else b=$(cmt "$s" "$t" "$c"); fi
  BEFORE+=("$b"); [[ -z "$b" ]] && nulls=$((nulls + 1))
done
[[ "${#BEFORE[@]}" == "87" ]] || fail "the fixture has ${#BEFORE[@]} objects, expected 87"
[[ "$nulls" == "73" ]] || fail "$nulls objects have no comment before 028, expected 73"
pass "1. the fixture: 87 objects, 73 with no comment before 028, the others with their earlier texts (snapshot $S0)"
# 2
apply 028_column_comments.sql
i=0
for o in $TARGETS; do IFS=. read -r s t c <<<"$o"
  if [[ -z "$c" ]]; then a=$(cmt "$s" "$t"); else a=$(cmt "$s" "$t" "$c"); fi
  [[ -n "$a" ]] || fail "$o has no comment after 028"
  [[ "$a" != "${BEFORE[$i]}" ]] || fail "$o: the comment did not change"
  i=$((i + 1))
done
[[ "$(cmt trading live_results risk_detail)" == "$D020" ]] || fail "028 moved the risk_detail comment (020 owns it)"
[[ "$(cmt backtest equity_curve risk_detail)" == *"migration 020." ]] || fail "028 moved backtest.equity_curve.risk_detail's comment"
[[ "$(cmt trading live_results gross_notional)" == "fixture: a comment 028 does not own" ]] || fail "028 moved a comment it does not own"
[[ "$(cmt backtest results transaction_costs)" == *"Migration 018." ]] || fail "028 moved backtest.results.transaction_costs's comment"
S1=$(snap)
[[ "$S1" != "$S0" ]] || fail "the comment set did not change"
changed=$(q "select count(*) from ($SNAP_SQL) s where not none")
pass "2. after 028 every one of the 87 objects carries a new comment; the comments 028 does not own are byte-identical ($changed objects commented in the two schemas)"
# 3
RS=$(cmt trading live_results risk_scale)
[[ "$RS" == "$T020 On equity rows the column still holds the reporter's recommended scale (the equity runner stores it); this sentence was added by migration 028." ]] || fail "risk_scale is not 020's text plus the one sentence"
[[ "${RS:0:${#T020}}" == "$T020" ]] || fail "risk_scale does not restate 020's text byte for byte"
has() { local o="$1" text; shift; IFS=. read -r s t c <<<"$o"
  if [[ -z "$c" ]]; then text=$(cmt "$s" "$t"); else text=$(cmt "$s" "$t" "$c"); fi
  for w in "$@"; do [[ "$text" == *"$w"* ]] || fail "$o: the comment lacks '$w'"; done; }
has trading.live_results.equity_to_margin_ratio "current_portfolio_value divided by margin_posted" "level of the book" "finalised" "NULL when the book posts no margin"
has trading.live_results.margin_cushion "NULL when the book posts no margin" "0 in a cash account"
has trading.live_results.margin_posted "cost basis"
has trading.live_results.gross_leverage "Futures rows: NULL" "gross_notional"
has trading.live_results.portfolio_var "sqrt(w' S w)" "NO contract multiplier" "NOT a value-at-risk" "NOT the book's risk" "value unchanged" "overlay_* columns added by migration 030"
has trading.live_results.max_correlation "the overlay applies no correlation cap"
has trading.live_results.jump_risk "not the overlay's R_jump"
has trading.live_results "at least 9 distinct symbols" "Sunday-to-Friday" "311.0574" "252" "NYSE sessions" "differs from the initial capital" "through the last settled row" "PERCENT" "Rows written before migration 028"
has trading.live_results.win_rate "winning_days / (winning_days + losing_days)"
has trading.live_results.downside_deviation "over EVERY grid return / n"
has trading.live_results.total_cumulative_return "current_portfolio_value / (1 + total_cumulative_return / 100)"
has trading.positions.average_price "NOT re-anchored when a bar is consumed late" "cost basis" "basis_ratio"
has trading.signals.signal_value "z-score" "LOOP_SPEC section 2.5" "equity slow rule" "LATEST forecast as of the run date"
has trading.equity_curve "a point dated D is settled when" "settled_at" "migration 029"
has trading.live_run_metadata.created_at "FIRST insert"
has backtest.results.beta "lag-1 autocorrelation of daily returns" "NOT a market beta"
has backtest.results.correlation "lag-1 autocorrelation" "NOT a correlation with a market"
has backtest.results.total_trades "ACCOUNT fill" "netted across the sleeves" "DOLLARS" "non-zero net position" "a fill through zero (a reversal) is ONE account fill with one cost" "is charged to the trade it closes"
has backtest.results "FRACTIONS" "not on the live statistics grid" "a fill through zero (a reversal) is ONE account fill with one cost"
has backtest.final_positions "DAILY snapshot"
n=0
for o in $REDEFINED; do has "$o" "Rows written before migration 028 keep the earlier definition."; n=$((n + 1)); done
[[ "$n" == "34" ]] || fail "$n redefined columns checked, expected 34"
bad=$(q "select count(*) from ($SNAP_SQL) s where (nspname, relname, attname) in (select split_part(o, '.', 1), split_part(o, '.', 2), split_part(o, '.', 3) from unnest(string_to_array('$TARGETS', ' ')) o) and (description ~ 'migration 0(1[0-9]|2[1-7])' or description ~ 'before migration 0(?!28)' or description ~ U&'[\2013\2014]') and not (relname = 'live_results' and attname = 'risk_scale')")
[[ "$bad" == "0" ]] || fail "$bad comment(s) of 028 name another first migration or carry a dash"
pass "3. risk_scale is 020's text in full plus the one sentence; the ruled texts are present; 34 redefined columns carry the first-date sentence, and every first migration named is 028"
# 4
[[ "$(fp trading.live_results)" == "$B1" && "$(fp backtest.results)" == "$B2" && "$(fp trading.signals)" == "$B3" ]] || fail "rows moved"
[[ "$(q "$COLS_SQL")" == "$C1" ]] || fail "columns, types or defaults changed"
[[ "$(q "$IDX_SQL")" == "$I1" ]] || fail "indexes changed"
[[ "$(q "$CON_SQL")" == "$K1" ]] || fail "constraints changed"
pass "4. rows byte-identical; columns, types, defaults, indexes and constraints unchanged"
# 5
apply 028_column_comments.sql
[[ "$(snap)" == "$S1" ]] || fail "028 is not idempotent"
$PSQL -c "ALTER TABLE trading.positions RENAME COLUMN average_price TO average_price_x; COMMENT ON COLUMN trading.live_results.volatility IS 'probe'"
if psql -X -q -v ON_ERROR_STOP=1 -f "$HERE/028_column_comments.sql" >/dev/null 2>&1; then fail "028 did not refuse a missing column"; fi
[[ "$(cmt trading live_results volatility)" == "probe" ]] || fail "a refused 028 changed a comment"
$PSQL -c "ALTER TABLE trading.positions RENAME COLUMN average_price_x TO average_price"
$PSQL -c "ALTER TABLE backtest.final_positions RENAME TO final_positions_x"
if psql -X -q -v ON_ERROR_STOP=1 -f "$HERE/028_column_comments.sql" >/dev/null 2>&1; then fail "028 did not refuse a missing table"; fi
[[ "$(cmt trading live_results volatility)" == "probe" ]] || fail "a refused 028 changed a comment"
$PSQL -c "ALTER TABLE backtest.final_positions_x RENAME TO final_positions"
apply 028_column_comments.sql
[[ "$(snap)" == "$S1" ]] || fail "028 does not restore its own texts"
pass "5. idempotent; a missing column and a missing table each make 028 refuse and change nothing"
# 6
apply 028_column_comments_rollback.sql
[[ "$(snap)" == "$S0" ]] || fail "the comment set after the rollback differs from the set before 028: $(snap) vs $S0"
i=0
for o in $TARGETS; do IFS=. read -r s t c <<<"$o"
  if [[ -z "$c" ]]; then a=$(cmt "$s" "$t"); else a=$(cmt "$s" "$t" "$c"); fi
  [[ "$a" == "${BEFORE[$i]}" ]] || fail "$o: the rollback did not restore the comment"
  i=$((i + 1))
done
[[ "$(cmt trading live_results risk_scale)" == "$T020" ]] || fail "the rollback did not restore 020's risk_scale text exactly"
[[ "$(q "$RS_MD5")" == "$M020" ]] || fail "risk_scale after the rollback is not 020's text byte for byte (md5 and length)"
[[ "$(cmt trading live_results risk_detail)" == "$D020" ]] || fail "the rollback moved the risk_detail comment"
[[ "$(fp trading.live_results)" == "$B1" && "$(fp backtest.results)" == "$B2" && "$(fp trading.signals)" == "$B3" ]] || fail "rows moved in the rollback"
[[ "$(q "$COLS_SQL")" == "$C1" ]] || fail "columns changed in the rollback"
apply 028_column_comments_rollback.sql
[[ "$(snap)" == "$S0" ]] || fail "the rollback is not idempotent"
pass "6a. the rollback restores every comment byte for byte (snapshot $S0); risk_scale carries 020's text exactly (md5 and length $M020); rows and columns unchanged; idempotent"
apply 028_column_comments.sql
[[ "$(snap)" == "$S1" ]] || fail "028 after its rollback differs"
pass "6b. 028 applies again after its rollback (snapshot $S1)"
echo "ALL OK"
