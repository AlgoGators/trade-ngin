"""The desk risk check (contract C9; desk.proto 1.3.0 GetDeskRisk): the breach rule over the
engine's risk_detail record, the configured limits, and the RPC end to end."""

import json

import grpc
import pytest

from conftest import DAY, PID, write_portfolio
from algogators import desk_pb2 as pb
from algogators import desk_pb2_grpc
from algogators_rpc.services.desk import risk as rk
from algogators_rpc.services.desk.risk import DeskRiskRow, derive_desk_risk, load_limits

RISK_JSON = {"schema": 2, "modules": [{
    "id": "carver", "type": "carver", "max_gross_leverage": 8.0, "max_net_leverage": 6.0,
    "R_max": 2.25, "R_jump_max": 4.5, "R_shock_max": 4.0, "per_name_cap": 2, "trim_max": 5,
    "confidence_level": 0.99}]}


def sym(symbol, asked, given, moved_by="none"):
    return {"symbol": symbol, "asked": asked, "given": given, "pass_given": given,
            "moved_by": moved_by}


def desk_row(symbols, m=1.0, binding="none", after="", by_hold="", refused=False,
             source="desk", report_only=False, audit_id=61):
    desk = {"mode": source, "audit_id": audit_id, "symbols": symbols,
            "overlay": {"ran": True, "refused": refused, "m": m, "binding": binding,
                        "risk_scale": 0.9, "over_limit_after_rounding": after,
                        "over_limit_by_hold": by_hold}}
    if report_only:
        desk["report_only"] = True
    return DeskRiskRow(book_source=source, risk_detail={"desk": desk}, gross_leverage=3.79,
                       net_leverage=-2.67, margin_cushion=0.877, equity_to_margin=6.92)


# -- the breach rule ---------------------------------------------------------------------------

@pytest.mark.parametrize("moved_by, category", [
    ("none", "none"), ("cap", "risk"), ("overlay", "risk"), ("clip", "risk"), ("trim", "risk"),
    ("sign_close", "rule"), ("rounding", "rule"), ("buffer", "rule"), ("search", "rule"),
    ("hold", "rule"), ("none,cap", "risk"), ("sign_close,rounding", "rule"), ("", "none"),
])
def test_category(moved_by, category):
    assert rk.category(moved_by) == category


def test_a_desk_book_moved_only_by_trading_rules_is_within_limits():
    # 2026-10-13: MBT asked -4, given 0 by sign_close (no long-to-short flip in a day).
    report = derive_desk_risk(desk_row([sym("6C.v.0", -2, -2), sym("MBT.v.0", -4, 0, "sign_close")]))
    assert report.state == "desk" and report.audit_id == 61
    assert report.within_limits and not report.breaches
    assert [(s.symbol, s.category) for s in report.symbols] == [("6C.v.0", "none"),
                                                                ("MBT.v.0", "rule")]
    assert report.gross_leverage == 3.79 and report.net_leverage == -2.67


@pytest.mark.parametrize("moved_by", ["cap", "overlay", "clip", "trim"])
def test_a_symbol_moved_by_a_risk_step_is_a_breach(moved_by):
    report = derive_desk_risk(desk_row([sym("ZN.v.0", 9, 4, moved_by)]))
    assert not report.within_limits
    assert [(b.term, b.detail) for b in report.breaches] == [
        (moved_by, f"ZN.v.0: asked 9, the {moved_by} step gave 4")]


def test_the_overlay_cutting_the_book_is_a_breach_named_by_its_term():
    report = derive_desk_risk(desk_row([sym("ES.v.0", 10, 8, "overlay")], m=0.8, binding="L_g"))
    assert not report.within_limits
    assert report.breaches[0].term == "L_g"
    assert "gross leverage (max_gross_leverage) bound" in report.breaches[0].detail
    assert "cut to 80%" in report.breaches[0].detail and "25% over" in report.breaches[0].detail
    assert report.m == 0.8 and report.binding_term == "L_g"


def test_over_limit_terms_and_a_refusal_are_breaches():
    report = derive_desk_risk(desk_row([], after="R;L_n", by_hold="CAP", refused=True))
    assert [b.term for b in report.breaches] == ["R", "L_n", "CAP", "refused"]
    assert not report.within_limits


def test_m_below_one_without_a_binding_term_is_not_a_breach():
    assert derive_desk_risk(desk_row([], m=0.999, binding="none")).within_limits


def test_an_approved_override_reports_its_breaches_but_is_waived():
    report = derive_desk_risk(desk_row([sym("ZN.v.0", 9, 9, "none")], m=0.8, binding="R",
                                       source="override", report_only=True, audit_id=70))
    assert report.state == "override" and report.approved_override and report.audit_id == 70
    assert not report.within_limits and report.breaches


def test_the_model_book_and_a_missing_book():
    model = derive_desk_risk(DeskRiskRow(book_source="model", risk_detail={"binding_term": "none"},
                                         gross_leverage=3.8))
    assert model.state == "model" and model.within_limits and model.gross_leverage == 3.8
    none = derive_desk_risk(None, [("per_name_cap", 2.0)])
    assert none.state == "no_book" and none.within_limits and none.limits == (("per_name_cap", 2.0),)


def test_a_malformed_record_does_not_crash():
    row = DeskRiskRow(book_source="desk", risk_detail={"desk": {"symbols": [None, {"x": 1}],
                                                                "overlay": "bad"}})
    report = derive_desk_risk(row)
    assert report.state == "desk" and report.within_limits and report.symbols == ()


# -- the configured limits ---------------------------------------------------------------------

def test_limits_come_from_the_carver_module_in_order(tmp_path):
    (tmp_path / "risk.json").write_text(json.dumps(RISK_JSON))
    assert load_limits(str(tmp_path)) == [
        ("max_gross_leverage", 8.0), ("max_net_leverage", 6.0), ("R_max", 2.25),
        ("R_jump_max", 4.5), ("R_shock_max", 4.0), ("per_name_cap", 2.0), ("trim_max", 5.0)]


def test_missing_or_broken_limits_are_empty(tmp_path):
    assert load_limits(str(tmp_path)) == []
    (tmp_path / "risk.json").write_text("{not json")
    assert load_limits(str(tmp_path)) == []
    (tmp_path / "risk.json").write_text(json.dumps({"modules": [{"type": "other"}]}))
    assert load_limits(str(tmp_path)) == []


# -- the RPC -----------------------------------------------------------------------------------

@pytest.fixture
def stub(channel):
    return desk_pb2_grpc.DeskServiceStub(channel)


def get(stub, date=DAY.isoformat(), pid=PID):
    return stub.GetDeskRisk(pb.DeskRiskRequest(portfolio_id=pid, date=date), timeout=5)


def test_rpc_reports_a_breach_with_the_configured_limits(stub, store, config_dir):
    d = write_portfolio(config_dir)
    (d / "risk.json").write_text(json.dumps(RISK_JSON))
    store.risk_rows[(PID, DAY)] = desk_row(
        [sym("ES.v.0", 10, 8, "overlay"), sym("MBT.v.0", -4, 0, "sign_close")], m=0.8, binding="L_g")
    reply = get(stub)
    assert reply.state == pb.DESK_RISK_STATE_DESK and reply.audit_id == 61
    assert not reply.within_limits and not reply.approved_override
    assert [b.term for b in reply.breaches] == ["L_g", "overlay"]
    assert [(s.symbol, s.category) for s in reply.symbols] == [("ES.v.0", "risk"),
                                                               ("MBT.v.0", "rule")]
    assert reply.HasField("gross_leverage") and reply.gross_leverage == pytest.approx(3.79)
    assert ("per_name_cap", 2.0) in [(lim.name, lim.value) for lim in reply.limits]


def test_rpc_on_a_day_without_a_book(stub, store):
    reply = get(stub)
    assert reply.state == pb.DESK_RISK_STATE_NO_BOOK and reply.within_limits
    assert not reply.HasField("gross_leverage") and list(reply.limits) == []


def test_rpc_validates_and_maps_a_store_error(stub, store):
    with pytest.raises(grpc.RpcError) as err:
        get(stub, date="13/10/2026")
    assert err.value.code() == grpc.StatusCode.INVALID_ARGUMENT
    store.error = "database error (OperationalError)"
    with pytest.raises(grpc.RpcError) as err:
        get(stub)
    assert err.value.code() == grpc.StatusCode.UNAVAILABLE


def test_rpc_runs_without_a_dispatcher(store):
    # GetDeskRisk is a read: it needs no command dispatcher, like GetRunStatus.
    from algogators_rpc.services.desk.servicer import DeskServicer
    servicer = DeskServicer(store)

    class Ctx:
        def abort(self, code, msg):
            raise AssertionError(msg)
    reply = servicer.GetDeskRisk(pb.DeskRiskRequest(portfolio_id=PID, date=DAY.isoformat()), Ctx())
    assert reply.state == pb.DESK_RISK_STATE_NO_BOOK
