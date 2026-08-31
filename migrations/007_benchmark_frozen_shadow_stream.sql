-- Add 'benchmark_frozen_shadow' to the portfolio_type CHECK constraints, for
-- the ADR-005 §7 parity gate.
--
-- Companion to 001-006. Run after all prior migrations.
--
--
-- WHY THIS IS NEEDED
-- -------------------
-- The §7 parity gate proves --engine frozen (migration 006 /
-- apps/tools/benchmark_replay.cpp) agrees with the existing live
-- benchmark_mode="live" pass before the default flips from "live" to
-- "deferred": replay the same dates with --engine frozen and compare,
-- position-for-position and cent-for-cent, against what the live pass
-- already wrote to the 'benchmark' stream.
--
-- That comparison needs frozen mode's output to land somewhere that does
-- NOT overwrite the 'benchmark' stream it's being compared against --
-- otherwise the parity run destroys the very data it's supposed to verify.
-- ADR-005 §7 calls this "a shadow schema (trading_shadow, or a
-- replay_run_id-tagged table)"; this migration takes the second option,
-- consistent with how migration 006 already solved the identical problem
-- for --engine current (a distinct 'benchmark_rebench' stream in the same
-- tables) rather than a separate schema -- no new schema/migration
-- machinery, same tables, one more CHECK value.
--
-- Usage: `benchmark_replay --portfolio X --engine frozen --through Y
-- --target-stream benchmark_frozen_shadow`, then compare against the
-- 'benchmark' stream for the same dates with scripts/parity_gate.sql.
--
--
-- SAFETY
-- ------
-- * Widens CHECK constraints only on portfolio_type: no existing row can
--   become invalid.
-- * No table changes, no data read/altered/deleted.
-- * Idempotent and transactional.

BEGIN;

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

COMMIT;
