BEGIN;

DO $$
BEGIN
    IF to_regclass('trading.investor_books') IS NOT NULL
       AND EXISTS (SELECT 1 FROM trading.investor_books) THEN
        RAISE EXCEPTION 'refusing_to_remove_onboarded_investor_books';
    END IF;
    IF EXISTS (SELECT 1 FROM trading.strategy_trading_days_metadata
                WHERE portfolio_id IS NOT NULL) THEN
        RAISE EXCEPTION 'refusing_to_remove_portfolio_scoped_trading_day_anchors';
    END IF;
END $$;

DROP FUNCTION IF EXISTS trading.onboard_investor_book(text,text,numeric,date,jsonb,text);
DROP TRIGGER IF EXISTS investor_trading_days_anchor_immutable
    ON trading.strategy_trading_days_metadata;
DROP FUNCTION IF EXISTS trading.guard_investor_trading_days_anchor();
DROP TABLE IF EXISTS trading.investor_book_strategies;
DROP TRIGGER IF EXISTS investor_books_immutable ON trading.investor_books;
DROP FUNCTION IF EXISTS trading.guard_investor_book_membership();
DROP FUNCTION IF EXISTS trading.guard_investor_book_immutability();
DROP TABLE IF EXISTS trading.investor_books;
DROP INDEX IF EXISTS trading.uq_trading_days_portfolio_strategy;
DROP INDEX IF EXISTS trading.uq_trading_days_legacy_strategy;
ALTER TABLE trading.strategy_trading_days_metadata
    ADD CONSTRAINT strategy_trading_days_metadata_pkey PRIMARY KEY (strategy_id);

COMMIT;
