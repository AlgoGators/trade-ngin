#!/usr/bin/env bash
# Verifies 011_live_results_and_executions_columns.sql against a real PostgreSQL.
#
# The properties under test:
#   1. Before 011: the columns AlgoLens reads out of trading.live_results do not
#      exist on a table built the way this repository's migrations leave it, and
#      selecting them fails. This is the deployment blocker 011 exists for.
#   2. 011 declares every one of them, and every column the engine's execution
#      writer names.
#   3. It is a no-op the second time: applying it twice changes nothing, and it
#      does not touch a column that already exists with a value in it.
#   4. It refuses, rather than inventing, when the table is not there at all.
#   5. 010 can run afterwards. Before 011 it could not: it UPDATEs profit_factor,
#      which did not exist, and failed the whole migration.
#
# Requires PGUSER/PGDATABASE and CONTAINER_NAME.

set -uo pipefail

CONTAINER_NAME="${CONTAINER_NAME:-test-postgres-011}"
PSQL="docker exec -i ${CONTAINER_NAME} psql -v ON_ERROR_STOP=1 -q -X -U ${PGUSER} -d ${PGDATABASE}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
pass=0
fail=0
ok()  { echo "  PASS  $1"; pass=$((pass + 1)); }
bad() { echo "  FAIL  $1"; fail=$((fail + 1)); }

has_column() {
    local table="$1" column="$2"
    local n
    n=$($PSQL -At -c "SELECT count(*) FROM information_schema.columns
                       WHERE table_schema='trading' AND table_name='$table'
                         AND column_name='$column'" 2>/dev/null)
    [ "$n" = "1" ]
}

apply() { $PSQL < "$HERE/$1" > /dev/null 2>&1; }

# The ten metrics AlgoLens reads instead of recomputing. If any is missing, the
# dashboard is a 500.
ALGOLENS_READS="sharpe_ratio sortino_ratio downside_deviation max_drawdown \
win_rate avg_win avg_loss profit_factor best_day worst_day"

echo "########## BASELINE: live_results as this repository leaves it ##########"
$PSQL <<'SQL'
DROP SCHEMA IF EXISTS trading CASCADE;
CREATE SCHEMA trading;

-- No migration here creates this table, so this is the shape it had before 011:
-- the columns the pre-metrics engine wrote, and nothing else.
CREATE TABLE trading.live_results (
    id BIGSERIAL PRIMARY KEY, config JSONB NOT NULL, portfolio_id TEXT NOT NULL,
    date DATE NOT NULL, current_portfolio_value NUMERIC,
    total_annualized_return NUMERIC, total_cumulative_return NUMERIC,
    volatility NUMERIC, daily_return NUMERIC, gross_leverage NUMERIC,
    net_leverage NUMERIC, margin_posted NUMERIC, equity_to_margin_ratio NUMERIC,
    margin_cushion NUMERIC, gross_notional NUMERIC, total_unrealized_pnl NUMERIC,
    total_realized_pnl NUMERIC, total_transaction_costs NUMERIC,
    cash_available NUMERIC
);

CREATE TABLE trading.executions (
    id BIGSERIAL PRIMARY KEY, strategy_id TEXT, portfolio_id TEXT, symbol TEXT,
    side TEXT, quantity NUMERIC, price NUMERIC
);

INSERT INTO trading.live_results (config, portfolio_id, date, current_portfolio_value, gross_leverage)
VALUES ('{"strategy_type":"LIVE_TF"}'::jsonb, 'BOOK_A', CURRENT_DATE, 500000, 7.5);
SQL
[ $? -eq 0 ] && ok "baseline built" || bad "baseline failed"

missing_before=0
for c in $ALGOLENS_READS; do
    has_column live_results "$c" && missing_before=$((missing_before + 1))
done
if [ "$missing_before" -eq 0 ]; then
    ok "none of the ten metrics AlgoLens reads exists before 011"
else
    bad "$missing_before metric column(s) already present before 011"
fi

echo ""
echo "########## 010 BEFORE 011: must not fail on a missing column ##########"
if apply 010_clear_profit_factor_sentinel.sql; then
    ok "010 succeeds (skips) when profit_factor does not exist"
else
    bad "010 failed on a table with no profit_factor column"
fi

echo ""
echo "########## APPLYING 011 ##########"
if apply 011_live_results_and_executions_columns.sql; then
    ok "011 applied"
else
    bad "011 failed"
fi

for c in $ALGOLENS_READS; do
    has_column live_results "$c" && ok "live_results.$c declared" \
                                 || bad "live_results.$c still missing"
done

for c in strategy_id gross_profit gross_loss winning_days losing_days total_days \
         active_positions total_pnl daily_pnl daily_realized_pnl \
         daily_unrealized_pnl daily_transaction_costs net_notional portfolio_var \
         max_correlation jump_risk risk_scale portfolio_leverage; do
    has_column live_results "$c" && ok "live_results.$c declared" \
                                 || bad "live_results.$c missing"
done

for c in exec_id order_id execution_time commissions_fees implicit_price_impact \
         slippage_market_impact total_transaction_costs is_partial strategy_name date; do
    has_column executions "$c" && ok "executions.$c declared" \
                              || bad "executions.$c missing"
done

echo ""
echo "########## The existing row is untouched ##########"
kept=$($PSQL -At -c "SELECT current_portfolio_value = 500000 AND gross_leverage = 7.5
                       FROM trading.live_results" 2>/dev/null)
[ "$kept" = "t" ] && ok "pre-existing values unchanged" || bad "pre-existing values changed"

echo ""
echo "########## Idempotence: 011 applied twice changes nothing ##########"
$PSQL -c "UPDATE trading.live_results SET sharpe_ratio = 1.25" > /dev/null 2>&1
if apply 011_live_results_and_executions_columns.sql; then
    ok "second 011 succeeded"
else
    bad "second 011 failed"
fi
kept=$($PSQL -At -c "SELECT sharpe_ratio = 1.25 FROM trading.live_results" 2>/dev/null)
[ "$kept" = "t" ] && ok "a populated column survives re-application" \
                  || bad "re-applying 011 disturbed a populated column"

echo ""
echo "########## 010 AFTER 011: clears the sentinel, spares a real ratio ##########"
$PSQL <<'SQL'
UPDATE trading.live_results SET profit_factor = 999.99, gross_loss = 0, gross_profit = 1200;
INSERT INTO trading.live_results (config, portfolio_id, date, profit_factor, gross_loss, gross_profit)
VALUES ('{"strategy_type":"LIVE_REAL"}'::jsonb, 'BOOK_B', CURRENT_DATE - 1, 1.75, 800, 1400);
SQL
if apply 010_clear_profit_factor_sentinel.sql; then
    ok "010 applied after 011"
else
    bad "010 failed after 011"
fi
cleared=$($PSQL -At -c "SELECT count(*) FROM trading.live_results WHERE profit_factor IS NULL" 2>/dev/null)
[ "$cleared" = "1" ] && ok "the sentinel row is now NULL" || bad "sentinel not cleared (got $cleared)"
real=$($PSQL -At -c "SELECT profit_factor FROM trading.live_results WHERE portfolio_id = 'BOOK_B'" 2>/dev/null)
[ "$real" = "1.75" ] && ok "a measured ratio is left alone" || bad "measured ratio changed to $real"

echo ""
echo "########## 011 refuses on a database with no such table ##########"
$PSQL -c "DROP TABLE trading.live_results CASCADE" > /dev/null 2>&1
if apply 011_live_results_and_executions_columns.sql; then
    bad "011 succeeded with no trading.live_results (should refuse)"
else
    ok "011 refuses rather than inventing the table"
fi

echo ""
echo "RESULT: $pass passed, $fail failed"
[ "$fail" -eq 0 ] || exit 1
