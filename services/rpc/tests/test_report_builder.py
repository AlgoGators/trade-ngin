"""The daily-report builder: a portfolio detail in, a valid schema_version 1 payload out."""

from datetime import date

from algogators_rpc.services.report.calc import build_strategy_detail
from algogators_rpc.services.report.builder import build_daily_payload, calendar_notices
from algogators_rpc.services.report.daily import parse_daily_report
from algogators_rpc.services.report.renderer import HtmlReportRenderer


def _detail():
    from datetime import datetime

    latest = {
        "current_portfolio_value": 510000.0,
        "volatility": 10.0,
        "total_annualized_return": 20.0,
        "date": datetime(2026, 10, 21, 16, 30),
        "daily_return": 0.5,
        "gross_leverage": 1.5,
        "net_leverage": 1.2,
        "portfolio_leverage": 1.3,
        "margin_posted": 40000.0,
        "equity_to_margin_ratio": 12.75,
        "margin_cushion": 0.8,
        "gross_notional": 750000.0,
        "total_unrealized_pnl": 4000.0,
        "total_realized_pnl": 6000.0,
        "total_transaction_costs": 350.0,
        "cash_available": 100000.0,
    }
    curve = [
        {"timestamp": datetime(2026, 10, d), "equity": 500000.0 + 1000 * d} for d in range(1, 21)
    ]
    rows = dict(
        latest=latest,
        equity_curve=curve,
        positions=[
            {"symbol": "ES.v.0", "quantity": 2, "average_price": 5400.0},
            {"symbol": "CL.v.0", "quantity": -1, "average_price": 68.0},
        ],
        executions=[
            {
                "symbol": "ES.v.0",
                "side": "BUY",
                "quantity": 1,
                "price": 5400.0,
                "commissions_fees": 2.1,
                "execution_time": datetime(2026, 10, 21, 9, 30),
            }
        ],
        yesterday_positions=[
            {
                "symbol": "NQ.v.0",
                "quantity": 1,
                "average_price": 19000.0,
                "daily_realized_pnl": 120.0,
            }
        ],
    )
    cfg = {
        "id": "qt",
        "name": "Trend Following",
        "description": "",
        "initial_equity": 500000.0,
        "managers": ["x"],
        "portfolio_id": "P",
        "strategy_type": "S",
    }
    return build_strategy_detail(cfg, rows)


def test_a_portfolio_detail_becomes_a_payload_the_report_accepts():
    payload = build_daily_payload(_detail(), "P", "Trend Following", date(2026, 10, 21))
    report = parse_daily_report(payload)
    assert [p.symbol for p in report.positions] == ["ES", "CL"]
    assert report.positions[1].notional < 0  # a short stays signed
    assert report.executions[0].cost == 2.1
    assert report.total_realized_pnl == 120.0
    assert len(report.daily_pnl) == len(report.equity) - 1
    assert sum(w.value for w in [type("W", (), {"value": v})() for _, v in report.composition]) > 0


def test_the_built_payload_renders():
    payload = build_daily_payload(_detail(), "P", "Trend Following", date(2026, 10, 21))
    email = HtmlReportRenderer().render_daily(parse_daily_report(payload))
    assert "Daily Trading Report" in email.html and "equity" in email.images


def test_calendar_notices_follow_the_weekday_and_the_holiday_table():
    assert calendar_notices(date(2026, 10, 25))[0]["title"] == "Yesterday was Saturday"  # Sunday
    assert calendar_notices(date(2026, 10, 26))[0]["title"] == "Yesterday was Sunday"  # Monday
    assert calendar_notices(date(2026, 10, 21)) == []  # Wednesday
    assert calendar_notices(date(2025, 12, 26))[0]["title"] == "Yesterday was Christmas Day"


def test_a_closed_day_says_nothing_was_executed_in_trade_ngins_words():
    kinds = [n["kind"] for n in calendar_notices(date(2025, 12, 26))]
    assert kinds == ["holiday", "info", "alert"]
    alert = calendar_notices(date(2025, 12, 26))[2]
    assert alert["title"] == "No Executions"
    assert alert["body"].startswith("Since yesterday (2025-12-25) was a market holiday")
    saturday = calendar_notices(date(2026, 10, 25))
    assert "the next trading" in saturday[2]["body"]
    sunday = calendar_notices(date(2026, 10, 26))
    assert "agricultural contracts" in sunday[1]["body"]


def test_a_day_with_settled_positions_gets_only_the_partial_banner():
    (only,) = calendar_notices(date(2025, 12, 26), settled=True)
    assert only["body"].startswith("Some futures settlement prices for 2025-12-25")
    assert only["footnote"] == "Please monitor these positions closely."


def test_the_payload_carries_trade_ngins_metric_groups_and_the_drawdown():
    payload = build_daily_payload(
        _detail(), "QT_CONSERVATIVE", "QT Conservative", date(2026, 10, 21)
    )
    report = parse_daily_report(payload)
    assert [m.group for m in report.metrics] == sorted(m.group for m in report.metrics)
    assert {m.group for m in report.metrics} == {0, 1, 2, 3}
    assert report.margin_posted is not None and report.drawdown
    assert all(v <= 0 for _, v in report.drawdown)
    assert report.previous_day and report.previous_day_label == "2026-10-20"


def test_cme_closure_applies_only_when_every_held_group_was_closed():
    from algogators_rpc.services.report.builder import holiday_name

    good_friday = date(2026, 4, 3)  # CME rates and FX settled; equity index, energy, metals closed
    assert holiday_name(good_friday, ["ES", "CL", "GC"]) == "Good Friday"
    assert holiday_name(good_friday, ["ES", "ZN"]) is None  # ZN settled
    assert holiday_name(good_friday) is None  # unknown book: a partial closure is not a closure
    assert holiday_name(date(2025, 1, 9), ["ES"]) == "National Day of Mourning (Jimmy Carter)"
    assert holiday_name(date(2025, 1, 9), ["ES", "CL"]) is None  # NYSE closed, CME traded the rest
    assert holiday_name(date(2025, 12, 25), []) == "Christmas Day"  # every group closed
