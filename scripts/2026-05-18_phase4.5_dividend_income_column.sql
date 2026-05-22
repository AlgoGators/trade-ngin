-- Phase 4.5: cumulative dividend income column on trading.live_results.
--
-- Run once before deploying the corresponding C++ code. Safe to re-run
-- (IF NOT EXISTS guard).
--
-- Usage:
--   PGPASSWORD=algogators psql -h <host> -U postgres -d new_algo_data \
--       -f scripts/2026-05-18_phase4.5_dividend_income_column.sql

ALTER TABLE trading.live_results
    ADD COLUMN IF NOT EXISTS total_dividend_income DECIMAL(18,8) DEFAULT 0;

COMMENT ON COLUMN trading.live_results.total_dividend_income IS
    'Cumulative dividend cash income for (strategy, portfolio) as of this date. '
    'Informational ONLY -- NOT added to total_pnl. closeadj captures dividend '
    'total-return via price continuity (Phase 4 avg_price frame-alignment fix); '
    'adding dividend cash on top would double-count. '
    'Source: in-process sum of CorporateActionsAuditLog dividend_events at daily finalization. '
    'Decomposition view: total_return = capital_appreciation + total_dividend_income.';
