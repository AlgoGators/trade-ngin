"""The engine's daily report, read into blocks the e-mail template lays out.

trade-ngin builds the report (EmailSender::generate_trading_report_body, the day's flags, the
charts) and hands it over as HTML in the manifest. This module reads that HTML into plain blocks: a
section heading, a table, a list of figures, a banner, a note. Every word, figure and footnote mark
stays as the engine wrote it; nothing is computed or decided here, so the report's numbers and the
rules for the day (banners, the numbered notes, the hidden tables) exist in one place only.
Markup this module does not know is kept as text, never dropped.
"""

from __future__ import annotations

import base64
import binascii
import json
import re
from dataclasses import dataclass, field
from datetime import date
from html import escape
from html.parser import HTMLParser
from pathlib import Path

from markupsafe import Markup

MANIFEST_VERSION = 1


class ReportInputError(ValueError):
    """A manifest or body the e-mail cannot be built from (the message is safe to log)."""


# --- the body as a tree ---------------------------------------------------------------------

_VOID = {"br", "img", "hr", "meta", "link", "input", "col", "wbr"}


@dataclass
class Node:
    tag: str
    attrs: dict
    children: list = field(default_factory=list)  # Node | str

    def cls(self) -> set:
        return set((self.attrs.get("class") or "").split())

    def style(self) -> str:
        return (self.attrs.get("style") or "").replace(" ", "").lower()

    def find(self, tag: str):
        for c in self.children:
            if isinstance(c, Node):
                if c.tag == tag:
                    return c
                hit = c.find(tag)
                if hit is not None:
                    return hit
        return None

    def find_all(self, tag: str) -> list:
        out = []
        for c in self.children:
            if isinstance(c, Node):
                if c.tag == tag:
                    out.append(c)
                out += c.find_all(tag)
        return out


class _Tree(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.root = Node("#root", {})
        self.stack = [self.root]

    def handle_starttag(self, tag, attrs):
        node = Node(tag, {k: v or "" for k, v in attrs})
        self.stack[-1].children.append(node)
        if tag not in _VOID:
            self.stack.append(node)

    def handle_startendtag(self, tag, attrs):
        self.stack[-1].children.append(Node(tag, {k: v or "" for k, v in attrs}))

    def handle_endtag(self, tag):
        for i in range(len(self.stack) - 1, 0, -1):
            if self.stack[i].tag == tag:
                del self.stack[i:]
                return

    def handle_data(self, data):
        self.stack[-1].children.append(data)


def parse_html(html: str) -> Node:
    tree = _Tree()
    tree.feed(html)
    tree.close()
    return tree.root


# --- inline content -------------------------------------------------------------------------

TONE_OF_CLASS = {"positive": "up", "negative": "down"}
TONE_COLOR = {"up": "#1A7F37", "down": "#B42318"}
_SPACE = re.compile(r"\s+")


@dataclass(frozen=True)
class Rich:
    """Inline content: safe HTML (only the engine's emphasis, tones and note marks) and its text."""

    html: Markup
    text: str

    def __bool__(self):
        return bool(self.text.strip())


def _tone_of(node: Node) -> str:
    for c in node.cls():
        if c in TONE_OF_CLASS:
            return TONE_OF_CLASS[c]
    return ""


def _inline_parts(nodes, html: list, text: list, skip_sup: bool = False) -> None:
    for n in nodes:
        if isinstance(n, str):
            t = _SPACE.sub(" ", n)
            html.append(escape(t))
            text.append(t)
            continue
        if n.tag in ("script", "style", "img", "head", "title"):
            continue
        if n.tag == "br":
            html.append("<br>")
            text.append("\n")
        elif n.tag in ("strong", "b"):
            html.append('<strong style="color:#14141A;">')
            _inline_parts(n.children, html, text, skip_sup)
            html.append("</strong>")
        elif n.tag in ("em", "i"):
            html.append("<em>")
            _inline_parts(n.children, html, text, skip_sup)
            html.append("</em>")
        elif n.tag == "sup":
            if skip_sup:
                continue
            inner_h, inner_t = [], []
            _inline_parts(n.children, inner_h, inner_t)
            mark = "".join(inner_t).strip()
            html.append(
                '<sup style="font-size:9px;line-height:0;color:#FF5C00;font-weight:700;">'
                + escape(mark)
                + "</sup>"
            )
            text.append(f"[{mark}]")
        elif n.tag == "span":
            tone = _tone_of(n)
            if tone:
                html.append(f'<span style="color:{TONE_COLOR[tone]};font-weight:600;">')
            _inline_parts(n.children, html, text, skip_sup)
            if tone:
                html.append("</span>")
        elif n.tag in ("div", "p", "li", "tr", "h1", "h2", "h3", "h4", "table"):
            # a block inside inline content: its own line
            if text and not "".join(text).endswith("\n"):
                html.append("<br>")
                text.append("\n")
            _inline_parts(n.children, html, text, skip_sup)
            html.append("<br>")
            text.append("\n")
        else:
            _inline_parts(n.children, html, text, skip_sup)


def _clean_breaks(html: str, text: str) -> tuple[str, str]:
    html = re.sub(r"^(\s|<br>)+|(\s|<br>)+$", "", html)
    html = re.sub(r"(<br>\s*){3,}", "<br><br>", html)
    lines = [ln.strip() for ln in text.split("\n")]
    text = "\n".join(lines).strip()
    text = re.sub(r"\n{3,}", "\n\n", text)
    return html, text


def rich(nodes, skip_sup: bool = False) -> Rich:
    html, text = [], []
    _inline_parts(nodes, html, text, skip_sup)
    h, t = _clean_breaks("".join(html), "".join(text))
    return Rich(Markup(h), t)


def plain(node_or_nodes) -> str:
    nodes = node_or_nodes.children if isinstance(node_or_nodes, Node) else node_or_nodes
    return rich(nodes).text


# --- blocks ---------------------------------------------------------------------------------


@dataclass(frozen=True)
class Section:
    title: str
    kind: str = "section"


@dataclass(frozen=True)
class Subheading:
    title: str
    kind: str = "subheading"


@dataclass(frozen=True)
class Caption:
    text: str
    kind: str = "caption"


@dataclass(frozen=True)
class Banner:
    """The engine's day-before banner (report_day_before.hpp): its title and lines."""

    title: str
    lines: tuple
    kind: str = "banner"


@dataclass(frozen=True)
class Callout:
    """An engine box: "alert" (alert-note, red) or "note" (footer-note, yellow)."""

    style: str
    body: Rich
    kind: str = "callout"


@dataclass(frozen=True)
class Cell:
    body: Rich
    tone: str = ""
    numeric: bool = False


@dataclass(frozen=True)
class Table:
    header: tuple
    rows: tuple  # tuples of Cells
    numeric_columns: tuple = ()
    kind: str = "table"


@dataclass(frozen=True)
class Figure:
    label: str
    value: Rich
    tone: str = ""


@dataclass(frozen=True)
class Figures:
    groups: tuple  # tuples of Figures; a rule separates the groups
    kind: str = "figures"


@dataclass(frozen=True)
class Image:
    cid: str
    alt: str
    kind: str = "image"


@dataclass(frozen=True)
class Paragraph:
    body: Rich
    small: bool = False
    kind: str = "paragraph"


_NUMERIC = re.compile(r"^[-+−]?\$?-?[\d,]+(\.\d+)?\s*(%|x)?$|^–$|^n/a$")


def _table(node: Node) -> Table:
    header: list = []
    rows: list = []
    for tr in node.find_all("tr"):
        cells = [c for c in tr.children if isinstance(c, Node) and c.tag in ("th", "td")]
        if not cells:
            continue
        if all(c.tag == "th" for c in cells) and not rows and not header:
            header = [plain(c) for c in cells]
            continue
        row = []
        for c in cells:
            body = rich(c.children)
            bare = rich(c.children, skip_sup=True).text.strip()
            row.append(Cell(body, _tone_of(c), bool(_NUMERIC.match(bare))))
        rows.append(tuple(row))
    width = max([len(header)] + [len(r) for r in rows] or [0])
    numeric = tuple(
        i
        for i in range(width)
        if any(i < len(r) for r in rows)
        and all(r[i].numeric or not r[i].body for r in rows if i < len(r))
    )
    return Table(tuple(header), tuple(rows), numeric)


def _figure(nodes) -> Figure | None:
    label, value, tone = "", [], ""
    for n in nodes:
        if isinstance(n, Node) and n.tag in ("strong", "b") and not label:
            label = plain(n).strip().rstrip(":").strip()
            continue
        if isinstance(n, Node) and not tone:
            spans = [n, *n.find_all("span")]
            tone = next((t for t in map(_tone_of, spans) if t), "")
        value.append(n)
    body = rich(value)
    text = body.text.strip().strip("|").strip()
    if not label and not text:
        return None
    if body.text.strip().endswith("|") or body.text.strip().startswith("|"):
        h = re.sub(r"^\s*\|\s*|\s*\|\s*$", "", str(body.html))
        body = Rich(Markup(h), text)
    return Figure(label, body, tone)


def _figures(node: Node) -> Figures:
    groups: list = [[]]
    current: list | None = None

    def close():
        nonlocal current
        if current is not None:
            fig = _figure(current)
            if fig is not None:
                groups[-1].append(fig)
        current = None

    for c in node.children:
        if isinstance(c, str):
            if current is not None:
                current.append(c)
            elif c.strip():
                current = [c]
            continue
        if c.tag == "div" or c.tag == "p":
            close()
            fig = _figure(c.children)
            if fig is not None:
                groups[-1].append(fig)
        elif c.tag == "br":
            if current is not None:
                close()
            elif groups[-1]:
                groups.append([])
        elif c.tag in ("strong", "b"):
            close()
            current = [c]
        else:
            if current is None:
                current = []
            current.append(c)
    close()
    return Figures(tuple(tuple(g) for g in groups if g))


def _has_figures(node: Node) -> bool:
    return any(
        isinstance(c, Node) and (c.tag in ("strong", "b") or "metric" in c.cls())
        for c in node.children
    )


@dataclass
class Report:
    """The engine's report as blocks, with what the header shows."""

    subject: str
    report_date: date
    portfolio_id: str
    portfolio_name: str
    blocks: list
    images: dict  # Content-ID -> bytes
    attachments: list  # paths

    def figure(self, *labels: str) -> Figure | None:
        for b in self.blocks:
            if isinstance(b, Figures):
                for g in b.groups:
                    for f in g:
                        if f.label in labels:
                            return f
        return None


class _Blocks:
    def __init__(self):
        self.out: list = []
        self.pending: list = []
        self.header_info = ""

    def flush(self):
        if self.pending:
            body = rich(self.pending)
            if body:
                self.out.append(Paragraph(body))
        self.pending = []

    def add(self, block):
        self.flush()
        self.out.append(block)

    def walk(self, nodes):
        for n in nodes:
            if isinstance(n, str):
                self.pending.append(n)
                continue
            self.node(n)
        self.flush()

    def node(self, n: Node):
        cls, style = n.cls(), n.style()
        if n.tag in ("html", "body", "#root") or (n.tag == "div" and "container" in cls):
            self.walk(n.children)
        elif n.tag in ("head", "style", "script", "title", "meta", "hr"):
            return
        elif "header-section" in cls:
            info = n.find("div")
            for d in n.find_all("div"):
                if "header-info" in d.cls():
                    info = d
            self.header_info = plain(info) if info is not None else ""
        elif n.tag == "h1":
            return
        elif n.tag == "h2":
            self.add(Section(plain(n)))
        elif n.tag in ("h3", "h4"):
            self.add(Subheading(plain(n)))
        elif n.tag == "table":
            self.add(_table(n))
        elif n.tag == "img":
            src = n.attrs.get("src", "")
            if src.startswith("cid:"):
                self.add(Image(src[4:], n.attrs.get("alt", "")))
        elif n.tag == "p":
            body = rich(n.children)
            text = body.text.strip()
            if text.startswith("Generated by"):
                return
            if body:
                self.add(Paragraph(body, small="font-size:13px" in style or "color:#666" in style))
        elif "alert-note" in cls:
            self.add(Callout("alert", rich(n.children)))
        elif "footer-note" in cls:
            self.add(Callout("note", rich(n.children)))
        elif "metrics-category" in cls or "summary-stats" in cls:
            if _has_figures(n):
                self.add(_figures(n))
            else:
                self.walk(n.children)
        elif "metric" in cls:
            fig = _figure(n.children)
            if fig is not None:
                self.add(Figures(((fig,),)))
        elif n.tag == "div" and n.find("h2") is not None and "metrics-section" not in cls:
            title = plain(n.find("h2"))
            lines = tuple(rich(p.children) for p in n.find_all("p") if rich(p.children))
            self.add(Banner(title, lines))
        elif n.tag == "div" and "font-size:11px" in style:
            self.add(Caption(plain(n)))
        elif n.tag == "div" and "font-size:12px" in style:
            body = rich(n.children)
            if body:
                self.add(Paragraph(body, small=True))
        elif n.tag == "div" and "font-size:13px" in style and _has_figures(n):
            self.add(_figures(n))
        else:
            self.flush()
            self.walk(n.children)


def read_body(html: str) -> tuple[list, str]:
    """The body's blocks and the header line ("date | name") the engine printed."""
    root = parse_html(html)
    walker = _Blocks()
    walker.walk(root.children)
    return walker.out, walker.header_info


def load_manifest(path: str | Path) -> dict:
    try:
        raw = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise ReportInputError(f"the manifest could not be read: {exc}") from None
    if not isinstance(raw, dict) or raw.get("version") != MANIFEST_VERSION:
        raise ReportInputError(f"the manifest is not version {MANIFEST_VERSION}")
    for key in ("portfolio_id", "date", "subject", "body_html"):
        if not isinstance(raw.get(key), str) or not raw[key].strip():
            raise ReportInputError(f"the manifest has no {key}")
    return raw


def build_report(manifest: dict) -> Report:
    try:
        day = date.fromisoformat(manifest["date"])
    except ValueError:
        raise ReportInputError(f"not a date: {manifest['date']!r}") from None
    blocks, header_info = read_body(manifest["body_html"])
    images = {}
    for img in manifest.get("images") or []:
        try:
            images[str(img["cid"])] = base64.b64decode(img["base64"], validate=False)
        except (KeyError, TypeError, binascii.Error):
            continue
    # an image whose chart was not handed over is left out (the engine's ledger line says why)
    blocks = [b for b in blocks if not isinstance(b, Image) or b.cid in images]
    name = header_info.split("|", 1)[1].strip() if "|" in header_info else ""
    return Report(
        subject=manifest["subject"],
        report_date=day,
        portfolio_id=manifest["portfolio_id"],
        portfolio_name=name or manifest["portfolio_id"],
        blocks=blocks,
        images=images,
        attachments=[str(a) for a in manifest.get("attachments") or []],
    )
