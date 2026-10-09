-- 024_drop_retired_qt_tables.sql
--
-- Ruling 28 (QT master document, 2026-10-08): drop the tables added to production outside merged
-- PRs that the design does not use: trading.risk_limits (#56's published limits; its only reader,
-- the AlgoLens pre-check, is gone), and the preview books registry trading.portfolios,
-- trading.strategy_book_memberships and trading.portfolio_assignments (strategy_registry carries
-- the grouping instead, migration 023). Nothing on main in trade-ngin or AlgoLens reads them, and no
-- foreign key points at them (checked on new_algo_data 2026-10-09: rows 0, 0, 4, 0).
--
-- Irreversible: there is no rollback file. The pre-QT backup on the trade-ngin host,
-- /home/ubuntu/backups/new_algo_data_trading_metadata_backtest_pre_qt_20261009T0225Z.dump,
-- holds their definitions and data (pg_restore -t <table>).

BEGIN;
DROP TABLE IF EXISTS trading.portfolio_assignments;
DROP TABLE IF EXISTS trading.strategy_book_memberships;
DROP TABLE IF EXISTS trading.portfolios;
DROP TABLE IF EXISTS trading.risk_limits;
COMMIT;
