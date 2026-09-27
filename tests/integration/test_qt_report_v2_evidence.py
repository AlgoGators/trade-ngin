"""Actual generated-v2 native report proof against explicitly synthetic owned PG.

Financial receipts are produced by the actual native accounting processor.
Only the owned fixture administrator changes immutable rows for refusal tests.
"""
from copy import deepcopy
from pathlib import Path
import sys

import pytest
from psycopg2.extras import Json

ROOT = next(parent for parent in Path(__file__).resolve().parents
            if (parent / ".review/trade-ngin-qt").is_dir())
INTEGRATION = ROOT / ".review/trade-ngin-qt/tests/integration"
sys.path.insert(0, str(INTEGRATION))
import test_qt_desk_accounting as producer
import test_qt_desk_storage as storage
import test_runtime_control_schema as substrate

for module, filename in ((producer, "test_qt_desk_accounting.py"),
                         (storage, "test_qt_desk_storage.py"),
                         (substrate, "test_runtime_control_schema.py")):
    assert Path(module.__file__).resolve() == INTEGRATION / filename
assert producer.ROOT.resolve() == ROOT / ".review/trade-ngin-qt"

# Register the actual complete fixture dependency graph on this selected module.
connection = substrate.connection
desk = storage.desk
accounting = producer.accounting
pytestmark = pytest.mark.parametrize("desk", ["futures"], indirect=True)


def state(conn):
    names = ("positions", "executions", "live_results", "equity_curve",
             "qt_execution_observations", "qt_desk_receipts", "qt_desk_results",
             "position_overrides", "desk_run_results", "qt_desk_accounting_inputs",
             "qt_desk_finalization_sources", "qt_decisions", "qt_previews")
    with conn.cursor() as cursor:
        result = {}
        for name in names:
            cursor.execute("SELECT to_jsonb(r)::text FROM trading." + name +
                           " r ORDER BY to_jsonb(r)::text")
            result[name] = cursor.fetchall()
    return result


def processed(accounting):
    conn, _ = accounting
    response = producer.run()
    assert response.returncode == 0 and "DESK_PROCESSED=1" in response.stdout
    with conn.cursor() as cursor:
        cursor.execute("SELECT payload->>'schema_version' FROM trading.qt_execution_observations "
                       "WHERE observation_id=%s AND decision_id=%s",
                       (producer.INPUT, storage.DECISION))
        assert cursor.fetchone() == ("qt-execution/v2",)
    before = state(conn)
    response = storage.report_proof(conn)
    assert response.returncode == 0 and response.stdout == "REPORT_PROOF=1\nWORKFLOW_REQUIRED=1\n"
    assert response.stderr == "" and state(conn) == before
    return conn


def refused_without_writes(conn):
    before = state(conn)
    response = storage.report_proof(conn)
    # Exact ordinary report refusal; a crash/setup error is never counted.
    assert response.returncode == 12 and response.stdout == "REPORT_PROOF=0\n"
    assert response.stderr == "" and state(conn) == before


def test_actual_v2_report_proves_generated_result_without_writes(accounting):
    processed(accounting)


def test_actual_v2_report_refuses_missing_producer_result(accounting):
    conn = processed(accounting)
    with conn.cursor() as cursor:
        cursor.execute("ALTER TABLE trading.desk_run_results DISABLE TRIGGER USER")
        cursor.execute("DELETE FROM trading.desk_run_results WHERE decision_id=%s", (storage.DECISION,))
        assert cursor.rowcount == 1
        cursor.execute("ALTER TABLE trading.desk_run_results ENABLE TRIGGER USER")
    refused_without_writes(conn)


def test_actual_v2_report_refuses_rehashed_financial_result(accounting):
    conn = processed(accounting)
    with conn.cursor() as cursor:
        cursor.execute("SELECT payload FROM trading.desk_run_results WHERE decision_id=%s", (storage.DECISION,))
        output = deepcopy(cursor.fetchone()[0])
        assert output["live_results"][0]["daily_pnl_exact"] != "999"
        output["live_results"][0]["daily_pnl_exact"] = "999"
        changed_digest = producer.digest(output)
        cursor.execute("ALTER TABLE trading.desk_run_results DISABLE TRIGGER USER")
        cursor.execute("UPDATE trading.desk_run_results SET payload=%s,content_digest=%s WHERE decision_id=%s",
                       (Json(output), changed_digest, storage.DECISION))
        assert cursor.rowcount == 1
        cursor.execute("ALTER TABLE trading.desk_run_results ENABLE TRIGGER USER")
        cursor.execute("SELECT payload,content_digest FROM trading.desk_run_results WHERE decision_id=%s", (storage.DECISION,))
        stored, stored_digest = cursor.fetchone()
        assert producer.digest(stored) == stored_digest == changed_digest
    refused_without_writes(conn)


def test_actual_v2_report_refuses_unknown_linked_observation_schema(accounting):
    conn = processed(accounting)
    with conn.cursor() as cursor:
        cursor.execute("SELECT payload FROM trading.qt_execution_observations WHERE observation_id=%s", (producer.INPUT,))
        observed = deepcopy(cursor.fetchone()[0])
        observed["schema_version"] = "qt-execution/unsupported-v99"
        changed_digest = producer.digest(observed)
        cursor.execute("ALTER TABLE trading.qt_execution_observations DISABLE TRIGGER USER")
        cursor.execute("UPDATE trading.qt_execution_observations SET payload=%s,content_digest=%s WHERE observation_id=%s",
                       (Json(observed), changed_digest, producer.INPUT))
        assert cursor.rowcount == 1
        cursor.execute("ALTER TABLE trading.qt_execution_observations ENABLE TRIGGER USER")
        cursor.execute("UPDATE trading.qt_desk_receipts SET publication_payload=jsonb_set(publication_payload,"
                       "'{observation_digest}',to_jsonb(%s::text)) WHERE decision_id=%s",
                       (changed_digest, storage.DECISION))
        assert cursor.rowcount == 1
        # Migration004 intentionally suppresses audit UPDATE via one exact
        # rule. Only this owned administrator corruption step disables it;
        # retain its actual identity/definition/state and restore before proof.
        rule_query = ("SELECT r.oid,r.ev_enabled,pg_get_ruledef(r.oid,true) FROM pg_rewrite r "
                      "JOIN pg_class c ON c.oid=r.ev_class JOIN pg_namespace n ON n.oid=c.relnamespace "
                      "WHERE n.nspname='trading' AND c.relname='position_overrides' "
                      "AND r.rulename='position_overrides_no_update'")
        cursor.execute(rule_query)
        rule_before = cursor.fetchall()
        assert len(rule_before) == 1 and rule_before[0][1] == "O"
        cursor.execute("ALTER TABLE trading.position_overrides DISABLE RULE position_overrides_no_update")
        try:
            cursor.execute("UPDATE trading.position_overrides SET risk_check_result=jsonb_set(risk_check_result,"
                           "'{observation_digest}',to_jsonb(%s::text)) WHERE risk_check_result->>'decision_id'=%s "
                           "AND risk_check_result->>'schema_version'='qt-desk-audit/v1'",
                           (changed_digest, storage.DECISION))
            assert cursor.rowcount > 0
        finally:
            cursor.execute("ALTER TABLE trading.position_overrides ENABLE RULE position_overrides_no_update")
        cursor.execute(rule_query)
        assert cursor.fetchall() == rule_before
        cursor.execute("SELECT o.payload,o.content_digest,r.publication_payload->>'observation_digest' "
                       "FROM trading.qt_execution_observations o JOIN trading.qt_desk_receipts r "
                       "ON r.decision_id=o.decision_id WHERE o.observation_id=%s", (producer.INPUT,))
        stored, stored_digest, receipt_digest = cursor.fetchone()
        assert producer.digest(stored) == stored_digest == receipt_digest == changed_digest
        assert stored["decision_id"] == storage.DECISION and stored["book_id"] == storage.BOOK
    refused_without_writes(conn)
