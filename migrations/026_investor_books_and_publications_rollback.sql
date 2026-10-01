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
DROP FUNCTION IF EXISTS trading.publish_system_investor_day(text,text,date,text);
DROP FUNCTION IF EXISTS trading.compute_system_investor_digest(text,text,date);
CREATE OR REPLACE FUNCTION trading.fence_runtime_publication_row()
RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE row_value jsonb;
DECLARE stream text;
BEGIN
    row_value := CASE WHEN TG_OP = 'DELETE' THEN to_jsonb(OLD) ELSE to_jsonb(NEW) END;
    stream := row_value->>'portfolio_type';
    IF TG_OP = 'UPDATE' AND
       (to_jsonb(OLD)->'strategy_id',to_jsonb(OLD)->'portfolio_id',to_jsonb(OLD)->'portfolio_type')
       IS DISTINCT FROM
       (to_jsonb(NEW)->'strategy_id',to_jsonb(NEW)->'portfolio_id',to_jsonb(NEW)->'portfolio_type') THEN
        RAISE EXCEPTION 'runtime_scope_move_unsupported';
    END IF;
    IF stream IS NOT NULL AND stream NOT IN
       ('system','qt','benchmark','benchmark_rebench','benchmark_frozen_shadow') THEN
        RAISE EXCEPTION 'runtime_stream_unsupported';
    END IF;
    IF TG_TABLE_NAME IN ('positions','equity_curve') AND
       stream IN ('benchmark','benchmark_rebench','benchmark_frozen_shadow') THEN
        PERFORM pg_advisory_xact_lock(hashtextextended(
            'algolens:qt-book:' || upper(btrim(row_value->>'portfolio_id')),0));
    ELSE
        PERFORM trading.lock_runtime_scope(row_value->>'strategy_id',row_value->>'portfolio_id',
                                          TG_TABLE_NAME = 'positions' AND stream = 'qt');
    END IF;
    IF TG_OP = 'DELETE' THEN RETURN OLD; END IF;
    RETURN NEW;
END $$;
DROP FUNCTION IF EXISTS trading.lock_investor_publication_scope(text,text);
DROP TRIGGER IF EXISTS investor_book_publications_immutable
    ON trading.investor_book_publications;
DROP FUNCTION IF EXISTS trading.refuse_investor_publication_mutation();
DROP TABLE IF EXISTS trading.investor_book_publications;
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
