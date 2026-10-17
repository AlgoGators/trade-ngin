"""Contract C10: the A -> B -> C approval. The QT's Approve on B publishes only when the engine's
book C equals B and is within the limits; otherwise the QT approves C, or the VP or President
approves B through an override, which then publishes B."""

import datetime as dt
import json

import pytest

from conftest import DAY, DISPATCH_AT, EQUITY, MARGINS, NY, PID, make_row
from algogators_rpc.services.desk import risk as rk
from algogators_rpc.services.desk.commands import Dispatcher
from algogators_rpc.services.desk.jobs import JobManager
from algogators_rpc.services.desk.risk import DeskRiskRow, MarginInputs
from algogators_rpc.services.desk.store import StoreError


def sym(symbol, asked, given, moved_by="none"):
    return {"symbol": symbol, "asked": asked, "given": given, "pass_given": given,
            "moved_by": moved_by}


def desk_book(audit_id, symbols, binding="none", m=1.0, after="", gross=3.0, net=2.0,
              cap=1.0, margin=400_000.0, equity=500_000.0, source="desk", checks=True):
    """A day's qt row as the engine writes it after a desk save (or an override): the pass's
    record and its measured checks (limits: gross 8, net 6, per-name 2)."""
    measured = {"tau": 0.2, "covariance_measured": True,
                "R": {"value": 0.1, "max": 0.45, "max_ratio": 2.25},
                "R_jump": {"value": 0.1, "max": 0.45, "max_ratio": 2.25},
                "R_shock": {"value": 0.1, "max": 0.45, "max_ratio": 2.25},
                "L_g": {"value": gross, "max": 8.0}, "L_n": {"value": net, "max": 6.0},
                "CAP": {"value": cap, "max": 2.0, "symbol": "ZN.v.0"},
                "TRIM": {"value": 0, "max": 5}}
    desk = {"mode": "override" if source == "override" else "desk", "audit_id": audit_id,
            "symbols": symbols,
            "overlay": {"ran": True, "refused": False, "m": m, "binding": binding,
                        "over_limit_after_rounding": after, "over_limit_by_hold": ""}}
    if source == "override":
        desk["report_only"] = True
    if checks:
        desk["checks"] = measured
    return DeskRiskRow(book_source=source, risk_detail={"desk": desk},
                       initial_margin=margin, equity=equity)


class Flow:
    def __init__(self, cstore, runner, settings):
        self.store, self.runner = cstore, runner
        self.book = None
        self.read_error = None
        self.now = DISPATCH_AT
        self.jobs = JobManager(cstore, settings, runner)
        self.margin = MarginInputs(MARGINS, EQUITY)
        self.dispatcher = Dispatcher(cstore, self.jobs, settings, clock=lambda: self.now,
                                     risk_reader=self._read,
                                     margin_reader=lambda portfolio_id, date: self.margin)

    def _read(self, portfolio_id, date):
        if self.read_error:
            raise StoreError(self.read_error)
        return self.book

    def modes(self):
        return [(c[0][c[0].index("--audit-id") + 1], c[0][3]) for c in self.runner.calls]

    def approve(self, save_id, publish_id, flow="auto", **payload):
        self.store.add(make_row(save_id, "save"))
        self.store.add(make_row(publish_id, "publish",
                                payload=dict({"flow": flow, "after": save_id}, **payload)))
        self.dispatcher.run_desk(save_id, PID, DAY)
        out = self.dispatcher.publish(publish_id, PID, DAY)
        self.jobs.wait_idle()
        return out


@pytest.fixture
def flow(cstore, runner, settings):
    return Flow(cstore, runner, settings)


def test_c_equal_to_b_and_within_limits_is_auto_approved(flow, cstore):
    flow.book = desk_book(80, [sym("ZN.v.0", 3, 3), sym("6C.v.0", -2, -2)])
    out = flow.approve(80, 81)
    assert out.status == "accepted"
    assert flow.modes() == [("80", "--desk"), ("81", "--publish")]
    assert cstore.rows[81].status == "done"


def test_a_change_by_dynopt_refuses_with_the_differences(flow, cstore):
    flow.book = desk_book(80, [sym("ZT.v.0", 8, 2, "cap"), sym("ZN.v.0", 3, 3)])
    flow.approve(80, 81)
    assert flow.modes() == [("80", "--desk")]  # nothing published
    row = cstore.rows[81]
    assert row.status == "refused" and row.result["reason"] == "changed"
    assert row.result["differs"] == [{"symbol": "ZT.v.0", "asked": 8.0, "given": 2.0,
                                      "moved_by": "cap", "category": "risk"}]
    assert row.result["within_limits"] is False and "cap" in row.result["breaches"]
    assert "approve the engine's book (C)" in row.message


def test_a_rule_step_change_also_needs_the_qt_to_choose(flow, cstore):
    flow.book = desk_book(80, [sym("MBT.v.0", -4, 0, "sign_close")])
    flow.approve(80, 81)
    row = cstore.rows[81]
    assert row.status == "refused" and row.result["reason"] == "changed"
    assert row.result["within_limits"] is True


def test_b_unchanged_but_over_a_cap_is_refused(flow, cstore):
    flow.book = desk_book(80, [sym("ZN.v.0", 3, 3)], gross=9.0)
    flow.approve(80, 81)
    row = cstore.rows[81]
    assert row.status == "refused" and row.result["reason"] == "outside_limits"
    assert row.result["over"] == ["L_g"]
    assert "VP or President" in row.message


def test_a_covariance_term_over_its_limit_is_reported_not_blocking(flow, cstore):
    # Decision 5: only the caps (gross, net, per-name, margin) block; R is reported.
    flow.book = desk_book(80, [sym("ZN.v.0", 3, 3)], after="R")
    flow.approve(80, 81)
    assert cstore.rows[81].status == "done"
    assert flow.modes() == [("80", "--desk"), ("81", "--publish")]


def test_a_bound_cap_does_not_block(flow, cstore):
    # The overlay cut the book to the gross cap: C is at, not over, the cap.
    flow.book = desk_book(80, [sym("ZN.v.0", 3, 3)], binding="L_g", m=0.8, gross=8.0)
    flow.approve(80, 81)
    assert cstore.rows[81].status == "done"


def test_accept_c_publishes_the_engines_book(flow, cstore):
    flow.book = desk_book(80, [sym("ZT.v.0", 8, 2, "cap")])
    flow.approve(80, 81, flow="accept_c", acknowledged=["ZT.v.0"])
    assert flow.modes() == [("80", "--desk"), ("81", "--publish")]
    assert cstore.rows[81].status == "done"


def test_accept_c_refuses_a_c_still_over_a_limit(flow, cstore):
    flow.book = desk_book(80, [sym("ZT.v.0", 8, 2, "cap")], after="L_g")
    flow.approve(80, 81, flow="accept_c")
    row = cstore.rows[81]
    assert row.status == "refused" and row.result["over"] == ["L_g"]


def test_a_failed_save_approves_nothing(flow, cstore, runner):
    runner.behave = runner.engine_records("failed", rc=1)
    flow.book = desk_book(80, [sym("ZN.v.0", 3, 3)])
    flow.approve(80, 81)
    row = cstore.rows[81]
    assert row.status == "refused" and row.result["reason"] == "incomplete"
    assert flow.modes() == [("80", "--desk")]


def test_a_book_from_another_save_is_stale(flow, cstore):
    flow.book = desk_book(79, [sym("ZN.v.0", 3, 3)])
    flow.approve(80, 81)
    row = cstore.rows[81]
    assert row.status == "refused" and row.result["reason"] == "stale"


def test_an_unreadable_risk_check_fails_retryably(flow, cstore):
    flow.read_error = "database error (OperationalError)"
    flow.approve(80, 81)
    assert cstore.rows[81].status == "failed"


def test_the_save_is_queued_first_when_only_the_publish_arrives(flow, cstore):
    # A lost RunDesk call (or a re-drive): the publish puts its save on the queue ahead of it.
    flow.book = desk_book(80, [sym("ZN.v.0", 3, 3)])
    cstore.add(make_row(80, "save"))
    cstore.add(make_row(81, "publish", payload={"flow": "auto", "after": 80}))
    flow.dispatcher.publish(81, PID, DAY)
    flow.jobs.wait_idle()
    assert flow.modes() == [("80", "--desk"), ("81", "--publish")]


@pytest.mark.parametrize("payload, needle", [
    ({"flow": "bogus", "after": 80}, "unknown approval flow"),
    ({"flow": "auto"}, "names no row"),
    ({"flow": "override_b", "after": 80}, "runs after a override_decision"),
])
def test_a_malformed_flow_is_refused(flow, cstore, payload, needle):
    cstore.add(make_row(80, "save"))
    cstore.add(make_row(81, "publish", payload=payload))
    out = flow.dispatcher.publish(81, PID, DAY)
    assert out.status == "refused" and needle in out.message


def test_the_cutoff_still_applies(flow, cstore):
    flow.book = desk_book(80, [sym("ZN.v.0", 3, 3)])
    cstore.add(make_row(80, "save"))
    cstore.add(make_row(81, "publish", payload={"flow": "auto", "after": 80},
                        created_at=dt.datetime(2026, 10, 7, 10, 1, tzinfo=NY)))
    out = flow.dispatcher.publish(81, PID, DAY)
    assert out.status == "refused"


def test_from_0930_the_approval_sends_at_once(flow, cstore):
    flow.now = dt.datetime(2026, 10, 7, 9, 40, tzinfo=NY)
    flow.book = desk_book(80, [sym("ZN.v.0", 3, 3)])
    flow.approve(80, 81)
    assert flow.runner.calls[-1][0][-1] == "--send-now"


def override_rows(cstore, approved=True):
    cstore.add(make_row(90, "override_request", status="done", requested_by="qt@x.com",
                        token_hash="h", token_expires_at=dt.datetime(2100, 1, 1, tzinfo=NY)))
    cstore.add(make_row(91, "override_decision", parent_id=90, approver_role="vp",
                        requested_by="vp@algogators.com", payload={"approved": approved}))
    cstore.add(make_row(92, "publish", requested_by="qt@x.com",
                        payload={"flow": "override_b", "after": 91}))


def test_an_approved_override_books_and_publishes_b(flow, cstore):
    override_rows(cstore)
    flow.book = desk_book(91, [sym("ZN.v.0", 60, 60)], source="override")
    flow.dispatcher.record_decision(91)
    flow.dispatcher.publish(92, PID, DAY)
    flow.jobs.wait_idle()
    assert flow.modes() == [("91", "--override"), ("92", "--publish")]
    assert cstore.rows[92].status == "done"


def test_a_rejected_override_publishes_nothing(flow, cstore):
    override_rows(cstore, approved=False)
    flow.dispatcher.record_decision(91)
    flow.dispatcher.publish(92, PID, DAY)
    flow.jobs.wait_idle()
    assert flow.modes() == []
    assert cstore.rows[92].status == "refused"
    assert cstore.rows[92].result["reason"] == "rejected"


# -- the multipliers (desk.proto 1.5.0) --------------------------------------------------------

def test_the_run_multipliers_come_from_the_stored_book():
    row = desk_book(80, [sym("ZN.v.0", 3, 3)], binding="L_g", m=0.8)
    row = DeskRiskRow(**{**row.__dict__, "risk_scale": 0.76})
    report = rk.derive_desk_risk(row)
    out = {x.name: x for x in rk.run_multipliers(report, row)}
    assert out["m"].value == 0.8 and "gross leverage" in out["m"].detail
    assert out["risk_scale"].value == 0.76
    none = rk.run_multipliers(rk.derive_desk_risk(None), None)
    assert [x.value for x in none] == [None, None]


def test_the_model_books_m_is_risk_requested():
    row = DeskRiskRow(book_source="model", risk_detail={"risk_requested": 0.9,
                                                        "binding_term": "R"})
    out = {x.name: x for x in rk.run_multipliers(rk.derive_desk_risk(row), row)}
    assert out["m"].value == 0.9 and "portfolio risk" in out["m"].detail


def test_the_configured_idm_and_fdm(tmp_path):
    (tmp_path / "defaults.json").write_text(json.dumps(
        {"strategy_defaults": {"fdm": [[1, 1], [2, 1.03], [6, 1.26]]}}))
    pdir = tmp_path / "portfolios" / "qt"
    pdir.mkdir(parents=True)
    (pdir / "portfolio.json").write_text(json.dumps({"strategies": {
        "TREND": {"enabled_live": True, "config": {"idm": 2.5, "ema_windows": [[2, 8]] * 6}},
        "OFF": {"enabled_live": False, "config": {"idm": 9}},
        "OWN_FDM": {"config": {"ema_windows": [[2, 8], [4, 16]], "fdm": [[2, 1.5]]}}}}))
    out = {x.name: x.value for x in rk.load_multipliers(str(tmp_path), str(pdir))}
    assert out == {"idm:TREND": 2.5, "fdm:TREND": 1.26, "fdm:OWN_FDM": 1.5}
    assert rk.load_multipliers(str(tmp_path), str(tmp_path / "missing")) == []


# -- decision 5: one gate for every publish path (HD 2026-10-16; Dom 2026-10-16) ---------------

def plain_publish(flow, publish_id=90):
    flow.store.add(make_row(publish_id, "publish"))
    out = flow.dispatcher.publish(publish_id, PID, DAY)
    flow.jobs.wait_idle()
    return flow.store.rows[publish_id]


def test_a_plain_publish_of_a_book_within_the_caps_publishes(flow):
    flow.book = desk_book(80, [sym("ZN.v.0", 3, 3)])
    assert plain_publish(flow).status == "done"
    assert flow.modes() == [("90", "--publish")]


def test_a_plain_publish_of_a_book_over_a_cap_is_refused(flow):
    # The same gate as Approve: the per-name cap is over, so nothing is published.
    flow.book = desk_book(80, [sym("ZN.v.0", 3, 3)], cap=2.5)
    row = plain_publish(flow)
    assert row.status == "refused" and row.result["over"] == ["CAP"]
    assert flow.modes() == []


def test_a_plain_publish_with_a_bound_limit_publishes(flow):
    flow.book = desk_book(80, [sym("ZT.v.0", 8, 2, "cap")], gross=8.0)
    assert plain_publish(flow).status == "done"


def test_a_plain_publish_fails_closed_when_the_risk_check_cannot_be_read(flow):
    # D-42: Approve never goes through with the risk check unread.
    flow.read_error = "database error (OperationalError)"
    row = plain_publish(flow)
    assert row.status == "failed" and "risk check" in row.message
    assert flow.modes() == []


def test_the_models_book_is_never_blocked(flow):
    flow.book = DeskRiskRow(book_source="model", initial_margin=600_000.0, equity=500_000.0)
    assert plain_publish(flow).status == "done"


def override_publish(flow, cstore):
    """An approved override's decision (91, booked by the engine) and its publish (92)."""
    override_rows(cstore)
    flow.dispatcher.record_decision(91)
    flow.dispatcher.publish(92, PID, DAY)
    flow.jobs.wait_idle()
    return cstore.rows[92]


def test_an_approved_override_may_pass_gross_net_and_per_name(flow, cstore):
    flow.book = desk_book(91, [sym("ZN.v.0", 60, 60)], gross=16.7, net=9.0, cap=13.0,
                          source="override")
    assert override_publish(flow, cstore).status == "done"


def test_an_approved_override_never_passes_the_margin_cap(flow, cstore):
    flow.book = desk_book(91, [sym("ZN.v.0", 60, 60)], gross=16.7, margin=616_369.0,
                          equity=486_164.0, source="override")
    row = override_publish(flow, cstore)
    assert row.status == "refused" and row.result["over"] == ["MARGIN"]
    assert "cannot be overridden" in row.message
    assert flow.modes() == [("91", "--override")]


def test_an_override_publish_needs_its_own_booking(flow, cstore):
    # The day's book is a later save, not decision 91's booking: nothing is published.
    flow.book = desk_book(95, [sym("ZN.v.0", 3, 3)])
    row = override_publish(flow, cstore)
    assert row.status == "refused" and row.result["reason"] == "stale"


def test_a_desk_book_whose_caps_were_not_measured_is_refused(flow):
    # No checks recorded (no pass answered): the caps cannot be shown to hold.
    flow.book = desk_book(80, [sym("ZN.v.0", 3, 3)], checks=False)
    row = plain_publish(flow)
    assert row.status == "refused"
    assert set(row.result["over"]) == {"L_g", "L_n", "CAP"}


def test_a_cap_labelled_bound_but_over_its_max_still_blocks():
    # The cap cut one symbol (the check reads "bound") yet the largest name is still over.
    report = rk.derive_desk_risk(desk_book(80, [sym("ZT.v.0", 8, 2, "cap")], cap=2.4))
    assert [c.status for c in report.checks if c.term == "CAP"] == [rk.BOUND]
    assert [b.term for b in rk.publish_blockers(report)] == ["CAP"]


def test_an_incomplete_book_is_never_reported_as_the_models():
    # An override refused by the margin cap leaves qt = B with book_source cleared.
    row = DeskRiskRow(book_source=None, gross_leverage=46.0, initial_margin=572_086.0,
                      equity=493_699.0)
    report = rk.derive_desk_risk(row)
    assert report.state == rk.INCOMPLETE and not report.within_limits
    assert [b.term for b in rk.publish_blockers(report)] == ["incomplete"]


def test_a_plain_publish_of_an_incomplete_book_is_refused(flow):
    flow.book = DeskRiskRow(book_source=None, initial_margin=572_086.0, equity=493_699.0)
    row = plain_publish(flow)
    assert row.status == "refused" and row.result["reason"] == "incomplete_book"
    assert "save again" in row.message
    assert flow.modes() == []


def test_a_plain_publish_of_an_override_booking_is_refused(flow):
    # After an override day is rescinded the qt book is still B as the override booked it: a
    # plain Approve would publish it past the caps with no approval (decisions 10 and 21).
    flow.book = desk_book(91, [sym("ZN.v.0", 60, 60)], cap=2.5, source="override")
    row = plain_publish(flow)
    assert row.status == "refused" and row.result["reason"] == "override_booking"
    assert "new override" in row.message
    assert flow.modes() == []


def test_an_override_approval_publishes_once(flow, cstore):
    # D-84: after its day is rescinded, the approval is spent; the booking is still B.
    flow.book = desk_book(91, [sym("ZN.v.0", 60, 60)], source="override")
    assert override_publish(flow, cstore).status == "done"
    cstore.add(make_row(93, "publish", requested_by="qt@x.com",
                        payload={"flow": "override_b", "after": 91}))
    flow.dispatcher.publish(93, PID, DAY)
    flow.jobs.wait_idle()
    row = cstore.rows[93]
    assert row.status == "refused" and row.result == {"reason": "approval_used", "after": 91,
                                                      "publish": 92}
    assert "needs a new override" in row.message
    assert flow.modes() == [("91", "--override"), ("92", "--publish")]


def test_the_gate_reads_the_portfolios_risk_json_like_get_desk_risk(flow, cstore, settings):
    # N5: one gate. A desk row with no recorded checks is compared with risk.json's maxima,
    # as GetDeskRisk does, so the blockers the desk service acts on are the ones AlgoLens shows.
    import dataclasses
    import pathlib

    from conftest import PDIR

    risk = pathlib.Path(settings.config_dir) / "portfolios" / PDIR / "risk.json"
    risk.write_text(json.dumps({"modules": [{"type": "carver", "max_gross_leverage": 8.0,
                                             "max_net_leverage": 6.0, "per_name_cap": 2.0}]}))
    book = desk_book(80, [sym("ZN.v.0", 3, 3)], checks=False)
    flow.book = dataclasses.replace(book, gross_leverage=9.5, net_leverage=1.0)

    flow.approve(80, 81)

    row = cstore.rows[81]
    assert row.status == "refused"
    assert "L_g" in row.result["over"]
    gate = rk.publish_blockers(rk.derive_desk_risk(flow.book, rk.load_limits(str(risk.parent))))
    assert [b.term for b in gate] == row.result["over"]
