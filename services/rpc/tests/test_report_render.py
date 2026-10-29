"""The book e-mail: the engine's report read into blocks, laid out, and put in a MIME message.

The bodies are the engine's own golden reports (tests/core/fixtures/email, kept equal to
EmailSender's output by tests/core/test_email_fixtures.cpp), so a change to the engine's markup
that this reader no longer understands shows here.
"""

import base64
from pathlib import Path

import pytest

from algogators_rpc.services.report.engine_report import (
    Banner,
    Callout,
    Figures,
    Image,
    ReportInputError,
    Section,
    Table,
    build_report,
    load_manifest,
    read_body,
)
from algogators_rpc.services.report.mailer import build_message
from algogators_rpc.services.report.renderer import HtmlReportRenderer

FIXTURES = Path(__file__).resolve().parents[3] / "tests" / "core" / "fixtures" / "email"
PNG = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg=="
)


def _body(name):
    path = FIXTURES / f"{name}.html"
    if not path.exists():
        pytest.skip(f"engine fixture {path} not present")
    return path.read_text(encoding="utf-8")


def _manifest(name="conservative_2026-04-28", day="2026-04-28", body=None, **extra):
    return {
        "version": 1,
        "portfolio_id": "QT_CONSERVATIVE",
        "date": day,
        "subject": f"[QT QT_CONSERVATIVE] Daily Trading Report - {day}",
        "body_html": body if body is not None else _body(name),
        "images": [],
        "attachments": [],
        **extra,
    }


def _figures(blocks):
    return {f.label: f for b in blocks if isinstance(b, Figures) for g in b.groups for f in g}


def test_every_section_of_the_engine_report_is_kept_in_order():
    blocks, header = read_body(_body("conservative_2026-04-28"))
    sections = [b.title for b in blocks if isinstance(b, Section)]
    assert sections[:5] == [
        "Today's Positions",
        "Daily Executions",
        "Yesterday's Finalized Position Results",
        "Previous trading day (2026-04-27)",
        "Performance Metrics",
    ]
    assert "Symbols Reference" in sections
    assert header.endswith("Conservative Portfolio")


def test_figures_are_the_engines_text_with_its_tones_and_groups():
    blocks, _ = read_body(_body("conservative_2026-04-28"))
    f = _figures(blocks)
    assert f["Total PnL (Net)"].value.text == "$3,505.07"
    assert f["Total PnL (Net)"].tone == "up"
    assert f["Sharpe Ratio"].value.text == "0.20"
    assert f["Sortino Ratio"].value.text == "0.27"
    assert f["Daily Total PnL (Net)"].value.text == "$604.19"
    perf = next(b for b in blocks if isinstance(b, Figures) and "Sharpe Ratio" in
                [x.label for g in b.groups for x in g])
    assert len(perf.groups) == 4  # the engine's four groups, separated by its blank lines


def test_the_settled_table_keeps_its_marks_and_the_roll_legs_their_block():
    blocks, _ = read_body(_body("conservative_2026-04-28"))
    tables = [b for b in blocks if isinstance(b, Table)]
    settled = next(t for t in tables if "Latest price" in t.header)
    he = next(r for r in settled.rows if r[0].body.text == "HE.v.0")
    assert he[3].body.text == "–[1]"  # no bar that day: the dash and note 1
    mbt = next(r for r in settled.rows if r[0].body.text == "QMBT.v.0")
    assert mbt[4].body.text == "$0.00[2]"  # rolled that day: note 2
    assert any(t.header and t.header[0] == "Roll Leg" for t in tables)
    notes = " ".join(b.body.text for b in blocks if hasattr(b, "body"))
    assert "Contract rolled that day. P&L is not booked on the roll." in notes


def test_the_day_before_banner_and_the_netting_note_come_through():
    blocks, _ = read_body(_body("base_2026-04-27"))
    banner = next(b for b in blocks if isinstance(b, Banner))
    assert banner.title == "Yesterday was Sunday"
    assert any("begin trading Sunday evening" in ln.text for ln in banner.lines)
    text = " ".join(b.body.text for b in blocks if hasattr(b, "body"))
    assert "Netting between strategies" in text


def test_the_risk_flag_at_the_top_of_the_body_is_an_alert():
    body = _body("conservative_2026-04-28").replace(
        '<div class="container">\n',
        '<div class="container">\n<div class="alert-note" id="risk-module-failure">\n'
        "<strong>RISK MODULE FAILED - BOOK HELD:</strong> the portfolio risk module x could not "
        "evaluate today's book (boom).\n</div>\n",
        1,
    )
    blocks, _ = read_body(body)
    first = next(b for b in blocks if isinstance(b, Callout))
    assert first.style == "alert" and first.body.text.startswith("RISK MODULE FAILED")


def test_charts_are_the_engines_images_and_a_chart_not_handed_over_is_left_out():
    body = '<div class="container"><h2>Charts</h2><h3>Equity Curve</h3><div><img src="cid:equity_chart" alt="Equity"></div><h3>Drawdown</h3><div><img src="cid:drawdown" alt="DD"></div></div>'
    m = _manifest(body=body, images=[{"cid": "equity_chart", "base64": base64.b64encode(PNG).decode()}])
    report = build_report(m)
    assert [b.cid for b in report.blocks if isinstance(b, Image)] == ["equity_chart"]
    assert report.images["equity_chart"] == PNG


def test_engine_text_is_escaped_and_no_script_survives():
    body = '<div class="container"><p>a &lt;b&gt; <script>alert(1)</script><a href="http://x">link</a></p></div>'
    email = HtmlReportRenderer().render(build_report(_manifest(body=body)))
    assert "<script>" not in email.html and "alert(1)" not in email.html
    assert "a &lt;b&gt;" in email.html
    assert "http://x" not in email.html


def test_rendered_email_has_the_tiles_the_subject_and_a_text_part():
    email = HtmlReportRenderer().render(build_report(_manifest()))
    assert email.subject == "[QT QT_CONSERVATIVE] Daily Trading Report - 2026-04-28"
    assert "$503,505.07" in email.html and "Portfolio value" in email.html
    assert 'role="presentation"' in email.html and "max-width: 600px" in email.html
    assert "Sortino Ratio: 0.27" in email.text
    assert "brandmark" in email.images


def test_every_fixture_renders():
    for name, day in (
        ("base_2026-04-27", "2026-04-27"),
        ("conservative_2026-04-28", "2026-04-28"),
        ("equity_2026-06-24", "2026-06-24"),
    ):
        email = HtmlReportRenderer().render(build_report(_manifest(name, day)))
        assert "Daily Trading Report" in email.html


def test_the_message_is_text_html_inline_charts_and_the_csv_files(tmp_path):
    csv = tmp_path / "positions_2026-04-28.csv"
    csv.write_text("symbol,quantity\nES,1\n", encoding="utf-8")
    report = build_report(_manifest())
    email = HtmlReportRenderer().render(report)
    msg = build_message("fund@x.org", ["a@x.org"], ["vp@x.org"], email,
                        [str(csv), str(tmp_path / "missing.csv")])
    assert msg.get_content_type() == "multipart/mixed"
    assert msg["Cc"] == "vp@x.org"
    names = [p.get_filename() for p in msg.iter_attachments()]
    assert names == ["positions_2026-04-28.csv"]  # the unreadable file is left off
    types = [p.get_content_type() for p in msg.walk()]
    assert "text/plain" in types and "text/html" in types and "image/jpeg" in types


def test_a_bad_manifest_is_refused(tmp_path):
    p = tmp_path / "m.json"
    p.write_text('{"version": 2}', encoding="utf-8")
    with pytest.raises(ReportInputError):
        load_manifest(p)
    with pytest.raises(ReportInputError):
        load_manifest(tmp_path / "absent.json")
