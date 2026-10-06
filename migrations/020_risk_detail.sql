-- 020_risk_detail.sql
--
-- Add risk_detail (jsonb, NULL) to trading.live_results AND backtest.equity_curve: the loop's
-- record of one rebalance (LOOP_SPEC v6.2 sections 7.2 and 7.3; T-LOOP commit (3)). One flat
-- object of nine keys: risk_requested, binding_term, over_limit_after_rounding_terms,
-- over_limit_after_rounding_excess, over_limit_by_hold_terms, over_limit_by_hold_symbols,
-- overlay_blind, sizing_capital, account_value. Written on every futures live row and on the
-- backtest.equity_curve row of every sized rebalance; NULL on equity rows, on a refused or
-- all-JUNK rebalance, and on the backtest's seed row and warm-up rows.
--
-- The same migration re-points the COMMENT of trading.live_results.risk_scale to the DELIVERED
-- scale. The column keeps its name and type; from this binary on a futures row stores the stored
-- book's gross notional over the capped target's gross notional, and the request (m_t) is the
-- risk_detail key risk_requested.
--
-- SAFETY: ADD COLUMN of a NULL-able column with no default is metadata-only; every existing row
-- reads NULL; no key, index or view is touched; type-guarded; transactional and idempotent.

BEGIN;
DO $$
DECLARE t text;
BEGIN
    IF to_regclass('trading.live_results') IS NULL THEN RAISE EXCEPTION 'trading.live_results does not exist'; END IF;
    IF to_regclass('backtest.equity_curve') IS NULL THEN RAISE EXCEPTION 'backtest.equity_curve does not exist'; END IF;
    FOR t IN SELECT table_schema || '.' || table_name || ' ' || data_type FROM information_schema.columns
              WHERE ((table_schema = 'trading' AND table_name = 'live_results') OR (table_schema = 'backtest' AND table_name = 'equity_curve'))
                AND column_name = 'risk_detail' AND data_type <> 'jsonb' LOOP
        RAISE EXCEPTION 'a risk_detail column exists with another type: %', t;
    END LOOP;
    IF NOT EXISTS (SELECT 1 FROM information_schema.columns WHERE table_schema = 'trading' AND table_name = 'live_results'
                   AND column_name = 'risk_scale') THEN
        RAISE EXCEPTION 'trading.live_results.risk_scale does not exist';
    END IF;
END $$;
ALTER TABLE trading.live_results ADD COLUMN IF NOT EXISTS risk_detail jsonb;
ALTER TABLE backtest.equity_curve ADD COLUMN IF NOT EXISTS risk_detail jsonb;
COMMENT ON COLUMN trading.live_results.risk_detail IS
    'The loop''s record of the day''s rebalance, one flat object: risk_requested (m_t, 1.0 with no cut), binding_term (R, R_jump, R_shock, L_g, L_n or none), over_limit_after_rounding_terms (the terms still above their limits after the trim, '';''-separated, or null), over_limit_after_rounding_excess (the largest of those excesses in units of the largest non-zero stored weight per contract, or null), over_limit_by_hold_terms (the terms held rows keep over, CAP for a held row beyond the per-name cap, or null), over_limit_by_hold_symbols (those held rows, sorted, space-separated, or null), overlay_blind (the gate window had fewer than 21 complete dates), sizing_capital (E_t, the capital the book was sized on), account_value (V_t at the same instant: the starting capital plus the cumulative settled net P&L E_t was built from). Written on futures rows; NULL on equity rows and on a refused or all-JUNK rebalance. LOOP_SPEC v6.2 section 7.3; migration 020.';
COMMENT ON COLUMN backtest.equity_curve.risk_detail IS
    'The loop''s record of the rebalance of this row''s date, the same nine keys as trading.live_results.risk_detail. Written on the row of every sized rebalance of a futures book; NULL on the seed row, the warm-up rows, a refused or all-JUNK rebalance and every equity row. LOOP_SPEC v6.2 section 7.3; migration 020.';
COMMENT ON COLUMN trading.live_results.risk_scale IS
    'The DELIVERED scale of the day''s rebalance: the stored book''s gross notional over the capped target''s gross notional, both at the raw signal closes (held rows counted in both); it can exceed 1. The requested multiplier m_t is never stored here: it is risk_detail.risk_requested. Rows written before migration 020''s binary hold the reporter''s recommended scale. LOOP_SPEC v6.2 sections 7.2 and 10; migration 020.';
COMMIT;
