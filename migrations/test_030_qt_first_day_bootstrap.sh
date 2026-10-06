#!/usr/bin/env bash
set -euo pipefail
psql_bin="${PSQL_BIN:-psql}"
if [[ -z "${QT_FIRST_DAY_TEST_DSN:-}" || -z "${MIGRATION_TEST_DB:-}" ]]; then
  echo "Set QT_FIRST_DAY_TEST_DSN and MIGRATION_TEST_DB to an owned disposable test database." >&2
  exit 2
fi
actual_db="$(${psql_bin} "${QT_FIRST_DAY_TEST_DSN}" -XAtc 'select current_database()')"
[[ "${actual_db}" == "${MIGRATION_TEST_DB}" && "${actual_db}" == *test* ]] || exit 2
root="$(cd "$(dirname "$0")" && pwd)"
${psql_bin} "${QT_FIRST_DAY_TEST_DSN}" -Xv ON_ERROR_STOP=1 <<'SQL'
CREATE SCHEMA trading;
CREATE TABLE trading.qt_model_seed_publications(publication_id uuid PRIMARY KEY);
CREATE TABLE trading.qt_decisions(decision_id uuid PRIMARY KEY);
CREATE TABLE trading.qt_desk_accounting_inputs(payload jsonb NOT NULL);
ALTER TABLE trading.qt_desk_accounting_inputs ADD CONSTRAINT qt_desk_accounting_inputs_payload_check
 CHECK(jsonb_typeof(payload)='object' AND payload->>'schema_version' IN
 ('qt-futures-accounting-input/v1','qt-futures-accounting-input/v2',
  'qt-equity-accounting-input/v1','qt-equity-accounting-input-empty-owner/v2'));
INSERT INTO trading.qt_model_seed_publications VALUES('10000000-0000-4000-8000-000000000001');
INSERT INTO trading.qt_decisions VALUES('20000000-0000-4000-8000-000000000001'),('20000000-0000-4000-8000-000000000002');
SQL
${psql_bin} "${QT_FIRST_DAY_TEST_DSN}" -Xv ON_ERROR_STOP=1 -f "${root}/030_qt_first_day_bootstrap.sql" >/dev/null
${psql_bin} "${QT_FIRST_DAY_TEST_DSN}" -Xv ON_ERROR_STOP=1 <<'SQL'
INSERT INTO trading.qt_first_day_anchors(anchor_id,decision_id,book_id,source_day,
 model_publication_id,execution_policy_revision,evaluation_policy_revision,
 content_digest,payload) VALUES(
 '30000000-0000-4000-8000-000000000001','20000000-0000-4000-8000-000000000001',
 'CONSERVATIVE_PORTFOLIO','2026-10-06','10000000-0000-4000-8000-000000000001',1,1,
 repeat('a',64),jsonb_build_object('schema_version','qt-first-day-anchor/v1',
 'anchor_id','30000000-0000-4000-8000-000000000001','decision_id','20000000-0000-4000-8000-000000000001',
 'book_id','CONSERVATIVE_PORTFOLIO','source_day','2026-10-06',
 'model_publication_id','10000000-0000-4000-8000-000000000001'));
DO $$ BEGIN
 BEGIN
  INSERT INTO trading.qt_first_day_anchors(anchor_id,decision_id,book_id,source_day,
   model_publication_id,execution_policy_revision,evaluation_policy_revision,content_digest,payload)
  SELECT '30000000-0000-4000-8000-000000000002','20000000-0000-4000-8000-000000000002',book_id,source_day,
   model_publication_id,execution_policy_revision,evaluation_policy_revision,content_digest,
   payload || '{"anchor_id":"30000000-0000-4000-8000-000000000002","decision_id":"20000000-0000-4000-8000-000000000002"}'::jsonb
  FROM trading.qt_first_day_anchors;
  RAISE EXCEPTION 'second first-day anchor accepted';
 EXCEPTION WHEN unique_violation THEN NULL; END;
 BEGIN
  UPDATE trading.qt_first_day_anchors SET content_digest=repeat('b',64);
  RAISE EXCEPTION 'anchor update accepted';
 EXCEPTION WHEN SQLSTATE 'P0001' THEN
  IF SQLERRM <> 'QT first-day anchor is append only' THEN RAISE; END IF;
 END;
END $$;
SQL
if ${psql_bin} "${QT_FIRST_DAY_TEST_DSN}" -Xv ON_ERROR_STOP=1 -f "${root}/030_qt_first_day_bootstrap_rollback.sql" >/dev/null 2>&1; then
  echo "Rollback should refuse populated first-day evidence." >&2; exit 1
fi
echo "qt-first-day migration: PASS"
