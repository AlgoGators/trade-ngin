-- Migration 006: Strategy Configuration Database Tables
--
-- Introduces config versioning and active-override mechanism:
--
-- trading.strategy_config: Stores QT's config overrides, versioned and append-only.
--   - portfolio_id identifies the strategy (e.g., BASE_PORTFOLIO)
--   - version is an integer; higher version is newer (no implicit ordering by timestamp)
--   - overrides is JSONB containing parameter deltas to merge ON TOP of file config
--   - reason is required (non-empty) for audit trail; a parameter change with no stated rationale is indistinguishable from an accident
--   - is_active enforces "at most one active version per portfolio_id" via unique index
--   - created_at and created_by track lineage
--
-- Partial unique index ensures at most one active row per portfolio:
--   WHERE is_active = true
-- This is critical: a config system where two versions are simultaneously active
-- is worse than no versioning.
--
-- trading.config_manifest: Written BY the engine; read by AlgoLens.
--   - Stores the effective config (file defaults + portfolio overrides + DB override)
--   - published_at tracks when this version was loaded (session start time)
--   - SECURITY: Manifest never contains database.* or credential fields (stripped before INSERT)
--
-- SECURITY GUARANTEES:
-- (1) ConfigLoader::load() rejects any override attempting to set database.host, database.port,
--     database.username, database.password, database.name, or email.password.
-- (2) AppConfig::to_json() (published to manifest) never includes database section or password field.
-- These layers prevent credential exfiltration via the dashboard and prevent config hijacking.
--
-- IDEMPOTENCY: All DDL is IF NOT EXISTS. Safe to re-run.

CREATE SCHEMA IF NOT EXISTS trading;

-- Versioned config overrides, append-only. One row = one version.
CREATE TABLE IF NOT EXISTS trading.strategy_config (
    id            BIGSERIAL PRIMARY KEY,
    portfolio_id  TEXT        NOT NULL,
    version       INTEGER     NOT NULL,
    overrides     JSONB       NOT NULL,
    reason        TEXT        NOT NULL,
    created_by    INTEGER,
    is_active     BOOLEAN     NOT NULL DEFAULT FALSE,
    created_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (portfolio_id, version),
    CONSTRAINT reason_not_empty CHECK (length(btrim(reason)) > 0)
);

-- Unique constraint: at most one active version per portfolio.
-- Partial index ensures is_active=false rows don't consume the uniqueness.
CREATE UNIQUE INDEX IF NOT EXISTS idx_strategy_config_active_per_portfolio
ON trading.strategy_config (portfolio_id)
WHERE is_active = true;

-- Manifest of effective config published by the engine.
-- Written after a successful ConfigLoader::load().
CREATE TABLE IF NOT EXISTS trading.config_manifest (
    id            BIGSERIAL PRIMARY KEY,
    portfolio_id  TEXT        NOT NULL,
    effective     JSONB       NOT NULL,
    published_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Index for fast lookup by portfolio and recency.
CREATE INDEX IF NOT EXISTS idx_config_manifest_by_portfolio_time
ON trading.config_manifest (portfolio_id, published_at DESC);
