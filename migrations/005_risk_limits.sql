-- Stage 1b: Publish engine risk limits to database for AlgoLens to consume.
--
-- AlgoLens validates manual position edits against risk limits before committing them.
-- The engine computes and applies risk limits (max gross/net leverage, per-symbol position
-- caps). Rather than reimplementing the limit math in Python (creating a second source of
-- truth that will drift), the engine publishes the limits it actually enforces to this
-- table, and AlgoLens compares against them.
--
-- This table is the single authoritative source of what risk limits are in effect for any
-- (strategy_id, portfolio_id) at any point in time.
--
--
-- APPEND-ONLY IS INTENTIONAL
-- -------------------------
-- History of what limits were in effect on any given day is useful for audit and debugging
-- drift between actual engine behavior and what was published. Rows are never updated or
-- deleted; new limits are published as new rows. A consumer reading with ORDER BY published_at
-- DESC LIMIT 1 always gets the current envelope.
--
--
-- SAFETY
-- ------
-- * Creates a new table only. Nothing existing is read, altered or deleted.
-- * Idempotent and transactional.
-- * Index supports the consumer's query (strategy_id, portfolio_id, published_at DESC).


BEGIN;

CREATE TABLE IF NOT EXISTS trading.risk_limits (
    id            BIGSERIAL PRIMARY KEY,
    strategy_id   TEXT        NOT NULL,
    portfolio_id  TEXT        NOT NULL,
    limits        JSONB       NOT NULL,
    published_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Index supporting the consumer query:
-- SELECT limits FROM trading.risk_limits WHERE strategy_id = $1 AND portfolio_id = $2
-- ORDER BY published_at DESC LIMIT 1
CREATE INDEX IF NOT EXISTS idx_risk_limits_strategy_portfolio_published
    ON trading.risk_limits (strategy_id, portfolio_id, published_at DESC);

COMMENT ON TABLE trading.risk_limits IS
    'Append-only publication of risk limits enforced by the engine (Stage 1b). '
    'AlgoLens queries this table to validate manual position edits. '
    'limits is a JSONB object with keys: max_gross_notional, max_symbol_notional (map), '
    'max_position_count. Rows are never updated; new rows are published as limits change.';

COMMIT;
