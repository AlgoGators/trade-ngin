-- 041_book_email_columns.sql
--
-- What the book e-mail shows that the run alone knew. The e-mail is rendered by the Python report
-- module (services/rpc/algogators_rpc/services/report) from the stored day after the run, so the
-- facts the C++ e-mail took from the run's memory are stored on the rows they describe:
--
--   trading.positions.mark_price          double precision NULL
--     A day-T row: the mark the row is valued at (the T-1 close; a withheld print's last
--     consumed close). NULL when the symbol printed no bar: the row is then valued at
--     average_price, its last known price (the e-mail's footnote 4).
--   trading.positions.settle_from_price   double precision NULL
--   trading.positions.settle_to_price     double precision NULL
--     A settled (Day T-1) row: the two prices the settle's own P&L used, from and to
--     (settled_table_prices). settle_to_price NULL: no bar that day.
--   trading.positions.settle_mark         text NULL, CHECK in ('NO_BAR', 'ROLL', 'WITHHELD')
--     Why a settled row booked no move: no bar that day, the contract rolled, the price was
--     rejected (the e-mail's footnotes 1, 2, 3). NULL otherwise.
--   trading.live_results.statistics_sessions_per_year   double precision NULL
--     K, the sessions a year of the statistics grid the row's statistics are taken on. While
--     total_days < K the annualized return is an extrapolation and the e-mail says so.
--
-- WRITERS: the live runners, on the rows they write from the binary that ships with this
-- migration. The binary names the columns only when the database has them, so the migration
-- may be applied before or after that binary. trading.positions rows copied between books
-- (qt_desk copy_book_day) carry the columns with them.
-- NO BACKFILL: a row written before keeps NULL; the e-mail then shows what it showed before.
--
-- SAFETY: ADD COLUMN of NULL-able columns with no default is metadata-only. The CHECK is
-- validated by one scan of trading.positions under the ALTER's lock (every row holds NULL and
-- passes). No existing column, value, key or index is touched. Type-guarded, transactional and
-- idempotent.
BEGIN;
DO $$
DECLARE
    c text; found text; def text;
BEGIN
    IF to_regclass('trading.positions') IS NULL THEN RAISE EXCEPTION '041: trading.positions does not exist'; END IF;
    IF to_regclass('trading.live_results') IS NULL THEN RAISE EXCEPTION '041: trading.live_results does not exist'; END IF;
    FOREACH c IN ARRAY ARRAY['mark_price', 'settle_from_price', 'settle_to_price'] LOOP
        SELECT data_type INTO found FROM information_schema.columns
         WHERE table_schema = 'trading' AND table_name = 'positions' AND column_name = c;
        IF found IS NOT NULL AND found <> 'double precision' THEN
            RAISE EXCEPTION 'trading.positions.% exists with another type (%)', c, found;
        END IF;
    END LOOP;
    SELECT data_type INTO found FROM information_schema.columns
     WHERE table_schema = 'trading' AND table_name = 'positions' AND column_name = 'settle_mark';
    IF found IS NOT NULL AND found <> 'text' THEN
        RAISE EXCEPTION 'trading.positions.settle_mark exists with another type (%)', found;
    END IF;
    SELECT pg_get_constraintdef(oid) INTO def FROM pg_constraint
     WHERE conrelid = 'trading.positions'::regclass AND conname = 'positions_settle_mark_check';
    IF def IS NOT NULL
       AND def <> 'CHECK ((settle_mark = ANY (ARRAY[''NO_BAR''::text, ''ROLL''::text, ''WITHHELD''::text])))' THEN
        RAISE EXCEPTION 'trading.positions_settle_mark_check exists with another definition (%)', def;
    END IF;
    SELECT data_type INTO found FROM information_schema.columns
     WHERE table_schema = 'trading' AND table_name = 'live_results'
       AND column_name = 'statistics_sessions_per_year';
    IF found IS NOT NULL AND found <> 'double precision' THEN
        RAISE EXCEPTION 'trading.live_results.statistics_sessions_per_year exists with another type (%)', found;
    END IF;
END $$;

ALTER TABLE trading.positions ADD COLUMN IF NOT EXISTS mark_price double precision;
ALTER TABLE trading.positions ADD COLUMN IF NOT EXISTS settle_from_price double precision;
ALTER TABLE trading.positions ADD COLUMN IF NOT EXISTS settle_to_price double precision;
ALTER TABLE trading.positions ADD COLUMN IF NOT EXISTS settle_mark text;
DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM pg_constraint
                    WHERE conrelid = 'trading.positions'::regclass
                      AND conname = 'positions_settle_mark_check') THEN
        ALTER TABLE trading.positions ADD CONSTRAINT positions_settle_mark_check
            CHECK (settle_mark IN ('NO_BAR', 'ROLL', 'WITHHELD'));
    END IF;
END $$;
ALTER TABLE trading.live_results ADD COLUMN IF NOT EXISTS statistics_sessions_per_year double precision;

COMMENT ON COLUMN trading.positions.mark_price IS $c041$A day-T row: the mark the book e-mail values the row at (the T-1 close; a withheld print's last consumed close). NULL when the symbol printed no bar that day, the row then valued at average_price (its last known price), and on every row written before migration 041.$c041$;
COMMENT ON COLUMN trading.positions.settle_from_price IS $c041$A settled (Day T-1) row: the price the settle's P&L was taken from (settled_table_prices). NULL on a row not settled by a binary of migration 041 or later.$c041$;
COMMENT ON COLUMN trading.positions.settle_to_price IS $c041$A settled (Day T-1) row: the price the settle's P&L was taken to. NULL when the symbol printed no bar that day (settle_mark NO_BAR) and on a row not settled by a binary of migration 041 or later.$c041$;
COMMENT ON COLUMN trading.positions.settle_mark IS $c041$Why a settled row booked no move: NO_BAR (no bar that day), ROLL (the contract rolled; P&L is not booked on the roll), WITHHELD (the price was rejected; P&L is booked with the next accepted price). NULL otherwise.$c041$;
COMMENT ON COLUMN trading.live_results.statistics_sessions_per_year IS $c041$K: the sessions a year of the statistics grid the row's statistics are taken on (config statistics.sessions_per_year). While total_days < K the annualized return is an extrapolation. NULL on rows written before migration 041.$c041$;
COMMIT;
