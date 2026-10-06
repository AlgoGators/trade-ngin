#!/usr/bin/env bash
set -euo pipefail

psql_bin="${PSQL_BIN:-psql}"
if [[ -z "${QT_DISPATCH_TEST_DSN:-}" || -z "${MIGRATION_TEST_DB:-}" ]]; then
    echo "Set QT_DISPATCH_TEST_DSN and MIGRATION_TEST_DB to an owned disposable test database." >&2
    exit 2
fi
actual_db="$(${psql_bin} "${QT_DISPATCH_TEST_DSN}" -XAtc 'select current_database()')"
if [[ "${actual_db}" != "${MIGRATION_TEST_DB}" || "${actual_db}" != *test* ]]; then
    echo "Refusing non-test or mismatched database: ${actual_db}" >&2
    exit 2
fi

root="$(cd "$(dirname "$0")" && pwd)"
${psql_bin} "${QT_DISPATCH_TEST_DSN}" -Xv ON_ERROR_STOP=1 <<'SQL'
CREATE SCHEMA trading;
CREATE TABLE trading.strategy_registry(
 id text PRIMARY KEY,strategy_type text NOT NULL,portfolio_id text NOT NULL,
 lifecycle text NOT NULL,is_active boolean NOT NULL);
CREATE TABLE trading.strategy_book_memberships(strategy_id text,portfolio_id text,
 PRIMARY KEY(strategy_id,portfolio_id));
CREATE TABLE trading.qt_model_seed_publications(
 publication_id uuid PRIMARY KEY,portfolio_id text NOT NULL,strategy_id text NOT NULL,
 source_day date NOT NULL,created_at timestamptz NOT NULL DEFAULT clock_timestamp());
CREATE TABLE trading.qt_decisions(
 decision_id uuid PRIMARY KEY,book_id text NOT NULL,source_day date NOT NULL,
 status text NOT NULL,model_publication_id uuid NOT NULL,created_at timestamptz NOT NULL DEFAULT clock_timestamp());
CREATE TABLE trading.qt_desk_receipts(
 decision_id uuid PRIMARY KEY,attempt_id uuid NOT NULL,status text NOT NULL);
INSERT INTO trading.strategy_registry VALUES
 ('trendfollowing','LIVE_TREND_FOLLOWING','CONSERVATIVE_PORTFOLIO','live',true),
 ('investor','LIVE_TREND_FOLLOWING','INVESTOR_A','live',true);
INSERT INTO trading.qt_model_seed_publications VALUES
 ('10000000-0000-4000-8000-000000000001','CONSERVATIVE_PORTFOLIO','LIVE_TREND_FOLLOWING','2026-10-06',clock_timestamp()),
 ('10000000-0000-4000-8000-000000000002','INVESTOR_A','LIVE_TREND_FOLLOWING','2026-10-06',clock_timestamp());
INSERT INTO trading.qt_decisions VALUES
 ('20000000-0000-4000-8000-000000000001','CONSERVATIVE_PORTFOLIO','2026-10-06','confirmed_decision','10000000-0000-4000-8000-000000000001',clock_timestamp()),
 ('20000000-0000-4000-8000-000000000002','INVESTOR_A','2026-10-06','confirmed_decision','10000000-0000-4000-8000-000000000002',clock_timestamp());
SQL

${psql_bin} "${QT_DISPATCH_TEST_DSN}" -Xv ON_ERROR_STOP=1 -f "${root}/029_qt_desk_dispatch_jobs.sql" >/dev/null

${psql_bin} "${QT_DISPATCH_TEST_DSN}" -Xv ON_ERROR_STOP=1 <<'SQL'
DO $$
DECLARE claimed record;
BEGIN
  IF EXISTS (
    SELECT 1 FROM pg_proc p, LATERAL aclexplode(coalesce(
      p.proacl, acldefault('f', p.proowner))) privilege
     WHERE p.oid IN (
       'trading.enqueue_qt_desk_dispatch(uuid,uuid,uuid,uuid,uuid)'::regprocedure,
       'trading.claim_qt_desk_dispatch(uuid,interval)'::regprocedure,
       'trading.finish_qt_desk_dispatch(uuid,uuid,text,text,text,interval)'::regprocedure,
       'trading.renew_qt_desk_dispatch(uuid,uuid,interval)'::regprocedure)
       AND privilege.grantee=0 AND privilege.privilege_type='EXECUTE') THEN
    RAISE EXCEPTION 'dispatcher definer function executable by PUBLIC';
  END IF;
  PERFORM trading.enqueue_qt_desk_dispatch(
    '20000000-0000-4000-8000-000000000001','30000000-0000-4000-8000-000000000001',
    '40000000-0000-4000-8000-000000000001','50000000-0000-4000-8000-000000000001',
    '60000000-0000-4000-8000-000000000001');
  BEGIN
    PERFORM trading.enqueue_qt_desk_dispatch(
      '20000000-0000-4000-8000-000000000002','30000000-0000-4000-8000-000000000002',
      '40000000-0000-4000-8000-000000000002','50000000-0000-4000-8000-000000000002',
      '60000000-0000-4000-8000-000000000002');
    RAISE EXCEPTION 'wrong scope was enqueued';
  EXCEPTION WHEN SQLSTATE 'P0001' THEN
    IF SQLERRM <> 'qt_dispatch_scope_unsupported' THEN RAISE; END IF;
  END;

  SELECT * INTO claimed FROM trading.claim_qt_desk_dispatch(
    '70000000-0000-4000-8000-000000000001','1 millisecond');
  IF claimed.decision_id <> '20000000-0000-4000-8000-000000000001'::uuid
     OR claimed.attempt_number <> 1 THEN RAISE EXCEPTION 'first claim mismatch'; END IF;
  IF EXISTS(SELECT 1 FROM trading.claim_qt_desk_dispatch(
    '70000000-0000-4000-8000-000000000002','5 minutes')) THEN
    RAISE EXCEPTION 'unexpired claim was stolen';
  END IF;
  PERFORM pg_sleep(0.01);
  SELECT * INTO claimed FROM trading.claim_qt_desk_dispatch(
    '70000000-0000-4000-8000-000000000002','5 minutes');
  IF claimed.attempt_number <> 2 THEN RAISE EXCEPTION 'expired lease was not reclaimed'; END IF;
  PERFORM trading.finish_qt_desk_dispatch(claimed.decision_id,
    '70000000-0000-4000-8000-000000000002','run','retry','tool_exit_6','0 seconds');
  SELECT * INTO claimed FROM trading.claim_qt_desk_dispatch(
    '70000000-0000-4000-8000-000000000003','5 minutes');
  IF claimed.attempt_number <> 3 THEN RAISE EXCEPTION 'retry claim mismatch'; END IF;
  PERFORM trading.finish_qt_desk_dispatch(claimed.decision_id,
    '70000000-0000-4000-8000-000000000003','bootstrap','dead_letter','first_day_bootstrap_unavailable',NULL);
  IF EXISTS(SELECT 1 FROM trading.claim_qt_desk_dispatch(
    '70000000-0000-4000-8000-000000000004','5 minutes')) THEN
    RAISE EXCEPTION 'terminal job was reclaimed';
  END IF;
  IF (SELECT count(*) FROM trading.qt_desk_dispatch_attempts) <> 5 THEN
    RAISE EXCEPTION 'attempt event ledger mismatch';
  END IF;
END $$;

DO $$ BEGIN
  BEGIN
    UPDATE trading.qt_desk_dispatch_attempts SET error_code='changed';
    RAISE EXCEPTION 'attempt update accepted';
  EXCEPTION WHEN SQLSTATE 'P0001' THEN
    IF SQLERRM <> 'QT dispatch attempts are append only' THEN RAISE; END IF;
  END;
END $$;
SQL

if "${psql_bin}" "${QT_DISPATCH_TEST_DSN}" -Xv ON_ERROR_STOP=1 \
    -f "${root}/029_qt_desk_dispatch_jobs_rollback.sql" >/dev/null 2>&1; then
    echo "Rollback should refuse populated dispatcher evidence." >&2
    exit 1
fi

echo "qt-desk-dispatch migration: PASS"
