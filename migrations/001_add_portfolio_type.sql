-- F1 / dual portfolio tracking: separate the system's own positions from the
-- portfolio QT actually trades, so discretionary alpha becomes measurable.
--
--   system = trade-ngin's pure signal-driven output, written untouched
--   qt     = starts as a copy of system, then QT edits it; THIS is what executes
--
-- Two equity curves, and the spread between them is QT's contribution.
--
--
-- WHY THIS IS MORE THAN "ADD A COLUMN"
-- -------------------------------------
-- ADR-000 C-5 specifies only ADD COLUMN portfolio_type. That is not sufficient.
-- The live primary key is:
--
--   positions_pkey UNIQUE (portfolio_id, strategy_id, strategy_name, date, symbol)
--
-- There is no system/qt discriminator in it, so writing a 'system' row and a
-- 'qt' row for the same portfolio/strategy/date/symbol is a primary key
-- violation -- the second insert simply fails. Dual portfolio is impossible to
-- write until the key itself includes portfolio_type. Same for the equity
-- curve's uniqueness constraint.
--
-- Verified against the live database (read-only introspection, 2026-07-25),
-- not inferred from documentation.
--
--
-- SAFETY
-- ------
-- * PostgreSQL 16.6, so ADD COLUMN with a constant DEFAULT is metadata-only --
--   no table rewrite, no long lock.
-- * Neither table is a TimescaleDB hypertable (only futures_data.ohlcv_1d is),
--   so there are no chunk-level complications.
-- * Both tables are small: positions ~3,781 rows / 6 MB, equity_curve ~270 rows.
--   The key rebuild is sub-second; no maintenance window needed.
-- * Adding a column to a key can only ever make it MORE permissive, so existing
--   rows cannot conflict under the new key.
-- * DEFAULT 'system' means every existing row is immediately correct: all
--   history to date IS the system portfolio. No backfill, no gap, nothing to
--   clean up afterwards.
-- * Wrapped in a transaction: it either fully applies or fully rolls back.
-- * Idempotent -- safe to re-run.
--
-- Rollback: migrations/001_add_portfolio_type_rollback.sql

BEGIN;

-- --------------------------------------------------------------------------
-- trading.positions
-- --------------------------------------------------------------------------

ALTER TABLE trading.positions
    ADD COLUMN IF NOT EXISTS portfolio_type TEXT NOT NULL DEFAULT 'system';

ALTER TABLE trading.positions
    DROP CONSTRAINT IF EXISTS positions_portfolio_type_check;
ALTER TABLE trading.positions
    ADD CONSTRAINT positions_portfolio_type_check
    CHECK (portfolio_type IN ('system', 'qt'));

-- Rebuild the primary key to include the discriminator. Without this, the two
-- streams collide and the second write fails.
ALTER TABLE trading.positions
    DROP CONSTRAINT IF EXISTS positions_pkey;
ALTER TABLE trading.positions
    ADD CONSTRAINT positions_pkey
    PRIMARY KEY (portfolio_id, strategy_id, strategy_name, date, symbol, portfolio_type);

-- Reading one stream at a time is the common access pattern (the execution
-- engine reads only 'qt'; attribution reads each separately).
CREATE INDEX IF NOT EXISTS idx_trading_positions_portfolio_type
    ON trading.positions (portfolio_type);

-- --------------------------------------------------------------------------
-- trading.equity_curve
-- --------------------------------------------------------------------------

ALTER TABLE trading.equity_curve
    ADD COLUMN IF NOT EXISTS portfolio_type TEXT NOT NULL DEFAULT 'system';

ALTER TABLE trading.equity_curve
    DROP CONSTRAINT IF EXISTS equity_curve_portfolio_type_check;
ALTER TABLE trading.equity_curve
    ADD CONSTRAINT equity_curve_portfolio_type_check
    CHECK (portfolio_type IN ('system', 'qt'));

-- Note: portfolio_id is nullable on this table (unlike positions, where it is
-- NOT NULL). That inconsistency predates this change and is left alone here --
-- under Postgres' default NULLS DISTINCT, rows with a NULL portfolio_id do not
-- conflict with each other, exactly as before.
ALTER TABLE trading.equity_curve
    DROP CONSTRAINT IF EXISTS trading_equity_curve_unique;
ALTER TABLE trading.equity_curve
    ADD CONSTRAINT trading_equity_curve_unique
    UNIQUE (portfolio_id, strategy_id, "timestamp", portfolio_type);

CREATE INDEX IF NOT EXISTS idx_trading_equity_curve_portfolio_type
    ON trading.equity_curve (portfolio_type);

COMMIT;
