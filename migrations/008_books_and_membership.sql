-- Books: portfolios as first-class objects, and a strategy in more than one.
--
-- `portfolio_id` has scoped every read in this schema since 001, but it existed
-- only as a column on trading.strategy_registry. Two consequences followed from
-- that, and this migration removes both:
--
--   1. A book could not exist before something was in it. You could not define
--      a book and then decide what belonged in it, because the book only came
--      into being once a strategy already named it.
--   2. A strategy could belong to exactly one book, even though positions,
--      equity_curve and live_results are all keyed on (strategy, portfolio)
--      pairs and would happily carry the same strategy in several.
--
-- AlgoLens was creating these three tables lazily at runtime so its Books tab
-- could work before this migration existed. That is removed in the same change
-- that adds this file: an application must not own the schema it queries.
--
--
-- THE PRIMARY BOOK STAYS
-- ----------------------
-- strategy_registry.portfolio_id is not replaced. It now means the PRIMARY book:
-- the single answer used wherever one is needed, and the one the engine reads.
-- Membership is additive on top of it.
--
-- The invariant that matters: portfolio_id must always name a book the strategy
-- is actually a member of. If it names a book the strategy has left, every
-- scoped read for that strategy returns nothing and it silently disappears from
-- the platform. The membership seed below establishes it, and the writer
-- repoints it on removal.
--
--
-- WHY THE SEED IS NOT OPTIONAL
-- ----------------------------
-- Membership is read in preference to the primary column. Creating the table
-- empty would therefore report every book as empty the moment this lands.
-- Seeding from strategy_registry makes the change a no-op on day one: every
-- strategy starts out in exactly the book it was already in.
--
--
-- SAFETY
-- ------
-- * Creates new tables and one nullability change. No existing row is deleted.
-- * The one ALTER relaxes a constraint (see below) and cannot fail on data.
-- * Idempotent and transactional.

BEGIN;

-- ---------------------------------------------------------------------------
-- Declared books
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS trading.portfolios (
    portfolio_id TEXT        PRIMARY KEY,
    name         TEXT        NOT NULL,
    description  TEXT        NOT NULL DEFAULT '',
    created_by   TEXT,
    created_at   TIMESTAMPTZ NOT NULL DEFAULT now()
);

COMMENT ON TABLE trading.portfolios IS
    'Books declared by a person, so one can exist before it holds anything. A '
    'book merely in use -- named by strategy_registry.portfolio_id or by a '
    'membership row -- is equally real and need not appear here.';

-- ---------------------------------------------------------------------------
-- Membership
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS trading.strategy_book_memberships (
    strategy_id  TEXT        NOT NULL,
    portfolio_id TEXT        NOT NULL,
    added_by     TEXT,
    added_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (strategy_id, portfolio_id)
);

-- The common read is "which books is this strategy in"; the primary key already
-- serves it. This index serves the other direction: "what is in this book".
CREATE INDEX IF NOT EXISTS idx_memberships_portfolio
    ON trading.strategy_book_memberships (portfolio_id, strategy_id);

COMMENT ON TABLE trading.strategy_book_memberships IS
    'Which books each strategy belongs to. Additive over '
    'strategy_registry.portfolio_id, which remains the primary book. A strategy '
    'must always belong to at least one book: every read is scoped by '
    '(strategy, portfolio), so a strategy in none is unreachable.';

-- Every existing strategy joins the book it already had. Without this the
-- platform would report every book empty the moment membership is read.
INSERT INTO trading.strategy_book_memberships (strategy_id, portfolio_id, added_by)
SELECT id, portfolio_id, 'migration-008'
FROM trading.strategy_registry
ON CONFLICT (strategy_id, portfolio_id) DO NOTHING;

-- ---------------------------------------------------------------------------
-- Audit of book changes
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS trading.portfolio_assignments (
    id                BIGSERIAL   PRIMARY KEY,
    strategy_id       TEXT        NOT NULL,
    user_id           TEXT,
    -- Both nullable, on purpose. A move between books has both; adding a
    -- strategy to a book has no origin; removing it has no destination.
    -- Requiring a destination would make removals unrecordable, and a change
    -- that cannot be recorded is one that must not be permitted at all.
    from_portfolio_id TEXT,
    to_portfolio_id   TEXT,
    lifecycle_at_move TEXT,
    reason            TEXT,
    consequences      JSONB       NOT NULL DEFAULT '[]'::jsonb,
    acknowledged      BOOLEAN     NOT NULL DEFAULT FALSE,
    created_at        TIMESTAMPTZ NOT NULL DEFAULT now(),
    CONSTRAINT portfolio_assignments_has_a_side
        CHECK (from_portfolio_id IS NOT NULL OR to_portfolio_id IS NOT NULL)
);

-- Relaxes the constraint for any database where this table was created by the
-- application before this migration existed, which required a destination.
ALTER TABLE trading.portfolio_assignments
    ALTER COLUMN to_portfolio_id DROP NOT NULL;

CREATE INDEX IF NOT EXISTS idx_portfolio_assignments_strategy
    ON trading.portfolio_assignments (strategy_id, created_at DESC);

-- The audit question that gets asked: which book changes were made over a
-- stated warning.
CREATE INDEX IF NOT EXISTS idx_portfolio_assignments_acknowledged
    ON trading.portfolio_assignments (created_at DESC)
    WHERE acknowledged;

-- Append-only, for the same reason as position_overrides in 004: an audit trail
-- that can be edited is not one. RULEs refuse the statement rather than
-- aborting the surrounding transaction.
CREATE OR REPLACE RULE portfolio_assignments_no_update AS
    ON UPDATE TO trading.portfolio_assignments DO INSTEAD NOTHING;

CREATE OR REPLACE RULE portfolio_assignments_no_delete AS
    ON DELETE TO trading.portfolio_assignments DO INSTEAD NOTHING;

COMMENT ON TABLE trading.portfolio_assignments IS
    'Append-only audit of book membership changes: who, when, from, to, why, '
    'and whether a stated consequence was acknowledged. UPDATE and DELETE are '
    'refused by rule. Written by AlgoLens (inserts only).';

COMMIT;
