\set ON_ERROR_STOP on

-- Synthetic P0 HOUSE substrate only. The caller must create this database by
-- cloning the owned issues-121/124 fixture immediately before each OLD or NEW
-- run. This fixture intentionally contains no investor-book registration: the
-- legacy EQUITY_MR_PORTFOLIO must exercise the governed QT house path.
DO $$
BEGIN
    IF current_database() <> 'trade_ngin_121124_p0_test'
       OR inet_server_addr() IS NOT NULL THEN
        RAISE EXCEPTION 'owned local trade_ngin_121124_p0_test database required';
    END IF;
END $$;

ALTER TABLE trading.live_results
    ADD COLUMN IF NOT EXISTS total_dividend_income numeric DEFAULT 0;
ALTER TABLE trading.live_results
    ADD COLUMN IF NOT EXISTS gross_leverage numeric;
ALTER TABLE trading.live_results
    ADD COLUMN IF NOT EXISTS created_at timestamptz DEFAULT now();

-- Pin globally allocated row IDs in a range reserved for this disposable gate,
-- independent of whatever investor scenarios populated the cloned fixture.
ALTER SEQUENCE trading.live_results_id_seq RESTART WITH 121124000;
ALTER SEQUENCE trading.risk_limits_id_seq RESTART WITH 121124000;

CREATE SEQUENCE IF NOT EXISTS trading.p0_uuid_seq
    MINVALUE 1 MAXVALUE 281474976710655 START 1;
ALTER SEQUENCE trading.p0_uuid_seq RESTART WITH 1;

CREATE OR REPLACE FUNCTION trading.p0_uuid()
RETURNS uuid LANGUAGE SQL VOLATILE AS $$
    SELECT ('12112400-0000-4000-8000-' ||
            lpad(to_hex(nextval('trading.p0_uuid_seq')), 12, '0'))::uuid
$$;

-- Runtime capture code calls gen_random_uuid() explicitly. Make the test
-- schema the first search-path element so those calls and column defaults use
-- the same deterministic provider in every child connection.
CREATE OR REPLACE FUNCTION trading.gen_random_uuid()
RETURNS uuid LANGUAGE SQL VOLATILE AS $$
    SELECT trading.p0_uuid()
$$;
DO $$
BEGIN
    EXECUTE format(
        'ALTER DATABASE %I SET search_path = trading, pg_catalog',
        current_database());
END $$;

CREATE OR REPLACE FUNCTION trading.p0_now()
RETURNS timestamptz LANGUAGE SQL IMMUTABLE AS $$
    SELECT '2026-10-01T00:00:00Z'::timestamptz
$$;

CREATE OR REPLACE FUNCTION trading.now()
RETURNS timestamptz LANGUAGE SQL IMMUTABLE AS $$
    SELECT trading.p0_now()
$$;

CREATE OR REPLACE FUNCTION trading.clock_timestamp()
RETURNS timestamptz LANGUAGE SQL IMMUTABLE AS $$
    SELECT trading.p0_now()
$$;

-- The frozen OLD runtime compares pg_get_expr output literally. PostgreSQL
-- qualifies built-ins when this deterministic schema also defines now(), even
-- though the stored default is unchanged. Normalize only that catalog display
-- function inside this disposable fixture; NEW fixes the runtime check itself.
CREATE OR REPLACE FUNCTION trading.pg_get_expr(pg_node_tree, oid)
RETURNS text LANGUAGE SQL STABLE AS $$
    SELECT replace(
        replace(pg_catalog.pg_get_expr($1,$2), 'pg_catalog.', ''),
        'trading.p0_now()', 'now()')
$$;

ALTER TABLE trading.investor_books
    ALTER COLUMN book_id SET DEFAULT trading.p0_uuid(),
    ALTER COLUMN created_at SET DEFAULT trading.p0_now(),
    ALTER COLUMN updated_at SET DEFAULT trading.p0_now();
ALTER TABLE trading.investor_book_publications
    ALTER COLUMN publication_id SET DEFAULT trading.p0_uuid(),
    ALTER COLUMN published_at SET DEFAULT trading.p0_now();
ALTER TABLE trading.positions
    ALTER COLUMN updated_at SET DEFAULT trading.p0_now();
ALTER TABLE trading.live_results
    ALTER COLUMN created_at SET DEFAULT trading.p0_now();
ALTER TABLE trading.live_run_metadata
    ALTER COLUMN created_at SET DEFAULT trading.p0_now();
ALTER TABLE trading.run_inputs
    ALTER COLUMN recorded_at SET DEFAULT trading.p0_now();
ALTER TABLE trading.risk_limits
    ALTER COLUMN published_at SET DEFAULT trading.p0_now();
ALTER TABLE trading.corp_action_applied
    ALTER COLUMN applied_at SET DEFAULT trading.p0_now();

-- The source fixture also drives investor-book verification. Remove that
-- registration from this disposable clone before admitting the legacy house
-- owner. Listing all three tables in one TRUNCATE satisfies their RESTRICT
-- foreign keys while preserving the production immutability triggers.
TRUNCATE TABLE trading.investor_book_access,
               trading.investor_book_publications,
               trading.investor_book_strategies,
               trading.investor_books;

DELETE FROM trading.strategy_book_memberships
WHERE strategy_id = 'p0-equity-house';
DELETE FROM trading.strategy_registry
WHERE id = 'p0-equity-house';

INSERT INTO trading.strategy_registry
    (id, strategy_type, portfolio_id, lifecycle, is_active,
     runtime_revision)
VALUES
    ('p0-equity-house', 'LIVE_EQUITY_MEAN_REVERSION',
     'EQUITY_MR_PORTFOLIO', 'live', true, 0);

-- Adding an ordinary membership bumps runtime_revision, but this fixture must
-- model the original uncontrolled owner at revision zero. Disable only that
-- audit trigger for this synthetic seed and restore it immediately.
ALTER TABLE trading.strategy_book_memberships
    DISABLE TRIGGER runtime_membership_revision;
INSERT INTO trading.strategy_book_memberships
    (strategy_id, portfolio_id)
VALUES
    ('p0-equity-house', 'EQUITY_MR_PORTFOLIO');
ALTER TABLE trading.strategy_book_memberships
    ENABLE TRIGGER runtime_membership_revision;

DELETE FROM equities_data.ohlcv_1d WHERE symbol = 'SYN';
DELETE FROM equities_data.corporate_action WHERE ticker = 'SYN';
INSERT INTO equities_data.ohlcv_1d
    (symbol, time, open, high, low, close, volume, div_cash,
     split_factor, delisting_date)
VALUES
    ('SYN','2026-10-30T00:00:00Z',200,200,200,200,1000000,0,1,NULL),
    ('SYN','2026-11-02T00:00:00Z',200,200,200,200,1000000,0,1,NULL),
    ('SYN','2026-11-03T00:00:00Z', 90, 90, 90, 90,1000000,0,1,NULL);

DO $$
BEGIN
    IF EXISTS (
        SELECT 1 FROM trading.investor_books
        WHERE portfolio_id = 'EQUITY_MR_PORTFOLIO'
    ) THEN
        RAISE EXCEPTION 'P0 fixture accidentally classified as investor book';
    END IF;
    IF NOT EXISTS (
        SELECT 1 FROM trading.strategy_registry
        WHERE id = 'p0-equity-house'
          AND strategy_type = 'LIVE_EQUITY_MEAN_REVERSION'
          AND portfolio_id = 'EQUITY_MR_PORTFOLIO'
          AND lifecycle = 'live'
          AND is_active
          AND runtime_revision = 0
    ) THEN
        RAISE EXCEPTION 'P0 house runtime owner is not admissible';
    END IF;
END $$;
