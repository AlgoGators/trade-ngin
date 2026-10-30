-- 040_book_email_columns_rollback.sql
--
-- Drops the columns of migration 040. DESTRUCTIVE: the stored marks, settle prices and K are
-- lost; the rows and every other column are kept. The book e-mail then shows the settled table
-- without its notes and no "extrapolated" label. Stop or roll back the binary of 040 first only
-- if it must keep writing them: the binary names the columns only when the database has them.
BEGIN;
ALTER TABLE trading.positions DROP CONSTRAINT IF EXISTS positions_settle_mark_check;
ALTER TABLE trading.positions DROP COLUMN IF EXISTS mark_price;
ALTER TABLE trading.positions DROP COLUMN IF EXISTS settle_from_price;
ALTER TABLE trading.positions DROP COLUMN IF EXISTS settle_to_price;
ALTER TABLE trading.positions DROP COLUMN IF EXISTS settle_mark;
ALTER TABLE trading.live_results DROP COLUMN IF EXISTS statistics_sessions_per_year;
COMMIT;
