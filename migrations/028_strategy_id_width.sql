-- Widen the three legacy strategy_id columns to the 100-byte client contract.
-- Combined live IDs are deterministic joins of enabled sleeve names and can exceed 50 bytes.
BEGIN;

DO $$
DECLARE
    table_name_value text;
    current_width integer;
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
        ELSIF current_width > 100 THEN
            RAISE EXCEPTION 'refusing to narrow trading.%.strategy_id from varchar(%) to 100',
                            table_name_value, current_width;
        ELSIF current_width < 100 THEN
            EXECUTE format(
                'ALTER TABLE trading.%I ALTER COLUMN strategy_id TYPE varchar(100)',
                table_name_value);
        END IF;
    END LOOP;
END $$;

COMMIT;
