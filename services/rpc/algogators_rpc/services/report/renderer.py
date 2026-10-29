"""Lay the engine's report out as the fund's e-mail: HTML from the template, a plain-text part."""

from dataclasses import dataclass
from pathlib import Path

from jinja2 import Environment, FileSystemLoader, StrictUndefined, select_autoescape

from .engine_report import Banner, Callout, Caption, Figures, Image, Paragraph, Report, Section
from .engine_report import Subheading, Table, TONE_COLOR

HERE = Path(__file__).resolve().parent

# The key numbers over the report: (tile label, the engine's figure labels, in preference order).
# Each value is the engine's own text; a figure the report does not carry has no tile.
TILES = (
    ("Portfolio value", ("Current Portfolio Value",)),
    ("Previous day PnL", ("Daily Total PnL (Net)",)),
    ("Total PnL", ("Total PnL (Net)",)),
    ("Positions", ("Sleeve Positions", "Active Positions", "Total Positions")),
)


@dataclass(frozen=True)
class RenderedEmail:
    subject: str
    html: str
    text: str
    images: dict  # content-id -> PNG/JPEG bytes


def _tiles(report: Report) -> list:
    out = []
    for label, keys in TILES:
        fig = report.figure(*keys)
        if fig is not None and fig.value:
            out.append({"label": label, "value": fig.value.text, "tone": fig.tone})
    return out


def _text(report: Report, preheader: str) -> str:
    lines = [
        f"AlgoGators daily trading report - {report.portfolio_name}",
        report.report_date.isoformat(),
        "",
    ]
    if preheader:
        lines += [preheader, ""]
    for b in report.blocks:
        if isinstance(b, Section):
            lines += ["", b.title.upper(), "-" * min(len(b.title), 60)]
        elif isinstance(b, Subheading):
            lines += ["", b.title]
        elif isinstance(b, Caption):
            lines.append(b.text)
        elif isinstance(b, Banner):
            lines += [b.title, *(ln.text for ln in b.lines), ""]
        elif isinstance(b, (Callout, Paragraph)):
            lines += [b.body.text, ""]
        elif isinstance(b, Figures):
            for g in b.groups:
                lines += [f"  {f.label}: {f.value.text}" if f.label else f"  {f.value.text}" for f in g]
                lines.append("")
        elif isinstance(b, Table):
            if b.header:
                lines.append("  " + " | ".join(b.header))
            lines += ["  " + " | ".join(c.body.text for c in row) for row in b.rows]
            lines.append("")
        elif isinstance(b, Image):
            lines.append(f"[chart: {b.alt or b.cid}]")
    text = "\n".join(lines)
    while "\n\n\n" in text:
        text = text.replace("\n\n\n", "\n\n")
    return text.strip() + "\n"


class HtmlReportRenderer:
    def __init__(self):
        self.env = Environment(
            loader=FileSystemLoader(str(HERE / "templates")),
            autoescape=select_autoescape(["html"]),
            undefined=StrictUndefined,
            trim_blocks=True,
            lstrip_blocks=True,
        )

    def render(self, report: Report) -> RenderedEmail:
        images = {"brandmark": (HERE / "assets" / "brandmark.jpg").read_bytes(), **report.images}
        tiles = _tiles(report)
        preheader = " · ".join(f"{t['label']} {t['value']}" for t in tiles)
        html = self.env.get_template("daily_report.html").render(
            report=report,
            subject=report.subject,
            preheader=preheader,
            tiles=tiles,
            tone_color=TONE_COLOR,
            date_long=report.report_date.strftime("%A, %B %d, %Y").replace(" 0", " "),
        )
        return RenderedEmail(
            subject=report.subject, html=html, text=_text(report, preheader), images=images
        )
