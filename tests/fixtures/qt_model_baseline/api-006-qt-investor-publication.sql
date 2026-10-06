-- Explicit release is separate from processing. No policy/grant is enabled here.
BEGIN;
CREATE TABLE IF NOT EXISTS trading.qt_investor_publication_policy (
    book_id text PRIMARY KEY,
    enabled boolean NOT NULL DEFAULT false,
    public_read_enabled boolean NOT NULL DEFAULT false,
    version bigint NOT NULL CHECK (version > 0)
);
CREATE TABLE IF NOT EXISTS trading.qt_investor_publish_grants (
    book_id text NOT NULL REFERENCES trading.qt_investor_publication_policy(book_id),
    user_id bigint NOT NULL REFERENCES auth.users(id) ON DELETE RESTRICT,
    active boolean NOT NULL DEFAULT false,
    version bigint NOT NULL CHECK (version > 0),
    PRIMARY KEY (book_id,user_id)
);
CREATE TABLE IF NOT EXISTS trading.qt_investor_publications (
    portfolio_id text NOT NULL,
    source_day date NOT NULL,
    portfolio_type text NOT NULL CHECK (portfolio_type = 'qt'),
    decision_id uuid NOT NULL UNIQUE REFERENCES trading.qt_decisions(decision_id) ON DELETE RESTRICT,
    attempt_id uuid NOT NULL,
    selected_book_digest text NOT NULL CHECK (selected_book_digest ~ '^[0-9a-f]{64}$'),
    processed_payload_digest text NOT NULL CHECK (processed_payload_digest ~ '^[0-9a-f]{64}$'),
    snapshot_digest text NOT NULL CHECK (snapshot_digest ~ '^[0-9a-f]{64}$'),
    snapshot jsonb NOT NULL CHECK (jsonb_typeof(snapshot) = 'object'),
    published_by bigint NOT NULL REFERENCES auth.users(id) ON DELETE RESTRICT,
    published_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    policy_version bigint NOT NULL CHECK (policy_version > 0),
    grant_version bigint NOT NULL CHECK (grant_version > 0),
    PRIMARY KEY (portfolio_id,source_day)
);
DROP TRIGGER IF EXISTS qt_investor_publications_immutable ON trading.qt_investor_publications;
CREATE TRIGGER qt_investor_publications_immutable BEFORE UPDATE OR DELETE
    ON trading.qt_investor_publications FOR EACH ROW EXECUTE FUNCTION trading.qt_reject_row_change();
DROP TRIGGER IF EXISTS qt_investor_publications_no_truncate ON trading.qt_investor_publications;
CREATE TRIGGER qt_investor_publications_no_truncate BEFORE TRUNCATE
    ON trading.qt_investor_publications FOR EACH STATEMENT EXECUTE FUNCTION trading.qt_reject_row_change();
COMMIT;
