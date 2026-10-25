"""Who the book e-mail goes to, and the history/symbol readers' output shape."""

import json
from types import SimpleNamespace

from algogators_rpc.services.report import data
from algogators_rpc.services.report.__main__ import book_recipients


def _settings(tmp_path, to_emails, approvers="vp=vp@x.org,president=pres@x.org"):
    d = tmp_path / "portfolios" / "qt"
    d.mkdir(parents=True)
    (d / "email.json").write_text(json.dumps({"to_emails": to_emails}), encoding="utf-8")
    return SimpleNamespace(config_dir=str(tmp_path), approvers=approvers)


def test_to_is_the_book_list_and_the_approvers_are_cc(tmp_path):
    to, cc = book_recipients(_settings(tmp_path, ["a@x.org", "b@x.org"]), "qt")
    assert to == ["a@x.org", "b@x.org"]
    assert cc == ["vp@x.org", "pres@x.org"]


def test_excluded_and_duplicate_addresses_are_never_addressed(tmp_path):
    s = _settings(tmp_path, ["a@x.org", "DHebrank@msn.com", "A@x.org"])
    to, cc = book_recipients(s, "qt", extra_cc="dhebrank@msn.com, a@x.org, c@x.org")
    assert to == ["a@x.org"]
    assert cc == ["c@x.org", "vp@x.org", "pres@x.org"]


def test_an_approver_who_is_also_in_the_book_is_not_mailed_twice(tmp_path):
    to, cc = book_recipients(_settings(tmp_path, ["vp@x.org", "a@x.org"]), "qt")
    assert to == ["vp@x.org", "a@x.org"]
    assert cc == ["pres@x.org"]


def test_missing_approvers_or_book_file_does_not_raise(tmp_path):
    to, cc = book_recipients(_settings(tmp_path, ["a@x.org"], approvers=""), "qt")
    assert (to, cc) == (["a@x.org"], [])
    to, cc = book_recipients(SimpleNamespace(config_dir=str(tmp_path), approvers=""), "nope")
    assert (to, cc) == ([], [])


class _Cur:
    def __init__(self, *results):
        self.results = list(results)
        self.sql = []

    def execute(self, sql, params=None):
        self.sql.append(sql)

    def fetchall(self):
        return self.results.pop(0)


def test_history_is_oldest_first_skips_nulls_and_scopes_the_book():
    import datetime as dt

    d = lambda n: dt.date(2026, 10, n)  # noqa: E731
    cur = _Cur([(d(3), 1.5, 1.0, 100.0), (d(2), None, 0.9, 90.0)], [(d(2), 4), (d(3), 6)])
    h = data.fetch_history(cur, "P", "S")
    assert [p["date"] for p in h["net_leverage"]] == ["2026-10-02", "2026-10-03"]
    assert [p["date"] for p in h["leverage"]] == ["2026-10-03"]  # the NULL gross is skipped
    assert [p["value"] for p in h["contracts_per_day"]] == [4.0, 6.0]
    assert "portfolio_type" in cur.sql[0] and "'qt'" in cur.sql[0]


def test_symbols_lists_rows_and_names_the_roots_with_no_metadata():
    cur = _Cur([("ES", "ES", "E-mini S&P", "HMUZ")])
    s = data.fetch_symbols(cur, ["ES", "ZZ"])
    assert s["rows"] == [{"db": "ES", "ib": "ES", "name": "E-mini S&P", "months": "HMUZ"}]
    assert s["missing"] == ["ZZ"]
