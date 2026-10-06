-- Indexes for the equity read paths the live runner exercises on every run.
--
--
-- WHY
-- ---
-- Measured at the full 852-symbol universe (not the 10 symbols currently
-- configured -- any strategy may hold any symbol with data):
--
--   get_delisting_dates                     14,144 ms
--   get_corporate_actions (deal terms)         491 ms, seq scan of 627k rows
--
-- equities_data.ohlcv_1d carries only its (symbol, time) primary key, and
-- equities_data.corporate_action carries NO index at all.
--
--
-- SCHEMA OWNERSHIP
-- ----------------
-- These live in equities_data, which the data pipeline owns rather than this
-- engine. Indexes are additive and non-destructive -- they change no row and no
-- column, they speed up every consumer of these tables, and the rollback drops
-- them cleanly. Flagging it explicitly so the ownership boundary is a conscious
-- crossing rather than an accident; worth mentioning to the data owner.
--
--
-- CHOICES
-- -------
--  * Both ohlcv_1d indexes are PARTIAL. Delisting dates exist for ~152 of 852
--    symbols and corporate actions for a tiny fraction of 4.5 M bars, so the
--    partial indexes are small while covering the whole predicate.
--  * The event index leads with `time` because the query filters a date range
--    across all held symbols; symbol is included for the index-only path.
--  * corporate_action is keyed (ticker, date) to match its access pattern:
--    deal terms for a set of held symbols over a window.
--
--
-- SAFETY
-- ------
--  * Idempotent (IF NOT EXISTS), additive, transactional.
--  * NOT using CONCURRENTLY: that cannot run inside a transaction block, and
--    these tables are not under live write pressure from this engine. If the
--    data pipeline is mid-load, run this when it is idle.

BEGIN;

-- get_delisting_dates: TERMINATION timing for held symbols.
CREATE INDEX IF NOT EXISTS idx_ohlcv_1d_delisting
    ON equities_data.ohlcv_1d (symbol, delisting_date)
    WHERE delisting_date IS NOT NULL;

-- get_per_bar_corporate_actions: PRICE_RESTATING events in a date window.
CREATE INDEX IF NOT EXISTS idx_ohlcv_1d_corp_events
    ON equities_data.ohlcv_1d ("time", symbol)
    WHERE COALESCE(div_cash, 0) <> 0 OR COALESCE(split_factor, 1) NOT IN (0, 1);

-- get_corporate_actions: frozen deal-terms table, currently unindexed.
CREATE INDEX IF NOT EXISTS idx_corporate_action_ticker_date
    ON equities_data.corporate_action (ticker, date);

COMMIT;

-- Planner statistics: corporate_action reports n_live_tup = 0 against 627k
-- actual rows, so the planner has been costing it blind.
ANALYZE equities_data.corporate_action;
ANALYZE equities_data.ohlcv_1d;
