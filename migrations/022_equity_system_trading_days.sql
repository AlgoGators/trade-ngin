-- Additive equity-only overload. Existing two/three argument callers remain byte-unchanged.
BEGIN;
-- Preserve the legacy global strategy key and NULL-book futures anchor.
-- No inferred book backfill or change to existing overloads.
ALTER TABLE trading.strategy_trading_days_metadata
    ADD COLUMN IF NOT EXISTS portfolio_id character varying(100);
CREATE FUNCTION trading.get_trading_days(
    p_strategy_id character varying,p_target_date date,p_portfolio_id character varying,
    p_portfolio_type character varying) RETURNS integer LANGUAGE plpgsql AS $function$
DECLARE v_start_date DATE; v_date_only boolean; v_timestamp boolean;
BEGIN
    IF p_portfolio_type IS DISTINCT FROM 'system' OR p_strategy_id IS NULL OR p_strategy_id='' OR
       p_portfolio_id IS NULL OR p_portfolio_id='' OR p_target_date IS NULL THEN
        RAISE EXCEPTION 'equity_system_owner_and_date_required';
    END IF;
    SELECT live_start_date INTO v_start_date FROM trading.strategy_trading_days_metadata
        WHERE strategy_id=p_strategy_id AND portfolio_id=p_portfolio_id
        ORDER BY live_start_date LIMIT 1;
    IF v_start_date IS NULL THEN
        SELECT atttypid='date'::regtype,atttypid='timestamptz'::regtype
            INTO v_date_only,v_timestamp FROM pg_attribute
            WHERE attrelid='trading.live_results'::regclass AND attname='date' AND NOT attisdropped;
        IF v_date_only THEN
            SELECT MIN(date) INTO v_start_date FROM trading.live_results
                WHERE strategy_id=p_strategy_id AND portfolio_id=p_portfolio_id AND portfolio_type='system'
                    AND date<=p_target_date;
        ELSIF v_timestamp THEN
            SELECT MIN((date AT TIME ZONE 'UTC')::date) INTO v_start_date FROM trading.live_results
                WHERE strategy_id=p_strategy_id AND portfolio_id=p_portfolio_id AND portfolio_type='system'
                    AND date < ((p_target_date+1)::timestamp AT TIME ZONE 'UTC');
        ELSE
            RAISE EXCEPTION 'equity_system_live_results_date_type_required';
        END IF;
    END IF;
    IF v_start_date IS NULL THEN RETURN 1; END IF;
    RETURN GREATEST(1,(p_target_date-v_start_date)+1);
END;
$function$;
COMMIT;
