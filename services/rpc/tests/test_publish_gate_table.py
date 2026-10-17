"""The design doc's publish-gate table, row by row (N5, N10; Dom 2026-10-16): which readings block
a desk book and which block an approved override. The rows the approval-flow tests already cover
end to end are here too, so the table has one test each."""

import copy

import pytest

from test_desk_risk import MEASURED, desk_row, sym
from algogators_rpc.services.desk import risk as rk

LIMITS = [("max_gross_leverage", 8.0), ("max_net_leverage", 6.0), ("R_max", 2.25),
          ("R_jump_max", 4.5), ("R_shock_max", 4.0), ("per_name_cap", 2.0), ("trim_max", 5.0)]
BOOK = [sym("ZN.v.0", 3, 3)]


def checks(**over):
    out = copy.deepcopy(MEASURED)
    for term, value in over.items():
        out[term]["value"] = value
    return out


def blockers(row):
    return [b.term for b in rk.publish_blockers(rk.derive_desk_risk(row, LIMITS))]


def desk(**kw):
    return desk_row(BOOK, **kw)


def override(**kw):
    return desk_row(BOOK, source="override", report_only=True, **kw)


@pytest.mark.parametrize("term, value", [("L_g", 9.5), ("L_n", 6.5), ("L_n", -6.5),
                                         ("CAP", 2.4)])
def test_gross_net_and_per_name_over_their_max_block_a_desk_book(term, value):
    assert blockers(desk(checks=checks(**{term: value}))) == [term]


@pytest.mark.parametrize("term, value", [("L_g", 16.7), ("L_n", 9.0), ("CAP", 13.0)])
def test_gross_net_and_per_name_over_their_max_do_not_block_an_override(term, value):
    assert blockers(override(checks=checks(**{term: value}))) == []


def test_margin_over_the_equity_blocks_a_desk_book_and_an_override():
    for row in (desk(checks=checks(), initial_margin=600_000.0, equity=500_000.0),
                override(checks=checks(), initial_margin=600_000.0, equity=500_000.0)):
        assert blockers(row) == ["MARGIN"]


def test_a_cap_not_measured_blocks_a_desk_book_fail_closed():
    assert set(blockers(desk(checks=None))) >= {"CAP"}


def test_the_overlay_refused_blocks_a_desk_book():
    assert blockers(desk(checks=checks(), refused=True)) != []


def test_on_an_override_an_unmeasured_cap_or_a_refusal_leaves_margin_only():
    # Gross, net and per-name are waived for an approved override, measured or not, and so is
    # the overlay's refusal; the margin cap still applies.
    assert blockers(override(checks=None, refused=True)) == []
    assert blockers(override(checks=None, refused=True, initial_margin=600_000.0,
                             equity=500_000.0)) == ["MARGIN"]


@pytest.mark.parametrize("make", [desk, override])
def test_a_limit_that_bound_blocks_neither(make):
    # C was cut to fit the cap: the overlay bound on CAP and the reading sits at its max.
    assert blockers(make(checks=checks(CAP=2.0), binding="CAP", m=0.8)) == []


@pytest.mark.parametrize("make", [desk, override])
@pytest.mark.parametrize("term, value", [("R", 0.9), ("R_jump", 1.5), ("R_shock", 1.2),
                                         ("TRIM", 9)])
def test_the_covariance_terms_and_the_trim_are_reported_not_blocking(make, term, value):
    row = make(checks=checks(**{term: value}))
    report = rk.derive_desk_risk(row, LIMITS)
    assert blockers(row) == []
    assert any(c.term == term for c in report.checks)


@pytest.mark.parametrize("make", [desk, override])
@pytest.mark.parametrize("rule", ["sign_close", "buffer", "rounding", "search", "hold"])
def test_a_trading_rule_move_is_reported_not_blocking(make, rule):
    row = desk_row([sym("ZN.v.0", 3, 1, moved_by=rule)], checks=checks(),
                   **({"source": "override", "report_only": True} if make is override else {}))
    assert blockers(row) == []


def test_the_models_own_book_is_never_blocked():
    row = rk.DeskRiskRow(book_source="model", initial_margin=600_000.0, equity=500_000.0)
    assert blockers(row) == []
