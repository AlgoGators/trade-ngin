"""The margin cap on a requested override (decision 10, Dom 2026-10-16): checked from the margin
data in the database before the request is e-mailed and before a decision books it, since no one
may approve a book past the fund's margin cap (C11)."""

import pytest

from conftest import EQUITY, MARGINS, make_row
from algogators_rpc.services.desk import risk as rk
from algogators_rpc.services.desk.risk import MarginInputs
from algogators_rpc.services.desk.store import StoreError
from algogators import desk_pb2 as pb
from algogators import desk_pb2_grpc
from test_commands import decide, decision, request_override, requested


@pytest.fixture
def stub(channel):
    return desk_pb2_grpc.DeskServiceStub(channel)


def book(*rows):
    return {"proposal": [{"symbol": s, "quantity": q, "strategy_name": st}
                         for s, q, st in rows]}


OVER = book(("ZN.v.0", 201, "TREND_FOLLOWING"))       # 201 x $2,500 > $500,000 of equity
WITHIN = book(("ZN.v.0", 60, "TREND_FOLLOWING"))


# -- the reading --------------------------------------------------------------------------------

@pytest.mark.parametrize("symbol, root", [("ZN.v.0", "ZN"), ("ES.v.0", "MES"), ("NQ.v.1", "MNQ"),
                                          ("YM.v.0", "MYM"), ("ES", "ES"), ("6E.v.0", "6E")])
def test_contract_root_reads_symbols_as_the_engines_registry_does(symbol, root):
    assert rk.contract_root(symbol) == root


def test_margin_is_posted_on_each_symbols_net_quantity():
    inputs = MarginInputs({"ZN": 2500.0, "MES": 2900.0}, 100_000.0)
    reading = rk.book_margin([("ZN.v.0", 10), ("ZN.v.0", -4), ("ES.v.0", -2), ("MES.v.0", 0)],
                             inputs)
    assert reading.initial_margin == pytest.approx(6 * 2500 + 2 * 2900)
    assert reading.measured and not reading.over


def test_a_symbol_with_no_margin_on_record_is_not_measured():
    reading = rk.book_margin([("ZN.v.0", 1), ("XX.v.0", 1)], MarginInputs({"ZN": 2500.0}, 1e6))
    assert reading.missing == ("XX.v.0",) and not reading.measured
    assert "XX.v.0" in rk.margin_refusal(reading, "2026-10-07")


def test_the_cap_is_100_percent_of_equity():
    at = rk.book_margin([("ZN.v.0", 200)], MarginInputs({"ZN": 2500.0}, 500_000.0))
    assert not at.over and rk.margin_refusal(at, "d") is None
    over = rk.book_margin([("ZN.v.0", 201)], MarginInputs({"ZN": 2500.0}, 500_000.0))
    assert over.over
    why = rk.margin_refusal(over, "2026-10-07")
    assert "$502,500" in why and "$500,000" in why and "no one can approve past it" in why


# -- the request ----------------------------------------------------------------------------------

def test_a_request_over_the_margin_cap_is_refused_and_not_mailed(stub, agent, cstore):
    cstore.add(make_row(40, "override_request", payload=OVER))
    request_override(stub, 40)
    agent.wait()
    row = cstore.rows[40]
    assert row.status == "refused", row.message
    assert row.result["reason"] == "outside_limits" and row.result["over"] == ["MARGIN"]
    assert row.result["equity"] == EQUITY
    assert "no one can approve past it" in row.message
    assert row.token_hash is None and agent.sent == []


def test_a_request_within_the_margin_cap_is_mailed(stub, agent, cstore):
    cstore.add(make_row(40, "override_request", payload=WITHIN))
    request_override(stub, 40)
    agent.wait()
    assert cstore.rows[40].status == "done", cstore.rows[40].message


def test_a_request_whose_margin_cannot_be_measured_is_refused(stub, agent, cstore):
    cstore.add(make_row(40, "override_request", payload=book(("XX.v.0", 1, "T"))))
    agent.margin = MarginInputs(MARGINS, EQUITY)
    request_override(stub, 40)
    agent.wait()
    row = cstore.rows[40]
    assert row.status == "refused" and row.result == {"reason": "margin_not_measured",
                                                      "missing": ["XX.v.0"]}
    assert row.token_hash is None


def test_a_request_with_no_equity_yet_is_refused(stub, agent, cstore):
    cstore.add(make_row(40, "override_request", payload=WITHIN))
    agent.margin = MarginInputs(MARGINS, None)
    request_override(stub, 40)
    agent.wait()
    assert cstore.rows[40].status == "refused"
    assert "equity" in cstore.rows[40].message


def test_a_request_with_no_proposal_snapshot_is_refused(stub, agent, cstore):
    cstore.add(make_row(40, "override_request", payload={}))
    request_override(stub, 40)
    agent.wait()
    assert cstore.rows[40].status == "refused"
    assert cstore.rows[40].result == {"reason": "no_proposal"}


def test_an_unreadable_margin_fails_the_request_retryably(stub, agent, cstore):
    def down(portfolio_id, date):
        raise StoreError("database error (OperationalError)")
    agent.dispatcher._margin_reader = down
    cstore.add(make_row(40, "override_request", payload=WITHIN))
    request_override(stub, 40)
    agent.wait()
    assert cstore.rows[40].status == "failed"
    assert "StoreError" in cstore.rows[40].message and agent.sent == []


# -- the decision ---------------------------------------------------------------------------------

def test_an_approval_over_the_margin_cap_is_refused_before_the_engine(stub, agent, cstore, runner):
    # Equity fell since the request was mailed: the cap is measured again when the VP decides.
    requested(cstore, payload=WITHIN)
    decision(cstore)
    agent.margin = MarginInputs(MARGINS, 100_000.0)
    reply = decide(stub, 51)
    agent.wait()
    assert reply.status == pb.COMMAND_STATUS_REFUSED
    assert cstore.rows[51].status == "refused"
    assert cstore.rows[51].result["over"] == ["MARGIN"]
    assert runner.calls == []


def test_an_approval_within_the_margin_cap_runs_the_override(stub, agent, cstore, runner):
    requested(cstore, payload=WITHIN)
    decision(cstore)
    decide(stub, 51)
    agent.wait()
    assert [c for c in runner.calls if "--override" in c[0]], runner.calls


def test_a_rejection_needs_no_margin_check(stub, agent, cstore, runner):
    requested(cstore, payload=OVER)
    decision(cstore, approved=False)
    agent.dispatcher._margin_reader = None
    decide(stub, 51)
    agent.wait()
    assert cstore.rows[51].status == "done" and cstore.rows[51].result == {"approved": False}
