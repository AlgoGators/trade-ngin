-- 013_executions_netting_adjustment.sql
--
-- Add trading.executions.netting_adjustment and backtest.executions.netting_adjustment: the
-- part of a sleeve row's own transaction cost the ACCOUNT did not pay (or, negative, paid on
-- top) because two or more sleeves of one portfolio traded the same symbol on the same date.
-- Also restate the definition of trading.live_results.equity_to_margin_ratio in its column
-- comment (STAGE3_PLAN section 23: "migration 013 carries the COMMENT ON COLUMN").
--
-- WHY
--
-- A multi-sleeve book (BASE: TREND_FOLLOWING and TREND_FOLLOWING_FAST) stores one execution
-- row per sleeve, each priced as if that sleeve traded alone. The account sends ONE order per
-- symbol and date: the signed sum of the sleeve quantities. Measured on the session clone
-- (T-7b-2 analysis/8b/DESIGN_8b.md), 6C.v.0 on 2026-04-24 stores TF BUY 1 and FAST SELL 1 at
-- $3.94919180 each while the account's 6C book stays at 2 contracts and no order is sent;
-- on 2026-04-29 both sleeves SELL 1 at $3.97287664 each while the account sells 2, which costs
-- C(2) = $8.95881745. HD 2026-09-25 (item 23): both sleeve rows are kept with their own cost,
-- so no position attribution is lost, and the account's netting is stated in its own column.
--
-- For one portfolio, date (backtest: bar) and symbol with n >= 2 sleeve rows:
--   Q            = sum of the signed sleeve quantities (the account's order)
--   C(q)         = the cost model on the same state and reference price; C(0) = 0
--   credit_total = sum C(q_i) - C(Q)
--   netting_adjustment_i = credit_total split pro rata to C(q_i), in 1e-8 units, largest
--                  remainder (a tie to the smaller strategy name), so the adjustments sum to
--                  credit_total exactly
--   net_cost_i   = total_transaction_costs_i - netting_adjustment_i   (derived, not stored)
-- The net costs of a symbol-date sum to C(Q). Positive: the sleeves crossed; negative: one
-- account order costs more than the sleeve orders priced alone (impact grows as |q|^1.5).
-- One sleeve row: 0. The P&L chain (live_results cost columns, the equity curve) keeps
-- charging total_transaction_costs (HD: "the book's P&L gross").
--
-- WHAT IT DOES NOT DO
--
-- No stored value changes. The ADD carries no default, so every row written before this
-- migration reads NULL ("not computed"; HD 2026-09-25 item 23); the default of 0 is set
-- AFTER the add, so it applies only to rows inserted later by a writer that does not name the
-- column. A binary built after this migration names the column in every executions INSERT
-- and fails on a database without it: apply this migration BEFORE deploying such a binary.
--
-- SAFETY
--
--   * ADD COLUMN without a default is metadata-only: no rewrite, no row touched.
--   * ALTER COLUMN ... SET DEFAULT is metadata-only and affects future inserts only.
--   * No key, index, constraint or view is touched; no foreign key references the column.
--   * Type-guarded: an existing netting_adjustment of another type makes this refuse.
--   * Transactional and idempotent (ADD COLUMN IF NOT EXISTS; SET DEFAULT and COMMENT repeat).
--
-- ORDER AND NUMBERING
--
-- Independent of 001-012. 013 is the number the plan assigned (STAGE3_PLAN sections 20 and
-- 23). origin/codex/qt-effective-position-reporting (unmerged) carries its own 012_ and 013_
-- files; HD 2026-09-25 item 23: that branch renumbers before it merges. At the merge the
-- executions column contract that 011 declares (origin/qt-platform-preview) gains this column.
--
-- FUTURES AND EQUITIES REACHABILITY
--
-- Every live runner writes trading.executions through PostgresDatabase::store_executions and
-- every backtest writes backtest.executions; all of them write 0 on a symbol-date with one
-- sleeve row, which is every row of CONSERVATIVE and of the equity books today. Only a book
-- with two or more sleeves on one symbol-date writes a non-zero value.

BEGIN;

DO $$
DECLARE
    s   text;
    t   text;
    exp text;
BEGIN
    FOREACH s IN ARRAY ARRAY['trading', 'backtest'] LOOP
        IF to_regclass(s || '.executions') IS NULL THEN
            RAISE EXCEPTION '%.executions does not exist; this migration adds a column to it, it '
                            'does not create the table', s;
        END IF;
        exp := CASE s WHEN 'trading' THEN 'numeric' ELSE 'double precision' END;
        SELECT data_type INTO t
          FROM information_schema.columns
         WHERE table_schema = s AND table_name = 'executions' AND column_name = 'netting_adjustment';
        IF t IS NOT NULL AND t <> exp THEN
            RAISE EXCEPTION '%.executions.netting_adjustment already exists as %, not %: refusing',
                            s, t, exp;
        END IF;
    END LOOP;
    IF NOT EXISTS (SELECT 1 FROM information_schema.columns
                    WHERE table_schema = 'trading' AND table_name = 'live_results'
                      AND column_name = 'equity_to_margin_ratio') THEN
        RAISE EXCEPTION 'trading.live_results.equity_to_margin_ratio does not exist';
    END IF;
END $$;

-- No default in the ADD: rows written before 013 stay NULL. Metadata-only.
ALTER TABLE trading.executions  ADD COLUMN IF NOT EXISTS netting_adjustment NUMERIC;
ALTER TABLE backtest.executions ADD COLUMN IF NOT EXISTS netting_adjustment DOUBLE PRECISION;

-- Rows inserted after 013 by a writer that does not name the column read 0.
ALTER TABLE trading.executions  ALTER COLUMN netting_adjustment SET DEFAULT 0;
ALTER TABLE backtest.executions ALTER COLUMN netting_adjustment SET DEFAULT 0.0;

COMMENT ON COLUMN trading.executions.netting_adjustment IS
    'Dollars. The part of this sleeve row''s own cost (total_transaction_costs, the sleeve''s '
    'as-if-alone cost C(q_i)) that the account did not pay because two or more sleeves of the '
    'portfolio traded the symbol on the same date. For the symbol-date: Q = signed sum of the '
    'sleeve quantities (the account''s order), credit_total = sum C(q_i) - C(Q) with C(0) = 0, '
    'split pro rata to C(q_i) in 1e-8 units. Positive: sleeves crossed; negative: one account '
    'order costs more than the sleeve orders priced alone. net_cost = total_transaction_costs - '
    'netting_adjustment; the net costs of a symbol-date sum to C(Q). A full cross (Q = 0) is two '
    'rows and no broker fill. 0 with one sleeve row; NULL on rows written before migration 013. '
    'P&L columns (live_results, equity_curve) charge total_transaction_costs.';

COMMENT ON COLUMN backtest.executions.netting_adjustment IS
    'As trading.executions.netting_adjustment, per run, bar and symbol (migration 013).';

COMMENT ON COLUMN trading.live_results.equity_to_margin_ratio IS
    'Current portfolio value divided by total posted margin, computed at the book level: one '
    'portfolio value over the sum of every position''s posted margin, so a mixed '
    'futures-plus-equity book sums both margin kinds under one portfolio value (HD 2026-09-10). '
    'The futures runners compute this; the equity runner writes gross notional over posted '
    'margin until T-8 moves it to this definition (migration 013).';

COMMIT;

-- ---------------------------------------------------------------------------
-- VERIFICATION -- run before and after.
--
--   -- 1. the column (after: numeric / double precision, nullable, default 0)
--   SELECT table_schema, data_type, is_nullable, column_default
--     FROM information_schema.columns
--    WHERE table_name = 'executions' AND column_name = 'netting_adjustment' ORDER BY 1;
--
--   -- 2. every pre-existing value unchanged (the hash of each row without the new key)
--   SELECT 'trading' s, count(*), md5(string_agg((to_jsonb(t) - 'netting_adjustment')::text, ';'
--          ORDER BY (to_jsonb(t) - 'netting_adjustment')::text)) FROM trading.executions t
--   UNION ALL
--   SELECT 'backtest', count(*), md5(string_agg((to_jsonb(t) - 'netting_adjustment')::text, ';'
--          ORDER BY (to_jsonb(t) - 'netting_adjustment')::text)) FROM backtest.executions t;
--   -- after: identical to before
--
--   -- 3. history is NULL, not 0 (after: equals the row count before)
--   SELECT count(*) FROM trading.executions WHERE netting_adjustment IS NULL;
-- ---------------------------------------------------------------------------
