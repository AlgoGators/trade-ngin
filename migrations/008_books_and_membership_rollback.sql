-- Rollback for 008_books_and_membership.sql
--
-- Drops the three tables 008 created. strategy_registry.portfolio_id is
-- untouched throughout -- it was never migrated away from, so reverting leaves
-- every strategy in the book it names and the platform reads exactly as it did
-- before 008.
--
-- The audit rows in trading.portfolio_assignments are destroyed by this. That
-- is the one irreversible part: the rules on the table refuse DELETE, but DROP
-- TABLE takes the rules with it. Copy the table out first if the history of who
-- changed which book still matters.
--
-- Idempotent and transactional.

BEGIN;

DROP TABLE IF EXISTS trading.portfolio_assignments;
DROP TABLE IF EXISTS trading.strategy_book_memberships;
DROP TABLE IF EXISTS trading.portfolios;

COMMIT;
