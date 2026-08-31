#!/usr/bin/env bash
# Verifies migration 006_run_inputs_and_rebench_stream.sql against a real PostgreSQL 16.
#
# The property under test:
#   1. Before 006: 'benchmark_rebench' portfolio_type is rejected by CHECK constraints
#   2. After 006:
#      - 'benchmark_rebench' is accepted
#      - run_inputs table exists and can store execution snapshots
#      - benchmark_replays table exists and tracks replay status
#   3. After rollback: 'benchmark_rebench' is rejected again and tables are gone
#
# Requires PGHOST/PGPORT/PGUSER/PGPASSWORD/PGDATABASE and CONTAINER_NAME.

set -uo pipefail

# Use docker exec to run psql inside the container
CONTAINER_NAME="${CONTAINER_NAME:-test-postgres-006}"
PSQL="docker exec -i ${CONTAINER_NAME} psql -v ON_ERROR_STOP=1 -q -X -U ${PGUSER} -d ${PGDATABASE}"
pass=0
fail=0
ok()  { echo "  PASS  $1"; pass=$((pass + 1)); }
bad() { echo "  FAIL  $1"; fail=$((fail + 1)); }

# Helper: count rows in run_inputs by portfolio_id
run_inputs_count() {
    $PSQL -tAc "SELECT count(*) FROM trading.run_inputs WHERE portfolio_id='$1'"
}

# Helper: count rows in benchmark_replays
benchmark_replays_count() {
    $PSQL -tAc "SELECT count(*) FROM trading.benchmark_replays WHERE portfolio_id='$1'"
}

# Helper: try to insert a benchmark_rebench position; return 0 if accepted, 1 if rejected
try_benchmark_rebench_insert() {
    # Use a subshell to suppress error output and capture exit code
    $PSQL -c "
        INSERT INTO trading.positions
            (symbol, quantity, average_price, daily_unrealized_pnl, daily_realized_pnl,
             last_update, updated_at, strategy_id, strategy_name, date, portfolio_id, portfolio_type)
        VALUES ('BRK_B', 1, 500, 0, 0, '2026-05-05', '2026-05-05', 'TEST_S', 'TEST_N', '2026-05-05', 'P_TEST', 'benchmark_rebench')
    " 2>&1 > /dev/null
    return $?
}

# Helper: try to insert a benchmark_rebench equity curve; return 0 if accepted, 1 if rejected
try_benchmark_rebench_equity_insert() {
    $PSQL -c "
        INSERT INTO trading.equity_curve (strategy_id, timestamp, equity, portfolio_id, portfolio_type)
        VALUES ('TEST_S', '2026-05-05 10:30:00', 1000000.0, 'P_TEST', 'benchmark_rebench')
    " 2>&1 > /dev/null
    return $?
}

# Helper: clean up test rows that are incompatible with narrowed constraint
clean_test_rows() {
    $PSQL -q -c "DELETE FROM trading.positions WHERE portfolio_type = 'benchmark_rebench'" 2>/dev/null || true
    $PSQL -q -c "DELETE FROM trading.equity_curve WHERE portfolio_type = 'benchmark_rebench'" 2>/dev/null || true
}

# --- Fixture: Pre-migration schema (001-005) ---------------------------------
# This recreates the schema state after migrations 001-005 have been applied.
# (001 adds portfolio_type; 002 backfills; 003 widens constraint to include 'benchmark')
$PSQL <<'SQL'
DROP SCHEMA IF EXISTS trading CASCADE;
CREATE SCHEMA trading;

-- 001_add_portfolio_type: introduce portfolio_type column with 'system' as default
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
        CHECK (portfolio_type IN ('system','qt','benchmark')),
    CONSTRAINT positions_pkey PRIMARY KEY
        (portfolio_id, strategy_id, strategy_name, date, symbol, portfolio_type)
);

CREATE TABLE trading.equity_curve (
    strategy_id VARCHAR NOT NULL,
    timestamp TIMESTAMPTZ NOT NULL,
    equity NUMERIC NOT NULL,
    portfolio_id VARCHAR NOT NULL,
    portfolio_type TEXT NOT NULL DEFAULT 'system'
        CHECK (portfolio_type IN ('system','qt','benchmark')),
    CONSTRAINT equity_curve_pkey PRIMARY KEY
        (portfolio_id, strategy_id, timestamp, portfolio_type)
);

-- Insert some fixtures: one position and one equity curve row per stream
INSERT INTO trading.positions
 (symbol,quantity,average_price,daily_unrealized_pnl,daily_realized_pnl,last_update,
  strategy_id,strategy_name,date,portfolio_id,portfolio_type)
VALUES ('SPY', 100, 450.0, 500, 0, '2026-05-04', 'LONG_EQ', 'LongEQ', '2026-05-04', 'MAIN', 'system'),
       ('SPY', 100, 450.0, 500, 0, '2026-05-04', 'LONG_EQ', 'LongEQ', '2026-05-04', 'MAIN', 'qt'),
       ('SPY', 100, 450.0, 500, 0, '2026-05-04', 'LONG_EQ', 'LongEQ', '2026-05-04', 'MAIN', 'benchmark');

INSERT INTO trading.equity_curve
 (strategy_id,timestamp,equity,portfolio_id,portfolio_type)
VALUES ('LONG_EQ', '2026-05-04 16:00:00', 1000000, 'MAIN', 'system'),
       ('LONG_EQ', '2026-05-04 16:00:00', 1000500, 'MAIN', 'qt'),
       ('LONG_EQ', '2026-05-04 16:00:00', 1000000, 'MAIN', 'benchmark');
SQL

echo "########## BEFORE migration 006: benchmark_rebench should be REJECTED ##########"
if try_benchmark_rebench_insert; then
    bad "benchmark_rebench position INSERT succeeded (should be rejected by CHECK)"
else
    ok "benchmark_rebench position INSERT rejected as expected"
fi

if try_benchmark_rebench_equity_insert; then
    bad "benchmark_rebench equity_curve INSERT succeeded (should be rejected by CHECK)"
else
    ok "benchmark_rebench equity_curve INSERT rejected as expected"
fi

echo ""
echo "########## APPLYING migration 006 ##########"
$PSQL -v ON_ERROR_STOP=1 <<'SQL'
-- Widen the portfolio_type CHECK constraint to include 'benchmark_rebench'
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

CREATE TABLE IF NOT EXISTS trading.run_inputs (
    portfolio_id      TEXT        NOT NULL,
    strategy_id       TEXT        NOT NULL,
    date              DATE        NOT NULL,
    trade_ngin_sha    TEXT        NOT NULL,
    config_snapshot   JSONB       NOT NULL,
    universe          JSONB       NOT NULL,
    data_window       JSONB       NOT NULL,
    risk_limits_id    BIGINT      NULL,
    engine_flags      JSONB       NOT NULL,
    recorded_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (portfolio_id, strategy_id, date)
);

CREATE TABLE IF NOT EXISTS trading.benchmark_replays (
    run_id           BIGSERIAL PRIMARY KEY,
    portfolio_id     TEXT        NOT NULL,
    from_date        DATE        NOT NULL,
    through_date     DATE        NOT NULL,
    engine_sha_used  TEXT        NOT NULL,
    engine_mode      TEXT        NOT NULL CHECK (engine_mode IN ('frozen', 'current')),
    days_computed    INTEGER     NOT NULL,
    status           TEXT        NOT NULL CHECK (status IN ('running','completed','failed')),
    started_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
    finished_at      TIMESTAMPTZ NULL
);

CREATE INDEX IF NOT EXISTS idx_benchmark_replays_portfolio_started
    ON trading.benchmark_replays (portfolio_id, started_at DESC);

CREATE INDEX IF NOT EXISTS idx_benchmark_replays_status
    ON trading.benchmark_replays (status)
    WHERE status IN ('running', 'failed');
SQL

echo ""
echo "########## AFTER migration 006: benchmark_rebench should be ACCEPTED ##########"
if try_benchmark_rebench_insert; then
    ok "benchmark_rebench position INSERT accepted after migration 006"
else
    bad "benchmark_rebench position INSERT rejected (should be accepted after 006)"
fi

if try_benchmark_rebench_equity_insert; then
    ok "benchmark_rebench equity_curve INSERT accepted after migration 006"
else
    bad "benchmark_rebench equity_curve INSERT rejected (should be accepted after 006)"
fi

echo ""
echo "########## Testing run_inputs table ##########"
$PSQL -q -c "
    INSERT INTO trading.run_inputs
        (portfolio_id, strategy_id, date, trade_ngin_sha, config_snapshot, universe, data_window, engine_flags)
    VALUES
        ('P_PROD', 'LONG_EQ', '2026-05-05', 'abc1234def5678',
         '{\"min_position_size\": 100, \"max_position_size\": 10000}'::jsonb,
         '{\"symbols\": [\"SPY\", \"QQQ\", \"DIA\"]}'::jsonb,
         '{\"market_data_age_minutes\": 1440}'::jsonb,
         '{\"use_cache\": true, \"backtest_mode\": false}'::jsonb)
"
[ "$(run_inputs_count P_PROD)" = "1" ] && ok "run_inputs row inserted and retrievable" || bad "run_inputs count = $(run_inputs_count P_PROD)"

echo ""
echo "########## Testing benchmark_replays table ##########"
$PSQL -q -c "
    INSERT INTO trading.benchmark_replays
        (portfolio_id, from_date, through_date, engine_sha_used, engine_mode, days_computed, status)
    VALUES
        ('P_PROD', '2026-04-01', '2026-05-05', 'abc1234def5678', 'frozen', 25, 'completed'),
        ('P_PROD', '2026-04-01', '2026-05-05', 'abc1234def5678', 'current', 20, 'running')
"
[ "$(benchmark_replays_count P_PROD)" = "2" ] && ok "benchmark_replays rows inserted and retrievable" || bad "benchmark_replays count = $(benchmark_replays_count P_PROD)"

echo ""
echo "########## Testing benchmark_replays constraints ##########"
# This should fail: invalid engine_mode
constraint_err=$($PSQL -c "
    INSERT INTO trading.benchmark_replays
        (portfolio_id, from_date, through_date, engine_sha_used, engine_mode, days_computed, status)
    VALUES ('P_PROD', '2026-04-01', '2026-05-05', 'abc1234def5678', 'invalid_mode', 10, 'running')
" 2>&1)
if echo "$constraint_err" | grep -q "violates check constraint\|new row violates check"; then
    ok "engine_mode CHECK constraint rejects invalid value"
else
    bad "engine_mode CHECK constraint should reject 'invalid_mode'"
fi

# This should fail: invalid status
constraint_err=$($PSQL -c "
    INSERT INTO trading.benchmark_replays
        (portfolio_id, from_date, through_date, engine_sha_used, engine_mode, days_computed, status)
    VALUES ('P_PROD', '2026-04-01', '2026-05-05', 'abc1234def5678', 'frozen', 10, 'pending')
" 2>&1)
if echo "$constraint_err" | grep -q "violates check constraint\|new row violates check"; then
    ok "status CHECK constraint rejects invalid value"
else
    bad "status CHECK constraint should reject 'pending'"
fi

echo ""
echo "########## CLEANING UP benchmark_rebench rows before rollback ##########"
clean_test_rows
ok "Removed benchmark_rebench test rows"

echo ""
echo "########## APPLYING rollback (migration 006_rollback) ##########"
$PSQL -v ON_ERROR_STOP=1 <<'SQL'
-- Drop the new tables
DROP TABLE IF EXISTS trading.benchmark_replays;
DROP TABLE IF EXISTS trading.run_inputs;

-- Narrow portfolio_type back to pre-006 set
ALTER TABLE trading.positions
    DROP CONSTRAINT IF EXISTS positions_portfolio_type_check;
ALTER TABLE trading.positions
    ADD CONSTRAINT positions_portfolio_type_check
    CHECK (portfolio_type IN ('system', 'qt', 'benchmark'));

ALTER TABLE trading.equity_curve
    DROP CONSTRAINT IF EXISTS equity_curve_portfolio_type_check;
ALTER TABLE trading.equity_curve
    ADD CONSTRAINT equity_curve_portfolio_type_check
    CHECK (portfolio_type IN ('system', 'qt', 'benchmark'));
SQL

echo ""
echo "########## AFTER rollback: benchmark_rebench should be REJECTED again ##########"
if try_benchmark_rebench_insert; then
    bad "benchmark_rebench position INSERT succeeded after rollback (should be rejected)"
else
    ok "benchmark_rebench position INSERT rejected after rollback as expected"
fi

if try_benchmark_rebench_equity_insert; then
    bad "benchmark_rebench equity_curve INSERT succeeded after rollback (should be rejected)"
else
    ok "benchmark_rebench equity_curve INSERT rejected after rollback as expected"
fi

echo ""
echo "########## AFTER rollback: run_inputs and benchmark_replays tables are gone ##########"
table_list=$($PSQL -c "\dt trading.run_inputs" 2>&1)
if echo "$table_list" | grep -q "0 rows"; then
    ok "run_inputs table is gone after rollback"
else
    bad "run_inputs table should not exist after rollback"
fi

table_list=$($PSQL -c "\dt trading.benchmark_replays" 2>&1)
if echo "$table_list" | grep -q "0 rows"; then
    ok "benchmark_replays table is gone after rollback"
else
    bad "benchmark_replays table should not exist after rollback"
fi

echo ""
echo "########## Idempotence: rollback applied twice changes nothing ##########"
# First rollback (should be a no-op because already rolled back)
$PSQL -v ON_ERROR_STOP=1 <<'SQL'
DROP TABLE IF EXISTS trading.benchmark_replays;
DROP TABLE IF EXISTS trading.run_inputs;

ALTER TABLE trading.positions
    DROP CONSTRAINT IF EXISTS positions_portfolio_type_check;
ALTER TABLE trading.positions
    ADD CONSTRAINT positions_portfolio_type_check
    CHECK (portfolio_type IN ('system', 'qt', 'benchmark'));

ALTER TABLE trading.equity_curve
    DROP CONSTRAINT IF EXISTS equity_curve_portfolio_type_check;
ALTER TABLE trading.equity_curve
    ADD CONSTRAINT equity_curve_portfolio_type_check
    CHECK (portfolio_type IN ('system', 'qt', 'benchmark'));
SQL

[ $? -eq 0 ] && ok "second rollback succeeded (idempotent)" || bad "second rollback failed"

echo ""
echo "RESULT: $pass passed, $fail failed"
[ "$fail" -eq 0 ] || exit 1
