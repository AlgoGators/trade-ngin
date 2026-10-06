-- Additive compatibility column: rollback is deliberately non-destructive.
-- Older binaries ignore this column and its zero default. Dropping it would
-- destroy the reconciliation evidence for already-published sleeve fills.

BEGIN;

DO $$
BEGIN
    RAISE EXCEPTION 'Refusing to drop trading.executions.netting_adjustment; preserve published reconciliation evidence';
END
$$;

COMMIT;
