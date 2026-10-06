-- Refuse to destroy immutable audit/admission history. No CASCADE.
BEGIN;
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
