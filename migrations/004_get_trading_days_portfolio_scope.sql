-- 004_get_trading_days_portfolio_scope.sql
--
-- Bring trading.get_trading_days() under version control, and repoint the futures runners at
-- the portfolio-scoped overload (E2-F6 / B5).
--
-- WHY THIS MIGRATION EXISTS AT ALL
--
-- Both overloads were created by hand against the live server and have never existed in the
-- repository. `grep -rl get_trading_days migrations/ scripts/ docs/migrations/` returns
-- nothing. The 3-arg scoped form was added during the B4/B6 work the same way.
--
-- That matters because the failure mode when the function is absent is SILENT. The runners
-- initialise `trading_days_count = 1` and only overwrite it if the query succeeds
-- (live_portfolio_conservative.cpp, live_portfolio.cpp, live_equity_mean_reversion.cpp). A
-- missing function makes execute_query error, the branch is skipped, the count stays at 1,
-- nothing throws and the run exits 0 -- poisoning five stored columns:
-- total_annualized_return, sharpe_ratio, sortino_ratio, total_days and win_rate (which
-- becomes winning_days * 100, roughly 9000%).
--
-- So a DB rebuild, restore, or fresh environment would silently corrupt reported futures
-- performance. This migration makes the definitions travel with the code.
--
-- BOTH BODIES ARE VERBATIM COPIES of what is running on the live server, taken with
-- pg_get_functiondef() on 2026-09-01 -- not reconstructions. CREATE OR REPLACE only; nothing
-- is dropped, and the 2-arg overload is deliberately retained for compatibility.
--
-- EXPECTED EFFECT ON NUMBERS: NONE, today. Both LIVE_TREND_FOLLOWING metadata rows carry the
-- same live_start_date (2025-10-05), so scoped and unscoped agree at 211 trading days. That
-- falsifiable zero IS the test: if any futures annualized figure moves after the runners are
-- repointed, the coincidence assumption was wrong and it must be investigated before
-- proceeding.
--
-- The latent risk this closes: the unscoped lookup is `ORDER BY live_start_date LIMIT 1` with
-- no portfolio predicate, so it takes the EARLIEST row across all portfolios. The moment a
-- BASE_PORTFOLIO row is added or edited with an earlier date, the conservative book's
-- annualization changes with no error and no log line.

BEGIN;

-- ---------------------------------------------------------------------------
-- 2-arg form: ORIGINAL, unscoped. Retained for compatibility; unused once the
-- futures runners are repointed. Do not delete -- external callers may exist.
-- ---------------------------------------------------------------------------
CREATE OR REPLACE FUNCTION trading.get_trading_days(
    p_strategy_id character varying,
    p_target_date date)
 RETURNS integer
 LANGUAGE plpgsql
AS $function$
DECLARE
    v_start_date DATE;
    v_trading_days INTEGER;
BEGIN
    SELECT live_start_date INTO v_start_date
    FROM trading.strategy_trading_days_metadata
    WHERE strategy_id = p_strategy_id
    ORDER BY live_start_date
    LIMIT 1;

    IF v_start_date IS NULL THEN
        SELECT MIN(DATE(date)) INTO v_start_date
        FROM trading.live_results
        WHERE strategy_id = p_strategy_id;
    END IF;

    IF v_start_date IS NULL THEN
        RETURN 1;
    END IF;

    v_trading_days := (p_target_date - v_start_date) + 1;
    RETURN GREATEST(1, v_trading_days);
END;
$function$;

-- ---------------------------------------------------------------------------
-- 3-arg form: portfolio-scoped. This is what every runner should call.
--
-- Note the fail-safe on a missing metadata row: it does NOT fall back to the
-- unscoped lookup. It tries the portfolio-scoped live_results fallback and, if
-- that is also empty, returns 1. A strategy moved onto a new portfolio therefore
-- needs its strategy_trading_days_metadata row seeded BEFORE its first run, or
-- annualization anchors to nothing.
-- ---------------------------------------------------------------------------
CREATE OR REPLACE FUNCTION trading.get_trading_days(
    p_strategy_id character varying,
    p_target_date date,
    p_portfolio_id character varying)
 RETURNS integer
 LANGUAGE plpgsql
AS $function$
DECLARE
    v_start_date DATE;
    v_trading_days INTEGER;
BEGIN
    -- Scoped by portfolio. The 2-arg form keys on strategy_id alone, which is ambiguous the
    -- moment one strategy_id runs under two portfolios (LIVE_TREND_FOLLOWING has a row for
    -- BASE_PORTFOLIO and one for CONSERVATIVE_PORTFOLIO), and its live_results fallback can
    -- anchor to a different portfolio's history entirely -- which is how the equity book
    -- came to report 356 days against a 10-day series (E2/B4).
    SELECT live_start_date INTO v_start_date
    FROM trading.strategy_trading_days_metadata
    WHERE strategy_id = p_strategy_id
      AND portfolio_id = p_portfolio_id
    ORDER BY live_start_date
    LIMIT 1;

    IF v_start_date IS NULL THEN
        SELECT MIN(DATE(date)) INTO v_start_date
        FROM trading.live_results
        WHERE strategy_id = p_strategy_id
          AND portfolio_id = p_portfolio_id;
    END IF;

    IF v_start_date IS NULL THEN
        RETURN 1;
    END IF;

    v_trading_days := (p_target_date - v_start_date) + 1;
    RETURN GREATEST(1, v_trading_days);
END;
$function$;

COMMIT;

-- ---------------------------------------------------------------------------
-- VERIFICATION -- run after applying, before repointing any runner.
-- Expect 211 / 211 / 211 for LIVE_TREND_FOLLOWING at 2026-05-03. Any other
-- result means the metadata rows have changed and the no-movement prediction
-- for the code repoint no longer holds.
--
--   SELECT trading.get_trading_days('LIVE_TREND_FOLLOWING', DATE '2026-05-03')                            AS unscoped,
--          trading.get_trading_days('LIVE_TREND_FOLLOWING', DATE '2026-05-03', 'CONSERVATIVE_PORTFOLIO')  AS scoped_cons,
--          trading.get_trading_days('LIVE_TREND_FOLLOWING', DATE '2026-05-03', 'BASE_PORTFOLIO')          AS scoped_base;
-- ---------------------------------------------------------------------------
