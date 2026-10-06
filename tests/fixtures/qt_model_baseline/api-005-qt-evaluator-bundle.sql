-- Add byte identity for the evaluator's complete native dependency closure.
-- Existing unpinned policy rows remain stored but cannot authorize evaluation.
-- No artifact path, digest, policy or human grant is populated here.
BEGIN;
ALTER TABLE trading.qt_source_policies
    ADD COLUMN evaluator_bundle_sha256 text
        CHECK (evaluator_bundle_sha256 IS NULL OR evaluator_bundle_sha256 ~ '^[0-9a-f]{64}$'),
    ADD CONSTRAINT qt_execution_policy_no_evaluator_bundle
        CHECK (purpose = 'evaluation' OR evaluator_bundle_sha256 IS NULL);
COMMIT;
