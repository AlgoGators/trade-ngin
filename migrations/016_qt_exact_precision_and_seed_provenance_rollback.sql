-- A downgrade discards no published provenance and no eight-place value.
BEGIN;
LOCK TABLE trading.positions, trading.qt_model_seed_publications,
    trading.qt_storage_capabilities IN ACCESS EXCLUSIVE MODE;

DO $preflight$
DECLARE p oid := to_regclass('trading.positions');
BEGIN
    IF p IS NULL OR to_regclass('trading.qt_model_seed_publications') IS NULL
       OR to_regclass('trading.qt_storage_capabilities') IS NULL THEN
        RAISE EXCEPTION 'qt016 rollback capability shape missing';
    END IF;
    IF EXISTS (SELECT 1 FROM (VALUES ('quantity'),('average_price')) wanted(name)
      LEFT JOIN pg_attribute a ON a.attrelid=p AND a.attname=wanted.name
        AND a.attnum>0 AND NOT a.attisdropped
      WHERE a.attnum IS NULL OR format_type(a.atttypid,a.atttypmod)<>'numeric(20,8)') THEN
        RAISE EXCEPTION 'qt016 rollback storage shape unsupported';
    END IF;
    IF EXISTS (SELECT 1 FROM trading.qt_model_seed_publications) THEN
        RAISE EXCEPTION 'qt016 rollback refuses loss of model seed provenance';
    END IF;
    IF EXISTS (SELECT 1 FROM trading.positions WHERE qt_proposal_revision IS NOT NULL) THEN
        RAISE EXCEPTION 'qt016 rollback refuses loss of proposal revision provenance';
    END IF;
    IF EXISTS (SELECT 1 FROM trading.positions
      WHERE quantity<>round(quantity,6) OR average_price<>round(average_price,6)
        OR abs(quantity)>=100000000000000 OR abs(average_price)>=100000000000000) THEN
        RAISE EXCEPTION 'qt016 rollback refuses six-place value loss';
    END IF;
END $preflight$;

DROP TABLE trading.qt_model_seed_publications;
DROP TABLE trading.qt_storage_capabilities;
DROP FUNCTION trading.refuse_qt_seed_mutation();
DROP TRIGGER qt_proposal_revision_stamp ON trading.positions;
DROP FUNCTION trading.stamp_qt_proposal_revision();
DROP INDEX trading.qt_proposal_revision_unique;
ALTER TABLE trading.positions DROP CONSTRAINT positions_qt_proposal_revision_check;
ALTER TABLE trading.positions DROP COLUMN qt_proposal_revision;
ALTER TABLE trading.positions
  ALTER COLUMN quantity TYPE numeric(20,6),
  ALTER COLUMN average_price TYPE numeric(20,6);
COMMIT;
