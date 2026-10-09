-- 025_qt_command_log_hardening_rollback.sql
--
-- Reverses 025 exactly: drops the three unique indexes, the insert guard and the truncate block,
-- puts back the 023 body of trading.position_overrides_guard(), and restores the grants as they
-- were on new_algo_data on 2026-10-09 (PUBLIC: every privilege on trading.live_run_metadata and
-- USAGE, SELECT on its sequence; svc_algolens: UPDATE and DELETE on position_overrides; no
-- direct grants for svc_trade_ngin on either table). Note that this re-opens
-- live_run_metadata to every role with USAGE on schema trading. Rows are untouched.

BEGIN;

DROP INDEX IF EXISTS trading.position_overrides_one_decision;
DROP INDEX IF EXISTS trading.position_overrides_one_open_publish;
DROP INDEX IF EXISTS trading.position_overrides_one_open_request;

DROP TRIGGER IF EXISTS position_overrides_insert_guard ON trading.position_overrides;
DROP FUNCTION IF EXISTS trading.position_overrides_insert_guard();
DROP TRIGGER IF EXISTS position_overrides_no_truncate ON trading.position_overrides;
DROP FUNCTION IF EXISTS trading.position_overrides_no_truncate();

CREATE OR REPLACE FUNCTION trading.position_overrides_guard() RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
    IF TG_OP = 'DELETE' THEN
        RAISE EXCEPTION 'trading.position_overrides rows are never deleted';
    END IF;
    -- Only the engine-owned columns (and token_hash being cleared once used) may change.
    IF (NEW.id, NEW.portfolio_id, NEW.date, NEW.kind, NEW.requested_by, NEW.reason, NEW.payload,
        NEW.parent_id, NEW.approver_role, NEW.created_at)
       IS DISTINCT FROM
       (OLD.id, OLD.portfolio_id, OLD.date, OLD.kind, OLD.requested_by, OLD.reason, OLD.payload,
        OLD.parent_id, OLD.approver_role, OLD.created_at) THEN
        RAISE EXCEPTION 'trading.position_overrides: only status, result, message, started_at, finished_at and the token may change';
    END IF;
    RETURN NEW;
END $$;

GRANT ALL ON trading.live_run_metadata TO PUBLIC;
GRANT USAGE, SELECT ON SEQUENCE trading.live_run_metadata_id_seq TO PUBLIC;

DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'svc_algolens') THEN
        GRANT UPDATE, DELETE ON trading.position_overrides TO svc_algolens;
    END IF;
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'svc_trade_ngin') THEN
        REVOKE SELECT, INSERT, UPDATE, DELETE ON trading.live_run_metadata FROM svc_trade_ngin;
        REVOKE USAGE, SELECT ON SEQUENCE trading.live_run_metadata_id_seq FROM svc_trade_ngin;
        REVOKE SELECT, INSERT, UPDATE, DELETE ON trading.position_overrides FROM svc_trade_ngin;
        REVOKE USAGE, SELECT ON SEQUENCE trading.position_overrides_id_seq FROM svc_trade_ngin;
    END IF;
END $$;

COMMENT ON TABLE trading.position_overrides IS
    'QT desk command log (docs/design/qt-contract.md section 4): one row per save, override request, override decision and publish. AlgoLens inserts; the engine moves status, started_at, finished_at, result and message. Never deleted. Migration 023.';

COMMIT;
