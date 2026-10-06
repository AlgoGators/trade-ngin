-- Preserve each sleeve's gross/as-if execution cost while recording the
-- account-level netting credit (or debit) separately. The effective cost is:
--     total_transaction_costs - netting_adjustment
-- Existing and non-netted executions have zero adjustment.

BEGIN;

DO $$
BEGIN
    IF to_regclass('trading.executions') IS NULL THEN
        RAISE EXCEPTION 'trading.executions does not exist';
    END IF;
END
$$;

ALTER TABLE trading.executions
    ADD COLUMN IF NOT EXISTS netting_adjustment NUMERIC NOT NULL DEFAULT 0;

DO $$
BEGIN
    IF NOT EXISTS (
        SELECT 1
          FROM pg_constraint
         WHERE conrelid = 'trading.executions'::regclass
           AND conname = 'executions_netting_adjustment_valid'
    ) THEN
        ALTER TABLE trading.executions
            ADD CONSTRAINT executions_netting_adjustment_valid CHECK (
                abs(netting_adjustment) <= 1000000000
                AND total_transaction_costs - netting_adjustment >= 0
            );
    END IF;
END
$$;

COMMENT ON COLUMN trading.executions.netting_adjustment IS
    'Sleeve share of sum(C(q_i))-C(Q); may be negative. Net cost is total_transaction_costs-netting_adjustment.';

COMMIT;
