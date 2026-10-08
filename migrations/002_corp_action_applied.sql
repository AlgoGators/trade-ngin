-- Durable dedup for corporate actions applied to live positions.
--
--
-- WHY THIS TABLE EXISTS
-- ---------------------
-- The corp-action applier must be idempotent: a daily cron run re-examines a
-- window of past ex-dates, and re-applying a split or a dividend to a position
-- silently corrupts share count and cost basis. Dedup was a JSON file at
-- <state_dir>/applied_corp_actions.json, where state_dir falls back to
-- <cwd>/state/<strategy_id> -- which inside the container is /app/state, and no
-- volume is declared for it. State loss is therefore the DEFAULT on any
-- redeploy, not an edge case.
--
-- That was survivable only because the applier's lookback was a fixed 14 days,
-- which bounded the blast radius while also silently DROPPING every event older
-- than the window (the reason live has been losing dividend value since
-- 2026-05-03). Widening the window to cover real outages is only safe once the
-- dedup record survives the container -- so the wide window and this table land
-- together.
--
--
-- KEY DESIGN
-- ----------
-- The natural key is what makes a re-application impossible:
--
--   (portfolio_id, strategy_id, strategy_name, symbol, action_type, ex_date)
--
-- strategy_name is in the key because two strategies in one portfolio hold
-- independent positions in the same symbol (trading.positions is keyed the same
-- way) and each must be adjusted exactly once. ex_date rather than an applied
-- timestamp: the event is the thing being deduped, not the run.
--
-- Dividend detail (qty_held, dividend_per_share, total_cash) rides along so
-- trading.live_results.total_dividend_income can be recomputed from the DB
-- rather than from a file. It stays INFORMATIONAL -- the equity price series is
-- total-return adjusted, so this value must never be added to P&L.
--
--
-- SAFETY
-- ------
--  * Idempotent: CREATE ... IF NOT EXISTS throughout; re-running is a no-op.
--  * Additive: creates a new table, touches no existing table or row.
--  * Transactional.
--  * Growth at full 852-symbol universe scale: roughly one row per symbol per
--    dividend (~4/yr) plus splits, i.e. order 3-4k rows/yr for a broad equity
--    universe. Trivial; the PK index serves the only access pattern (load all
--    rows for one portfolio+strategy at startup).

BEGIN;

CREATE TABLE IF NOT EXISTS trading.corp_action_applied (
    portfolio_id        TEXT        NOT NULL,
    strategy_id         TEXT        NOT NULL,
    strategy_name       TEXT        NOT NULL,
    symbol              TEXT        NOT NULL,
    action_type         TEXT        NOT NULL,
    ex_date             DATE        NOT NULL,
    applied_at          TIMESTAMPTZ NOT NULL DEFAULT now(),
    qty_held            DOUBLE PRECISION,
    dividend_per_share  DOUBLE PRECISION,
    total_cash          DOUBLE PRECISION,
    CONSTRAINT corp_action_applied_pkey
        PRIMARY KEY (portfolio_id, strategy_id, strategy_name, symbol, action_type, ex_date)
);

COMMENT ON TABLE trading.corp_action_applied IS
    'Idempotency record for corporate actions applied to live positions. One row '
    'per (portfolio, strategy, strategy_name, symbol, action, ex-date). Replaces the '
    'non-durable applied_corp_actions.json state file. Dividend columns are '
    'informational only -- never add total_cash to P&L, the price series is '
    'total-return adjusted.';

COMMIT;
