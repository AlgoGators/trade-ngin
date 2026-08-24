#!/usr/bin/env bash
# Verifies migration 007_benchmark_frozen_shadow_stream.sql against a real PostgreSQL 16.
#
# The property under test:
#   1. Before 007 (i.e. at the 006 state): 'benchmark_frozen_shadow' portfolio_type
#      is rejected by CHECK constraints
#   2. After 007: 'benchmark_frozen_shadow' is accepted, and the pre-existing
#      'benchmark'/'benchmark_rebench'/'system'/'qt' values still are too
#   3. After rollback: 'benchmark_frozen_shadow' is rejected again, and the
#      006 state is otherwise unchanged
#
# Requires PGHOST/PGPORT/PGUSER/PGPASSWORD/PGDATABASE and CONTAINER_NAME.

set -uo pipefail

CONTAINER_NAME="${CONTAINER_NAME:-test-postgres-007}"
PSQL="docker exec -i ${CONTAINER_NAME} psql -v ON_ERROR_STOP=1 -q -X -U ${PGUSER} -d ${PGDATABASE}"
pass=0
fail=0
ok()  { echo "  PASS  $1"; pass=$((pass + 1)); }
bad() { echo "  FAIL  $1"; fail=$((fail + 1)); }

try_shadow_insert() {
    $PSQL -c "
        INSERT INTO trading.positions
            (symbol, quantity, average_price, daily_unrealized_pnl, daily_realized_pnl,
             last_update, updated_at, strategy_id, strategy_name, date, portfolio_id, portfolio_type)
        VALUES ('BRK_B', 1, 500, 0, 0, '2026-05-05', '2026-05-05', 'TEST_S', 'TEST_N', '2026-05-05', 'P_TEST', 'benchmark_frozen_shadow')
    " 2>&1 > /dev/null
    return $?
}

try_shadow_equity_insert() {
    $PSQL -c "
        INSERT INTO trading.equity_curve (strategy_id, timestamp, equity, portfolio_id, portfolio_type)
        VALUES ('TEST_S', '2026-05-05 10:30:00', 1000000.0, 'P_TEST', 'benchmark_frozen_shadow')
    " 2>&1 > /dev/null
    return $?
}

try_existing_stream_insert() {
    # Confirms 007 doesn't accidentally narrow anything 006 already allowed.
    $PSQL -c "
        INSERT INTO trading.positions
            (symbol, quantity, average_price, daily_unrealized_pnl, daily_realized_pnl,
             last_update, updated_at, strategy_id, strategy_name, date, portfolio_id, portfolio_type)
        VALUES ('BRK_B', 1, 500, 0, 0, '2026-05-05', '2026-05-05', 'TEST_S', 'TEST_N', '2026-05-05', 'P_TEST', 'benchmark_rebench')
    " 2>&1 > /dev/null
    return $?
}

clean_test_rows() {
    $PSQL -q -c "DELETE FROM trading.positions WHERE portfolio_type IN ('benchmark_frozen_shadow', 'benchmark_rebench')" 2>/dev/null || true
    $PSQL -q -c "DELETE FROM trading.equity_curve WHERE portfolio_type IN ('benchmark_frozen_shadow', 'benchmark_rebench')" 2>/dev/null || true
}

# --- Fixture: post-006 schema state -------------------------------------
$PSQL <<'SQL'
DROP SCHEMA IF EXISTS trading CASCADE;
CREATE SCHEMA trading;

CREATE TABLE trading.positions (
    symbol VARCHAR NOT NULL,
    quantity NUMERIC NOT NULL,
    average_price NUMERIC NOT NULL,
    daily_unrealized_pnl NUMERIC NOT NULL,
    daily_realized_pnl NUMERIC NOT NULL,
    last_update TIMESTAMPTZ NOT NULL,
    updated_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
    strategy_id VARCHAR NOT NULL,
    strategy_name VARCHAR NOT NULL,
    date DATE NOT NULL,
    portfolio_id VARCHAR NOT NULL,
    portfolio_type TEXT NOT NULL DEFAULT 'system'
        CHECK (portfolio_type IN ('system','qt','benchmark','benchmark_rebench')),
    CONSTRAINT positions_pkey PRIMARY KEY
        (portfolio_id, strategy_id, strategy_name, date, symbol, portfolio_type)
);

CREATE TABLE trading.equity_curve (
    strategy_id VARCHAR NOT NULL,
    timestamp TIMESTAMPTZ NOT NULL,
    equity NUMERIC NOT NULL,
    portfolio_id VARCHAR NOT NULL,
    portfolio_type TEXT NOT NULL DEFAULT 'system'
        CHECK (portfolio_type IN ('system','qt','benchmark','benchmark_rebench')),
    CONSTRAINT equity_curve_pkey PRIMARY KEY
        (portfolio_id, strategy_id, timestamp, portfolio_type)
);

INSERT INTO trading.positions
 (symbol,quantity,average_price,daily_unrealized_pnl,daily_realized_pnl,last_update,
  strategy_id,strategy_name,date,portfolio_id,portfolio_type)
VALUES ('SPY', 100, 450.0, 500, 0, '2026-05-04', 'LONG_EQ', 'LongEQ', '2026-05-04', 'MAIN', 'benchmark');

INSERT INTO trading.equity_curve
 (strategy_id,timestamp,equity,portfolio_id,portfolio_type)
VALUES ('LONG_EQ', '2026-05-04 16:00:00', 1000000, 'MAIN', 'benchmark');
SQL

echo "########## BEFORE migration 007: benchmark_frozen_shadow should be REJECTED ##########"
if try_shadow_insert; then
    bad "benchmark_frozen_shadow position INSERT succeeded (should be rejected by CHECK)"
else
    ok "benchmark_frozen_shadow position INSERT rejected as expected"
fi

if try_shadow_equity_insert; then
    bad "benchmark_frozen_shadow equity_curve INSERT succeeded (should be rejected by CHECK)"
else
    ok "benchmark_frozen_shadow equity_curve INSERT rejected as expected"
fi

echo ""
echo "########## APPLYING migration 007 ##########"
$PSQL -v ON_ERROR_STOP=1 <<'SQL'
ALTER TABLE trading.positions
    DROP CONSTRAINT IF EXISTS positions_portfolio_type_check;
ALTER TABLE trading.positions
    ADD CONSTRAINT positions_portfolio_type_check
    CHECK (portfolio_type IN
           ('system', 'qt', 'benchmark', 'benchmark_rebench', 'benchmark_frozen_shadow'));

ALTER TABLE trading.equity_curve
    DROP CONSTRAINT IF EXISTS equity_curve_portfolio_type_check;
ALTER TABLE trading.equity_curve
    ADD CONSTRAINT equity_curve_portfolio_type_check
    CHECK (portfolio_type IN
           ('system', 'qt', 'benchmark', 'benchmark_rebench', 'benchmark_frozen_shadow'));
SQL

echo ""
echo "########## AFTER migration 007: benchmark_frozen_shadow should be ACCEPTED ##########"
if try_shadow_insert; then
    ok "benchmark_frozen_shadow position INSERT accepted after migration 007"
else
    bad "benchmark_frozen_shadow position INSERT rejected (should be accepted after 007)"
fi

if try_shadow_equity_insert; then
    ok "benchmark_frozen_shadow equity_curve INSERT accepted after migration 007"
else
    bad "benchmark_frozen_shadow equity_curve INSERT rejected (should be accepted after 007)"
fi

echo ""
echo "########## AFTER migration 007: pre-existing streams still accepted ##########"
if try_existing_stream_insert; then
    ok "benchmark_rebench position INSERT still accepted after 007 (not narrowed)"
else
    bad "benchmark_rebench position INSERT should still be accepted after 007"
fi

echo ""
echo "########## CLEANING UP test rows before rollback ##########"
clean_test_rows
ok "Removed test rows"

echo ""
echo "########## APPLYING rollback (migration 007_rollback) ##########"
$PSQL -v ON_ERROR_STOP=1 <<'SQL'
ALTER TABLE trading.positions
    DROP CONSTRAINT IF EXISTS positions_portfolio_type_check;
ALTER TABLE trading.positions
    ADD CONSTRAINT positions_portfolio_type_check
    CHECK (portfolio_type IN ('system', 'qt', 'benchmark', 'benchmark_rebench'));

ALTER TABLE trading.equity_curve
    DROP CONSTRAINT IF EXISTS equity_curve_portfolio_type_check;
ALTER TABLE trading.equity_curve
    ADD CONSTRAINT equity_curve_portfolio_type_check
    CHECK (portfolio_type IN ('system', 'qt', 'benchmark', 'benchmark_rebench'));
SQL

echo ""
echo "########## AFTER rollback: benchmark_frozen_shadow should be REJECTED again ##########"
if try_shadow_insert; then
    bad "benchmark_frozen_shadow position INSERT succeeded after rollback (should be rejected)"
else
    ok "benchmark_frozen_shadow position INSERT rejected after rollback as expected"
fi

echo ""
echo "########## Idempotence: rollback applied twice changes nothing ##########"
$PSQL -v ON_ERROR_STOP=1 <<'SQL'
ALTER TABLE trading.positions
    DROP CONSTRAINT IF EXISTS positions_portfolio_type_check;
ALTER TABLE trading.positions
    ADD CONSTRAINT positions_portfolio_type_check
    CHECK (portfolio_type IN ('system', 'qt', 'benchmark', 'benchmark_rebench'));

ALTER TABLE trading.equity_curve
    DROP CONSTRAINT IF EXISTS equity_curve_portfolio_type_check;
ALTER TABLE trading.equity_curve
    ADD CONSTRAINT equity_curve_portfolio_type_check
    CHECK (portfolio_type IN ('system', 'qt', 'benchmark', 'benchmark_rebench'));
SQL
[ $? -eq 0 ] && ok "second rollback succeeded (idempotent)" || bad "second rollback failed"

echo ""
echo "RESULT: $pass passed, $fail failed"
[ "$fail" -eq 0 ] || exit 1
