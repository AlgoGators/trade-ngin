-- Preserve exact Decimal8 position PnL and all legacy monetary values.
-- No data backfill, policy/grant/profile activation or producer execution.
BEGIN;
LOCK TABLE trading.positions, trading.qt_storage_capabilities IN ACCESS EXCLUSIVE MODE;
DO $preflight$
DECLARE p oid := to_regclass('trading.positions');
BEGIN
    IF p IS NULL OR NOT EXISTS (SELECT 1 FROM trading.qt_storage_capabilities
      WHERE capability_name='qt_exact_decimal8' AND capability_version=1)
      OR EXISTS (SELECT 1 FROM trading.qt_storage_capabilities
      WHERE capability_name='qt_position_accounting_decimal8') THEN
      RAISE EXCEPTION 'qt020 exact predecessor capability unsupported';
    END IF;
    IF EXISTS (SELECT 1 FROM (VALUES
        ('quantity','numeric(20,8)'),('average_price','numeric(20,8)'),
        ('daily_realized_pnl','numeric(20,6)'),('daily_unrealized_pnl','numeric(20,6)')) expected(name,kind)
        LEFT JOIN pg_attribute a ON a.attrelid=p AND a.attname=expected.name
          AND a.attnum>0 AND NOT a.attisdropped
        WHERE a.attnum IS NULL OR format_type(a.atttypid,a.atttypmod)<>expected.kind OR NOT a.attnotnull)
    THEN RAISE EXCEPTION 'qt020 position column predecessor unsupported'; END IF;
    IF NOT EXISTS (SELECT 1 FROM pg_proc f JOIN pg_language l ON l.oid=f.prolang
      WHERE f.oid=to_regprocedure('trading.refuse_qt_seed_mutation()')
        AND f.prorettype='trigger'::regtype AND f.pronargs=0 AND l.lanname='plpgsql'
        AND regexp_replace(f.prosrc,'\s','','g')=
          'BEGINRAISEEXCEPTION''qtmodelseedandstoragecapabilityareimmutable'';END')
      OR (SELECT count(*) FROM pg_trigger t
          WHERE t.tgrelid='trading.qt_storage_capabilities'::regclass AND NOT t.tgisinternal
            AND t.tgfoid=to_regprocedure('trading.refuse_qt_seed_mutation()') AND t.tgenabled='O'
            AND t.tgqual IS NULL AND t.tgattr::text='' AND t.tgnargs=0
            AND octet_length(t.tgargs)=0 AND t.tgconstraint=0
            AND NOT t.tgdeferrable AND NOT t.tginitdeferred
            AND ((t.tgname='qt_storage_capability_immutable' AND t.tgtype=27)
              OR (t.tgname='qt_storage_capability_no_truncate' AND t.tgtype=34)))<>2
    THEN RAISE EXCEPTION 'qt020 immutable capability guards unsupported'; END IF;
END $preflight$;
ALTER TABLE trading.positions
    ALTER COLUMN daily_realized_pnl TYPE numeric(22,8),
    ALTER COLUMN daily_unrealized_pnl TYPE numeric(22,8);
INSERT INTO trading.qt_storage_capabilities(capability_name,capability_version)
    VALUES('qt_position_accounting_decimal8',1);
COMMIT;
