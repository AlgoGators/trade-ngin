-- 004_get_trading_days_portfolio_scope_rollback.sql
--
-- Rollback for 004. Restores trading.get_trading_days() to the 2-arg form alone.
--
-- READ THIS BEFORE RUNNING IT.
--
-- Dropping the 3-arg overload while any runner still calls it does NOT produce an error the
-- operator will notice. The runners initialise `trading_days_count = 1` and only overwrite it
-- when the query succeeds, so a missing function leaves the count at 1 and the run exits 0
-- with five corrupted columns: total_annualized_return, sharpe_ratio, sortino_ratio,
-- total_days, and win_rate (which becomes winning_days * 100, roughly 9000%).
--
-- So the order matters and is the reverse of the forward migration:
--
--   1. Revert the CODE first -- point every runner back at the 2-arg call.
--   2. Rebuild and confirm no source calls the 3-arg form:
--        grep -rn "get_trading_days" apps/ src/ | grep -c "portfolio_id"     -> expect 0
--   3. Only then run this file.
--
-- The equity runner (live_equity_mean_reversion.cpp) has called the 3-arg form since
-- 232b619e and depends on it for correct annualization: unscoped, LIVE_EQUITY_MEAN_REVERSION
-- anchors to a stale 2025-08-15 BASE_PORTFOLIO row and reports 356 trading days against a
-- 10-day series -- a 36x understatement of annualized return (E2/B4). Rolling back the
-- function without also reverting the equity runner reintroduces that defect.

BEGIN;

DROP FUNCTION IF EXISTS trading.get_trading_days(character varying, date, character varying);

-- The 2-arg form is left exactly as it is. It was never modified by 004 -- that migration
-- only brought its existing definition under version control -- so there is nothing to
-- restore here.

COMMIT;

-- Verify the 2-arg form still answers and the 3-arg form is gone:
--   SELECT trading.get_trading_days('LIVE_TREND_FOLLOWING', DATE '2026-05-03');   -- expect 211
--   SELECT count(*) FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace
--    WHERE n.nspname = 'trading' AND p.proname = 'get_trading_days';             -- expect 1
