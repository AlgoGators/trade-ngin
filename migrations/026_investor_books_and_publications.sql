-- Immutable system-model investor-book onboarding and portfolio-scoped anchors.
BEGIN;

ALTER TABLE trading.strategy_trading_days_metadata
    ADD COLUMN IF NOT EXISTS portfolio_id varchar(100);

ALTER TABLE trading.strategy_trading_days_metadata
    DROP CONSTRAINT IF EXISTS strategy_trading_days_metadata_pkey;

CREATE UNIQUE INDEX IF NOT EXISTS uq_trading_days_legacy_strategy
    ON trading.strategy_trading_days_metadata(strategy_id)
    WHERE portfolio_id IS NULL;
CREATE UNIQUE INDEX IF NOT EXISTS uq_trading_days_portfolio_strategy
    ON trading.strategy_trading_days_metadata(strategy_id, portfolio_id)
    WHERE portfolio_id IS NOT NULL;

CREATE TABLE IF NOT EXISTS trading.investor_books (
    book_id uuid PRIMARY KEY DEFAULT gen_random_uuid(),
    config_key varchar(64) NOT NULL UNIQUE,
    portfolio_id varchar(100) NOT NULL UNIQUE,
    initial_capital numeric(30,8) NOT NULL CHECK (initial_capital > 0),
    opening_date date NOT NULL,
    model_stream varchar(16) NOT NULL DEFAULT 'system'
        CHECK (model_stream = 'system'),
    is_active boolean NOT NULL DEFAULT true,
    created_by varchar(200) NOT NULL CHECK (btrim(created_by) <> ''),
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now(),
    CHECK (config_key ~ '^[a-z0-9][a-z0-9_-]{0,63}$'),
    CHECK (portfolio_id ~ '^[A-Za-z0-9][A-Za-z0-9_-]{0,99}$')
);

CREATE TABLE IF NOT EXISTS trading.investor_book_strategies (
    portfolio_id varchar(100) NOT NULL
        REFERENCES trading.investor_books(portfolio_id) ON DELETE RESTRICT,
    strategy_id varchar(100) NOT NULL
        CHECK (strategy_id ~ '^[A-Za-z0-9][A-Za-z0-9_-]{0,99}$'),
    PRIMARY KEY (portfolio_id, strategy_id)
);

CREATE OR REPLACE FUNCTION trading.guard_investor_book_immutability()
RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    IF NEW.book_id IS DISTINCT FROM OLD.book_id
       OR NEW.config_key IS DISTINCT FROM OLD.config_key
       OR NEW.portfolio_id IS DISTINCT FROM OLD.portfolio_id
       OR NEW.initial_capital IS DISTINCT FROM OLD.initial_capital
       OR NEW.opening_date IS DISTINCT FROM OLD.opening_date
       OR NEW.model_stream IS DISTINCT FROM OLD.model_stream
       OR NEW.created_by IS DISTINCT FROM OLD.created_by
       OR NEW.created_at IS DISTINCT FROM OLD.created_at THEN
        RAISE EXCEPTION 'investor_book_identity_is_immutable';
    END IF;
    NEW.updated_at := now();
    RETURN NEW;
END $$;

DROP TRIGGER IF EXISTS investor_books_immutable ON trading.investor_books;
CREATE TRIGGER investor_books_immutable
BEFORE UPDATE ON trading.investor_books
FOR EACH ROW EXECUTE FUNCTION trading.guard_investor_book_immutability();

CREATE OR REPLACE FUNCTION trading.guard_investor_book_membership()
RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    IF current_setting('trade_ngin.investor_onboarding', true) IS DISTINCT FROM 'on' THEN
        RAISE EXCEPTION 'investor_book_strategy_membership_is_immutable';
    END IF;
    RETURN CASE WHEN TG_OP = 'DELETE' THEN OLD ELSE NEW END;
END $$;

DROP TRIGGER IF EXISTS investor_book_strategies_immutable
    ON trading.investor_book_strategies;
CREATE TRIGGER investor_book_strategies_immutable
BEFORE INSERT OR UPDATE OR DELETE ON trading.investor_book_strategies
FOR EACH ROW EXECUTE FUNCTION trading.guard_investor_book_membership();

CREATE OR REPLACE FUNCTION trading.guard_investor_trading_days_anchor()
RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE
    candidate_strategy text := CASE WHEN TG_OP = 'DELETE' THEN OLD.strategy_id ELSE NEW.strategy_id END;
    candidate_portfolio text := CASE WHEN TG_OP = 'DELETE' THEN OLD.portfolio_id ELSE NEW.portfolio_id END;
BEGIN
    IF EXISTS (
        SELECT 1 FROM trading.investor_book_strategies
         WHERE portfolio_id = candidate_portfolio
           AND strategy_id = candidate_strategy
    ) AND current_setting('trade_ngin.investor_onboarding', true) IS DISTINCT FROM 'on' THEN
        RAISE EXCEPTION 'investor_book_trading_days_anchor_is_immutable';
    END IF;
    RETURN CASE WHEN TG_OP = 'DELETE' THEN OLD ELSE NEW END;
END $$;

DROP TRIGGER IF EXISTS investor_trading_days_anchor_immutable
    ON trading.strategy_trading_days_metadata;
CREATE TRIGGER investor_trading_days_anchor_immutable
BEFORE INSERT OR UPDATE OR DELETE ON trading.strategy_trading_days_metadata
FOR EACH ROW EXECUTE FUNCTION trading.guard_investor_trading_days_anchor();

CREATE OR REPLACE FUNCTION trading.onboard_investor_book(
    p_config_key text,
    p_portfolio_id text,
    p_initial_capital numeric,
    p_opening_date date,
    p_strategy_ids jsonb,
    p_created_by text
) RETURNS uuid LANGUAGE plpgsql AS $$
DECLARE
    existing trading.investor_books%ROWTYPE;
    normalized text[];
    stored text[];
    candidate text;
    created_id uuid;
BEGIN
    IF p_config_key IS NULL OR p_config_key !~ '^[a-z0-9][a-z0-9_-]{0,63}$'
       OR p_portfolio_id IS NULL OR p_portfolio_id !~ '^[A-Za-z0-9][A-Za-z0-9_-]{0,99}$'
       OR p_initial_capital IS NULL OR p_initial_capital <= 0
       OR p_opening_date IS NULL
       OR p_created_by IS NULL OR btrim(p_created_by) = '' OR length(p_created_by) > 200
       OR p_strategy_ids IS NULL OR jsonb_typeof(p_strategy_ids) <> 'array'
       OR jsonb_array_length(p_strategy_ids) = 0 THEN
        RAISE EXCEPTION 'investor_book_onboarding_input_invalid';
    END IF;

    SELECT array_agg(value ORDER BY value)
      INTO normalized
      FROM jsonb_array_elements_text(p_strategy_ids) AS value;
    IF array_length(normalized, 1) IS DISTINCT FROM jsonb_array_length(p_strategy_ids)
       OR EXISTS (SELECT 1 FROM unnest(normalized) value
                   WHERE value !~ '^[A-Za-z0-9][A-Za-z0-9_-]{0,99}$')
       OR (SELECT count(DISTINCT value) FROM unnest(normalized) value)
            <> array_length(normalized, 1) THEN
        RAISE EXCEPTION 'investor_book_strategy_ids_invalid';
    END IF;

    PERFORM pg_advisory_xact_lock(hashtextextended(p_portfolio_id, 0));
    SELECT * INTO existing FROM trading.investor_books
     WHERE portfolio_id = p_portfolio_id OR config_key = p_config_key
     FOR UPDATE;

    IF FOUND THEN
        SELECT array_agg(strategy_id ORDER BY strategy_id) INTO stored
          FROM trading.investor_book_strategies
         WHERE portfolio_id = existing.portfolio_id;
        IF existing.config_key IS DISTINCT FROM p_config_key
           OR existing.portfolio_id IS DISTINCT FROM p_portfolio_id
           OR existing.initial_capital IS DISTINCT FROM p_initial_capital
           OR existing.opening_date IS DISTINCT FROM p_opening_date
           OR existing.model_stream IS DISTINCT FROM 'system'
           OR existing.created_by IS DISTINCT FROM p_created_by
           OR stored IS DISTINCT FROM normalized THEN
            RAISE EXCEPTION 'investor_book_onboarding_conflict';
        END IF;
        RETURN existing.book_id;
    END IF;

    PERFORM set_config('trade_ngin.investor_onboarding', 'on', true);
    INSERT INTO trading.investor_books
        (config_key, portfolio_id, initial_capital, opening_date, model_stream, created_by)
    VALUES
        (p_config_key, p_portfolio_id, p_initial_capital, p_opening_date, 'system', p_created_by)
    RETURNING book_id INTO created_id;

    FOREACH candidate IN ARRAY normalized LOOP
        INSERT INTO trading.investor_book_strategies(portfolio_id, strategy_id)
        VALUES (p_portfolio_id, candidate);
        INSERT INTO trading.strategy_trading_days_metadata
            (strategy_id, live_start_date, portfolio_id)
        VALUES (candidate, p_opening_date, p_portfolio_id)
        ON CONFLICT (strategy_id, portfolio_id) WHERE portfolio_id IS NOT NULL
        DO NOTHING;
        IF NOT EXISTS (
            SELECT 1 FROM trading.strategy_trading_days_metadata
             WHERE strategy_id = candidate AND portfolio_id = p_portfolio_id
               AND live_start_date = p_opening_date
        ) THEN
            RAISE EXCEPTION 'investor_book_anchor_conflict';
        END IF;
    END LOOP;
    PERFORM set_config('trade_ngin.investor_onboarding', 'off', true);
    RETURN created_id;
END $$;

COMMIT;
