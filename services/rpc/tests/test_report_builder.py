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
    assert report.executions[0].cost == 2.1  # the fill's own cost, as the row shows it
    assert report.executions[0].net_cost == 1.5  # less the 0.60 netting adjustment
    assert [e.symbol for e in report.roll_legs] == ["CL"]
    assert report.total_cost == 1.5 + 2.45  # what the account paid
    assert report.netting_note.startswith("Netting between strategies: own costs $4.55")


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


# --- per-sleeve tables and migration 041's stored marks ---------------------------------------


def _sleeves_rows(**over):
    """Two sleeves, one symbol held by both, rows written by a binary of migration 041."""
    return _rows(
        positions=[
            {"strategy_name": "TREND_FOLLOWING", "symbol": "ES.v.0", "quantity": 2,
             "average_price": 5390.0, "mark_price": 5400.0},
            {"strategy_name": "TREND_FOLLOWING_FAST", "symbol": "ES.v.0", "quantity": -1,
             "average_price": 5390.0, "mark_price": 5400.0},
            {"strategy_name": "TREND_FOLLOWING", "symbol": "HE.v.0", "quantity": 1,
             "average_price": 91.3, "mark_price": None},
        ],
        yesterday_positions=[
            {"strategy_name": "TREND_FOLLOWING", "symbol": "ES.v.0", "quantity": 2,
             "average_price": 5390.0, "daily_realized_pnl": 500.0, "settle_from_price": 5380.0,
             "settle_to_price": 5390.0, "settle_mark": None},
            {"strategy_name": "TREND_FOLLOWING", "symbol": "HE.v.0", "quantity": 1,
             "average_price": 91.3, "daily_realized_pnl": 0.0, "settle_from_price": 91.3,
             "settle_to_price": None, "settle_mark": "NO_BAR"},
            {"strategy_name": "TREND_FOLLOWING_FAST", "symbol": "MBT.v.0", "quantity": 1,
             "average_price": 77740.0, "daily_realized_pnl": 0.0, "settle_from_price": 79070.0,
             "settle_to_price": 77740.0, "settle_mark": "ROLL"},
        ],
        **over,
    )


def _sleeves_report(day=date(2026, 10, 21), flags=ReportFlags(), **over):
    cfg = {"id": "qt", "name": "Trend Following", "description": "", "initial_equity": 500000.0,
           "portfolio_id": "P", "strategy_type": "S"}
    detail = build_strategy_detail(cfg, _sleeves_rows(**over))
    return parse_daily_report(build_daily_payload(detail, "QT_C", "QT C", day, flags=flags))


def test_two_sleeves_holding_one_symbol_are_two_rows():
    report = _sleeves_report()
    assert [(p.sleeve, p.symbol) for p in report.positions] == [
        ("Trend Following", "ES"), ("Trend Following Fast", "ES"), ("Trend Following", "HE")]
    assert report.sleeves[:2] == ("Trend Following", "Trend Following Fast")
    # the weight is the row's share of its own sleeve's notional
    fast = [p for p in report.positions if p.sleeve == "Trend Following Fast"]
    assert fast[0].weight == 1.0
    summary = {m.label: m.value for m in report.sleeve_summary}
    assert summary["Sleeve positions"] == "3" and summary["Total notional"] == "$750,000.00"


def test_a_row_is_valued_at_its_mark_and_a_row_with_no_bar_is_noted():
    report = _sleeves_report()
    es = report.positions[0]
    assert es.price == 5400.0 and es.note == 0  # the mark, not the average price
    he = report.positions[2]
    assert he.price == 91.3 and he.note > 0  # no bar: the last known price, with its note
    assert dict(report.notes)[he.note].startswith("No price bar for this contract that day")


def test_the_settled_table_shows_the_settles_prices_and_its_marks():
    report = _sleeves_report()
    rows = {(r.sleeve, r.symbol): r for r in report.realized}
    es = rows[("Trend Following", "ES")]
    assert (es.entry_price, es.exit_price, es.note) == (5380.0, 5390.0, 0)
    he = rows[("Trend Following", "HE")]
    assert he.exit_price is None and dict(report.notes)[he.note].startswith("No price received")
    mbt = rows[("Trend Following Fast", "MBT")]
    assert dict(report.notes)[mbt.note] == "Contract rolled that day. PnL is not booked on the roll."
    # numbered in the order they first appear: the positions' note first
    assert [n for n, _ in report.notes] == [1, 2, 3]


def test_rows_settled_before_migration_041_carry_no_notes():
    report = _report()  # the rows of _rows() have no migration-041 columns
    assert report.notes == ()
    assert all(p.note == 0 for p in report.positions)


def test_a_missing_settle_price_on_a_saturday_is_the_partial_banner():
    saturday_after = date(2026, 10, 25)  # the settled day is Saturday 10-24
    titles = [n.title for n in _sleeves_report(day=saturday_after).notices]
    assert titles == ["Yesterday was Saturday"]
    body = _sleeves_report(day=saturday_after).notices[0].body
    assert body.startswith("Some futures settlement prices for 2026-10-24 are not available")
    # a weekday with a row missing: footnote 1 only, no banner
    assert _sleeves_report(day=date(2026, 10, 22)).notices == ()


def test_the_closed_forms_follow_the_engine_word_for_word():
    sat = day_notices(date(2026, 10, 25), ReportFlags(no_session=True))
    assert [n["title"] for n in sat] == ["Yesterday was Saturday", "No Executions", "Note"]
    assert sat[1]["body"].startswith("Futures markets were closed yesterday, so no executions")
    sun = day_notices(date(2026, 10, 26), ReportFlags(no_session=True))
    assert sun[0]["body"].startswith("Agricultural futures settlement prices for Sunday")
    assert sun[2]["body"] == "Yesterday's PnL data is not available for agricultural contracts."


def test_the_annualized_return_is_labelled_while_under_a_year_of_sessions():
    labels = lambda r: [m.label for m in r.metrics]  # noqa: E731
    young = _report(latest=_latest(total_days=120, statistics_sessions_per_year=311.0574))
    assert "Annualized return (extrapolated: under one year of sessions)" in labels(young)
    grown = _report(latest=_latest(total_days=400, statistics_sessions_per_year=311.0574))
    assert "Annualized return" in labels(grown)
    unknown = _report()  # a row written before migration 041: no K, no label
    assert "Annualized return" in labels(unknown)


def test_the_sizing_capital_is_the_days_stored_reading():
    sized = _report(latest=_latest(risk_detail={"sizing_capital": 498765.4}))
    values = {m.label: m.value for m in sized.metrics}
    assert values["Sizing capital"] == "$498,765.40"
    unsized = {m.label: m.value for m in _report().metrics}
    assert unsized["Sizing capital"].startswith("no reading")


def test_the_sleeved_report_renders_its_tables_and_notes():
    email = HtmlReportRenderer().render_daily(_sleeves_report())
    assert email.html.count("Trend Following Fast") >= 2  # positions and the settled table
    assert "No price received that day" in email.html and "<sup" in email.html


# --- the review's fixes ---------------------------------------------------------------------------


def test_a_statistic_stored_null_has_no_line_never_a_zero():
    report = _report(latest=_latest(equity_to_margin_ratio=None, margin_cushion=None,
                                    sharpe_ratio=None))
    labels = [m.label for m in report.metrics]
    assert "Equity-to-margin ratio" not in labels and "Margin cushion" not in labels
    assert "Sharpe ratio" not in labels and "Sortino ratio" in labels


def test_a_row_with_no_account_value_is_no_report():
    assert _detail(latest=_latest(current_portfolio_value=None)) is None


def test_an_equity_book_has_its_own_notes_and_no_futures_sections():
    flags = ReportFlags(equities=True)
    report = _report(flags)
    labels = [m.label for m in report.metrics]
    for gone in ("Sizing capital", "Equity-to-margin ratio", "Margin cushion", "Roll costs"):
        assert gone not in labels, gone
    assert report.symbols == ()
    assert report.footnotes[0] == "This strategy is based on daily OHLCV data for equity assets."
    assert not any("tradeable universe" in n for n in report.footnotes)


def test_the_settled_table_is_labelled_with_the_settled_rows_own_date():
    # an equity Monday settles Friday: the table says Friday, as the previous-day box does
    prev = {**_previous(), "date": date(2026, 10, 16)}
    report = _report(previous=prev)
    assert report.realized_label == "2026-10-16" == report.previous_day_label


def test_a_borrow_row_is_neither_a_trade_nor_a_roll_leg():
    rows = _rows()
    borrow = {**rows["executions"][0], "execution_type": "BORROW", "quantity": 0}
    report = _report(executions=[*rows["executions"], borrow])
    assert len(report.executions) == 1 and len(report.roll_legs) == 1


ROLLS = {
    "lines": ["Silver, December to March: market move booked -$1,075; contract price difference "
              "0.670 ($3,350, not profit or loss); roll cost $45.29."],
    "rollover": [{"label": "SI", "held": "SIZ6", "target": "SIH7", "by": "11-20",
                  "deadline": "11-25", "deadline_kind": "first notice"}],
    "front_month": {"ES": "ESZ6"},
    "contract_months": {"ES": "Mar Jun Sep Dec"},
    "footer": "Each position is held in the front contract of its cycle and rolled to the next one "
              "before the first notice day or the last trading day.",
    "roll_bar_note": "Contract rolled that day. The old contract's move that day is booked; the price "
                     "difference between the two contracts is not profit or loss.",
    "roll_bar_symbols": ["NQ.v.0"],
}


def test_the_engines_roll_texts_reach_the_report():
    cfg = {"id": "qt", "name": "Trend Following", "description": "", "initial_equity": 500000.0,
           "portfolio_id": "P", "strategy_type": "S"}
    payload = build_daily_payload(
        build_strategy_detail(cfg, _rows()), "QT_C", "QT C", date(2026, 10, 21),
        symbols={"rows": [{"db": "ES", "ib": "ES", "name": "E-mini S&P", "months": "HMUZ"}],
                 "missing": []},
        flags=ReportFlags(rolls=ROLLS),
    )
    report = parse_daily_report(payload)
    assert report.roll_lines == tuple(ROLLS["lines"])
    assert report.rollover[0]["held"] == "SIZ6"
    assert (report.symbols[0].front_month, report.symbols[0].months) == ("ESZ6", "Mar Jun Sep Dec")
    assert report.footnotes[0].endswith("the first notice day or the last trading day.")
    nq = next(r for r in report.realized if r.symbol == "NQ")
    assert dict(report.notes)[nq.note] == ROLLS["roll_bar_note"]
    html = HtmlReportRenderer().render_daily(report).html
    for needle in ("Rollover notice", "SIH7", "Front month", "ESZ6", "Silver, December to March"):
        assert needle in html, needle


def test_a_chart_that_cannot_be_drawn_costs_that_chart_only(monkeypatch):
    from algogators_rpc.services.report import charts

    def boom(_points):
        raise ValueError("no")

    monkeypatch.setattr(charts, "drawdown", boom)
    email = HtmlReportRenderer().render_daily(_report())
    assert "Drawdown from the peak: not shown (ValueError)" in email.html
    assert "drawdown" not in email.images and "equity" in email.images
