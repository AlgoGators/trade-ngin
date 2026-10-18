"""The desk risk check (contract C9; desk.proto 1.3.0 GetDeskRisk, 1.4.0 checks): the breach rule over the
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


# The engine's desk.checks of a save (desk_checks_json): a book inside every limit.
MEASURED = {
    "tau": 0.2, "covariance_measured": True,
    "R": {"value": 0.31, "max": 0.45, "max_ratio": 2.25},
    "R_jump": {"value": 0.52, "max": 0.9, "max_ratio": 4.5},
    "R_shock": {"value": 0.61, "max": 0.8, "max_ratio": 4.0},
    "L_g": {"value": 3.79, "max": 8.0},
    "L_n": {"value": -2.67, "max": 6.0},
    "CAP": {"value": 1.94, "max": 2.0, "symbol": "ZT.v.0"},
    "TRIM": {"value": 0.0, "max": 5},
}


def desk_row(symbols, m=1.0, binding="none", after="", by_hold="", refused=False,
             source="desk", report_only=False, audit_id=61, checks=None,
             initial_margin=82945.0, equity=494971.49):
    desk = {"mode": source, "audit_id": audit_id, "symbols": symbols,
            "overlay": {"ran": True, "refused": refused, "m": m, "binding": binding,
                        "risk_scale": 0.9, "over_limit_after_rounding": after,
                        "over_limit_by_hold": by_hold}}
    if report_only:
        desk["report_only"] = True
    if checks is not None:
        desk["checks"] = checks
    return DeskRiskRow(book_source=source, risk_detail={"desk": desk}, gross_leverage=3.79,
                       net_leverage=-2.67, margin_cushion=0.877, equity_to_margin=6.92,
                       initial_margin=initial_margin, equity=equity)


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


# -- every check, breach or not (1.4.0) --------------------------------------------------------

def by_term(report):
    return {c.term: c for c in report.checks}


def test_every_check_is_reported_with_its_max_when_nothing_breaches():
    report = derive_desk_risk(desk_row([sym("6C.v.0", -2, -2)], checks=MEASURED))
    assert report.within_limits
    assert [c.term for c in report.checks] == ["R", "R_jump", "R_shock", "L_g", "L_n", "CAP",
                                               "TRIM", "MARGIN"]
    assert all(c.status == rk.WITHIN for c in report.checks)
    r = by_term(report)["R"]
    assert (r.value, r.max, r.unit) == (0.31, 0.45, rk.PERCENT)
    assert "max 2.25 x risk target 20%" in r.detail
    cap = by_term(report)["CAP"]
    assert (cap.value, cap.max, cap.unit) == (1.94, 2.0, rk.MULTIPLE) and "ZT.v.0" in cap.detail
    assert by_term(report)["TRIM"].max == 5 and by_term(report)["TRIM"].unit == rk.CONTRACTS
    assert by_term(report)["L_n"].value == -2.67  # signed; the limit is on |L_n|


def test_a_cap_cut_and_a_binding_term_mark_their_checks_bound():
    report = derive_desk_risk(desk_row([sym("ZT.v.0", 8, 2, "cap")], m=0.8, binding="L_g",
                                       checks=MEASURED))
    assert by_term(report)["CAP"].status == rk.BOUND
    assert by_term(report)["L_g"].status == rk.BOUND
    assert by_term(report)["R"].status == rk.WITHIN


def test_over_limit_terms_and_the_trim_mark_their_checks():
    checks = dict(MEASURED, TRIM={"value": 3.0, "max": 5})
    report = derive_desk_risk(desk_row([], after="R", by_hold="CAP", checks=checks))
    assert by_term(report)["R"].status == rk.OVER
    assert by_term(report)["CAP"].status == rk.OVER
    assert by_term(report)["TRIM"].status == rk.BOUND


def test_a_reading_over_its_max_is_over_even_unmarked():
    checks = dict(MEASURED, L_n={"value": -6.5, "max": 6.0})
    assert by_term(derive_desk_risk(desk_row([], checks=checks)))["L_n"].status == rk.OVER


def test_a_blind_window_is_not_measured_with_its_max():
    checks = {k: (dict(v, value=None) if k in ("R", "R_jump", "R_shock") else v)
              for k, v in MEASURED.items()}
    checks["covariance_measured"] = False
    r = by_term(derive_desk_risk(desk_row([], checks=checks)))["R"]
    assert r.status == rk.NOT_MEASURED and r.value is None and r.max == 0.45
    assert "blind" in r.detail


def test_a_refusal_and_an_unmeasured_save_report_the_configured_maxes():
    limits = load_limits_from(RISK_JSON)
    report = derive_desk_risk(desk_row([], refused=True), limits)
    checks = by_term(report)
    assert checks["R"].max == 2.25 and checks["R"].unit == rk.TAU
    assert checks["R"].status == rk.NOT_MEASURED
    assert checks["L_g"].value == 3.79 and checks["L_g"].max == 8.0
    assert checks["refused"].status == rk.OVER
    old = derive_desk_risk(desk_row([]), limits)  # a row written before the checks existed
    assert "not recorded" in by_term(old)["R"].detail and "refused" not in by_term(old)


def test_the_model_book_and_no_book_report_the_configured_maxes():
    limits = load_limits_from(RISK_JSON)
    model = derive_desk_risk(DeskRiskRow(book_source="model", risk_detail={}, gross_leverage=3.8,
                                         net_leverage=1.0), limits)
    checks = by_term(model)
    assert checks["L_g"].value == 3.8 and checks["L_g"].status == rk.WITHIN
    assert checks["CAP"].max == 2.0 and checks["CAP"].status == rk.NOT_MEASURED
    none = derive_desk_risk(None, limits)
    assert [c.term for c in none.checks][:3] == ["R", "R_jump", "R_shock"]
    assert all(c.status == rk.NOT_MEASURED for c in none.checks)


def load_limits_from(data):
    carver = data["modules"][0]
    return [(k, float(carver[k])) for k in rk.LIMIT_KEYS if k in carver]


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
    # 1.4.0: no checks recorded by the engine, so the configured maxes, not measured.
    checks = {c.term: c for c in reply.checks}
    assert checks["R"].max == 2.25 and not checks["R"].HasField("value")
    assert checks["R"].status == pb.RISK_CHECK_STATUS_NOT_MEASURED


def test_rpc_reports_every_measured_check(stub, store):
    store.risk_rows[(PID, DAY)] = desk_row([sym("ZT.v.0", 8, 2, "cap")], checks=MEASURED)
    checks = {c.term: c for c in get(stub).checks}
    assert list(checks) == ["R", "R_jump", "R_shock", "L_g", "L_n", "CAP", "TRIM", "MARGIN"]
    assert checks["R"].value == pytest.approx(0.31) and checks["R"].max == pytest.approx(0.45)
    assert checks["R"].unit == "percent" and checks["R"].label == "Portfolio risk (R)"
    assert checks["CAP"].status == pb.RISK_CHECK_STATUS_BOUND
    assert checks["L_g"].status == pb.RISK_CHECK_STATUS_WITHIN


def test_rpc_carries_the_publish_gate(stub, store):
    # 1.6.0, decision 5: a bound cap does not block; the margin cap over does.
    store.risk_rows[(PID, DAY)] = desk_row([sym("ZT.v.0", 8, 2, "cap")], checks=MEASURED)
    reply = get(stub)
    assert not reply.blocks_publish and list(reply.blockers) == []
    store.risk_rows[(PID, DAY)] = desk_row([sym("ZT.v.0", 8, 2, "cap")], checks=MEASURED,
                                           initial_margin=600_000.0, equity=500_000.0)
    reply = get(stub)
    assert reply.blocks_publish and [b.term for b in reply.blockers] == ["MARGIN"]


def test_rpc_reports_an_incomplete_book(stub, store):
    store.risk_rows[(PID, DAY)] = rk.DeskRiskRow(book_source=None, gross_leverage=46.0)
    reply = get(stub)
    assert reply.state == pb.DESK_RISK_STATE_INCOMPLETE and not reply.within_limits
    assert reply.blocks_publish and [b.term for b in reply.blockers] == ["incomplete"]


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



# -- the fund's hard cap (C11) -----------------------------------------------------------------

def capped(row, margin, equity):
    return DeskRiskRow(**{**row.__dict__, "initial_margin": margin, "equity": equity})


def test_the_margin_check_is_within_at_or_under_the_equity():
    report = derive_desk_risk(capped(desk_row([], checks=MEASURED), 82945.0, 494971.49))
    cap = by_term(report)["MARGIN"]
    assert (cap.value, cap.max, cap.unit, cap.status) == (82945.0, 494971.49, rk.USD, rk.WITHIN)
    assert report.within_limits
    at = by_term(derive_desk_risk(capped(desk_row([], checks=MEASURED), 100.0, 100.0)))["MARGIN"]
    assert at.status == rk.WITHIN


def test_margin_over_the_equity_is_a_breach():
    report = derive_desk_risk(capped(desk_row([], checks=MEASURED), 7_000_000.0, 494971.49))
    assert by_term(report)["MARGIN"].status == rk.OVER
    assert not report.within_limits and "MARGIN" in [b.term for b in report.breaches]


def test_the_margin_check_on_a_model_day_and_without_a_book():
    model = DeskRiskRow(book_source="model", risk_detail={}, initial_margin=50.0, equity=100.0)
    assert by_term(derive_desk_risk(model))["MARGIN"].status == rk.WITHIN
    assert by_term(derive_desk_risk(None))["MARGIN"].status == rk.NOT_MEASURED


def test_the_models_book_shows_the_covariance_limits_in_percent_like_a_desk_book(tmp_path):
    # D-80: one unit in both states. With no measured checks the R limits were ratios to tau
    # ("2.25 tau"); a desk save showed the same limit as 0.45 (percent). With tau known both read
    # percent.
    (tmp_path / "portfolio.json").write_text(json.dumps({"strategies": {
        "OFF": {"enabled_live": False, "config": {"risk_target": 0.5}},
        "TREND": {"enabled_live": True, "config": {"risk_target": 0.2}}}}))
    assert rk.load_tau(str(tmp_path)) == 0.2
    limits = [("R_max", 2.25), ("R_jump_max", 4.5), ("max_gross_leverage", 8.0)]
    model = derive_desk_risk(DeskRiskRow(book_source="model"), limits, rk.load_tau(str(tmp_path)))
    by_term = {c.term: c for c in model.checks}
    assert (by_term["R"].unit, round(by_term["R"].max, 6)) == ("percent", 0.45)
    assert round(by_term["R_jump"].max, 6) == 0.9
    assert by_term["L_g"].unit == "multiple" and by_term["L_g"].max == 8.0
    # Without tau the ratio stays, labelled as such.
    plain = derive_desk_risk(DeskRiskRow(book_source="model"), limits)
    assert {c.term: c.unit for c in plain.checks}["R"] == "tau"
