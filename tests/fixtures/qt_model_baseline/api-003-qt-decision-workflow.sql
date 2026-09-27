-- QT decision workflow state. Apply only after auth.users exists.
-- This migration does not enable any book or configure a human grant.
BEGIN;

CREATE SCHEMA IF NOT EXISTS trading;

CREATE TABLE IF NOT EXISTS trading.qt_workflow_capabilities (
    book_id text PRIMARY KEY,
    enabled boolean NOT NULL,
    version bigint NOT NULL CHECK (version > 0),
    updated_at timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS trading.qt_action_grants (
    user_id bigint NOT NULL REFERENCES auth.users(id) ON DELETE RESTRICT,
    capability text NOT NULL CHECK (capability IN ('qt_submit', 'qt_approve')),
    active boolean NOT NULL,
    version bigint NOT NULL CHECK (version > 0),
    updated_at timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (user_id, capability)
);

CREATE TABLE IF NOT EXISTS trading.qt_approver_allowlist (
    person_id text PRIMARY KEY,
    display_label text NOT NULL,
    user_id bigint NOT NULL UNIQUE REFERENCES auth.users(id) ON DELETE RESTRICT,
    active boolean NOT NULL,
    mapping_version bigint NOT NULL CHECK (mapping_version > 0),
    updated_at timestamptz NOT NULL DEFAULT now(),
    CONSTRAINT qt_approver_identity CHECK (
        (person_id = 'eric_shwartz' AND display_label = 'eric shwartz') OR
        (person_id = 'john_riley' AND display_label = 'john riley') OR
        (person_id = 'xander_robbins' AND display_label = 'xander robbins') OR
        (person_id = 'hemdutt_rao' AND display_label = 'hemdutt rao') OR
        (person_id = 'dominick_dupuoy' AND display_label = 'dominick dupuoy')
    )
);

CREATE TABLE IF NOT EXISTS trading.qt_drafts (
    draft_id uuid PRIMARY KEY,
    book_id text NOT NULL,
    source_day date NOT NULL,
    revision bigint NOT NULL CHECK (revision > 0),
    model_publication_id uuid,
    model_publication_version bigint CHECK (model_publication_version > 0),
    seed_digest text,
    source_digest text NOT NULL,
    provenance_digest text NOT NULL,
    draft_digest text NOT NULL,
    selection_payload jsonb NOT NULL CHECK (jsonb_typeof(selection_payload) = 'object'),
    created_by bigint NOT NULL REFERENCES auth.users(id) ON DELETE RESTRICT,
    updated_by bigint NOT NULL REFERENCES auth.users(id) ON DELETE RESTRICT,
    created_at timestamptz NOT NULL DEFAULT now(),
    updated_at timestamptz NOT NULL DEFAULT now(),
    CONSTRAINT qt_draft_publication_pair CHECK
        ((model_publication_id IS NULL) = (model_publication_version IS NULL)),
    UNIQUE (book_id, source_day, revision),
    UNIQUE (book_id, source_day, draft_id, revision)
);

CREATE TABLE IF NOT EXISTS trading.qt_draft_heads (
    book_id text NOT NULL,
    source_day date NOT NULL,
    draft_id uuid NOT NULL,
    revision bigint NOT NULL,
    PRIMARY KEY (book_id, source_day),
    FOREIGN KEY (book_id, source_day, draft_id, revision)
        REFERENCES trading.qt_drafts(book_id, source_day, draft_id, revision)
        ON DELETE RESTRICT
);

CREATE TABLE IF NOT EXISTS trading.qt_previews (
    preview_id uuid PRIMARY KEY,
    book_id text NOT NULL,
    source_day date NOT NULL,
    draft_id uuid NOT NULL,
    draft_revision bigint NOT NULL,
    draft_digest text NOT NULL,
    source_digest text NOT NULL,
    provenance_digest text NOT NULL,
    read_set_digest text NOT NULL,
    optimizer_book_digest text NOT NULL,
    selected_book_digest text NOT NULL,
    payload_digest text NOT NULL,
    payload jsonb NOT NULL CHECK (jsonb_typeof(payload) = 'object'),
    read_set_payload jsonb NOT NULL CHECK (jsonb_typeof(read_set_payload) = 'object'),
    evaluator_build text NOT NULL,
    policy_version text NOT NULL,
    availability text NOT NULL CHECK (availability IN ('ready', 'unavailable')),
    created_by bigint NOT NULL REFERENCES auth.users(id) ON DELETE RESTRICT,
    created_at timestamptz NOT NULL DEFAULT now(),
    state text NOT NULL CHECK (state IN ('pending', 'pending_override', 'confirmed_decision', 'stale')),
    FOREIGN KEY (book_id, source_day, draft_id, draft_revision)
        REFERENCES trading.qt_drafts(book_id, source_day, draft_id, revision)
        ON DELETE RESTRICT,
    UNIQUE (preview_id, book_id, source_day),
    UNIQUE (preview_id, book_id, source_day, draft_id, draft_revision)
);

CREATE TABLE IF NOT EXISTS trading.qt_decisions (
    decision_id uuid PRIMARY KEY,
    preview_id uuid NOT NULL UNIQUE,
    book_id text NOT NULL,
    source_day date NOT NULL,
    status text NOT NULL CHECK (status IN ('pending_override', 'confirmed_decision')),
    model_publication_id uuid,
    provenance_digest text NOT NULL,
    draft_id uuid NOT NULL,
    draft_revision bigint NOT NULL,
    selected_book_digest text NOT NULL,
    read_set_digest text NOT NULL,
    workflow_capability_version bigint NOT NULL CHECK (workflow_capability_version > 0),
    submitter_grant_version bigint NOT NULL CHECK (submitter_grant_version > 0),
    policy_version text NOT NULL,
    payload jsonb NOT NULL CHECK (jsonb_typeof(payload) = 'object'),
    created_by bigint NOT NULL REFERENCES auth.users(id) ON DELETE RESTRICT,
    created_at timestamptz NOT NULL DEFAULT now(),
    FOREIGN KEY (preview_id, book_id, source_day, draft_id, draft_revision)
        REFERENCES trading.qt_previews(preview_id, book_id, source_day, draft_id, draft_revision)
        ON DELETE RESTRICT,
    FOREIGN KEY (book_id, source_day, draft_id, draft_revision)
        REFERENCES trading.qt_drafts(book_id, source_day, draft_id, revision)
        ON DELETE RESTRICT,
    UNIQUE (decision_id, book_id)
);

CREATE TABLE IF NOT EXISTS trading.qt_override_requests (
    request_id uuid PRIMARY KEY,
    decision_id uuid NOT NULL UNIQUE REFERENCES trading.qt_decisions(decision_id) ON DELETE RESTRICT,
    eligibility_version bigint NOT NULL CHECK (eligibility_version > 0),
    required_approvals smallint NOT NULL DEFAULT 2 CHECK (required_approvals = 2),
    created_at timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS trading.qt_override_approvals (
    approval_id uuid PRIMARY KEY,
    request_id uuid NOT NULL REFERENCES trading.qt_override_requests(request_id) ON DELETE RESTRICT,
    person_id text NOT NULL REFERENCES trading.qt_approver_allowlist(person_id) ON DELETE RESTRICT,
    user_id bigint NOT NULL REFERENCES auth.users(id) ON DELETE RESTRICT,
    mapping_version bigint NOT NULL CHECK (mapping_version > 0),
    grant_version bigint NOT NULL CHECK (grant_version > 0),
    approved_at timestamptz NOT NULL DEFAULT now(),
    UNIQUE (request_id, person_id),
    UNIQUE (request_id, user_id)
);

CREATE TABLE IF NOT EXISTS trading.qt_desk_receipts (
    decision_id uuid PRIMARY KEY REFERENCES trading.qt_decisions(decision_id) ON DELETE RESTRICT,
    attempt_id uuid NOT NULL UNIQUE,
    status text NOT NULL CHECK (status IN ('pending', 'processed', 'failed')),
    published_book_digest text,
    processed_at timestamptz,
    publication_payload jsonb,
    report_eligibility_status text NOT NULL CHECK (report_eligibility_status IN ('eligible', 'unavailable')),
    report_reason_codes jsonb NOT NULL CHECK (jsonb_typeof(report_reason_codes) = 'array'),
    row_manifest_digest text,
    CONSTRAINT qt_processed_receipt_digest CHECK (status <> 'processed' OR published_book_digest IS NOT NULL),
    CONSTRAINT qt_eligible_receipt_manifest CHECK
        (report_eligibility_status <> 'eligible' OR row_manifest_digest IS NOT NULL)
);

CREATE TABLE IF NOT EXISTS trading.qt_idempotency (
    actor_id bigint NOT NULL REFERENCES auth.users(id) ON DELETE RESTRICT,
    book_id text NOT NULL,
    operation text NOT NULL CHECK (operation IN ('save_draft', 'create_preview', 'confirm_preview', 'approve_override')),
    scope_id text NOT NULL,
    idempotency_key uuid NOT NULL,
    request_digest text NOT NULL,
    response_payload jsonb NOT NULL CHECK (jsonb_typeof(response_payload) = 'object'),
    created_at timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (actor_id, book_id, operation, scope_id, idempotency_key)
);

CREATE INDEX IF NOT EXISTS qt_drafts_book_day_idx ON trading.qt_drafts(book_id, source_day);
CREATE INDEX IF NOT EXISTS qt_previews_book_day_idx ON trading.qt_previews(book_id, source_day);
CREATE INDEX IF NOT EXISTS qt_decisions_book_day_idx ON trading.qt_decisions(book_id, source_day);
CREATE INDEX IF NOT EXISTS qt_approvals_request_idx ON trading.qt_override_approvals(request_id);

-- Revisions and evidence are immutable. Only the explicit preview/decision
-- state columns may advance; approvals and requests are append-only.
CREATE OR REPLACE FUNCTION trading.qt_reject_row_change() RETURNS trigger
LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'QT evidence is immutable'; END $$;

CREATE OR REPLACE FUNCTION trading.qt_guard_preview_update() RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
    IF (to_jsonb(NEW) - 'state') IS DISTINCT FROM (to_jsonb(OLD) - 'state') THEN
        RAISE EXCEPTION 'QT preview evidence is immutable';
    END IF;
    IF NOT (
        (OLD.state = 'pending' AND NEW.state IN ('stale', 'pending_override', 'confirmed_decision')) OR
        (OLD.state = 'pending_override' AND NEW.state IN ('stale', 'confirmed_decision'))
    ) THEN
        RAISE EXCEPTION 'invalid QT preview state transition';
    END IF;
    RETURN NEW;
END $$;

CREATE OR REPLACE FUNCTION trading.qt_guard_decision_update() RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
    IF (to_jsonb(NEW) - 'status') IS DISTINCT FROM (to_jsonb(OLD) - 'status')
       OR OLD.status <> 'pending_override' OR NEW.status <> 'confirmed_decision' THEN
        RAISE EXCEPTION 'QT decision evidence is immutable';
    END IF;
    RETURN NEW;
END $$;

DROP TRIGGER IF EXISTS qt_drafts_immutable ON trading.qt_drafts;
CREATE TRIGGER qt_drafts_immutable BEFORE UPDATE OR DELETE ON trading.qt_drafts
    FOR EACH ROW EXECUTE FUNCTION trading.qt_reject_row_change();
DROP TRIGGER IF EXISTS qt_previews_immutable ON trading.qt_previews;
CREATE TRIGGER qt_previews_immutable BEFORE UPDATE ON trading.qt_previews
    FOR EACH ROW EXECUTE FUNCTION trading.qt_guard_preview_update();
DROP TRIGGER IF EXISTS qt_previews_no_delete ON trading.qt_previews;
CREATE TRIGGER qt_previews_no_delete BEFORE DELETE ON trading.qt_previews
    FOR EACH ROW EXECUTE FUNCTION trading.qt_reject_row_change();
DROP TRIGGER IF EXISTS qt_decisions_immutable ON trading.qt_decisions;
CREATE TRIGGER qt_decisions_immutable BEFORE UPDATE ON trading.qt_decisions
    FOR EACH ROW EXECUTE FUNCTION trading.qt_guard_decision_update();
DROP TRIGGER IF EXISTS qt_decisions_no_delete ON trading.qt_decisions;
CREATE TRIGGER qt_decisions_no_delete BEFORE DELETE ON trading.qt_decisions
    FOR EACH ROW EXECUTE FUNCTION trading.qt_reject_row_change();
DROP TRIGGER IF EXISTS qt_requests_immutable ON trading.qt_override_requests;
CREATE TRIGGER qt_requests_immutable BEFORE UPDATE OR DELETE ON trading.qt_override_requests
    FOR EACH ROW EXECUTE FUNCTION trading.qt_reject_row_change();
DROP TRIGGER IF EXISTS qt_approvals_immutable ON trading.qt_override_approvals;
CREATE TRIGGER qt_approvals_immutable BEFORE UPDATE OR DELETE ON trading.qt_override_approvals
    FOR EACH ROW EXECUTE FUNCTION trading.qt_reject_row_change();
DROP TRIGGER IF EXISTS qt_idempotency_immutable ON trading.qt_idempotency;
CREATE TRIGGER qt_idempotency_immutable BEFORE UPDATE OR DELETE ON trading.qt_idempotency
    FOR EACH ROW EXECUTE FUNCTION trading.qt_reject_row_change();

-- Row triggers do not fire for TRUNCATE. Statement triggers also reject a
-- TRUNCATE reached indirectly through CASCADE from an auth or parent table.
DROP TRIGGER IF EXISTS qt_drafts_no_truncate ON trading.qt_drafts;
CREATE TRIGGER qt_drafts_no_truncate BEFORE TRUNCATE ON trading.qt_drafts
    FOR EACH STATEMENT EXECUTE FUNCTION trading.qt_reject_row_change();
DROP TRIGGER IF EXISTS qt_previews_no_truncate ON trading.qt_previews;
CREATE TRIGGER qt_previews_no_truncate BEFORE TRUNCATE ON trading.qt_previews
    FOR EACH STATEMENT EXECUTE FUNCTION trading.qt_reject_row_change();
DROP TRIGGER IF EXISTS qt_decisions_no_truncate ON trading.qt_decisions;
CREATE TRIGGER qt_decisions_no_truncate BEFORE TRUNCATE ON trading.qt_decisions
    FOR EACH STATEMENT EXECUTE FUNCTION trading.qt_reject_row_change();
DROP TRIGGER IF EXISTS qt_requests_no_truncate ON trading.qt_override_requests;
CREATE TRIGGER qt_requests_no_truncate BEFORE TRUNCATE ON trading.qt_override_requests
    FOR EACH STATEMENT EXECUTE FUNCTION trading.qt_reject_row_change();
DROP TRIGGER IF EXISTS qt_approvals_no_truncate ON trading.qt_override_approvals;
CREATE TRIGGER qt_approvals_no_truncate BEFORE TRUNCATE ON trading.qt_override_approvals
    FOR EACH STATEMENT EXECUTE FUNCTION trading.qt_reject_row_change();
DROP TRIGGER IF EXISTS qt_idempotency_no_truncate ON trading.qt_idempotency;
CREATE TRIGGER qt_idempotency_no_truncate BEFORE TRUNCATE ON trading.qt_idempotency
    FOR EACH STATEMENT EXECUTE FUNCTION trading.qt_reject_row_change();

COMMIT;
