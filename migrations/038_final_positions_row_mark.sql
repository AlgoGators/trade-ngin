-- 038_final_positions_row_mark.sql
-- Numbered 032 until 2026-10-21; renumbered 038 when main-nonQT joined main (main has its own 028 and 029).
-- The migration was applied to new_algo_data as 032 on 2026-10-12: do not re-apply it there. The text below is
-- exactly as applied, so its comments and stored COMMENT texts still call it 032.
--
-- row_mark on backtest.final_positions (T-8D-2 R50, admitted into T-8b by HD; LOOP_SPEC v6.2
-- section 7.5 and COLUMN_PLAN section 2, where this migration is numbered 026; T-8b commit (5)).
--
--   backtest.final_positions.row_mark   text NULL, CHECK (row_mark = 'NO_BAR')
--     'NO_BAR'  "no bar: held, marked at last close": the row of a futures symbol that printed no
--               bar on the row's date. Its realized_pnl is 0, its quantity and average_price kept.
--     NULL      every other row.
-- NO BACKFILL: a row written before this migration keeps NULL. The binary this migration ships
-- with names the column in every INSERT of the table, so 032 is applied before that binary runs.
--
-- COMMENTS RESTATED (a COMMENT replaces the whole text; the rollback restores the earlier text
-- byte for byte). Each keeps its text and changes ONE clause:
--   * the ratios that have no value without a denominator (T-8D R12, T-8b commit (3)): they are
--     stored NULL, where migration 028 and 031 described a number (999.99, 999 or 0):
--     trading.live_results.profit_factor, backtest.results.profit_factor (028's texts);
--     backtest.results.sharpe_ratio, sortino_ratio, calmar_ratio (031's texts).
--   * the TABLE comment of backtest.signals: no writer; reserved (T-8D-2 R51, T-8b commit (4):
--     the writer paths were deleted, the table stays).
--
-- SAFETY: ADD COLUMN of a NULL-able column with no default is metadata-only. The CHECK is added
-- once and validated by one scan of the table under the ALTER's lock: every existing row holds
-- NULL in the new column and passes. No existing column, value, key or index is touched;
-- type-guarded; transactional and idempotent.

BEGIN;
DO $$
DECLARE
    o text; p text[]; found text; def text;
BEGIN
    IF to_regclass('backtest.final_positions') IS NULL THEN RAISE EXCEPTION '032: backtest.final_positions does not exist'; END IF;
    IF to_regclass('backtest.signals') IS NULL THEN RAISE EXCEPTION '032: backtest.signals does not exist'; END IF;
    FOREACH o IN ARRAY ARRAY['trading.live_results.profit_factor', 'backtest.results.profit_factor',
            'backtest.results.sharpe_ratio', 'backtest.results.sortino_ratio', 'backtest.results.calmar_ratio',
            'backtest.final_positions.realized_pnl', 'backtest.final_positions.quantity',
            'backtest.final_positions.average_price', 'backtest.final_positions.instrument_id'] LOOP
        p := string_to_array(o, '.');
        IF to_regclass(p[1] || '.' || p[2]) IS NULL THEN RAISE EXCEPTION '032: % does not exist', p[1] || '.' || p[2]; END IF;
        IF NOT EXISTS (SELECT 1 FROM information_schema.columns
                WHERE table_schema = p[1] AND table_name = p[2] AND column_name = p[3]) THEN
            RAISE EXCEPTION '032: column % does not exist', o;
        END IF;
    END LOOP;
    SELECT data_type INTO found FROM information_schema.columns
     WHERE table_schema = 'backtest' AND table_name = 'final_positions' AND column_name = 'row_mark';
    IF found IS NOT NULL AND found <> 'text' THEN
        RAISE EXCEPTION 'backtest.final_positions.row_mark exists with another type (%)', found;
    END IF;
    SELECT pg_get_constraintdef(oid) INTO def FROM pg_constraint
     WHERE conrelid = 'backtest.final_positions'::regclass AND conname = 'final_positions_row_mark_check';
    IF def IS NOT NULL AND def <> 'CHECK ((row_mark = ''NO_BAR''::text))' THEN
        RAISE EXCEPTION 'backtest.final_positions_row_mark_check exists with another definition (%)', def;
    END IF;
END $$;
ALTER TABLE backtest.final_positions ADD COLUMN IF NOT EXISTS row_mark text;
DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM pg_constraint
            WHERE conrelid = 'backtest.final_positions'::regclass AND conname = 'final_positions_row_mark_check') THEN
        ALTER TABLE backtest.final_positions
            ADD CONSTRAINT final_positions_row_mark_check CHECK (row_mark = 'NO_BAR');
    END IF;
END $$;
COMMENT ON COLUMN backtest.final_positions.row_mark IS $c032$'NO_BAR' or NULL; the CHECK admits no other value. 'NO_BAR' means: no bar, held, marked at last close. It is on a FUTURES row whose symbol printed no bar on the row's date (the run's bars hold no row of the symbol that date: a Saturday point, a Sunday or a holiday the symbol has no session on, a bar the feed lacks). The position is held on the date and marked at its last close: quantity and average_price are written as on any other row (the mark changes neither); realized_pnl is 0, because the row books nothing and the equity curve books nothing for the symbol that date (its next bar books against its last close before the gap); instrument_id is the contract held after the symbol's last consumed bar. NULL on every other row: a row whose symbol printed a bar that date (realized_pnl can be 0 there too and the row is NOT marked: a bar with no earlier close to book against, a bar the cycle withheld, a change bar of a roll); every row of an equity (cash) book, which is never marked; and every row written before migration 032 (no backfill), where a no-bar row is not marked. T-8D-2 R50; LOOP_SPEC v6.2 section 7.5 (numbered 026 there); migration 032.$c032$;
COMMENT ON COLUMN trading.live_results."profit_factor" IS $c032$gross_profit / gross_loss. NULL when gross_loss is 0 (no losing session on the grid; an empty series included): the ratio has no denominator, so it has no value, and NULL is not a measured 0. Rows written before migration 032 hold a number there instead: 999.99 with a profit and no loss, 0 with neither. The grid, K, the base and the settled-row rule are in the comment on the table. Rows written before migration 028 keep the earlier definition.$c032$;
COMMENT ON COLUMN backtest.results."profit_factor" IS $c032$The summed P&L of the winning trades over the summed loss of the others. NULL when that loss is 0 (no losing trade; a run with no trade included): the ratio has no denominator, so it has no value, and NULL is not a measured 0. Rows written before migration 032 hold a number there instead: 999 with a profit and no loss, 0 with neither. A trade and its P&L in dollars are defined on total_trades. Rows written before migration 028 keep the earlier definition.$c032$;
COMMENT ON COLUMN backtest.results."sharpe_ratio" IS $c032$The mean grid return x K over volatility, risk-free rate 0. NULL when volatility is 0 or the series has no return: the ratio has no denominator, so it has no value, and NULL is not a measured 0. Rows written before migration 032 hold 0 there instead. Taken on the run's statistics grid with its sessions a year K (see the comment on the table). Rows written before migration 031 keep the earlier definition.$c032$;
COMMENT ON COLUMN backtest.results."sortino_ratio" IS $c032$The mean grid return x K over downside_volatility. NULL when downside_volatility is 0 (no grid return below 0) or the series has no return: the ratio has no denominator, so it has no value, and NULL is not a measured 0. Rows written before migration 032 hold a number there instead: 999 when the mean is not negative, 0 when it is. Taken on the run's statistics grid with its sessions a year K (see the comment on the table). Rows written before migration 031 keep the earlier definition.$c032$;
COMMENT ON COLUMN backtest.results."calmar_ratio" IS $c032$The annualised return on the grid, the CAGR (1 + R)^(K / n) - 1 with R the return from the base to the last grid level and n the count of grid returns, over max_drawdown. NULL when max_drawdown is 0: the ratio has no denominator, so it has no value, and NULL is not a measured 0. Rows written before migration 032 hold a number there instead: 999 when the CAGR is not negative, 0 when it is. Taken on the run's statistics grid with its sessions a year K (see the comment on the table). Rows written before migration 031 keep the earlier definition.$c032$;
COMMENT ON TABLE backtest.signals IS $c032$No writer; reserved. No engine path writes this table: a backtest publishes no signals (the per-day forecasts are not stored), and the rows a database may still hold predate the removal of the writer and are not a reconciliation source. The table is kept so that a writer can be added without a schema change; whoever adds one restates this comment and the columns' meaning.$c032$;
COMMIT;
