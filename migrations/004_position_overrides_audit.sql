-- F2: the audit trail for manual position edits.
--
-- Shape per ADR-000 C-5. Required regardless of how DECISION-1 lands: whether a
-- risk-breaching edit is blocked outright or allowed with an explicit override,
-- every manual change to a live book has to be attributable afterwards. For a
-- fund that is table stakes, not a feature.
--
--
-- APPEND-ONLY IS ENFORCED, NOT JUST INTENDED
-- ------------------------------------------
-- An audit trail that can be edited is not an audit trail. ADR-000 describes
-- this table as append-only; a comment does not make it so. The rules below
-- reject UPDATE and DELETE at the database, so the guarantee holds even against
-- a bug, a careless psql session, or a compromised application account.
--
-- Postgres RULEs are used rather than trigger-raised exceptions because they
-- refuse the statement outright rather than aborting the surrounding
-- transaction -- an accidental UPDATE cannot take a trading run down with it.
--
-- A superuser can still drop the rules. That is deliberate: the goal is to make
-- tampering impossible by accident and obvious on purpose, not to defeat someone
-- with the keys to the database.
--
--
-- SAFETY
-- ------
-- * Creates a new table only. Nothing existing is read, altered or deleted.
-- * Idempotent and transactional.

BEGIN;

CREATE TABLE IF NOT EXISTS trading.position_overrides (
    id                BIGSERIAL PRIMARY KEY,

    -- Who. FK to auth.users is declared in ADR-000 but deliberately NOT enforced
    -- here: auth is owned by AlgoLens (C-3) and lives in the same database only
    -- by convention. A hard FK across an ownership boundary would couple
    -- trade-ngin's migrations to AlgoLens' schema.
    user_id           INTEGER NOT NULL,
    source_app        TEXT NOT NULL
                      CHECK (source_app IN ('algolens', 'manual_db_edit')),

    -- What
    strategy_id       TEXT NOT NULL,
    symbol            TEXT NOT NULL,
    before_state      JSONB NOT NULL,   -- full position row prior to the edit
    after_state       JSONB NOT NULL,   -- full position row after the edit

    -- Why. Required and non-empty: an override with no stated reason is
    -- indistinguishable from an accident when it is read back months later.
    reason            TEXT NOT NULL
                      CHECK (length(btrim(reason)) > 0),

    -- What the gate said, and whether it was overruled anyway.
    risk_check_result JSONB NOT NULL,   -- {passed: bool, breaches: [...]}
    overrode_risk     BOOLEAN NOT NULL DEFAULT FALSE,

    created_at        TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Reading the trail is always "what happened to this book/strategy, recently".
CREATE INDEX IF NOT EXISTS idx_position_overrides_strategy_created
    ON trading.position_overrides (strategy_id, created_at DESC);

CREATE INDEX IF NOT EXISTS idx_position_overrides_symbol_created
    ON trading.position_overrides (symbol, created_at DESC);

-- Finding every override that bypassed the risk gate is the single most likely
-- audit question, so it gets its own partial index.
CREATE INDEX IF NOT EXISTS idx_position_overrides_overrode_risk
    ON trading.position_overrides (created_at DESC)
    WHERE overrode_risk;

-- Append-only.
CREATE OR REPLACE RULE position_overrides_no_update AS
    ON UPDATE TO trading.position_overrides DO INSTEAD NOTHING;

CREATE OR REPLACE RULE position_overrides_no_delete AS
    ON DELETE TO trading.position_overrides DO INSTEAD NOTHING;

COMMENT ON TABLE trading.position_overrides IS
    'Append-only audit of manual position edits (ADR-000 C-5). UPDATE and DELETE '
    'are refused by rule. Written by AlgoLens (inserts only) and by trade-ngin.';

COMMIT;
