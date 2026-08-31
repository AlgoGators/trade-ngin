-- Add run_inputs table to track engine execution inputs and benchmark_replays
-- for multi-mode benchmark replay operations.
--
-- Companion to 001-005. Run after all prior migrations.
--
--
-- WHY RUN_INPUTS
-- ---------------
-- Every execution of trade-ngin writes out positions and equity curves, but
-- consumes only the live market data and position state. To reproduce a past
-- execution or to replay with a different market datasource, we need to
-- record exactly what inputs the engine saw. This table captures that snapshot:
-- the universe selection, data window, risk limits configuration, and engine
-- flags (all as JSONB), plus the engine version that produced the output.
--
-- Recording these alongside every execution is the foundation for:
--   - Post-hoc forensics: "why did the engine make that trade"
--   - Multi-mode benchmark replay: "what would have happened if we replayed
--     with the actual data instead of yesterday's snapshot"
--   - Configuration audit trails: "when did this setting change"
--
--
-- WHY BENCHMARK_REPLAYS
-- ----------------------
-- Benchmark replays use the run_inputs to re-execute the engine in two modes:
--   frozen    uses yesterday's cached market data (like the live engine)
--   current   uses today's actual market data (the counterfactual)
-- This table tracks execution status, performance, and timing for each replay
-- attempt, keyed by portfolio and date range. A single portfolio over a date
-- range can have multiple attempts (e.g., retrying a failed compute), so
-- run_id is a bigserial sequence.
--
-- The engine_mode and status fields are constrained to their closed set of
-- valid values at the database layer to prevent invalid states from being
-- written by accident or bug.
--
--
-- WHY WIDEN PORTFOLIO_TYPE CONSTRAINTS
-- -------------------------------------
-- The benchmark_replays table will write 'benchmark_rebench' entries to the
-- positions and equity_curve tables when replaying in 'current' mode. To keep
-- the benchmark counterfactual clean (never mixing 'frozen' and 'current'
-- results in one row), writes go to a distinct stream value. This requires
-- widening the existing portfolio_type CHECK constraints that currently only
-- allow ('system', 'qt', 'benchmark').
--
-- The stream values are now:
--   system           live algorithmic output (traditional "system" stream)
--   qt               actual holdings after human edits (traditional "qt" stream)
--   benchmark        counterfactual with default parameters (traditional)
--   benchmark_rebench replay output in 'current' mode (new; never mixed with frozen)
--
--
-- SAFETY
-- ------
-- * Widens CHECK constraints only on portfolio_type: no existing row can become
--   invalid.
-- * Creates two new tables (run_inputs, benchmark_replays): no existing data
--   is read, altered or deleted.
-- * Idempotent and transactional.

BEGIN;

-- Widen the portfolio_type CHECK constraint to include 'benchmark_rebench'
-- (used by benchmark replay in 'current' mode, never mixed with 'frozen').
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

-- Record the exact inputs consumed by an engine execution, enabling later
-- replay and forensics.
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

COMMENT ON TABLE trading.run_inputs IS
    'Snapshot of the exact inputs consumed by a trade-ngin execution, '
    'enabling later replay and forensics (ADR-005 §5.2).';

-- Track benchmark replay executions: when a replay was attempted, in which
-- mode, with how much progress, and whether it succeeded.
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

COMMENT ON TABLE trading.benchmark_replays IS
    'Execution status of benchmark replay attempts (ADR-005 §5.3). '
    'Tracks progress and outcome for replays of a portfolio over a date range.';

-- Indexing for the common query: "show me all replays for this portfolio,
-- most recent first".
CREATE INDEX IF NOT EXISTS idx_benchmark_replays_portfolio_started
    ON trading.benchmark_replays (portfolio_id, started_at DESC);

-- Indexing for the audit question: "which replays are still running or failed"
CREATE INDEX IF NOT EXISTS idx_benchmark_replays_status
    ON trading.benchmark_replays (status)
    WHERE status IN ('running', 'failed');

COMMIT;
