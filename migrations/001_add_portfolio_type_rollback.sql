-- Rollback for 001_add_portfolio_type.sql
--
-- Restores the original primary key and uniqueness constraint and drops the
-- portfolio_type column.
--
-- ⚠️ DESTRUCTIVE IF 'qt' ROWS EXIST.
-- Once both streams are being written, the original 5-column key can no longer
-- represent the data: a 'system' row and a 'qt' row for the same
-- portfolio/strategy/date/symbol become duplicates under it, and restoring the
-- old key would fail outright. This script therefore refuses to run while any
-- 'qt' rows are present, rather than silently destroying them.
--
-- If you genuinely intend to discard the qt stream, delete those rows
-- deliberately first -- that should be a conscious decision, not a side effect
-- of a rollback.
--
-- Note the deployment escape hatch: rolling back the SCHEMA is rarely what you
-- want. To stop trading the qt stream, point the execution engine back at
-- 'system'. The qt rows can sit there ignored and harmless -- no data loss, no
-- schema change, and it is reversible in seconds.

BEGIN;

DO $$
DECLARE
    qt_positions BIGINT;
    qt_equity    BIGINT;
BEGIN
    SELECT count(*) INTO qt_positions FROM trading.positions    WHERE portfolio_type = 'qt';
    SELECT count(*) INTO qt_equity    FROM trading.equity_curve WHERE portfolio_type = 'qt';

    IF qt_positions > 0 OR qt_equity > 0 THEN
        RAISE EXCEPTION
            'Refusing to roll back: % qt position row(s) and % qt equity_curve row(s) exist. '
            'Restoring the original key would make them duplicates. Delete them deliberately '
            'first if that is genuinely intended.',
            qt_positions, qt_equity;
    END IF;
END $$;

-- trading.positions
DROP INDEX IF EXISTS trading.idx_trading_positions_portfolio_type;

ALTER TABLE trading.positions DROP CONSTRAINT IF EXISTS positions_pkey;
ALTER TABLE trading.positions
    ADD CONSTRAINT positions_pkey
    PRIMARY KEY (portfolio_id, strategy_id, strategy_name, date, symbol);

ALTER TABLE trading.positions DROP CONSTRAINT IF EXISTS positions_portfolio_type_check;
ALTER TABLE trading.positions DROP COLUMN IF EXISTS portfolio_type;

-- trading.equity_curve
DROP INDEX IF EXISTS trading.idx_trading_equity_curve_portfolio_type;

ALTER TABLE trading.equity_curve DROP CONSTRAINT IF EXISTS trading_equity_curve_unique;
ALTER TABLE trading.equity_curve
    ADD CONSTRAINT trading_equity_curve_unique
    UNIQUE (portfolio_id, strategy_id, "timestamp");

ALTER TABLE trading.equity_curve DROP CONSTRAINT IF EXISTS equity_curve_portfolio_type_check;
ALTER TABLE trading.equity_curve DROP COLUMN IF EXISTS portfolio_type;

COMMIT;
