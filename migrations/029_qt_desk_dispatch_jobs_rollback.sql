BEGIN;
DO $$ BEGIN
  IF to_regclass('trading.qt_desk_dispatch_attempts') IS NOT NULL
     AND EXISTS(SELECT 1 FROM trading.qt_desk_dispatch_attempts) THEN
    RAISE EXCEPTION 'refusing to discard QT dispatch attempt evidence';
  END IF;
  IF to_regclass('trading.qt_desk_dispatch_jobs') IS NOT NULL
     AND EXISTS(SELECT 1 FROM trading.qt_desk_dispatch_jobs) THEN
    RAISE EXCEPTION 'refusing to discard QT dispatch jobs';
  END IF;
END $$;
DROP FUNCTION IF EXISTS trading.finish_qt_desk_dispatch(uuid,uuid,text,text,text,interval);
DROP FUNCTION IF EXISTS trading.renew_qt_desk_dispatch(uuid,uuid,interval);
DROP FUNCTION IF EXISTS trading.claim_qt_desk_dispatch(uuid,interval);
DROP FUNCTION IF EXISTS trading.enqueue_qt_desk_dispatch(uuid,uuid,uuid,uuid,uuid);
DROP TABLE IF EXISTS trading.qt_desk_dispatch_attempts;
DROP TABLE IF EXISTS trading.qt_desk_dispatch_jobs;
DROP FUNCTION IF EXISTS trading.qt_dispatch_job_guard();
DROP FUNCTION IF EXISTS trading.qt_dispatch_attempts_immutable();
COMMIT;
