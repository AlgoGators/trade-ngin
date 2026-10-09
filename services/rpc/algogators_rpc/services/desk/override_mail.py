"""The override request e-mail (contract section 6, RequestOverride; D4, D5).

Done in Python, not by the engine. The worker:

1. reads the approvers from QT_APPROVERS ("vp=<email>,president=<email>", both required);
2. generates a one-time token, stores sha256(token) and an expiry 48 h from now() on the row;
3. builds an HTML e-mail with the approval link and the three books side by side (model =
   system, desk request = qt_proposal, the engine's desk result = qt with moved_by);
4. sends it through the portfolio's email.json SMTP settings, or, when e-mail is disabled,
   writes it into the row's result so an operator can still approve.

The token and the SMTP password are never logged. The token appears only in the e-mail (and in
the result when e-mail is disabled, by design), and only its hash is stored.
"""

from __future__ import annotations

import hashlib
import html
import json
import logging
import secrets
import smtplib
from dataclasses import dataclass
from email.message import EmailMessage
from typing import Callable, Dict, List, Optional, Sequence, Tuple
from urllib.parse import urlencode

from .command_store import Books, CommandRow
from .config import CommandSettings
from .portfolios import portfolios_root

log = logging.getLogger("desk.override")

TOKEN_HOURS = 48
ROLES = ("vp", "president")


class ApproverError(ValueError):
    pass


def parse_approvers(raw: str) -> Dict[str, str]:
    """"vp=a@x,president=b@y" -> {"vp": "a@x", "president": "b@y"}; both roles required."""
    out: Dict[str, str] = {}
    for part in (raw or "").split(","):
        if "=" not in part:
            continue
        role, email = (p.strip() for p in part.split("=", 1))
        if role.lower() in ROLES and email:
            out[role.lower()] = email
    missing = [r for r in ROLES if r not in out]
    if missing:
        raise ApproverError("QT_APPROVERS must be set to \"vp=<email>,president=<email>\"; "
                            "missing: " + ", ".join(missing))
    return out


def hash_token(token: str) -> str:
    return hashlib.sha256(token.encode("utf-8")).hexdigest()


def approve_link(base: str, token: str) -> str:
    return f"{base}?{urlencode({'token': token})}"


@dataclass(frozen=True)
class SmtpConfig:
    host: str
    port: int
    username: str
    password: str
    from_email: str
    use_tls: bool

    def __repr__(self) -> str:  # never the password
        return f"SmtpConfig({self.username}@{self.host}:{self.port})"


def load_smtp(settings: CommandSettings, portfolio_dir: str) -> Tuple[Optional[SmtpConfig], str]:
    """Returns (config, "") when e-mail is enabled, else (None, why it is disabled)."""
    if settings.email_disabled:
        return None, "QT_EMAIL_DISABLED=1"
    path = portfolios_root(settings.config_dir) / portfolio_dir / "email.json"
    if not path.is_file():
        return None, f"no {path}"
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        return None, f"cannot read {path}: {type(exc).__name__}"
    if not isinstance(data, dict):
        return None, f"{path} is not an object"
    if data.get("enabled") is False:
        return None, f"{path} has enabled=false"
    user, password = str(data.get("username") or ""), str(data.get("password") or "")
    if not user or not password or user.startswith("YOUR_") or password.startswith("YOUR_"):
        return None, f"{path} has no SMTP credentials"
    if not data.get("smtp_host"):
        return None, f"{path} has no smtp_host"
    try:
        port = int(data.get("smtp_port") or 587)
    except (TypeError, ValueError):
        return None, f"{path} smtp_port is not a number"
    from_email = str(data.get("from_email") or "")
    if not from_email or from_email.startswith("YOUR_"):
        from_email = user
    return SmtpConfig(host=str(data["smtp_host"]), port=port, username=user, password=password,
                      from_email=from_email, use_tls=bool(data.get("use_tls", True))), ""


def send_smtp(cfg: SmtpConfig, to: Sequence[str], subject: str, body_html: str) -> None:
    msg = EmailMessage()
    msg["Subject"] = subject
    msg["From"] = cfg.from_email
    msg["To"] = ", ".join(to)
    msg.set_content("This message is HTML; open it in a client that shows HTML.")
    msg.add_alternative(body_html, subtype="html")
    with smtplib.SMTP(cfg.host, cfg.port, timeout=30) as smtp:
        if cfg.use_tls:
            smtp.starttls()
        smtp.login(cfg.username, cfg.password)
        smtp.send_message(msg)


def _qty(value: Optional[float]) -> str:
    if value is None:
        return "&ndash;"
    return str(int(value)) if float(value).is_integer() else f"{value:g}"


def build_email(row: CommandRow, books: Books, link: str) -> Tuple[str, str]:
    subject = f"QT override request: {row.portfolio_id} {row.date}"
    system, proposal, qt = (books.get(b, {}) for b in ("system", "qt_proposal", "qt"))
    symbols = sorted(set(system) | set(proposal) | set(qt))
    e = html.escape
    rows = []
    for sym in symbols:
        m, p, q = system.get(sym), proposal.get(sym), qt.get(sym)
        changed = (p.quantity if p else None) != (q.quantity if q else None)
        style = ' style="background:#fff3cd"' if changed else ""
        rows.append(
            f"<tr{style}><td>{e(sym)}</td>"
            f"<td align=\"right\">{_qty(m.quantity if m else None)}</td>"
            f"<td align=\"right\">{_qty(p.quantity if p else None)}</td>"
            f"<td align=\"right\">{_qty(q.quantity if q else None)}</td>"
            f"<td>{e((q.moved_by if q else None) or '')}</td></tr>")
    table = ("<table border=\"1\" cellpadding=\"4\" cellspacing=\"0\">"
             "<tr><th>Symbol</th><th>Model book</th><th>Desk request</th>"
             "<th>Engine desk result</th><th>Moved by</th></tr>"
             + ("".join(rows) or "<tr><td colspan=\"5\">no positions recorded</td></tr>")
             + "</table>")
    body = (
        "<html><body>"
        f"<p>The QT desk asks to book its selection for <b>{e(row.portfolio_id)}</b> on "
        f"<b>{e(str(row.date))}</b> exactly as requested, overriding the engine's risk pass.</p>"
        f"<p>Requested by: {e(row.requested_by)}<br>Reason: {e(row.reason or '')}</p>"
        f"<p><a href=\"{e(link)}\">Review and approve or reject</a><br>"
        f"{e(link)}</p>"
        f"<p>The link works once and expires in {TOKEN_HOURS} hours. The requester may not "
        "approve their own request.</p>"
        f"{table}"
        "<p>Model book: the untouched model run (system). Desk request: the desk's proposal "
        "(qt_proposal). Engine desk result: what the one-pass risk run gave back (qt), with the "
        "step that moved each symbol.</p>"
        "</body></html>")
    return subject, body


Sender = Callable[[SmtpConfig, Sequence[str], str, str], None]


@dataclass(frozen=True)
class MailOutcome:
    status: str          # done | failed
    result: Optional[dict]
    message: str


def run_override_request(row: CommandRow, portfolio_dir: str, settings: CommandSettings,
                         store, sender: Sender = send_smtp) -> MailOutcome:
    """Everything after the claim. The caller records the outcome on the row."""
    try:
        approvers = parse_approvers(settings.approvers)
    except ApproverError as exc:
        return MailOutcome("failed", None, str(exc))
    books = store.books(row.portfolio_id, row.date)
    token = secrets.token_urlsafe(32)
    expires = store.set_token(row.id, hash_token(token), TOKEN_HOURS)
    if expires is None:
        return MailOutcome("failed", None, "could not store the approval token (row no longer "
                                           "running)")
    subject, body = build_email(row, books, approve_link(settings.approve_url_base, token))
    to: List[str] = [approvers[r] for r in ROLES]
    smtp, why = load_smtp(settings, portfolio_dir)
    if smtp is None:
        log.info("override e-mail disabled; body written to the row's result",
                 extra={"audit_id": row.id, "portfolio_id": row.portfolio_id, "error": why})
        return MailOutcome("done", {"emailed": [], "email_disabled": True,
                                    "email": {"to": to, "subject": subject, "body": body}},
                           "e-mail disabled: body logged in result")
    try:
        sender(smtp, to, subject, body)
    except Exception as exc:  # smtplib errors can echo the server reply, never our password
        log.error("override e-mail failed",
                  extra={"audit_id": row.id, "portfolio_id": row.portfolio_id,
                         "error": type(exc).__name__})
        return MailOutcome("failed", None, f"override e-mail failed: {type(exc).__name__}")
    log.info("override e-mail sent", extra={"audit_id": row.id,
                                            "portfolio_id": row.portfolio_id})
    return MailOutcome("done", {"emailed": list(ROLES)}, "override e-mail sent to vp and president")
