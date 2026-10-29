"""Report charts as PNG bytes, drawn to match algogators.com rather than matplotlib's defaults.

No frame, no legend box, no gridline clutter: a hairline baseline, one quiet horizontal grid, the
website's ink and orange, and Poppins from the website's own font files. The last value is
labelled on the chart itself so nobody reads it off an axis.

Charts are drawn at twice their display width so they stay sharp on high-density screens.
"""

import io
from pathlib import Path

import matplotlib

matplotlib.use("Agg")

import matplotlib.dates as mdates
import matplotlib.pyplot as plt
from matplotlib import font_manager
from matplotlib.ticker import FuncFormatter

from .daily import compact_money, money

INK = "#14141A"
ORANGE = "#FF5C00"
MUTED = "#6C6A66"
HAIRLINE = "#D9D6CF"
GRID = "#EEECE7"
PAPER = "#F6F4EF"
GRAYS = ("#4A4B55", "#9C9BA6", "#C9C7C1", "#E4E1DA")

ASSETS = Path(__file__).resolve().parent / "assets"
DISPLAY_WIDTH_PX = 680
SCALE = 2
_DPI = 200

_font_loaded = False


def _use_brand_font():
    global _font_loaded
    if not _font_loaded:
        font_manager.fontManager.addfont(str(ASSETS / "Poppins-Regular.ttf"))
        _font_loaded = True
    plt.rcParams["font.family"] = ["Poppins", "DejaVu Sans"]


def _compact_money(value, _pos=None):
    return compact_money(value)


def _figure(height_px):
    _use_brand_font()
    width_in = DISPLAY_WIDTH_PX * SCALE / _DPI
    fig, ax = plt.subplots(figsize=(width_in, height_px * SCALE / _DPI), dpi=_DPI)
    fig.patch.set_facecolor("white")
    ax.set_facecolor("white")
    for side in ("top", "right", "left"):
        ax.spines[side].set_visible(False)
    ax.spines["bottom"].set_color(HAIRLINE)
    ax.spines["bottom"].set_linewidth(1)
    ax.tick_params(axis="both", length=0, labelsize=8, labelcolor=MUTED, pad=6)
    ax.grid(axis="y", color=GRID, linewidth=1)
    ax.set_axisbelow(True)
    return fig, ax


def _png(fig):
    fig.tight_layout(pad=0.6)
    buf = io.BytesIO()
    fig.savefig(buf, format="png", facecolor="white")
    plt.close(fig)
    return buf.getvalue()


def _dates(labels):
    from datetime import date

    return [date.fromisoformat(x) for x in labels]


def _date_axis(ax, days):
    ax.xaxis.set_major_locator(mdates.AutoDateLocator(minticks=3, maxticks=6))
    ax.xaxis.set_major_formatter(mdates.DateFormatter("%b %d"))


def equity_curve(points):
    """points: ((iso date, value), ...). A single orange line with the latest value labelled."""
    fig, ax = _figure(250)
    xs = _dates([p[0] for p in points])
    ys = [p[1] for p in points]
    ax.plot(xs, ys, color=ORANGE, linewidth=2.2, solid_capstyle="round")
    ax.fill_between(xs, ys, min(ys), color=ORANGE, alpha=0.07, linewidth=0)
    ax.scatter([xs[-1]], [ys[-1]], color=ORANGE, s=26, zorder=3)
    ax.annotate(
        money(ys[-1]),
        (xs[-1], ys[-1]),
        xytext=(0, 9),
        textcoords="offset points",
        ha="right",
        fontsize=9,
        color=INK,
        fontweight="bold",
    )
    span = (max(ys) - min(ys)) or abs(max(ys)) or 1
    ax.set_ylim(min(ys) - span * 0.08, max(ys) + span * 0.22)
    ax.yaxis.set_major_formatter(FuncFormatter(_compact_money))
    _date_axis(ax, len(xs))
    return _png(fig)


def daily_pnl(points):
    """Bars per day: ink when up, orange when down, with a solid zero line."""
    fig, ax = _figure(220)
    xs = _dates([p[0] for p in points])
    ys = [p[1] for p in points]
    colors = [INK if y >= 0 else ORANGE for y in ys]
    ax.bar(xs, ys, color=colors, width=0.62, linewidth=0)
    ax.axhline(0, color=INK, linewidth=1)
    ax.yaxis.set_major_formatter(FuncFormatter(_compact_money))
    _date_axis(ax, len(xs))
    return _png(fig)


def pnl_by_symbol(points):
    """Horizontal bars, best at the top. The value sits at the end of each bar."""
    ordered = sorted(points, key=lambda p: p[1])
    height = max(150, 28 * len(ordered) + 40)
    fig, ax = _figure(height)
    ax.grid(axis="y", visible=False)
    ax.grid(axis="x", color=GRID, linewidth=1)
    labels = [p[0] for p in ordered]
    vals = [p[1] for p in ordered]
    ax.barh(labels, vals, color=[INK if v >= 0 else ORANGE for v in vals], height=0.56, linewidth=0)
    ax.axvline(0, color=INK, linewidth=1)
    ax.spines["bottom"].set_visible(False)
    ax.xaxis.set_major_formatter(FuncFormatter(_compact_money))
    ax.tick_params(axis="y", labelsize=9, labelcolor=INK)
    pad = (max(map(abs, vals)) or 1) * 0.02
    for i, v in enumerate(vals):
        ax.text(
            v + (pad if v >= 0 else -pad),
            i,
            money(v),
            va="center",
            ha="left" if v >= 0 else "right",
            fontsize=8,
            color=MUTED,
        )
    lo, hi = min([*vals, 0]), max([*vals, 0])
    margin = (hi - lo or 1) * 0.16
    ax.set_xlim(lo - margin, hi + margin)
    return _png(fig)


def composition(points):
    """One 100% bar split by group, labelled underneath. Replaces the pie chart."""
    items = [(label, abs(v)) for label, v in points if abs(v) > 0]
    total = sum(v for _, v in items) or 1
    items.sort(key=lambda it: -it[1])
    fig, ax = _figure(120 + 18 * ((len(items) + 2) // 3))
    ax.grid(visible=False)
    for side in ("bottom",):
        ax.spines[side].set_visible(False)
    ax.set_yticks([])
    ax.set_xticks([])
    palette = (ORANGE, INK, *GRAYS)
    left = 0.0
    for i, (_label, v) in enumerate(items):
        share = v / total
        ax.barh(
            [0],
            [share],
            left=left,
            color=palette[min(i, len(palette) - 1)],
            height=0.5,
            linewidth=1.5,
            edgecolor="white",
        )
        left += share
    ax.set_xlim(0, 1)
    ax.set_ylim(-1.9, 0.45)
    cols = 3
    for i, (label, v) in enumerate(items):
        x = (i % cols) / cols
        y = -0.62 - (i // cols) * 0.52
        ax.add_patch(
            plt.Rectangle(
                (x, y - 0.1),
                0.012,
                0.2,
                color=palette[min(i, len(palette) - 1)],
                transform=ax.transData,
                clip_on=False,
            )
        )
        ax.text(
            x + 0.02, y, f"{label}  {v / total * 100:.0f}%", va="center", fontsize=8.5, color=INK
        )
    return _png(fig)


def _line(points, color=ORANGE):
    xs = _dates([p[0] for p in points])
    return xs, [p[1] for p in points], color


def leverage(gross, net=()):
    """Gross leverage in orange and net in ink, each labelled at its last point."""
    fig, ax = _figure(200)
    ys_all = []
    for points, color in ((gross, ORANGE), (net, INK)):
        if not points:
            continue
        xs, ys, color = _line(points, color)
        ax.plot(xs, ys, color=color, linewidth=2.0, solid_capstyle="round")
        ax.annotate(
            f"{ys[-1]:.2f}x",
            (xs[-1], ys[-1]),
            xytext=(0, 8),
            textcoords="offset points",
            ha="right",
            fontsize=9,
            color=color,
            fontweight="bold",
        )
        ys_all += ys
    lo, hi = min([*ys_all, 0]), max(ys_all)
    ax.set_ylim(lo - (hi - lo or 1) * 0.08, hi + (hi - lo or 1) * 0.22)
    ax.axhline(0, color=INK, linewidth=1)
    ax.yaxis.set_major_formatter(FuncFormatter(lambda v, _p=None: f"{v:.1f}x"))
    _date_axis(ax, len(ys_all))
    return _png(fig)


def margin_posted(points):
    """Margin posted per day: a single line, latest value labelled."""
    return equity_curve(points)


def contracts_per_day(points):
    """Contracts traded per day as ink bars."""
    fig, ax = _figure(200)
    xs = _dates([p[0] for p in points])
    ax.bar(xs, [p[1] for p in points], color=INK, width=0.62, linewidth=0)
    ax.axhline(0, color=INK, linewidth=1)
    ax.yaxis.set_major_formatter(FuncFormatter(lambda v, _p=None: f"{v:,.0f}"))
    _date_axis(ax, len(xs))
    return _png(fig)


def drawdown(points):
    """Percent below the running peak: an orange line under a zero baseline, worst point named."""
    fig, ax = _figure(200)
    xs = _dates([p[0] for p in points])
    ys = [p[1] for p in points]
    ax.plot(xs, ys, color=ORANGE, linewidth=2.2, solid_capstyle="round")
    ax.fill_between(xs, ys, 0, color=ORANGE, alpha=0.07, linewidth=0)
    ax.axhline(0, color=INK, linewidth=1)
    low = min(range(len(ys)), key=ys.__getitem__)
    if ys[low] < 0:
        ax.scatter([xs[low]], [ys[low]], color=ORANGE, s=26, zorder=3)
        ax.annotate(
            f"{ys[low]:.2f}%",
            (xs[low], ys[low]),
            xytext=(0, -14),
            textcoords="offset points",
            ha="center",
            fontsize=9,
            color=INK,
            fontweight="bold",
        )
    top = max(ys[low] * -0.25, 0.05) if ys[low] < 0 else 0.5
    ax.set_ylim(min(ys) * 1.35 if min(ys) < 0 else -0.5, top)
    ax.yaxis.set_major_formatter(FuncFormatter(lambda v, _p=None: f"{v:.1f}%"))
    _date_axis(ax, len(xs))
    return _png(fig)
