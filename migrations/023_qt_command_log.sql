-- 023_qt_command_log.sql
--
-- The QT platform's command log and the remaining schema of docs/design/qt-contract.md (sections 2,
-- 4 and 5). Applied after 021 (books) and 022 (strategy_config).
--
--   1. trading.position_overrides is rebuilt as the desk's command log: one row per save, override
--      request, override decision and publish, keyed by portfolio and book date. AlgoLens inserts
--      rows; the engine (desk-agent) moves status/result. Rows are never deleted, and only the
--      engine-owned columns may change (trigger). The old #55 shape had no portfolio or date and an
--      UPDATE ... DO INSTEAD NOTHING rule that made recording a decision impossible; it holds 0 rows
--      on new_algo_data (checked 2026-10-09) and this migration refuses if any row exists.
--   2. trading.live_results.book_source: which path produced a qt row (model, desk, override).
--      Master doc ruling 9: each day's record says which happened.
--   3. trading.strategy_registry.portfolio_group and desk_editable (ruling 20 and section 2 of the
--      contract), and the two QT portfolios.
--
-- The ruling-28 drops (risk_limits, portfolios, strategy_book_memberships, portfolio_assignments)
-- are in 024, applied separately.
--
-- SAFETY: transactional; refuses on a non-empty position_overrides; otherwise additive.

BEGIN;

DO $$
BEGIN
    IF to_regclass('trading.live_results') IS NULL THEN RAISE EXCEPTION 'trading.live_results does not exist'; END IF;
    IF to_regclass('trading.strategy_registry') IS NULL THEN RAISE EXCEPTION 'trading.strategy_registry does not exist'; END IF;
    IF to_regclass('trading.position_overrides') IS NOT NULL THEN
        IF EXISTS (SELECT 1 FROM trading.position_overrides) THEN
            RAISE EXCEPTION 'trading.position_overrides holds rows; 023 rebuilds it and refuses to discard them';
        END IF;
    END IF;
END $$;

-- 1. The command log.
DROP TABLE IF EXISTS trading.position_overrides CASCADE;

CREATE TABLE trading.position_overrides (
    id               bigserial PRIMARY KEY,
    portfolio_id     text        NOT NULL,
    date             date        NOT NULL,
    kind             text        NOT NULL CHECK (kind IN ('save', 'override_request', 'override_decision', 'publish')),
    status           text        NOT NULL DEFAULT 'pending'
                                 CHECK (status IN ('pending', 'running', 'done', 'refused', 'failed')),
    requested_by     text        NOT NULL CHECK (length(btrim(requested_by)) > 0),
    reason           text,
    payload          jsonb       NOT NULL DEFAULT '{}'::jsonb CHECK (jsonb_typeof(payload) = 'object'),
    parent_id        bigint      REFERENCES trading.position_overrides (id),
    approver_role    text        CHECK (approver_role IN ('vp', 'president')),
    token_hash       text,
    token_expires_at timestamptz,
    result           jsonb,
    message          text,
    created_at       timestamptz NOT NULL DEFAULT now(),
    started_at       timestamptz,
    finished_at      timestamptz,
    CONSTRAINT position_overrides_reason_required
        CHECK (kind NOT IN ('save', 'override_request') OR length(btrim(coalesce(reason, ''))) > 0),
    CONSTRAINT position_overrides_decision_shape
        CHECK (kind <> 'override_decision' OR (parent_id IS NOT NULL AND approver_role IS NOT NULL))
);

CREATE INDEX position_overrides_portfolio_date_kind ON trading.position_overrides (portfolio_id, date, kind);
CREATE INDEX position_overrides_pending ON trading.position_overrides (status) WHERE status IN ('pending', 'running');
CREATE UNIQUE INDEX position_overrides_one_publish
    ON trading.position_overrides (portfolio_id, date) WHERE kind = 'publish' AND status = 'done';
CREATE UNIQUE INDEX position_overrides_token ON trading.position_overrides (token_hash) WHERE token_hash IS NOT NULL;

COMMENT ON TABLE trading.position_overrides IS
    'QT desk command log (docs/design/qt-contract.md section 4): one row per save, override request, override decision and publish. AlgoLens inserts; the engine moves status, started_at, finished_at, result and message. Never deleted. Migration 023.';

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

CREATE TRIGGER position_overrides_guard
    BEFORE UPDATE OR DELETE ON trading.position_overrides
    FOR EACH ROW EXECUTE FUNCTION trading.position_overrides_guard();

-- 2. Which path produced a book's day.
ALTER TABLE trading.live_results ADD COLUMN IF NOT EXISTS book_source text;
ALTER TABLE trading.live_results DROP CONSTRAINT IF EXISTS live_results_book_source_check;
ALTER TABLE trading.live_results ADD CONSTRAINT live_results_book_source_check
    CHECK (book_source IS NULL OR book_source IN ('model', 'desk', 'override'));
COMMENT ON COLUMN trading.live_results.book_source IS
    'For a qt row: model (no desk save that day, qt = the model result), desk (the one pass ran on the desk proposal) or override (an approved override booked the proposal exactly). NULL on system rows. Migration 023.';

-- 3. Registry grouping and desk editing, and the two QT portfolios.
ALTER TABLE trading.strategy_registry ADD COLUMN IF NOT EXISTS portfolio_group text;
ALTER TABLE trading.strategy_registry ADD COLUMN IF NOT EXISTS desk_editable boolean NOT NULL DEFAULT false;

INSERT INTO trading.strategy_registry
    (id, strategy_type, portfolio_id, name, description, initial_equity, managers, is_active, sort_order,
     lifecycle, portfolio_group, desk_editable)
VALUES
    ('qt_conservative', 'LIVE_TREND_FOLLOWING', 'QT_CONSERVATIVE_PORTFOLIO', 'QT Trend Following',
     'Conservative trend following, desk-edited by the QT desk', 500000, '["QT desk"]'::jsonb, true, 10,
     'live', 'qt_conservative', true),
    ('qt_conservative_model', 'LIVE_TREND_FOLLOWING', 'QT_CONSERVATIVE_MODEL_PORTFOLIO', 'QT Trend Following (model)',
     'The same strategy and capital with desk editing off: the untouched model book', 500000, '["AlgoLens System"]'::jsonb, true, 11,
     'live', 'qt_conservative', false)
ON CONFLICT (id) DO NOTHING;


COMMIT;
