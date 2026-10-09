"""PostgresRunStatusStore against a scripted fake connection: which columns it asks for when
optional columns are or are not there. No database needed."""

import datetime as dt

import pytest

from conftest import UTC
from algogators_rpc.services.desk.config import DbConfig
from algogators_rpc.services.desk.store import PostgresRunStatusStore, StoreError

DAY = dt.date(2026, 10, 7)
T0 = dt.datetime(2026, 10, 7, 13, 30, tzinfo=UTC)
T1 = dt.datetime(2026, 10, 7, 13, 41, tzinfo=UTC)

BASE_META = ["date", "strategy_id", "portfolio_id", "strategy_allocations", "portfolio_config",
             "strategy_configs"]


class FakeCursor:
    def __init__(self, columns, meta_rows, results):
        self.columns = columns          # {table: [column names]}
        self.meta_rows = meta_rows
        self.results = results          # (count, max_created_at)
        self.sql = []
        self._next = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def execute(self, query, params=()):
        text = query if isinstance(query, str) else query.as_string(None)
        self.sql.append(text)
        if "information_schema.columns" in text:
            self._next = [(c,) for c in self.columns.get(params[1], [])]
        elif "FROM trading.live_run_metadata" in text:
            self._next = self.meta_rows
        elif "FROM trading.live_results" in text:
            self._next = [self.results if "max(created_at)" in text else (self.results[0],)]
        else:
            raise AssertionError(text)

    def fetchall(self):
        return self._next

    def fetchone(self):
        return self._next[0]


class FakeConn:
    def __init__(self, cur):
        self.cur = cur

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def cursor(self):
        return self.cur


def make_store(cur):
    store = PostgresRunStatusStore(DbConfig("h", "5432", "u", "n", "pw"))
    store._connect = lambda: FakeConn(cur)
    return store


def test_columns_as_on_main_today():
    # main: no publish columns yet; created_at exists on both tables.
    cur = FakeCursor({"live_run_metadata": BASE_META + ["created_at"],
                      "live_results": ["portfolio_id", "date", "created_at"]},
                     meta_rows=[({"risk_refusal": {}}, T0)], results=(1, T1))
    facts = make_store(cur).run_facts("P", DAY)
    meta_sql = next(s for s in cur.sql if "FROM trading.live_run_metadata" in s)
    assert "published" not in meta_sql and '"created_at"::timestamptz' in meta_sql
    assert not facts.has_publish_columns
    assert facts.metadata[0].created_at == T0 and facts.metadata[0].published_at is None
    assert facts.has_results and facts.results_written_at == T1


def test_no_optional_columns_at_all():
    cur = FakeCursor({"live_run_metadata": BASE_META, "live_results": ["portfolio_id", "date"]},
                     meta_rows=[({},)], results=(0, None))
    facts = make_store(cur).run_facts("P", DAY)
    meta_sql = next(s for s in cur.sql if "FROM trading.live_run_metadata" in s)
    assert "created_at" not in meta_sql and "ORDER BY" not in meta_sql
    assert facts.metadata[0].created_at is None
    assert not facts.has_results and facts.results_written_at is None


def test_publish_columns_present():
    cur = FakeCursor({"live_run_metadata": BASE_META + ["created_at", "published_by",
                                                        "published_at"],
                      "live_results": ["created_at"]},
                     meta_rows=[({}, T0, "dom", T1)], results=(1, T1))
    facts = make_store(cur).run_facts("P", DAY)
    assert facts.has_publish_columns
    assert facts.metadata[0].published_by == "dom" and facts.metadata[0].published_at == T1


def test_cutoff_columns_present():
    """Migration 027 (contract C7): publish_source and sent_at are read when they exist."""
    cur = FakeCursor({"live_run_metadata": BASE_META + ["created_at", "published_by",
                                                        "published_at", "publish_source",
                                                        "sent_at"],
                      "live_results": ["created_at"]},
                     meta_rows=[({}, T0, "system:fallback-10am", T1, "fallback", T1)],
                     results=(1, T1))
    facts = make_store(cur).run_facts("P", DAY)
    meta_sql = next(s for s in cur.sql if "FROM trading.live_run_metadata" in s)
    assert '"sent_at"::timestamptz' in meta_sql and '"publish_source"' in meta_sql
    assert facts.metadata[0].publish_source == "fallback" and facts.metadata[0].sent_at == T1


def test_missing_metadata_table_is_a_store_error():
    cur = FakeCursor({}, meta_rows=[], results=(0, None))
    with pytest.raises(StoreError, match="live_run_metadata not found"):
        make_store(cur).run_facts("P", DAY)


def test_driver_error_becomes_store_error_without_detail():
    store = PostgresRunStatusStore(DbConfig("h", "5432", "u", "n", "pw"))

    def boom():
        raise ConnectionError("password=pw rejected")
    store._connect = boom
    with pytest.raises(StoreError) as err:
        store.run_facts("P", DAY)
    assert "pw" not in str(err.value) and "ConnectionError" in str(err.value)
