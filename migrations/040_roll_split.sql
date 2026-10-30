-- 040_roll_split.sql
--
-- The split of every roll (HD 2026-10-23, ruling 18; ROLLS_STEP1_PLAN section 11c; worked example
-- docs/after_equities/stage3/lead2/analysis/ROLL_WORKED_EXAMPLE_SI_2025-11.md). With portfolio.json's
-- per_contract_rolls on, a roll bar carries both contracts' closes, so the bar splits into three
-- figures, stored on the roll's closing leg:
--
--   trading.executions and backtest.executions
--     roll_bar_move     the held (old) contract's move booked on the roll bar, in dollars:
--                       quantity x (old close on the roll bar - its previous consumed close) x
--                       multiplier, signed as the P&L it is (a long gains when the price rises).
--                       It is P&L and is already inside the day's P&L; this column only names it.
--     roll_gap_points   the new contract's close minus the old contract's close on the roll bar,
--                       in price points (the opening leg's price minus the closing leg's).
--     roll_gap_value    roll_gap_points x quantity x multiplier, signed with the position (a long
--                       pays it, a short receives it). Shown, NEVER P&L: the same exposure held in
--                       a contract that costs that much more (or less).
--
--   backtest.results
--     roll_bar_moves    the run's sum of roll_bar_move over its executions
--     roll_gap_values   the run's sum of roll_gap_value over its executions
--   The third total, the roll costs, is migration 018's roll_costs (both legs of every roll).
--
-- ONE ROLL, ONE SET OF VALUES. A roll is two ROLL rows per sleeve holding the symbol (closing leg,
-- opening leg). The three values sit on the CLOSING leg only (live exec_id ..._RC, backtest RL- with
-- an even number), the row of the contract left, from whose close both the move and the gap are
-- measured; the opening leg holds NULL. Two sleeves holding one symbol each carry their own
-- quantity's values on their own closing leg, and the book's figure is their sum. So a sum over
-- rows counts each roll once. A roll's cost is its two legs' total_transaction_costs (015, 018).
--
-- NULL: every row with the switch off (the binary then names none of the columns), every row
-- that is not a ROLL leg, every opening leg, and a roll with no two closes on one bar (the micros,
-- which stay on the vendor's continuous series until their full-size contracts' bars are loaded:
-- the vendor's switch bar books 0 and the gap is not known). The results totals are NULL with the
-- switch off and 0 for a switch-on run that legged no roll. A CHECK keeps the three together and
-- on ROLL rows: all three NULL, or all three set on a ROLL row.
--
-- WHY IT MOVES NUMBERS
--
-- It moves none. Five NULL-able columns with no default: every existing row reads NULL and no
-- writer changes an existing value. The binary that ships with it writes the columns only with
-- per_contract_rolls on, so a switch-off run's statements and stored cells are the ones before 040.
-- No P&L, cost, position or statistic reads them.
--
-- SAFETY
--
--   * Refuses unless the three tables exist with execution_type on both executions tables (015).
--   * Type-guarded: a column of one of these names with another type makes it refuse.
--   * Value-guarded: on a second apply, a cell that breaks the rule above (a value on a row that is
--     not a ROLL leg, or one or two of the three set) makes it refuse and change nothing.
--   * ADD COLUMN of a NULL-able column with no default is metadata-only. Each CHECK is validated
--     by one scan under the ALTER's lock; every existing row holds NULL and passes. No existing
--     column, value, key or index is touched.
--   * One transaction; a second apply is a no-op.
--
-- ORDER: needs 015 (execution_type), 018 (backtest.results cost columns). Apply it before a binary
-- that runs with per_contract_rolls on. Applied to the stage-3 scratch clones only; HD applies it to
-- production.

BEGIN;
DO $$
DECLARE
    t text;
    n bigint;
BEGIN
    IF to_regclass('trading.executions') IS NULL THEN RAISE EXCEPTION '040: trading.executions does not exist'; END IF;
    IF to_regclass('backtest.executions') IS NULL THEN RAISE EXCEPTION '040: backtest.executions does not exist'; END IF;
    IF to_regclass('backtest.results') IS NULL THEN RAISE EXCEPTION '040: backtest.results does not exist'; END IF;
    IF (SELECT count(*) FROM information_schema.columns WHERE table_schema IN ('trading', 'backtest')
          AND table_name = 'executions' AND column_name = 'execution_type') <> 2 THEN
        RAISE EXCEPTION '040: execution_type is missing on an executions table (migration 015 first)';
    END IF;
    IF (SELECT count(*) FROM information_schema.columns WHERE table_schema = 'backtest'
          AND table_name = 'results' AND column_name = 'roll_costs') <> 1 THEN
        RAISE EXCEPTION '040: backtest.results.roll_costs is missing (migration 018 first)';
    END IF;
    FOR t IN SELECT table_schema || '.' || table_name || '.' || column_name || ' ' || data_type
               FROM information_schema.columns
              WHERE (table_schema = 'trading' AND table_name = 'executions'
                     AND column_name IN ('roll_bar_move', 'roll_gap_points', 'roll_gap_value')
                     AND data_type <> 'numeric')
                 OR (table_schema = 'backtest' AND table_name = 'executions'
                     AND column_name IN ('roll_bar_move', 'roll_gap_points', 'roll_gap_value')
                     AND data_type <> 'double precision')
                 OR (table_schema = 'backtest' AND table_name = 'results'
                     AND column_name IN ('roll_bar_moves', 'roll_gap_values')
                     AND data_type <> 'double precision') LOOP
        RAISE EXCEPTION '040: a roll split column exists with another type: %', t;
    END LOOP;
    -- A second apply: the cells already there must follow the rule.
    IF (SELECT count(*) FROM information_schema.columns WHERE table_schema = 'trading' AND table_name = 'executions'
          AND column_name IN ('roll_bar_move', 'roll_gap_points', 'roll_gap_value')) = 3 THEN
        EXECUTE 'SELECT count(*) FROM trading.executions WHERE NOT ((roll_bar_move IS NULL) = (roll_gap_points IS NULL)
                   AND (roll_gap_points IS NULL) = (roll_gap_value IS NULL)
                   AND (roll_bar_move IS NULL OR execution_type = ''ROLL''))' INTO n;
        IF n > 0 THEN RAISE EXCEPTION '040: % trading.executions row(s) carry a roll split off a ROLL leg or incomplete', n; END IF;
    END IF;
    IF (SELECT count(*) FROM information_schema.columns WHERE table_schema = 'backtest' AND table_name = 'executions'
          AND column_name IN ('roll_bar_move', 'roll_gap_points', 'roll_gap_value')) = 3 THEN
        EXECUTE 'SELECT count(*) FROM backtest.executions WHERE NOT ((roll_bar_move IS NULL) = (roll_gap_points IS NULL)
                   AND (roll_gap_points IS NULL) = (roll_gap_value IS NULL)
                   AND (roll_bar_move IS NULL OR execution_type = ''ROLL''))' INTO n;
        IF n > 0 THEN RAISE EXCEPTION '040: % backtest.executions row(s) carry a roll split off a ROLL leg or incomplete', n; END IF;
    END IF;
END $$;

ALTER TABLE trading.executions ADD COLUMN IF NOT EXISTS roll_bar_move numeric;
ALTER TABLE trading.executions ADD COLUMN IF NOT EXISTS roll_gap_points numeric;
ALTER TABLE trading.executions ADD COLUMN IF NOT EXISTS roll_gap_value numeric;
ALTER TABLE backtest.executions ADD COLUMN IF NOT EXISTS roll_bar_move double precision;
ALTER TABLE backtest.executions ADD COLUMN IF NOT EXISTS roll_gap_points double precision;
ALTER TABLE backtest.executions ADD COLUMN IF NOT EXISTS roll_gap_value double precision;
ALTER TABLE backtest.results ADD COLUMN IF NOT EXISTS roll_bar_moves double precision;
ALTER TABLE backtest.results ADD COLUMN IF NOT EXISTS roll_gap_values double precision;

ALTER TABLE trading.executions DROP CONSTRAINT IF EXISTS chk_executions_roll_split;
ALTER TABLE trading.executions ADD CONSTRAINT chk_executions_roll_split CHECK (
    (roll_bar_move IS NULL) = (roll_gap_points IS NULL) AND (roll_gap_points IS NULL) = (roll_gap_value IS NULL)
    AND (roll_bar_move IS NULL OR execution_type = 'ROLL'));
ALTER TABLE backtest.executions DROP CONSTRAINT IF EXISTS chk_backtest_executions_roll_split;
ALTER TABLE backtest.executions ADD CONSTRAINT chk_backtest_executions_roll_split CHECK (
    (roll_bar_move IS NULL) = (roll_gap_points IS NULL) AND (roll_gap_points IS NULL) = (roll_gap_value IS NULL)
    AND (roll_bar_move IS NULL OR execution_type = 'ROLL'));

COMMENT ON COLUMN trading.executions.roll_bar_move IS
    'Per-contract rolls (per_contract_rolls on), on a roll''s CLOSING leg only: the held (old) contract''s move booked on the roll bar, in dollars, quantity x (old close on the roll bar - its previous consumed close) x multiplier. It is P&L, already in the day''s P&L; this column names it. NULL on the opening leg, on every row that is not a ROLL leg, with the switch off, and on a roll with no two closes on one bar (the micros on the vendor''s series). One roll, one set of values: sum over rows. Migration 040.';
COMMENT ON COLUMN trading.executions.roll_gap_points IS
    'Per-contract rolls, on a roll''s CLOSING leg only: the new contract''s close minus the old contract''s close on the roll bar, in price points (the opening leg''s price minus the closing leg''s). NULL where roll_bar_move is NULL. Migration 040.';
COMMENT ON COLUMN trading.executions.roll_gap_value IS
    'Per-contract rolls, on a roll''s CLOSING leg only: roll_gap_points x quantity x multiplier, signed with the position (a long pays it). Shown, NEVER profit or loss: the same exposure in a contract that costs that much more. NULL where roll_bar_move is NULL. Migration 040.';
COMMENT ON COLUMN backtest.executions.roll_bar_move IS
    'Per-contract rolls (per_contract_rolls on), on a roll''s CLOSING leg only (RL- with an even number): the held (old) contract''s move booked on the roll bar, in dollars, quantity x (old close on the roll bar - its previous consumed close) x multiplier. It is P&L, already in the cycle''s P&L; this column names it. NULL on the opening leg, on every row that is not a ROLL leg, with the switch off, and on a roll with no two closes on one bar (the micros on the vendor''s series). One roll, one set of values: sum over rows. Migration 040.';
COMMENT ON COLUMN backtest.executions.roll_gap_points IS
    'Per-contract rolls, on a roll''s CLOSING leg only: the new contract''s close minus the old contract''s close on the roll bar, in price points (the opening leg''s price minus the closing leg''s). NULL where roll_bar_move is NULL. Migration 040.';
COMMENT ON COLUMN backtest.executions.roll_gap_value IS
    'Per-contract rolls, on a roll''s CLOSING leg only: roll_gap_points x quantity x multiplier, signed with the position (a long pays it). Shown, NEVER profit or loss: the same exposure in a contract that costs that much more. NULL where roll_bar_move is NULL. Migration 040.';
COMMENT ON COLUMN backtest.results.roll_bar_moves IS
    'Per-contract rolls: the sum of the run''s backtest.executions.roll_bar_move (the old contracts'' moves booked on the roll bars, dollars; P&L already in the equity curve). NULL with per_contract_rolls off; 0 for a switch-on run that legged no roll. The roll costs are roll_costs (018). Migration 040.';
COMMENT ON COLUMN backtest.results.roll_gap_values IS
    'Per-contract rolls: the sum of the run''s backtest.executions.roll_gap_value (the price differences between the contracts rolled out of and into, in dollars; never profit or loss). NULL with per_contract_rolls off; 0 for a switch-on run that legged no roll. Migration 040.';
COMMIT;
