-- 006_corp_action_applied_basis_ratio.sql
--
-- Add a nullable `basis_ratio` to trading.corp_action_applied so the adjusted cost basis a
-- class-1 event wrote can be inverted back to the broker's frame FROM THE LEDGER ALONE
-- (F-8; docs/BROKER_BASIS_RECONCILIATION.md).
--
-- WHY THIS COLUMN EXISTS
--
-- `trading.positions.average_price` for an equity is restated on every class-1 ex-date the
-- book held through: a split divides it by F, a dividend divides it by (1 + d/c) with c the
-- RAW ex-date close. A broker never touches cost basis for a dividend. So the two frames
-- differ by exactly
--
--     B_broker = B_book x PRODUCT (1 + d_i / c_i)
--
-- over the dividends applied since the position's inception. That product is the only thing
-- standing between the number this engine stores and the number a statement prints, and F-8
-- is the rule that says which differences are expected.
--
-- Today the table stores qty_held, dividend_per_share and total_cash -- the CASH-FLOW audit
-- figures -- and no ratio and no close. The factor is therefore not recoverable from the
-- ledger: reconstructing it needs a join back to equities_data.ohlcv_1d for the raw ex-date
-- close of every event, per symbol, for the lifetime of the holding. Worse, it is not
-- recoverable AT ALL once the position closes, because a closed row carries no basis
-- (average_price = 0, AVERAGE_PRICE_LIFECYCLE.md rule 5) -- the dedup row is the last
-- remaining record of the chain, and it does not carry the chain.
--
-- `basis_ratio` is exactly PositionAdjustment.ratio_change, the number the applier already
-- computed and already logs ("ratio 25.000000"). Storing it makes identity 2 of the rule a
-- single query with no join.
--
-- WHAT USES IT
--
-- Reconciliation and reporting only. Nothing reads it back to make a trading decision, and
-- nothing about dedup changes: the natural key is untouched, so idempotency is exactly as it
-- was. src/live/broker_frame.{hpp,cpp} carries the pure arithmetic; the live equity runner
-- logs "basis adjusted=X raw-equivalent=Y (n events)" after the class-1 apply.
--
-- NULL means UNKNOWN, NOT 1.0. Rows written before this migration carry NULL. A basis chain
-- containing one cannot be inverted from the ledger, and a reconciliation must say so rather
-- than quietly treating the event as having had no effect -- treating NULL as 1.0 would
-- report a split-adjusted basis as broker-equivalent, which is wrong by the split factor.
--
-- FUTURES REACHABILITY
--
-- trading.corp_action_applied is written and read by exactly two files:
-- src/live/corporate_actions_audit_log.cpp and apps/strategies/live_equity_mean_reversion.cpp.
-- live_portfolio.cpp and live_portfolio_conservative.cpp contain no reference to corp_action,
-- CorporateAction or dividend. No futures runner touches this table, so no futures number can
-- move because of this migration. The shared library relinks, so the futures binaries' hashes
-- change without their behaviour changing -- argue preservation from the call graph, not the
-- hash (the same note migration 005 carries).
--
-- The equity-side rebuild risk is 005's: on a database rebuilt WITHOUT this migration, every
-- INSERT into trading.corp_action_applied names a column that does not exist. That is not
-- silent -- store_applied_corp_actions_in returns an error and the transaction carrying the
-- adjusted positions rolls back with it, so the run fails loudly rather than storing positions
-- with no dedup record. Apply 006 before running the equity book against a fresh database.
--
-- SAFETY
--   * Additive and idempotent: ADD COLUMN IF NOT EXISTS, nullable, no default, no rewrite of
--     existing rows and no table rewrite on PostgreSQL 11+.
--   * Touches no other table, no index, no constraint; the primary key is unchanged.
--   * Transactional.
--   * Independent of 005 in both directions: run_date and basis_ratio are unrelated columns
--     on the same table and neither reads the other.
--
-- NUMBERING NOTE: the E4 audit and PHASE_B_E3_E4_FIXES.md both call this "migration 005".
-- 005 was taken by corp_action_applied.run_date (E2-F23 / F23-C-prime, commit 4e9cfabe) on
-- the same day. This is 006.

BEGIN;

DO $$
BEGIN
    IF NOT EXISTS (
        SELECT 1 FROM information_schema.tables
        WHERE table_schema = 'trading' AND table_name = 'corp_action_applied'
    ) THEN
        RAISE EXCEPTION 'migration 002 has not been applied: trading.corp_action_applied does not exist';
    END IF;
END $$;

ALTER TABLE trading.corp_action_applied
    ADD COLUMN IF NOT EXISTS basis_ratio DOUBLE PRECISION;

COMMENT ON COLUMN trading.corp_action_applied.basis_ratio IS
    'The factor this event divided the cost basis by: F for a SPLIT or ADR_SPLIT, '
    '(1 + dividend/raw_close_on_ex_date) for a DIVIDEND, 1.0 for an event that moved '
    'no basis. Multiplying trading.positions.average_price by the product of these '
    'over a holding recovers the BROKER-frame basis, which never moves on a dividend '
    '(F-8; docs/BROKER_BASIS_RECONCILIATION.md). NULL on rows written before migration '
    '006 and on TERMINATION rows, and it means UNKNOWN, never 1.0: a chain containing a '
    'NULL cannot be inverted from the ledger and a reconciliation must say so. '
    'Reconciliation only -- nothing reads this back to make a trading decision, and it '
    'is not part of the natural key.';

COMMIT;

-- ---------------------------------------------------------------------------
-- VERIFICATION -- run after applying.
--
--   SELECT column_name, data_type, is_nullable
--     FROM information_schema.columns
--    WHERE table_schema = 'trading' AND table_name = 'corp_action_applied'
--      AND column_name = 'basis_ratio';
--   -- expect: basis_ratio | double precision | YES
--
--   SELECT count(*) AS total, count(basis_ratio) AS carried
--     FROM trading.corp_action_applied;
--   -- expect carried = 0 immediately after applying; every class-1 row written from now
--   -- on carries a ratio.
--
--   -- identity 2 of the rule, from the ledger alone:
--   SELECT p.symbol, p.average_price AS basis_book,
--          p.average_price * exp(sum(ln(a.basis_ratio))) AS basis_broker
--     FROM trading.positions p
--     JOIN trading.corp_action_applied a
--       ON a.portfolio_id = p.portfolio_id AND a.strategy_id = p.strategy_id
--      AND a.symbol = p.symbol AND a.action_type = 'DIVIDEND'
--      AND a.basis_ratio IS NOT NULL
--    WHERE p.date = $1 AND p.portfolio_id = $2 AND p.strategy_id = $3
--    GROUP BY p.symbol, p.average_price;
-- ---------------------------------------------------------------------------
