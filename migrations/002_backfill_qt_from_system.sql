-- Backfill the qt stream from system for all existing history.
--
-- Companion to 001_add_portfolio_type.sql. Run immediately after it.
--
--
-- WHY THIS IS NEEDED
-- ------------------
-- Migration 001 labels every existing row 'system'. The engine is now changing
-- to read "what did we actually hold yesterday" from the qt stream. On the first
-- run after 001, yesterday has system rows but NO qt rows -- so that read comes
-- back empty and the engine concludes it is the first trading day ever. It would
-- then reset position buffering and compute executions as if opening every
-- position from flat. On a live book that is a real disruption, not a cosmetic
-- one.
--
-- A silent "fall back to system if qt is empty" in the reader would paper over
-- this, but it is the wrong fix: it cannot distinguish "not seeded yet" from
-- "QT deliberately closed everything out", and would silently resurrect
-- positions a human had removed. Backfilling the history instead makes the
-- ambiguity impossible.
--
--
-- WHY THIS IS SEMANTICALLY CORRECT
-- --------------------------------
-- The qt stream means "what we actually held". For all history to date nobody
-- was editing positions -- the engine's output WAS what we held. So qt = system
-- is not an approximation for that period; it is exactly right.
--
--
-- SAFETY
-- ------
-- * Idempotent: WHERE NOT EXISTS means re-running inserts nothing.
-- * Never overwrites: only ever inserts rows that are absent.
-- * Small: roughly doubles trading.positions (about 3.8k rows) and
--   trading.equity_curve (about 270 rows). Both trivial.
-- * Transactional.

BEGIN;

-- trading.positions
INSERT INTO trading.positions
    (symbol, quantity, average_price, daily_unrealized_pnl, daily_realized_pnl,
     last_update, updated_at, strategy_id, strategy_name, date, portfolio_id, portfolio_type)
SELECT
    s.symbol, s.quantity, s.average_price, s.daily_unrealized_pnl, s.daily_realized_pnl,
    s.last_update, s.updated_at, s.strategy_id, s.strategy_name, s.date, s.portfolio_id, 'qt'
FROM trading.positions s
WHERE s.portfolio_type = 'system'
  AND NOT EXISTS (
      SELECT 1 FROM trading.positions q
      WHERE q.portfolio_id  = s.portfolio_id
        AND q.strategy_id   = s.strategy_id
        AND q.strategy_name = s.strategy_name
        AND q.date          = s.date
        AND q.symbol        = s.symbol
        AND q.portfolio_type = 'qt'
  );

-- trading.equity_curve
INSERT INTO trading.equity_curve
    (strategy_id, timestamp, equity, portfolio_id, portfolio_type)
SELECT
    s.strategy_id, s.timestamp, s.equity, s.portfolio_id, 'qt'
FROM trading.equity_curve s
WHERE s.portfolio_type = 'system'
  AND NOT EXISTS (
      SELECT 1 FROM trading.equity_curve q
      WHERE q.portfolio_id IS NOT DISTINCT FROM s.portfolio_id
        AND q.strategy_id  = s.strategy_id
        AND q.timestamp    = s.timestamp
        AND q.portfolio_type = 'qt'
  );

COMMIT;
