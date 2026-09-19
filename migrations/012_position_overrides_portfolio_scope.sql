-- Scope future position-override audit rows by portfolio without rewriting
-- immutable legacy evidence.
--
-- Existing rows retain a NULL portfolio_id.  A separate append-only table holds
-- a scope only where the existing positions data identifies exactly one
-- portfolio for the legacy row's (strategy_id, symbol) pair.  Ambiguous and
-- unmatched legacy rows deliberately remain unscoped.
--
-- Idempotent and transactional.  This migration expects the tables created by
-- the preceding Trade Ngin migrations to exist; it does not invent them.

BEGIN;

ALTER TABLE trading.position_overrides
    ADD COLUMN IF NOT EXISTS portfolio_id TEXT;

-- NOT VALID validates future INSERT/UPDATEs while retaining pre-012 NULL rows
-- as immutable evidence.  Recreate the constraint so repeated applications
-- preserve that legacy-compatible behavior.
ALTER TABLE trading.position_overrides
    DROP CONSTRAINT IF EXISTS position_overrides_new_rows_require_portfolio;
ALTER TABLE trading.position_overrides
    ADD CONSTRAINT position_overrides_new_rows_require_portfolio
    CHECK (portfolio_id IS NOT NULL) NOT VALID;

CREATE TABLE IF NOT EXISTS trading.position_override_legacy_scopes (
    override_id     BIGINT PRIMARY KEY REFERENCES trading.position_overrides(id),
    portfolio_id    TEXT NOT NULL,
    inference_basis JSONB NOT NULL,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- A repeated migration must not use INSERT ... ON CONFLICT against this
-- relation after its append-only UPDATE/DELETE rules exist: PostgreSQL rejects
-- ON CONFLICT on relations with rules.  Serialize the inference writer and
-- exclude already-attributed rows explicitly instead.
LOCK TABLE trading.position_override_legacy_scopes IN SHARE ROW EXCLUSIVE MODE;

-- The migration is the only automatic writer: a scope is recorded only when
-- exactly one distinct portfolio currently matches the legacy strategy/symbol.
-- No position_overrides row is ever updated to carry this inferred value.
WITH uniquely_inferred AS (
    SELECT
        override_row.id AS override_id,
        MIN(position_row.portfolio_id) AS portfolio_id,
        jsonb_build_object(
            'method', 'unique-current-position-portfolio-by-strategy-and-symbol',
            'strategy_id', override_row.strategy_id,
            'symbol', override_row.symbol,
            'candidate_portfolio_ids',
                jsonb_agg(DISTINCT position_row.portfolio_id ORDER BY position_row.portfolio_id)
        ) AS inference_basis
    FROM trading.position_overrides AS override_row
    JOIN trading.positions AS position_row
      ON position_row.strategy_id = override_row.strategy_id
     AND position_row.symbol = override_row.symbol
    WHERE override_row.portfolio_id IS NULL
    GROUP BY override_row.id, override_row.strategy_id, override_row.symbol
    HAVING count(DISTINCT position_row.portfolio_id) = 1
)
INSERT INTO trading.position_override_legacy_scopes
    (override_id, portfolio_id, inference_basis)
SELECT override_id, portfolio_id, inference_basis
FROM uniquely_inferred AS inferred
WHERE NOT EXISTS (
    SELECT 1
    FROM trading.position_override_legacy_scopes AS existing
    WHERE existing.override_id = inferred.override_id
);

CREATE INDEX IF NOT EXISTS idx_position_overrides_portfolio_strategy_created
    ON trading.position_overrides (portfolio_id, strategy_id, created_at DESC);

-- The legacy mapping itself is audit evidence: it is insert-only just like the
-- original audit rows.  Rules refuse accidental UPDATE or DELETE statements.
CREATE OR REPLACE RULE position_override_legacy_scopes_no_update AS
    ON UPDATE TO trading.position_override_legacy_scopes DO INSTEAD NOTHING;
CREATE OR REPLACE RULE position_override_legacy_scopes_no_delete AS
    ON DELETE TO trading.position_override_legacy_scopes DO INSTEAD NOTHING;

COMMENT ON TABLE trading.position_override_legacy_scopes IS
    'Append-only, uniquely inferred portfolio scope for legacy position override '
    'audit rows. Rows without exactly one candidate portfolio remain unscoped in '
    'trading.position_overrides. UPDATE and DELETE are refused by rule.';

COMMIT;
