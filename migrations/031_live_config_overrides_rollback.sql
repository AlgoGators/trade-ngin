-- Refuse to destroy immutable audit/admission history. No CASCADE.
BEGIN;
CREATE OR REPLACE FUNCTION trading.publish_system_investor_day(
    p_portfolio_id text,
    p_strategy_id text,
    p_source_day date,
    p_producer_version text
) RETURNS uuid LANGUAGE plpgsql AS $$
DECLARE
    matched_book_id uuid;
    calculated_digest text;
    existing record;
    created_id uuid;
BEGIN
    IF p_source_day IS NULL OR p_producer_version IS NULL
       OR btrim(p_producer_version) = '' THEN
        RAISE EXCEPTION 'investor_publication_input_invalid';
    END IF;
    matched_book_id := trading.lock_investor_publication_scope(
        p_strategy_id, p_portfolio_id);
    IF p_source_day < (SELECT opening_date FROM trading.investor_books
                       WHERE book_id = matched_book_id) THEN
        RAISE EXCEPTION 'investor_publication_precedes_opening_date';
    END IF;
    calculated_digest := trading.compute_system_investor_digest(
        upper(btrim(p_portfolio_id)), p_strategy_id, p_source_day);

    SELECT * INTO existing FROM trading.investor_book_publications
     WHERE portfolio_id = upper(btrim(p_portfolio_id))
       AND source_day = p_source_day
     FOR UPDATE;
    IF FOUND THEN
        IF existing.strategy_id IS DISTINCT FROM p_strategy_id
           OR existing.model_stream IS DISTINCT FROM 'system'
           OR existing.content_digest IS DISTINCT FROM calculated_digest THEN
            RAISE EXCEPTION 'investor_publication_conflict';
        END IF;
        RETURN existing.publication_id;
    END IF;

    INSERT INTO trading.investor_book_publications
        (book_id, portfolio_id, source_day, strategy_id, model_stream,
         content_digest, producer_id, producer_version)
    VALUES
        (matched_book_id, upper(btrim(p_portfolio_id)), p_source_day,
         p_strategy_id, 'system', calculated_digest, 'trade-ngin',
         p_producer_version)
    RETURNING publication_id INTO created_id;
    RETURN created_id;
END $$;

DROP FUNCTION IF EXISTS trading.publish_system_investor_day(text,text,date,text,uuid);
LOCK TABLE trading.live_config_versions,trading.live_config_activations,trading.live_config_active,
    trading.live_config_attempt_selections,trading.live_config_attempt_safety IN ACCESS EXCLUSIVE MODE;
DO $$ BEGIN
    IF EXISTS(SELECT 1 FROM trading.live_config_versions) OR
       EXISTS(SELECT 1 FROM trading.live_config_activations) OR
       EXISTS(SELECT 1 FROM trading.live_config_active) OR
       EXISTS(SELECT 1 FROM trading.live_config_attempt_selections) OR
       EXISTS(SELECT 1 FROM trading.live_config_attempt_safety) THEN
        RAISE EXCEPTION 'live_config_history_exists';
    END IF;
END $$;
DROP FUNCTION trading.initialize_live_config_attempt_v2(text);
DROP FUNCTION trading.recover_live_config_attempt(text,text);
DROP FUNCTION trading.finish_live_config_attempt(text,text,text);
DROP FUNCTION trading.mark_live_config_unsafe(text);
DROP FUNCTION trading.assert_live_config_running(text);
DROP FUNCTION trading.live_config_financial_state(text,text);
DROP TRIGGER live_config_retry_guard ON trading.live_config_attempt_selections;
DROP FUNCTION trading.check_live_config_retry();
DROP TABLE trading.live_config_attempt_safety;
DROP TABLE trading.live_config_attempt_selections;
DROP TABLE trading.live_config_active;
DROP TABLE trading.live_config_activations;
DROP TABLE trading.live_config_versions;
DROP FUNCTION trading.check_live_config_attempt_selection();
DROP FUNCTION trading.activate_live_config_candidate();
DROP FUNCTION trading.check_live_config_candidate();
DROP FUNCTION trading.lock_live_config_scope(text,text);
DROP FUNCTION trading.refuse_live_config_mutation();
COMMIT;
