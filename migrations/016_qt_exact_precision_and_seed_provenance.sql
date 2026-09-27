-- Source-only migration. Apply only after reviewed 015, inside an approved rollout.
-- No historical MODEL baseline is inferred or backfilled.
BEGIN;
LOCK TABLE trading.positions IN ACCESS EXCLUSIVE MODE;

DO $preflight$
DECLARE
    p oid := to_regclass('trading.positions');
    stream_check text;
BEGIN
    IF p IS NULL OR to_regclass('trading.qt_model_seed_publications') IS NOT NULL
       OR to_regclass('trading.qt_storage_capabilities') IS NOT NULL THEN
        RAISE EXCEPTION 'qt016 prerequisites or reserved table shape unsupported';
    END IF;
    SELECT regexp_replace(pg_get_constraintdef(oid),'\s','','g') INTO stream_check
      FROM pg_constraint WHERE conrelid=p AND conname='positions_portfolio_type_check'
        AND contype='c' AND convalidated;
    IF stream_check IS DISTINCT FROM
      'CHECK((portfolio_type=ANY(ARRAY[''system''::text,''qt''::text,''benchmark''::text,''benchmark_rebench''::text,''benchmark_frozen_shadow''::text,''qt_proposal''::text])))'
      OR (SELECT count(*) FROM pg_constraint WHERE conrelid=p AND conname='positions_portfolio_type_check')<>1
      OR NOT EXISTS (SELECT 1 FROM pg_constraint WHERE conrelid=p AND conname='positions_pkey'
                     AND contype='p') THEN
        RAISE EXCEPTION 'qt016 requires reviewed proposal015 stream and key';
    END IF;
    IF EXISTS (
      SELECT 1 FROM (VALUES ('quantity'),('average_price')) wanted(name)
      LEFT JOIN pg_attribute a ON a.attrelid=p AND a.attname=wanted.name
        AND a.attnum>0 AND NOT a.attisdropped
      WHERE a.attnum IS NULL OR format_type(a.atttypid,a.atttypmod)<>'numeric(20,6)'
        OR NOT a.attnotnull
    ) THEN RAISE EXCEPTION 'qt016 exact storage predecessor unsupported'; END IF;
    IF EXISTS (SELECT 1 FROM trading.positions
      WHERE abs(quantity)>=1000000000000 OR abs(average_price)>=1000000000000) THEN
        RAISE EXCEPTION 'qt016 widening exceeds numeric(20,8) integer range';
    END IF;
    -- Decimal8 stores the scaled value in signed int64, a narrower domain
    -- than numeric(20,8). Compare as exact SQL numeric before changing type.
    IF EXISTS (SELECT 1 FROM trading.positions
      WHERE quantity < -92233720368.54775808::numeric
         OR quantity >  92233720368.54775807::numeric
         OR average_price < -92233720368.54775808::numeric
         OR average_price >  92233720368.54775807::numeric) THEN
        RAISE EXCEPTION 'qt016 widening exceeds signed Decimal8 raw range';
    END IF;
END $preflight$;

ALTER TABLE trading.positions
  ALTER COLUMN quantity TYPE numeric(20,8),
  ALTER COLUMN average_price TYPE numeric(20,8);

-- Existing proposals remain unresolved. A caller cannot choose or replay a
-- revision, even for a same-value UPDATE.
ALTER TABLE trading.positions ADD COLUMN qt_proposal_revision uuid;
ALTER TABLE trading.positions ADD CONSTRAINT positions_qt_proposal_revision_check
  CHECK (portfolio_type='qt_proposal' OR qt_proposal_revision IS NULL);
CREATE UNIQUE INDEX qt_proposal_revision_unique ON trading.positions(qt_proposal_revision)
  WHERE qt_proposal_revision IS NOT NULL;
CREATE FUNCTION trading.stamp_qt_proposal_revision() RETURNS trigger
LANGUAGE plpgsql AS $$ BEGIN IF NEW.portfolio_type = 'qt_proposal' THEN NEW.qt_proposal_revision := gen_random_uuid(); ELSE NEW.qt_proposal_revision := NULL; END IF; RETURN NEW; END $$;
CREATE TRIGGER qt_proposal_revision_stamp BEFORE INSERT OR UPDATE
ON trading.positions FOR EACH ROW
EXECUTE FUNCTION trading.stamp_qt_proposal_revision();

CREATE TABLE trading.qt_storage_capabilities (
    capability_name text PRIMARY KEY,
    capability_version bigint NOT NULL CHECK (capability_version>0),
    installed_at timestamptz NOT NULL DEFAULT now()
);
INSERT INTO trading.qt_storage_capabilities (capability_name,capability_version)
VALUES ('qt_exact_decimal8',1);

CREATE TABLE trading.qt_model_seed_publications (
    publication_id uuid PRIMARY KEY,
    attempt_id uuid UNIQUE,
    portfolio_id text NOT NULL CHECK (length(btrim(portfolio_id))>0),
    strategy_id text NOT NULL CHECK (length(btrim(strategy_id))>0),
    source_day date NOT NULL,
    publication_version bigint NOT NULL CHECK (publication_version>0),
    system_components jsonb NOT NULL CHECK (jsonb_typeof(system_components)='array'
                                            AND jsonb_array_length(system_components)>0),
    seed_digest text NOT NULL CHECK (seed_digest ~ '^[0-9a-f]{64}$'),
    proposal_components jsonb NOT NULL CHECK (jsonb_typeof(proposal_components)='array'),
    proposal_manifest_digest text NOT NULL CHECK (proposal_manifest_digest ~ '^[0-9a-f]{64}$'),
    producer_version text NOT NULL CHECK (length(btrim(producer_version))>0),
    created_at timestamptz NOT NULL DEFAULT now(),
    UNIQUE (portfolio_id,source_day,publication_version),
    UNIQUE (portfolio_id,strategy_id,source_day,attempt_id)
);

CREATE FUNCTION trading.refuse_qt_seed_mutation() RETURNS trigger
LANGUAGE plpgsql AS $$ BEGIN
    RAISE EXCEPTION 'qt model seed and storage capability are immutable';
END $$;
CREATE TRIGGER qt_model_seed_immutable BEFORE UPDATE OR DELETE
ON trading.qt_model_seed_publications FOR EACH ROW
EXECUTE FUNCTION trading.refuse_qt_seed_mutation();
CREATE TRIGGER qt_model_seed_no_truncate BEFORE TRUNCATE
ON trading.qt_model_seed_publications FOR EACH STATEMENT
EXECUTE FUNCTION trading.refuse_qt_seed_mutation();
CREATE TRIGGER qt_storage_capability_immutable BEFORE UPDATE OR DELETE
ON trading.qt_storage_capabilities FOR EACH ROW
EXECUTE FUNCTION trading.refuse_qt_seed_mutation();
CREATE TRIGGER qt_storage_capability_no_truncate BEFORE TRUNCATE
ON trading.qt_storage_capabilities FOR EACH STATEMENT
EXECUTE FUNCTION trading.refuse_qt_seed_mutation();
COMMIT;
