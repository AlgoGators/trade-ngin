-- Safe rollback: refuse to narrow while any stored identity needs the widened contract.
BEGIN;

DO $$
DECLARE
    table_name_value text;
    current_width integer;
    oversized_count bigint;
BEGIN
    FOREACH table_name_value IN ARRAY ARRAY['positions', 'live_results', 'signals'] LOOP
        SELECT character_maximum_length INTO current_width
          FROM information_schema.columns
         WHERE table_schema = 'trading'
           AND table_name = table_name_value
           AND column_name = 'strategy_id';

        IF current_width IS NULL THEN
            RAISE EXCEPTION 'trading.%.strategy_id is missing or is not varchar',
                            table_name_value;
        ELSIF current_width <= 50 THEN
            CONTINUE;
        END IF;

        EXECUTE format(
            'SELECT count(*) FROM trading.%I WHERE length(strategy_id) > 50',
            table_name_value) INTO oversized_count;
        IF oversized_count > 0 THEN
            RAISE EXCEPTION
                'refusing to narrow trading.%.strategy_id: % row(s) exceed 50 bytes',
                table_name_value, oversized_count;
        END IF;

        EXECUTE format(
            'ALTER TABLE trading.%I ALTER COLUMN strategy_id TYPE varchar(50)',
            table_name_value);
    END LOOP;
END $$;

COMMIT;
