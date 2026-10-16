"""PostgresCommandStore against a scripted fake connection: the SQL it sends, and that every
UPDATE touches only the columns the 023 trigger lets change. No database needed."""

import datetime as dt
import re

import pytest

from conftest import UTC
from algogators_rpc.services.desk.command_store import COLUMNS, PostgresCommandStore
from algogators_rpc.services.desk.config import DbConfig
from algogators_rpc.services.desk.store import StoreError

DAY = dt.date(2026, 10, 7)
T0 = dt.datetime(2026, 10, 7, 13, 30, tzinfo=UTC)
ENGINE_COLUMNS = {"status", "result", "message", "started_at", "finished_at", "token_hash",
                  "token_expires_at"}


def record(id=1, kind="save", status="pending", payload=None):
    values = {c: None for c in COLUMNS}
    values.update(id=id, portfolio_id="P", date=DAY, kind=kind, status=status,
                  requested_by="desk@x", payload=payload, created_at=T0)
    return tuple(values[c] for c in COLUMNS)


class FakeCursor:
    def __init__(self, replies):
        self.replies = list(replies)   # one list of rows per execute
        self.sql, self.params = [], []
        self._rows = []

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def execute(self, query, params=()):
        self.sql.append(query)
        self.params.append(params)
        self._rows = self.replies.pop(0) if self.replies else []

    def fetchone(self):
        return self._rows[0] if self._rows else None

    def fetchall(self):
        return self._rows


class FakeConn:
    def __init__(self, cur):
        self.cur = cur

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def cursor(self):
        return self.cur


def make(replies):
    cur = FakeCursor(replies)
    store = PostgresCommandStore(DbConfig("h", "5432", "u", "n", "pw"))
    store.autocommit = []

    def connect(autocommit=True):
        store.autocommit.append(autocommit)
        return FakeConn(cur)
    store._connect = connect
    return store, cur


def set_columns(sql):
    m = re.search(r"SET (.*?) WHERE", sql, re.S)
    return {part.split("=")[0].strip() for part in m.group(1).split(",")}


def test_claim_only_takes_a_pending_row():
    store, cur = make([[record(status="running")]])
    row = store.claim(7)
    sql = cur.sql[0]
    assert sql.startswith("UPDATE trading.position_overrides")
    assert "WHERE id = %s AND status = 'pending' RETURNING" in sql
    assert "status = 'running'" in sql and "started_at = now()" in sql
    assert cur.params[0] == (7,)
    assert row.status == "running" and row.payload == {}


def test_claim_of_a_taken_row_returns_none():
    store, _ = make([[]])
    assert store.claim(7) is None


def test_finish_only_moves_a_running_row_and_wraps_result():
    store, cur = make([[(7,)]])
    assert store.finish(7, "failed", {"a": 1}, "boom") is True
    sql = cur.sql[0]
    assert "WHERE id = %s AND status = 'running'" in sql and "finished_at = now()" in sql
    status, result, message, audit_id = cur.params[0]
    assert (status, message, audit_id) == ("failed", "boom", 7)
    assert result.obj == {"a": 1}  # psycopg Jsonb
    store, cur = make([[]])
    assert store.finish(7, "done", None, "x") is False and cur.params[0][1] is None
    with pytest.raises(ValueError):
        store.finish(7, "running", None, "x")


def test_record_mail_stores_token_expiry_and_result_together():
    store, cur = make([[(T0,)]])
    assert store.record_mail(7, "ab" * 32, 48, {"emailed": ["vp"], "emailed_at": "t"}) == T0
    sql = cur.sql[0]
    assert "token_expires_at = now() + make_interval(hours => %s)" in sql
    assert "result = %s" in sql and "WHERE id = %s AND status = 'running'" in sql
    token_hash, hours, result, audit_id = cur.params[0]
    assert (token_hash, hours, audit_id) == ("ab" * 32, 48, 7)
    assert result.obj == {"emailed": ["vp"], "emailed_at": "t"}
    store, _ = make([[]])
    assert store.record_mail(7, "h", 48, {}) is None


def test_running_rows_carry_their_age_by_the_database_clock():
    store, cur = make([[record(2, status="running") + (12.5,),
                        record(3, status="running") + (None,)]])
    rows = store.running_rows()
    assert [(r.id, age) for r, age in rows] == [(2, 12.5), (3, None)]
    assert "extract(epoch FROM now() - started_at)" in cur.sql[0]
    assert "WHERE status = 'running' ORDER BY id" in cur.sql[0]


def test_requeue_runs_in_a_transaction_that_marks_recovery():
    store, cur = make([[], [(4,)]])
    assert store.requeue(4, "re-driven") is True
    assert store.autocommit == [False]       # SET LOCAL needs a transaction
    assert cur.sql[0] == "SET LOCAL algogators.recovery = 'on'"
    assert "SET status = 'pending', started_at = NULL" in cur.sql[1]
    assert "WHERE id = %s AND status = 'running'" in cur.sql[1]
    assert cur.params[1] == ("re-driven", 4)
    store, _ = make([[], []])
    assert store.requeue(4, "x") is False


def test_pending_rows_and_decisions_and_published():
    store, cur = make([[record(2), record(3, kind="publish")],
                       [record(5, kind="override_decision")], [(T0,)]])
    rows = store.pending_rows()
    assert [r.id for r in rows] == [2, 3] and rows[1].kind == "publish"
    assert "WHERE status = 'pending' ORDER BY id" in cur.sql[0]
    assert [r.id for r in store.decisions_for(4)] == [5]
    assert "kind = 'override_decision' AND parent_id = %s" in cur.sql[1]
    assert cur.params[1] == (4,)
    assert store.published_at("P", DAY) == T0
    assert "FROM trading.live_run_metadata" in cur.sql[2] and cur.params[2] == ("P", DAY)


def test_publishes_after_reads_the_flow_rows_of_one_approval():
    store, cur = make([[record(7, kind="publish", payload={"flow": "override_b", "after": 5})]])
    assert [r.id for r in store.publishes_after(5)] == [7]
    assert "kind = 'publish' AND payload->>'after' = %s" in cur.sql[0]
    assert cur.params[0] == ("5",)


def test_every_update_touches_only_engine_columns():
    store, cur = make([[record()], [(1,)], [(T0,)], [], [(1,)]])
    store.claim(1)
    store.finish(1, "done", None, "m")
    store.record_mail(1, "h", 48, {})
    store.requeue(1, "m")
    updates = [s for s in cur.sql if s.startswith("UPDATE")]
    assert len(updates) == 4
    for sql in updates:
        assert set_columns(sql) <= ENGINE_COLUMNS, sql


def test_newest_pending_publish():
    store, cur = make([[record(9, kind="publish")]])
    assert store.newest_pending_publish("P", DAY).id == 9
    sql = cur.sql[0]
    assert "kind = 'publish' AND status = 'pending'" in sql and "ORDER BY id DESC LIMIT 1" in sql
    assert cur.params[0] == ("P", DAY)


def test_books_sum_by_symbol_per_book():
    store, cur = make([[("qt", "ES", 4, "cap"), ("system", "ES", 3, None),
                        ("qt_proposal", "ES", 5, None)]])
    books = store.books("P", DAY)
    assert "sum(quantity)" in cur.sql[0] and "GROUP BY portfolio_type, symbol" in cur.sql[0]
    assert books["qt"]["ES"].quantity == 4 and books["qt"]["ES"].moved_by == "cap"
    assert books["system"]["ES"].quantity == 3 and books["qt_proposal"]["ES"].quantity == 5


def test_driver_error_becomes_store_error_without_detail():
    store = PostgresCommandStore(DbConfig("h", "5432", "u", "n", "pw"))

    def boom():
        raise ConnectionError("password=pw rejected")
    store._connect = boom
    with pytest.raises(StoreError) as err:
        store.get_row(1)
    assert "pw" not in str(err.value) and "ConnectionError" in str(err.value)


# -- rescind (contract C8) -------------------------------------------------------------------------

def meta(by="d@x", at=T0, source="desk", sent=None):
    return (by, at, source, sent)


def test_rescind_clears_the_desk_approval_and_finishes_the_row_in_one_transaction():
    # live_run_metadata; the approving publish row (a plain publish: no flow); the two UPDATEs.
    store, cur = make([[meta(), meta(None, None, None)], [(None,)], [], [(9,)]])
    res = store.rescind(9, "P", DAY, "rescinded", "D@X")
    assert res.outcome == "rescinded" and res.published_by == "d@x" and res.published_at == T0
    assert store.autocommit == [False]
    assert "FOR UPDATE" in cur.sql[0] and cur.params[0] == ("P", DAY)
    assert "kind = 'publish'" in cur.sql[1] and "ORDER BY id DESC" in cur.sql[1]
    assert cur.sql[2].startswith("UPDATE trading.live_run_metadata")
    assert set_columns(cur.sql[2]) == {"published_by", "published_at", "publish_source"}
    assert set_columns(cur.sql[3]) <= ENGINE_COLUMNS
    assert "status = 'running' AND kind = 'rescind'" in cur.sql[3]
    assert cur.params[3][0].obj == {"rescinded": {"published_by": "d@x",
                                                  "published_at": T0.isoformat(), "flow": None}}


@pytest.mark.parametrize("rows, outcome", [
    ([], "unpublished"),
    ([meta(None, None, None)], "unpublished"),
    ([meta(source="fallback")], "not_desk"),
    ([meta(), meta(source="model-only")], "not_desk"),
    ([meta(), meta(None, None, None, sent=T0)], "sent"),
])
def test_rescind_refusals_write_nothing(rows, outcome):
    store, cur = make([rows])
    assert store.rescind(9, "P", DAY, "m", "d@x").outcome == outcome
    assert len(cur.sql) == 1


def test_anyone_may_rescind_a_desk_approval():
    store, cur = make([[meta(by="d@x")], [("accept_c",)], [], [(9,)]])
    res = store.rescind(9, "P", DAY, "m", "e@x", ("vp@x", "pres@x"))
    assert res.outcome == "rescinded" and res.flow == "accept_c"


def test_only_an_approver_may_rescind_an_override_day_and_others_write_nothing():
    store, cur = make([[meta(by="d@x")], [("override_b",)]])
    res = store.rescind(9, "P", DAY, "m", "d@x", ("vp@x", "pres@x"))
    assert res.outcome == "not_approver" and res.flow == "override_b"
    assert len(cur.sql) == 2 and not any(q.startswith("UPDATE") for q in cur.sql)
    store, cur = make([[meta(by="d@x")], [("override_b",)], [], [(9,)]])
    assert store.rescind(9, "P", DAY, "m", " VP@X ", ("vp@x", "pres@x")).outcome == "rescinded"


def test_rescind_of_a_row_no_longer_running_rolls_back():
    store, cur = make([[meta()], [(None,)], [], []])
    assert store.rescind(9, "P", DAY, "m", "d@x").outcome == "not_running"
    assert len(cur.sql) == 4
