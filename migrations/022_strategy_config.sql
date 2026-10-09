-- 022_strategy_config.sql
--
-- Desk settings from the database (QT plan E2; rulings 2, 3, 4, 24, 26).
--
-- 1. trading.strategy_config: the ONE new table of the settings lane. One row per desk change to
--    a portfolio's settings, versioned and append-only. `overrides` is a JSON object in the shape
--    of the portfolio's merged config files (defaults.json + portfolio.json, with risk.json under
--    "risk"), deep-merged on top of the files at the start of a LIVE run only (ruling 4: backtests
--    keep the file config). At most one row per portfolio is active (partial unique index); with
--    no active row the run uses its files unchanged. A change states its reason and its author.
--    The engine refuses a run whose active row cannot be applied (ruling 24) and never falls back
--    to the files; the checks it makes (no null, no credential/database/email key, no key the
--    files do not hold) are in ConfigLoader, not here.
--
-- 2. trading.live_run_metadata gains:
--      settings_used jsonb  the credential-free effective config the run traded on, as
--                           {"strategy_config_version": <n or null>, "config": {...}};
--                           written by every live run of a binary from this migration on;
--      published_by text, published_at timestamptz  the desk publish record (QT plan E8, ruling
--                           18); added here so the table changes once. NULL until E8 writes them.
--    Rows written before this migration read NULL in all three.
--
-- Ported from PR #60's migration 008 (strategy_config DDL only; its config_manifest table is NOT
-- created: ruling 26 puts the settings used on live_run_metadata). created_by is text here (#60
-- had integer): AlgoLens records the author's login.
--
-- SAFETY: CREATE TABLE / ADD COLUMN of NULL-able columns with no default are metadata-only; no
-- existing row, key, index or view of live_run_metadata is touched; type-guarded (an existing
-- strategy_config or column of another shape makes 022 refuse); transactional and idempotent.

BEGIN;
DO $$
DECLARE bad text;
BEGIN
    IF to_regclass('trading.live_run_metadata') IS NULL THEN
        RAISE EXCEPTION 'trading.live_run_metadata does not exist';
    END IF;
    -- A strategy_config left by another branch (PR #60's 008 had created_by integer and no
    -- object check) must not be taken for this one.
    IF to_regclass('trading.strategy_config') IS NOT NULL THEN
        FOR bad IN
            SELECT e.col || ' expected ' || e.typ || ' got ' || coalesce(c.data_type, 'nothing')
              FROM (VALUES ('id', 'bigint'), ('portfolio_id', 'text'), ('version', 'integer'),
                           ('overrides', 'jsonb'), ('reason', 'text'), ('created_by', 'text'),
                           ('created_at', 'timestamp with time zone'), ('is_active', 'boolean')) e(col, typ)
              LEFT JOIN information_schema.columns c
                ON c.table_schema = 'trading' AND c.table_name = 'strategy_config' AND c.column_name = e.col
             WHERE c.data_type IS DISTINCT FROM e.typ
        LOOP
            RAISE EXCEPTION 'trading.strategy_config exists with another shape: %', bad;
        END LOOP;
    END IF;
    FOR bad IN
        SELECT column_name || ' ' || data_type FROM information_schema.columns
         WHERE table_schema = 'trading' AND table_name = 'live_run_metadata'
           AND ((column_name = 'settings_used' AND data_type <> 'jsonb')
             OR (column_name = 'published_by' AND data_type <> 'text')
             OR (column_name = 'published_at' AND data_type <> 'timestamp with time zone'))
    LOOP
        RAISE EXCEPTION 'trading.live_run_metadata has a column of another type: %', bad;
    END LOOP;
END $$;

CREATE TABLE IF NOT EXISTS trading.strategy_config (
    id            bigserial   PRIMARY KEY,
    portfolio_id  text        NOT NULL,
    version       integer     NOT NULL,
    overrides     jsonb       NOT NULL,
    reason        text        NOT NULL,
    created_by    text        NOT NULL,
    created_at    timestamptz NOT NULL DEFAULT now(),
    is_active     boolean     NOT NULL DEFAULT false,
    CONSTRAINT strategy_config_portfolio_version_key UNIQUE (portfolio_id, version),
    CONSTRAINT strategy_config_version_positive CHECK (version > 0),
    CONSTRAINT strategy_config_overrides_object CHECK (jsonb_typeof(overrides) = 'object'),
    CONSTRAINT strategy_config_reason_not_empty CHECK (length(btrim(reason)) > 0),
    CONSTRAINT strategy_config_created_by_not_empty CHECK (length(btrim(created_by)) > 0)
);

-- At most one active version per portfolio: two active rows would be worse than no versioning.
CREATE UNIQUE INDEX IF NOT EXISTS strategy_config_one_active_per_portfolio
    ON trading.strategy_config (portfolio_id) WHERE is_active;

COMMENT ON TABLE trading.strategy_config IS
    'Desk settings changes, one versioned row per change, at most one active per portfolio. overrides is deep-merged over the portfolio''s config files at the start of a live run only; a row that cannot be applied refuses the run (ruling 24). QT plan E2; migration 022.';
COMMENT ON COLUMN trading.strategy_config.overrides IS
    'A JSON object in the shape of the merged config files (defaults.json + portfolio.json, risk.json under "risk"). Every key must exist in the files; no null; no database, email, portfolio_id or credential key. Arrays replace the file''s array whole.';

ALTER TABLE trading.live_run_metadata ADD COLUMN IF NOT EXISTS settings_used jsonb;
ALTER TABLE trading.live_run_metadata ADD COLUMN IF NOT EXISTS published_by text;
ALTER TABLE trading.live_run_metadata ADD COLUMN IF NOT EXISTS published_at timestamptz;

COMMENT ON COLUMN trading.live_run_metadata.settings_used IS
    'The credential-free effective config the run traded on: {"strategy_config_version": n or null (files only), "config": the merged files plus the active strategy_config overrides, without the database and email sections}. NULL on rows written before migration 022''s binary. QT plan E2; migration 022.';
COMMENT ON COLUMN trading.live_run_metadata.published_by IS
    'Who published the day''s desk book (QT plan E8). NULL until published. Migration 022.';
COMMENT ON COLUMN trading.live_run_metadata.published_at IS
    'When the day''s desk book was published (QT plan E8). NULL until published. Migration 022.';
COMMIT;
