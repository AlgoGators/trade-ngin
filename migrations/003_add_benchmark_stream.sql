-- Add the 'benchmark' stream: the untouched counterfactual portfolio.
--
-- Companion to 001 and 002. Run after both.
--
--
-- WHY A THIRD STREAM
-- ------------------
-- The two existing streams answer different questions, and neither answers the
-- cumulative one:
--
--   qt        what we actually held. QT edits this; it is the real book.
--   system    what the algorithm said TODAY, given the real book. Comparing it
--             against qt scores each individual decision.
--   benchmark what the algorithm would have compounded to on its own, never
--             seeing a single QT edit. Comparing it against qt scores the
--             cumulative contribution.
--
-- The distinction matters because position buffering makes today's target depend
-- on yesterday's position. 'system' is therefore computed from the REAL book and
-- drifts along with QT's decisions -- useful for "was yesterday's tweak sensible"
-- but useless for "has QT added value over the year", because the comparison is
-- never independent of the thing being measured.
--
-- 'benchmark' chains only off its own prior positions, so it stays a genuine
-- counterfactual.
--
--
-- SAFETY
-- ------
-- * Widens a CHECK constraint only -- no existing row can become invalid.
-- * No data is written, moved or deleted.
-- * Idempotent and transactional.

BEGIN;

ALTER TABLE trading.positions
    DROP CONSTRAINT IF EXISTS positions_portfolio_type_check;
ALTER TABLE trading.positions
    ADD CONSTRAINT positions_portfolio_type_check
    CHECK (portfolio_type IN ('system', 'qt', 'benchmark'));

ALTER TABLE trading.equity_curve
    DROP CONSTRAINT IF EXISTS equity_curve_portfolio_type_check;
ALTER TABLE trading.equity_curve
    ADD CONSTRAINT equity_curve_portfolio_type_check
    CHECK (portfolio_type IN ('system', 'qt', 'benchmark'));

COMMIT;
