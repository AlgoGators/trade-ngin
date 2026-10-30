"""Render a DailyReport to an email: HTML from the template plus inline PNG charts."""

from dataclasses import dataclass
from pathlib import Path
from types import SimpleNamespace

from jinja2 import Environment, FileSystemLoader, StrictUndefined, select_autoescape

from . import charts, daily

HERE = Path(__file__).resolve().parent

# Mail clients show only the first ~90 characters of the body next to the subject.
_FMT = SimpleNamespace(
    money=daily.money,
    signed_money=daily.signed_money,
    quantity=daily.quantity,
    price=daily.price,
    percent=daily.percent,
)


def _tone(value):
    return "down" if value < 0 else "up"


def _stats(report):
    stats = [
        {"label": "Positions", "value": f"{len(report.positions)}", "tone": "up"},
        {"label": "Notional", "value": daily.compact_money(report.total_notional), "tone": "up"},
    ]
    if report.realized:
        stats.append(
            {
                "label": "Realized PnL",
                "value": daily.signed_money(report.total_realized_pnl),
                "tone": _tone(report.total_realized_pnl),
            }
        )
    else:
        stats.append({"label": "Realized PnL", "value": "n/a", "tone": "up"})
    stats.append({"label": "Trades", "value": f"{len(report.executions)}", "tone": "up"})
    return stats


@dataclass(frozen=True)
class RenderedEmail:
    subject: str
    html: str
    text: str
    images: dict  # content-id -> PNG/JPEG bytes


class HtmlReportRenderer:
    def __init__(self):
        self.env = Environment(
            loader=FileSystemLoader(str(HERE / "templates")),
            autoescape=select_autoescape(["html"]),
            undefined=StrictUndefined,
            trim_blocks=True,
            lstrip_blocks=True,
        )

    def render_daily(self, report):
        images = {"brandmark": (HERE / "assets" / "brandmark.jpg").read_bytes()}
        chart_rows = []
        for cid, title, data, draw in (
            ("equity", "Equity curve", report.equity, charts.equity_curve),
            ("drawdown", "Drawdown from the peak", report.drawdown, charts.drawdown),
            (
                "leverage",
                "Leverage",
                report.leverage,
                lambda pts: charts.leverage(pts, report.net_leverage),
            ),
            ("margin", "Margin posted", report.margin_history, charts.margin_posted),
            ("daily_pnl", "Daily PnL", report.daily_pnl, charts.daily_pnl),
            (
                "contracts",
                "Contracts traded per day",
                report.contracts_per_day,
                charts.contracts_per_day,
            ),
            ("pnl_by_symbol", "PnL by symbol", report.pnl_by_symbol, charts.pnl_by_symbol),
            ("composition", "Portfolio composition", report.composition, charts.composition),
        ):
            if data:
                try:
                    images[cid] = draw(data)
                except Exception as exc:  # noqa: BLE001 - a chart's failure is its own line
                    chart_rows.append({"cid": "", "title": title,
                                       "missing": f"not shown ({type(exc).__name__})"})
                    continue
                chart_rows.append({"cid": cid, "title": title, "missing": ""})

        subject = daily.subject(report)
        pnl = (
            f"Realized {daily.signed_money(report.total_realized_pnl)}. " if report.realized else ""
        )
        preheader = (
            f"{pnl}{len(report.positions)} positions, "
            f"{daily.money(report.total_notional)} notional, "
            f"{len(report.executions)} trades."
        )
        html = self.env.get_template("daily_report.html").render(
            report=report,
            subject=subject,
            preheader=preheader,
            fmt=_FMT,
            stats=_stats(report),
            charts=chart_rows,
            date_long=report.report_date.strftime("%A, %B %d, %Y").replace(" 0", " "),
        )
        return RenderedEmail(
            subject=subject, html=html, text=self._text(report, preheader), images=images
        )

    @staticmethod
    def _text(report, preheader):
        lines = [
            f"AlgoGators daily trading report - {report.portfolio_name}",
            report.report_date.isoformat(),
            "",
            preheader,
            "",
        ]
        for n in report.notices:
            lines += [n.title, n.body, ""]
        if report.positions:
            lines.append("Positions")
            lines += [
                f"  {p.symbol}: {daily.quantity(p.quantity)} @ {daily.price(p.price)} "
                f"({daily.money(p.notional)})"
                for p in report.positions
            ]
            lines.append("")
        if report.executions:
            lines.append("Executions")
            lines += [
                f"  {e.side} {daily.quantity(e.quantity)} {e.symbol} @ {daily.price(e.price)}"
                for e in report.executions
            ]
            lines.append("")
        lines += list(report.footnotes)
        return "\n".join(lines).strip() + "\n"
