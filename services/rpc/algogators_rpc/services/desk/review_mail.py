"""The desk's review e-mail of an evening build (issues #191, #192; catchup.py).

After the evening run builds the desk portfolio's book for the next trading day, the scheduler
mails QT_REVIEW_RECIPIENTS once per build: the target day, the bar date it was built from, each
symbol's new target against the position held now (the last published day's book) with the
change, the risk pass's moved_by, the symbols data-ngin reported missing, whether the desk's
saved edits were kept, whether it supersedes an earlier e-mail for the same day, the desk link
and the deadline.

It goes through the portfolio's email.json SMTP settings, as the override e-mail does
(override_mail.py). QT_EMAIL_DISABLED=1: nothing is sent; the body is written to
<QT_LOG_DIR>/qt-review-mail.log instead. Either way the build is recorded in
<QT_LOG_DIR>/qt-review-mail.json (the alert state's pattern), so a restart does not mail it again.
"""

from __future__ import annotations

import datetime as dt
import html
import json
import logging
import os
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Mapping, Optional, Sequence, Tuple

log = logging.getLogger("desk.review")

STATE = "qt-review-mail.json"
BODY_LOG = "qt-review-mail.log"
DEADLINE = "Approve before 09:30 ET or the model book is sent at 10:00."
KEEP_DAYS = 30


@dataclass(frozen=True)
class Line:
    quantity: float
    moved_by: Optional[str] = None


# {book: {symbol: Line}}
DayBooks = Dict[str, Dict[str, Line]]


@dataclass(frozen=True)
class Review:
    portfolio_id: str
    target: dt.date               # the trading day the book is for
    bar_date: dt.date             # the bars it was built from (ops.data_ready.bar_date)
    built_at: dt.datetime         # when its system book was written
    books: DayBooks               # the target day's system and qt books
    held_date: Optional[dt.date]  # the last published day before the target
    held: Dict[str, Line]         # its book (qt on a desk portfolio)
    desk_edits_kept: bool
    data_status: str              # complete | partial
    missing: Tuple[str, ...] = ()
    missing_last_bar: Mapping[str, dt.date] = field(default_factory=dict)
    supersedes: Optional[str] = None   # when the earlier e-mail of the day went out
    desk_url: str = ""


def _qty(value: Optional[float]) -> str:
    if value is None:
        return "&ndash;"
    return str(int(value)) if float(value).is_integer() else f"{value:g}"


def _change(new: Optional[float], old: Optional[float]) -> str:
    delta = (new or 0.0) - (old or 0.0)
    if abs(delta) < 1e-9:
        return "0"
    text = str(int(delta)) if float(delta).is_integer() else f"{delta:g}"
    return f"+{text}" if delta > 0 else text


def build(review: Review) -> Tuple[str, str]:
    e = html.escape
    qt = review.books.get("qt", {})
    system = review.books.get("system", {})
    target_book = qt or system
    symbols = sorted(set(target_book) | set(system) | set(review.held))
    show_model = review.desk_edits_kept
    rows = []
    for sym in symbols:
        new = target_book.get(sym)
        old = review.held.get(sym)
        model = system.get(sym)
        new_q = new.quantity if new else None
        old_q = old.quantity if old else None
        changed = abs((new_q or 0.0) - (old_q or 0.0)) >= 1e-9
        style = ' style="background:#fff3cd"' if changed else ""
        moved = (new.moved_by if new else None) or ""
        cells = [f"<td>{e(sym)}</td>",
                 f"<td align=\"right\">{_qty(old_q)}</td>",
                 f"<td align=\"right\">{_qty(new_q)}</td>",
                 f"<td align=\"right\">{_change(new_q, old_q)}</td>"]
        if show_model:
            cells.append(f"<td align=\"right\">{_qty(model.quantity if model else None)}</td>")
        cells.append(f"<td>{e(moved)}</td>")
        rows.append(f"<tr{style}>" + "".join(cells) + "</tr>")
    held_label = (f"Held now ({review.held_date})" if review.held_date
                  else "Held now (no published day)")
    head = ["Symbol", held_label, f"New target ({review.target})", "Change"]
    if show_model:
        head.append("Model book")
    head.append("Moved by (risk)")
    table = ("<table border=\"1\" cellpadding=\"4\" cellspacing=\"0\"><tr>"
             + "".join(f"<th>{e(h)}</th>" for h in head) + "</tr>"
             + ("".join(rows) or f"<tr><td colspan=\"{len(head)}\">no positions recorded</td></tr>")
             + "</table>")
    notes = []
    if review.supersedes:
        notes.append(f"<p><b>This e-mail supersedes the one sent at {e(review.supersedes)} for "
                     f"{e(str(review.target))}</b>: the book was rebuilt from newer data.</p>")
    if review.desk_edits_kept:
        notes.append("<p><b>The desk's saved edits for this day were kept.</b> Only the model "
                     "(system) book was rebuilt; the new target column is the desk's book, the "
                     "model book column is the rebuilt model.</p>")
    if review.missing:
        items = []
        for sym in review.missing:
            last = review.missing_last_bar.get(sym)
            items.append(f"<li>{e(sym)}: last bar {e(str(last)) if last else 'none'}</li>")
        notes.append(f"<p>data-ngin reported {len(review.missing)} symbol(s) missing for "
                     f"{e(str(review.bar_date))} (status {e(review.data_status)}); each is "
                     "within the engine's staleness tolerance and is held at its last price:</p>"
                     f"<ul>{''.join(items)}</ul>")
    else:
        notes.append(f"<p>Data for {e(str(review.bar_date))}: {e(review.data_status)}, no "
                     "symbol missing.</p>")
    link = review.desk_url
    subject = (f"QT review: {review.portfolio_id} book for {review.target} "
               f"(bars of {review.bar_date})" + (" - updated" if review.supersedes else ""))
    body = (
        "<html><body>"
        f"<p>The model built <b>{e(review.portfolio_id)}</b>'s book for trading day "
        f"<b>{e(str(review.target))}</b> from the bars of <b>{e(str(review.bar_date))}</b> "
        f"(built {e(review.built_at.astimezone(dt.timezone.utc).strftime('%Y-%m-%d %H:%M'))} "
        "UTC).</p>"
        + "".join(notes)
        + f"<p><b>{e(DEADLINE)}</b></p>"
        f"<p>Review and approve on the AlgoLens desk: <a href=\"{e(link)}\">{e(link)}</a> "
        f"(Portfolio tab, {e(review.portfolio_id)}).</p>"
        f"{table}"
        "<p>Held now: the book of the last published day. New target: the book the desk "
        "approves for the target day. Moved by: the step of the risk pass that moved the "
        "symbol (empty: not moved).</p>"
        "<p>From the QT scheduler on engine-rpc; one e-mail per build.</p>"
        "</body></html>")
    return subject, body


class ReviewState:
    """Which builds were mailed: {"<pid>|<target>|<built_at>": {"at": iso, "to": [...],
    "email_disabled": bool}} in <QT_LOG_DIR>/qt-review-mail.json."""

    def __init__(self, log_dir: str):
        self.dir = Path(log_dir)

    @staticmethod
    def key(portfolio_id: str, target: dt.date, built_at: dt.datetime) -> str:
        return f"{portfolio_id}|{target.isoformat()}|" \
               f"{built_at.astimezone(dt.timezone.utc).isoformat()}"

    def _path(self) -> Path:
        return self.dir / STATE

    def load(self) -> Dict[str, dict]:
        try:
            data = json.loads(self._path().read_text(encoding="utf-8"))
            return data if isinstance(data, dict) else {}
        except (OSError, ValueError):
            return {}

    def sent(self, portfolio_id: str, target: dt.date, built_at: dt.datetime) -> bool:
        return self.key(portfolio_id, target, built_at) in self.load()

    def earlier(self, portfolio_id: str, target: dt.date) -> Optional[str]:
        """When the newest earlier e-mail for the same day went out (None: none)."""
        prefix = f"{portfolio_id}|{target.isoformat()}|"
        times = [v.get("at", "") for k, v in self.load().items()
                 if k.startswith(prefix) and isinstance(v, dict)]
        return max(times) if times else None

    def record(self, portfolio_id: str, target: dt.date, built_at: dt.datetime, at: dt.datetime,
               to: Sequence[str], email_disabled: bool) -> bool:
        state = self.load()
        state[self.key(portfolio_id, target, built_at)] = {
            "at": at.astimezone(dt.timezone.utc).isoformat(timespec="seconds"),
            "to": list(to), "email_disabled": email_disabled}
        keep = (at.date() - dt.timedelta(days=KEEP_DAYS)).isoformat()
        state = {k: v for k, v in state.items() if k.split("|")[1] >= keep}
        try:
            self.dir.mkdir(parents=True, exist_ok=True)
            tmp = self._path().with_suffix(".tmp")
            tmp.write_text(json.dumps(state, sort_keys=True, indent=1), encoding="utf-8")
            os.replace(tmp, self._path())
            return True
        except OSError as exc:
            log.error("cannot write the review e-mail state", extra={"error": type(exc).__name__})
            return False

    def log_body(self, at: dt.datetime, to: List[str], subject: str, body: str) -> None:
        """QT_EMAIL_DISABLED=1: the e-mail that would have gone out."""
        try:
            self.dir.mkdir(parents=True, exist_ok=True)
            with open(self.dir / BODY_LOG, "a", encoding="utf-8") as f:
                f.write(f"=== {at.astimezone(dt.timezone.utc).isoformat(timespec='seconds')} "
                        f"to {', '.join(to)} (not sent: QT_EMAIL_DISABLED=1)\n"
                        f"Subject: {subject}\n{body}\n")
        except OSError as exc:
            log.error("cannot write the review e-mail log", extra={"error": type(exc).__name__})
