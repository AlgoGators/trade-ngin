-- EXPLICITLY SYNTHETIC deterministic capture inputs, never a production migration.
-- Install AFTER base.sql, BEFORE exact native migrations, in an owned empty DB.
-- All provider definitions/defaults/sequence/settings remain captured raw evidence.
DO $$ BEGIN
 IF current_database() !~ '^algolens_test_[a-zA-Z0-9_]+$' OR inet_server_addr() IS NOT NULL
    OR EXISTS (SELECT 1 FROM auth.users) OR EXISTS (SELECT 1 FROM trading.positions)
 THEN RAISE EXCEPTION 'owned empty synthetic capture database required'; END IF;
END $$;
CREATE SEQUENCE trading.qt_capture_uuid_seq MINVALUE 1 MAXVALUE 281474976710655 START 1;
CREATE FUNCTION trading.gen_random_uuid() RETURNS uuid LANGUAGE SQL VOLATILE AS $$
 SELECT ('c0decafe-0000-4000-8000-' || lpad(to_hex(nextval('trading.qt_capture_uuid_seq')),12,'0'))::uuid
$$;
CREATE FUNCTION trading.now() RETURNS timestamptz LANGUAGE SQL IMMUTABLE AS $$
 SELECT '2026-09-26T12:00:00Z'::timestamptz
$$;
CREATE FUNCTION trading.clock_timestamp() RETURNS timestamptz LANGUAGE SQL IMMUTABLE AS $$
 SELECT '2026-09-26T12:00:00Z'::timestamptz
$$;
SET search_path = trading,pg_catalog;
-- Persist ONLY on this explicitly owned disposable DB for the real child sessions.
DO $$ BEGIN EXECUTE format('ALTER DATABASE %I SET search_path = trading,pg_catalog',current_database()); END $$;
-- Base was installed before the providers. Rebind only its volatile defaults;
-- later exact migrations bind now()/clock_timestamp() to the visible providers.
ALTER TABLE trading.positions ALTER COLUMN updated_at SET DEFAULT trading.now();
ALTER TABLE trading.live_run_metadata ALTER COLUMN created_at SET DEFAULT trading.now();
ALTER TABLE trading.run_inputs ALTER COLUMN recorded_at SET DEFAULT trading.now();
