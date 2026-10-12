-- 028_ops_data_ready.sql
--
-- Data readiness for the QT evening run (interim design of
-- https://github.com/AlgoGators/trade-ngin/issues/191; #192 later carries the same rows over
-- NATS). Additive, transactional and idempotent: re-applying it is a no-op.
--
--   1. Schema ops and table ops.data_ready: one row per (dataset, bar_date), written by data-ngin
--      when its pull of that dataset for that bar date finishes (an upsert: a re-pull rewrites
--      the row and moves finished_at, which makes the engine-rpc scheduler rebuild the next
--      trading day's books once more). status: complete (every expected symbol loaded), partial
--      (missing names the symbols, as futures_data.ohlcv_1d.symbol spells them), failed.
--      The scheduler reads dataset 'futures_ohlcv_1d'.
--   2. Grants:
--        - qt_engine_app and svc_trade_ngin (the engine and its desk service): USAGE on ops,
--          SELECT on ops.data_ready. They never write it.
--        - postgres (data-ngin connects as postgres today): SELECT, INSERT, UPDATE. A superuser
--          needs none of this; it is written down so the grant moves with data-ngin when it gets
--          its own role (svc_data_ngin: grant the same three then).
--      Roles that do not exist on the server are skipped.
--
-- Apply (as postgres, on new_algo_data):
--     psql -X -v ON_ERROR_STOP=1 -d new_algo_data -f migrations/028_ops_data_ready.sql

BEGIN;

DO $$
DECLARE
    bad text;
BEGIN
    IF to_regclass('ops.data_ready') IS NULL THEN
        RETURN;
    END IF;
    -- Re-applied over a table of another shape: refuse instead of silently keeping it.
    SELECT string_agg(c, ', ') INTO bad
    FROM unnest(ARRAY['dataset', 'bar_date', 'expected', 'loaded', 'missing', 'status', 'run_id',
                      'finished_at']) AS c
    WHERE NOT EXISTS (SELECT 1 FROM information_schema.columns
                      WHERE table_schema = 'ops' AND table_name = 'data_ready'
                        AND column_name = c);
    IF bad IS NOT NULL THEN
        RAISE EXCEPTION 'ops.data_ready exists without the column(s): %', bad;
    END IF;
END $$;

CREATE SCHEMA IF NOT EXISTS ops;

CREATE TABLE IF NOT EXISTS ops.data_ready (
    dataset     text        NOT NULL,
    bar_date    date        NOT NULL,
    expected    int         NOT NULL,
    loaded      int         NOT NULL,
    missing     text[]      NOT NULL DEFAULT '{}',
    status      text        NOT NULL CHECK (status IN ('complete', 'partial', 'failed')),
    run_id      text,
    finished_at timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (dataset, bar_date)
);

COMMENT ON SCHEMA ops IS
    'Operational state shared between the AlgoGators services (migration 028).';
COMMENT ON TABLE ops.data_ready IS
    'Migration 028 (issue #191): data-ngin''s pull of a dataset for a bar date is done. Upserted by data-ngin; read by the engine-rpc scheduler, which builds the next trading day''s QT books once per finished_at.';
COMMENT ON COLUMN ops.data_ready.missing IS
    'Symbols expected but not loaded (futures_data.ohlcv_1d.symbol spelling); empty when complete.';
COMMENT ON COLUMN ops.data_ready.finished_at IS
    'When the pull finished. A re-pull moves it, and the scheduler rebuilds the unpublished target day.';

DO $$
DECLARE
    r text;
BEGIN
    FOREACH r IN ARRAY ARRAY['qt_engine_app', 'svc_trade_ngin'] LOOP
        IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = r) THEN
            EXECUTE format('GRANT USAGE ON SCHEMA ops TO %I', r);
            EXECUTE format('GRANT SELECT ON ops.data_ready TO %I', r);
        END IF;
    END LOOP;
    -- data-ngin (connects as postgres today).
    GRANT USAGE ON SCHEMA ops TO postgres;
    GRANT SELECT, INSERT, UPDATE ON ops.data_ready TO postgres;
END $$;

COMMIT;
