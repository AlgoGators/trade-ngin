-- Backfill equities_data.ticker_aliases from the frozen corporate_action feed.
--
--
-- WHY
-- ---
-- ticker_aliases carried 16 hand-curated rows. The SERIES_CONTINUITY handler
-- uses it to re-key a position when a symbol is renamed, so at 852 symbols --
-- and any strategy may hold any of them -- 16 rows means renames are mostly
-- invisible and a held position silently stops matching its own history.
--
-- equities_data.corporate_action carries the rename history through its freeze
-- date (2025-08-29). It cannot supply renames after that, but everything before
-- is exactly the data this table is for.
--
--
-- DIRECTION -- VERIFIED, NOT ASSUMED
-- ----------------------------------
-- For action = 'tickerchangefrom': ticker is the NEW symbol, contraticker the
-- OLD one. Checked against all three spot-checkable curated rows:
--
--   curated HRS -> LHX   source: tickerchangefrom ticker=LHX contra=HRS
--   curated JEC -> J     source: tickerchangefrom ticker=J   contra=JEC
--   curated UTX -> RTX   source: tickerchangefrom ticker=RTX contra=UTX
--
-- Getting this backwards would map every renamed position to the wrong symbol,
-- so it is verified rather than inferred from the column names.
--
-- Date note: curated UTX->RTX records effective_until = 2020-04-03 while the
-- source row dates the change 2020-04-02. Backfilled rows use the source date
-- as-is; the one-day convention difference is left visible rather than
-- normalised, and curated rows are never touched.
--
--
-- SAFETY
-- ------
--  * ADDITIVE ONLY. Inserts nothing that already exists on the table's
--    (historical_ticker, current_symbol) key; never UPDATEs or DELETEs, so the
--    16 curated rows -- which carry verification notes -- survive untouched.
--  * Idempotent: re-running inserts zero rows.
--  * Scoped to pairs where at least one side is a symbol we actually have bars
--    for; aliases for symbols absent from ohlcv_1d could never be looked up.
--  * Transactional.

BEGIN;

INSERT INTO equities_data.ticker_aliases (historical_ticker, current_symbol, effective_until, note)
SELECT DISTINCT ON (ca.contraticker, ca.ticker)
       ca.contraticker AS historical_ticker,
       ca.ticker       AS current_symbol,
       ca.date::date   AS effective_until,
       'backfilled from corporate_action tickerchangefrom (' || ca.date || ')' AS note
FROM equities_data.corporate_action ca
WHERE ca.action = 'tickerchangefrom'
  AND ca.contraticker IS NOT NULL
  AND ca.contraticker <> ''
  AND ca.ticker IS NOT NULL
  AND ca.ticker <> ''
  AND ca.contraticker <> ca.ticker
  AND (
        ca.ticker       IN (SELECT DISTINCT symbol FROM equities_data.ohlcv_1d)
     OR ca.contraticker IN (SELECT DISTINCT symbol FROM equities_data.ohlcv_1d)
      )
  AND NOT EXISTS (
        SELECT 1 FROM equities_data.ticker_aliases ta
        WHERE ta.historical_ticker = ca.contraticker
          AND ta.current_symbol    = ca.ticker
      )
ORDER BY ca.contraticker, ca.ticker, ca.date DESC;

COMMIT;
