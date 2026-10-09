-- 025_qt_command_log_hardening.sql
--
-- QT hardening (2026-10-09; docs/design/qt-contract.md C2 and C4, and the grants review). Applied
-- after 023 (and 024). Transactional; refuses when existing rows would break the new indexes.
--
--   1. One decision per request, one open publish and one open override request per day (C2):
--      unique partial indexes on trading.position_overrides.
--        - override_decision: one per parent_id while pending, running or done (a refused or
--          failed decision does not count, so the request can still be decided).
--        - publish: one per (portfolio_id, date) while pending or running (023 already allows
--          one done publish per day).
--        - override_request: one per (portfolio_id, date) while pending or running. "Done, not
--          yet decided and not expired" depends on other rows and on now(), which no index can
--          express: AlgoLens enforces that part under its lock.
--   2. The command row life cycle (C4), by trigger:
--        - an INSERT must be 'pending' with started_at, finished_at, result, message, token_hash
--          and token_expires_at NULL;
--        - status moves pending -> running, running -> done | refused | failed, and
--          running -> pending only inside a transaction that set algogators.recovery = 'on'
--          (the desk service's recovery of a stale or orphaned row, command_store.requeue);
--        - a terminal row (done, refused, failed) is final: any UPDATE of it is refused;
--        - TRUNCATE is refused (statement trigger), as DELETE already is (023).
--      The 023 guard function is replaced in place (same trigger), keeping its column rule.
--   3. Grants.
--        - trading.live_run_metadata and its id sequence: PUBLIC had every privilege
--          (arwdDxt on the table, rU on the sequence). The roles that use the table keep
--          explicit grants (granted here first, idempotently, where the role exists); then
--          PUBLIC loses everything. Checked on new_algo_data 2026-10-09: fund_member,
--          quant_dev, quant_research_ro, leadership_ro and investor_relations_ro keep SELECT;
--          quant_dev_rw, quant_trading_rw (svc_trade_ngin, qt_engine_app), leadership_rw and
--          svc_algolens (qt_algolens_app) keep SELECT, INSERT, UPDATE, DELETE and the sequence.
--          svc_airflow and svc_data_ngin have no USAGE on schema trading, so PUBLIC gave them
--          nothing. postgres (owner, superuser) is unaffected.
--        - trading.position_overrides: svc_algolens loses UPDATE, DELETE and TRUNCATE; it keeps
--          SELECT, INSERT and USAGE on the id sequence (granted explicitly). AlgoLens never
--          updates a command row and, since AlgoLens #116, takes no FOR UPDATE lock on one.
--          svc_trade_ngin (the engine and the desk service) gets SELECT, INSERT, UPDATE and
--          DELETE explicitly (it already has them through quant_trading_rw; DELETE stays
--          refused by the 023 trigger).
--      Compatible with AlgoLens' emulation (algolens-api/tests/fixtures/sql/025_emulated.sql):
--      same index names and predicates, same insert rule, same transitions (the recovery move
--      additionally needs algogators.recovery = 'on'), same truncate block.
--
-- Not done here, on purpose: SET NOT NULL on position_overrides or live_run_metadata columns
-- that hold NULLs today (the spec leaves them; see migrations/README.md).

BEGIN;

DO $$
DECLARE
    dup text;
BEGIN
    IF to_regclass('trading.position_overrides') IS NULL THEN
        RAISE EXCEPTION 'trading.position_overrides does not exist (apply 023 first)';
    END IF;
    IF to_regprocedure('trading.position_overrides_guard()') IS NULL THEN
        RAISE EXCEPTION 'trading.position_overrides_guard() does not exist (apply 023 first)';
    END IF;
    IF to_regclass('trading.live_run_metadata') IS NULL THEN
        RAISE EXCEPTION 'trading.live_run_metadata does not exist';
    END IF;

    SELECT string_agg(parent_id::text, ', ') INTO dup FROM (
        SELECT parent_id FROM trading.position_overrides
        WHERE kind = 'override_decision' AND status IN ('pending', 'running', 'done')
        GROUP BY parent_id HAVING count(*) > 1) d;
    IF dup IS NOT NULL THEN
        RAISE EXCEPTION 'override requests with more than one live or done decision: %; resolve them first', dup;
    END IF;
    SELECT string_agg(portfolio_id || ' ' || date, ', ') INTO dup FROM (
        SELECT portfolio_id, date FROM trading.position_overrides
        WHERE kind = 'publish' AND status IN ('pending', 'running')
        GROUP BY portfolio_id, date HAVING count(*) > 1) d;
    IF dup IS NOT NULL THEN
        RAISE EXCEPTION 'days with more than one open publish: %; resolve them first', dup;
    END IF;
    SELECT string_agg(portfolio_id || ' ' || date, ', ') INTO dup FROM (
        SELECT portfolio_id, date FROM trading.position_overrides
        WHERE kind = 'override_request' AND status IN ('pending', 'running')
        GROUP BY portfolio_id, date HAVING count(*) > 1) d;
    IF dup IS NOT NULL THEN
        RAISE EXCEPTION 'days with more than one open override request: %; resolve them first', dup;
    END IF;
END $$;

-- 1. C2 indexes.
CREATE UNIQUE INDEX IF NOT EXISTS position_overrides_one_decision
    ON trading.position_overrides (parent_id)
    WHERE kind = 'override_decision' AND status IN ('pending', 'running', 'done');
CREATE UNIQUE INDEX IF NOT EXISTS position_overrides_one_open_publish
    ON trading.position_overrides (portfolio_id, date)
    WHERE kind = 'publish' AND status IN ('pending', 'running');
CREATE UNIQUE INDEX IF NOT EXISTS position_overrides_one_open_request
    ON trading.position_overrides (portfolio_id, date)
    WHERE kind = 'override_request' AND status IN ('pending', 'running');

-- 2. C4 life cycle.
CREATE OR REPLACE FUNCTION trading.position_overrides_insert_guard() RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
    IF NEW.status IS DISTINCT FROM 'pending'
       OR NEW.started_at IS NOT NULL OR NEW.finished_at IS NOT NULL
       OR NEW.result IS NOT NULL OR NEW.message IS NOT NULL
       OR NEW.token_hash IS NOT NULL OR NEW.token_expires_at IS NOT NULL THEN
        RAISE EXCEPTION 'trading.position_overrides: a new row must be pending, with started_at, finished_at, result, message and the token NULL';
    END IF;
    RETURN NEW;
END $$;

DROP TRIGGER IF EXISTS position_overrides_insert_guard ON trading.position_overrides;
CREATE TRIGGER position_overrides_insert_guard
    BEFORE INSERT ON trading.position_overrides
    FOR EACH ROW EXECUTE FUNCTION trading.position_overrides_insert_guard();

CREATE OR REPLACE FUNCTION trading.position_overrides_guard() RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
    IF TG_OP = 'DELETE' THEN
        RAISE EXCEPTION 'trading.position_overrides rows are never deleted';
    END IF;
    -- 023: only the engine-owned columns (and token_hash being cleared once used) may change.
    IF (NEW.id, NEW.portfolio_id, NEW.date, NEW.kind, NEW.requested_by, NEW.reason, NEW.payload,
        NEW.parent_id, NEW.approver_role, NEW.created_at)
       IS DISTINCT FROM
       (OLD.id, OLD.portfolio_id, OLD.date, OLD.kind, OLD.requested_by, OLD.reason, OLD.payload,
        OLD.parent_id, OLD.approver_role, OLD.created_at) THEN
        RAISE EXCEPTION 'trading.position_overrides: only status, result, message, started_at, finished_at and the token may change';
    END IF;
    -- 025 (C4): terminal rows are final; status moves only forward, or back to pending by recovery.
    IF OLD.status IN ('done', 'refused', 'failed') THEN
        RAISE EXCEPTION 'trading.position_overrides row % is % and final; insert a new row to retry', OLD.id, OLD.status;
    END IF;
    IF NEW.status IS DISTINCT FROM OLD.status THEN
        IF OLD.status = 'pending' AND NEW.status = 'running' THEN
            NULL;
        ELSIF OLD.status = 'running' AND NEW.status IN ('done', 'refused', 'failed') THEN
            NULL;
        ELSIF OLD.status = 'running' AND NEW.status = 'pending'
              AND current_setting('algogators.recovery', true) = 'on' THEN
            NULL;
        ELSE
            RAISE EXCEPTION 'trading.position_overrides row %: % -> % is not an allowed status change', OLD.id, OLD.status, NEW.status;
        END IF;
    END IF;
    RETURN NEW;
END $$;

CREATE OR REPLACE FUNCTION trading.position_overrides_no_truncate() RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
    RAISE EXCEPTION 'trading.position_overrides is never truncated';
END $$;

DROP TRIGGER IF EXISTS position_overrides_no_truncate ON trading.position_overrides;
CREATE TRIGGER position_overrides_no_truncate
    BEFORE TRUNCATE ON trading.position_overrides
    FOR EACH STATEMENT EXECUTE FUNCTION trading.position_overrides_no_truncate();

-- 3. Grants. Explicit grants first, for every role that exists, then PUBLIC goes.
DO $$
DECLARE
    r text;
BEGIN
    FOREACH r IN ARRAY ARRAY['fund_member', 'quant_dev', 'quant_research_ro', 'leadership_ro',
                             'investor_relations_ro'] LOOP
        IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = r) THEN
            EXECUTE format('GRANT SELECT ON trading.live_run_metadata TO %I', r);
        END IF;
    END LOOP;
    FOREACH r IN ARRAY ARRAY['quant_dev_rw', 'quant_trading_rw', 'leadership_rw', 'svc_algolens',
                             'svc_trade_ngin'] LOOP
        IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = r) THEN
            EXECUTE format('GRANT SELECT, INSERT, UPDATE, DELETE ON trading.live_run_metadata TO %I', r);
            EXECUTE format('GRANT USAGE, SELECT ON SEQUENCE trading.live_run_metadata_id_seq TO %I', r);
        END IF;
    END LOOP;
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'svc_algolens') THEN
        GRANT SELECT, INSERT ON trading.position_overrides TO svc_algolens;
        GRANT USAGE, SELECT ON SEQUENCE trading.position_overrides_id_seq TO svc_algolens;
        REVOKE UPDATE, DELETE, TRUNCATE ON trading.position_overrides FROM svc_algolens;
    END IF;
    IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'svc_trade_ngin') THEN
        GRANT SELECT, INSERT, UPDATE, DELETE ON trading.position_overrides TO svc_trade_ngin;
        GRANT USAGE, SELECT ON SEQUENCE trading.position_overrides_id_seq TO svc_trade_ngin;
    END IF;
END $$;

REVOKE ALL ON trading.live_run_metadata FROM PUBLIC;
REVOKE ALL ON SEQUENCE trading.live_run_metadata_id_seq FROM PUBLIC;

COMMENT ON TABLE trading.position_overrides IS
    'QT desk command log (docs/design/qt-contract.md section 4): one row per save, override request, override decision and publish. AlgoLens inserts pending rows; the engine moves status, started_at, finished_at, result and message (pending -> running -> done/refused/failed; running -> pending only by the agent''s recovery). Terminal rows are final. Never deleted or truncated. Migrations 023, 025.';

COMMIT;
