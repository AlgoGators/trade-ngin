-- Rollback for 012_position_overrides_portfolio_scope.sql.
--
-- Removing portfolio_id or the legacy companion after either has data would
-- discard audit attribution.  Refuse that destructive rollback.  An empty
-- freshly-applied migration may be rolled back safely and idempotently.

-- Read committed also sees any writer that finished while we waited for locks,
-- even when the session default is repeatable read.
BEGIN ISOLATION LEVEL READ COMMITTED;

-- Lock the parent first, matching the forward migration's DDL order. These
-- transaction-held locks exclude INSERTs before either attribution check.
LOCK TABLE trading.position_overrides IN ACCESS EXCLUSIVE MODE;

DO $$
DECLARE
    overrides_have_scoped_rows BOOLEAN := FALSE;
    companion_has_rows BOOLEAN := FALSE;
BEGIN
    IF to_regclass('trading.position_override_legacy_scopes') IS NOT NULL THEN
        LOCK TABLE trading.position_override_legacy_scopes IN ACCESS EXCLUSIVE MODE;
    END IF;

    IF EXISTS (
        SELECT 1
        FROM information_schema.columns
        WHERE table_schema = 'trading'
          AND table_name = 'position_overrides'
          AND column_name = 'portfolio_id'
    ) THEN
        EXECUTE 'SELECT EXISTS (SELECT 1 FROM trading.position_overrides '
             || 'WHERE portfolio_id IS NOT NULL)'
            INTO overrides_have_scoped_rows;
        IF overrides_have_scoped_rows THEN
            RAISE EXCEPTION
                'cannot roll back 012: portfolio-scoped position override rows exist';
        END IF;
    END IF;

    IF to_regclass('trading.position_override_legacy_scopes') IS NOT NULL THEN
        EXECUTE 'SELECT EXISTS (SELECT 1 FROM trading.position_override_legacy_scopes)'
            INTO companion_has_rows;
        IF companion_has_rows THEN
            RAISE EXCEPTION
                'cannot roll back 012: inferred legacy scope rows exist';
        END IF;
    END IF;
END;
$$;

DROP INDEX IF EXISTS trading.idx_position_overrides_portfolio_strategy_created;
ALTER TABLE trading.position_overrides
    DROP CONSTRAINT IF EXISTS position_overrides_new_rows_require_portfolio;
DROP TABLE IF EXISTS trading.position_override_legacy_scopes;
ALTER TABLE trading.position_overrides
    DROP COLUMN IF EXISTS portfolio_id;

COMMIT;
