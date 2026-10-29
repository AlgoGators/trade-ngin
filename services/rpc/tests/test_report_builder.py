"""The daily-report builder: a day's stored rows in, a valid schema_version 1 payload out."""

from datetime import date, datetime

from algogators_rpc.services.report.builder import (
    ReportFlags,
    build_daily_payload,
    day_notices,
    footnotes,
    holiday_name,
)
from algogators_rpc.services.report.calc import build_strategy_detail
from algogators_rpc.services.report.daily import parse_daily_report
from algogators_rpc.services.report.renderer import HtmlReportRenderer


def _latest(**over):
    row = {
        "current_portfolio_value": 510000.0,
        "volatility": 10.0,
        "total_annualized_return": 20.0,
        "sharpe_ratio": 1.37,
        "sortino_ratio": 2.11,
        "max_drawdown": 3.25,
        "total_pnl": 10000.0,
        "total_cumulative_return": 2.0,
        "total_roll_costs": 12.5,
        "date": date(2026, 10, 21),
        "daily_return": 0.5,
        "gross_leverage": None,
        "net_leverage": 1.2,
        "portfolio_leverage": 1.3,
        "margin_posted": 40000.0,
        "equity_to_margin_ratio": 12.75,
        "margin_cushion": 0.8,
        "gross_notional": 750000.0,
        "total_unrealized_pnl": 0.0,
        "total_realized_pnl": 10350.0,
        "total_transaction_costs": 350.0,
        "cash_available": 470000.0,
    }
    row.update(over)
    return row


def _previous():
    return {
        "date": date(2026, 10, 20),
        "daily_return": 0.12,
        "daily_unrealized_pnl": 0.0,
        "daily_realized_pnl": 608.12,
        "daily_transaction_costs": 3.93,
        "daily_roll_costs": 2.45,
        "daily_pnl": 604.19,
    }


def _rows(**over):
    curve = [
        {"timestamp": datetime(2026, 10, d), "equity": 500000.0 + 1000 * d} for d in range(1, 21)
    ]
    rows = dict(
        latest=_latest(),
        previous=_previous(),
        start=date(2025, 10, 5),
        book="qt",
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
                "netting_adjustment": 0.6,
                "execution_type": "STRATEGY",
                "execution_time": datetime(2026, 10, 21, 9, 30),
            },
            {
                "symbol": "CL.v.0",
                "side": "SELL",
                "quantity": 1,
                "price": 68.5,
                "commissions_fees": 2.45,
                "netting_adjustment": 0,
                "execution_type": "ROLL",
                "execution_time": datetime(2026, 10, 21, 9, 30),
            },
        ],
        yesterday_positions=[
            {"symbol": "ES.v.0", "quantity": 1, "average_price": 5390.0, "daily_realized_pnl": 500.0},
            {"symbol": "NQ.v.0", "quantity": 1, "average_price": 19000.0, "daily_realized_pnl": 120.0},
            {"symbol": "ZN.v.0", "quantity": 0, "average_price": 110.0, "daily_realized_pnl": 0.0},
        ],
    )
    rows.update(over)
    return rows


def _detail(**over):
    cfg = {"id": "qt", "name": "Trend Following", "description": "", "initial_equity": 500000.0,
           "portfolio_id": "P", "strategy_type": "S"}
    return build_strategy_detail(cfg, _rows(**over))


def _report(flags=ReportFlags(), **over):
    payload = build_daily_payload(_detail(**over), "QT_CONSERVATIVE", "QT Conservative",
                                  date(2026, 10, 21), flags=flags)
    return parse_daily_report(payload)


def test_every_figure_is_the_stored_one_not_recomputed():
    m = {x.label: x.value for x in _report().metrics}
    assert m["Sharpe ratio"] == "1.37"  # stored, not annualized return / volatility
    assert m["Sortino ratio"] == "2.11"
    assert m["Max drawdown"] == "3.25%"  # stored, not from the equity curve
    assert m["Total PnL (net)"] == "+$10,000.00"  # stored total_pnl
    assert m["Total cumulative return"] == "2.00%"
    assert m["Roll costs"] == "$12.50"
    assert m["Gross leverage"] == "1.30x"  # gross falls back to portfolio_leverage


def test_the_previous_day_box_is_the_settled_rows_stored_figures():
    report = _report()
    assert report.previous_day_label == "2026-10-20"
    box = {x.label: x.value for x in report.previous_day}
    assert box["Daily realized PnL (gross)"] == "+$608.12"
    assert box["Daily transaction costs"] == "$3.93"
    assert box["Daily roll costs"] == "$2.45"
    assert box["Daily total PnL (net)"] == "+$604.19"


def test_the_settled_table_lists_every_held_row_with_its_stored_pnl():
    report = _report()
    assert [r.symbol for r in report.realized] == ["ES", "NQ"]  # ZN was flat; ES did not change
    assert report.total_realized_pnl == 620.0
    es = report.realized[0]
    assert (es.entry_price, es.exit_price) == (5390.0, 5400.0)


def test_roll_legs_are_costs_never_trades_and_the_cost_is_after_netting():
    report = _report()
    assert [e.symbol for e in report.executions] == ["ES"]
    assert report.executions[0].cost == 1.5  # 2.10 own cost less the 0.60 netting adjustment
    assert [e.symbol for e in report.roll_legs] == ["CL"]
    assert report.total_cost == 1.5 + 2.45


def test_an_ordinary_day_has_no_banner_whatever_its_weekday():
    assert day_notices(date(2026, 10, 26)) == []  # a Monday: the run found its Sunday session
    assert day_notices(date(2025, 12, 26)) == []  # the day after Christmas, session found


def test_no_session_is_the_runs_flag_and_the_calendar_only_names_the_day():
    monday = day_notices(date(2026, 10, 26), ReportFlags(no_session=True))
    assert [n["title"] for n in monday] == ["Yesterday was Sunday", "No Executions", "Note"]
    weekday = day_notices(date(2026, 10, 22), ReportFlags(no_session=True))  # a feed hole
    assert [n["title"] for n in weekday] == ["No Executions", "Note"]
    assert weekday[0]["body"].startswith("No session was found for yesterday (2026-10-21)")
    holiday = day_notices(date(2025, 12, 26), ReportFlags(no_session=True))
    assert holiday[0]["title"] == "Yesterday was Christmas Day"


def test_a_no_session_day_has_no_settled_table():
    assert _report(ReportFlags(no_session=True)).realized == ()


def test_a_held_book_is_flagged_first():
    first = day_notices(date(2026, 10, 21), ReportFlags(alert="the portfolio risk module x failed"))
    assert first[0]["kind"] == "alert" and "risk module x" in first[0]["body"]


def test_the_footnote_carries_the_books_own_start_date():
    assert footnotes("2025-10-05")[1] == "All values reflect a trading start date of October 5th, 2025."
    assert footnotes("2026-03-01")[1].endswith("March 1st, 2026.")
    assert not any("start date" in n for n in footnotes(None))
    assert any("October 5th, 2025" in n for n in _report().footnotes)


def test_the_built_payload_renders_with_roll_legs():
    email = HtmlReportRenderer().render_daily(_report())
    assert "Daily Trading Report" in email.html and "equity" in email.images
    assert "Roll legs" in email.html and "Sortino ratio" in email.html


def test_no_row_for_the_day_is_no_report():
    assert _detail(latest=None) is None


def test_cme_closure_applies_only_when_every_held_group_was_closed():
    good_friday = date(2026, 4, 3)  # CME rates and FX settled; equity index, energy, metals closed
    assert holiday_name(good_friday, ["ES", "CL", "GC"]) == "Good Friday"
    assert holiday_name(good_friday, ["ES", "ZN"]) is None  # ZN settled
    assert holiday_name(good_friday) is None  # unknown book: a partial closure is not a closure
    assert holiday_name(date(2025, 12, 25), []) == "Christmas Day"  # every group closed
