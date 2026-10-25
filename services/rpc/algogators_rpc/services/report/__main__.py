"""Send (or preview) one portfolio's daily book e-mail.

    python -m algogators_rpc.services.report --portfolio-id QT_CONSERVATIVE --date 2026-10-23
    ... --to me@example.org            send to that address only (no Cc)
    ... --preview-dir /tmp/out         write the HTML and charts there, send nothing

Exit status: 0 sent (or previewed), 1 not sent (one line on stderr, never a password), 2 bad
arguments. Database settings are the engine's (DB_* or defaults.json), SMTP settings the
portfolio's email.json (desk/override_mail.load_smtp); QT_EMAIL_DISABLED=1 sends nothing and fails.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

import psycopg
from psycopg.rows import dict_row

from ..desk.config import ConfigError, load_settings
from ..desk.override_mail import ApproverError, load_smtp, parse_approvers
from ..desk.portfolios import PortfolioConfigError, resolve_portfolio_dir
from . import data
from .builder import build_daily_payload
from .calc import build_strategy_detail
from .daily import parse_daily_report
from .mailer import send_message
from .renderer import HtmlReportRenderer

# Never mailed, whatever the config says.
EXCLUDED = {"dhebrank@msn.com"}


def _clean(addrs, seen):
    out = []
    for a in addrs:
        a = (a or "").strip()
        if a and a.lower() not in EXCLUDED and a.lower() not in seen:
            seen.add(a.lower())
            out.append(a)
    return out


def book_recipients(settings, portfolio_dir: str, extra_cc: str = ""):
    """(to, cc): to_emails of the portfolio's email.json; the approvers and QT_MAIL_CC in Cc."""
    path = Path(settings.config_dir) / "portfolios" / portfolio_dir / "email.json"
    try:
        raw = json.loads(path.read_text(encoding="utf-8")).get("to_emails")
    except (OSError, ValueError, AttributeError):
        raw = None
    seen: set = set()
    to = _clean(raw if isinstance(raw, list) else [], seen)
    cc_raw = [a for a in extra_cc.replace(";", ",").split(",")]
    try:
        cc_raw += list(parse_approvers(settings.approvers).values())
    except ApproverError:
        pass
    return to, _clean(cc_raw, seen)


def build_email(conn, portfolio_id: str, day):
    """The rendered e-mail for the portfolio's day, from the database."""
    with conn.cursor() as cur:
        cfg = data.fetch_registry(cur, portfolio_id)
        rows = data.fetch_detail_rows(cur, cfg["strategy_type"], portfolio_id)
    detail = build_strategy_detail(cfg, rows)
    if detail is None:
        raise data.ReportDataError(f"no results stored yet for {portfolio_id}")
    roots = [p["symbol"].replace(".v.0", "") for p in detail["positions"]]
    with conn.cursor(row_factory=psycopg.rows.tuple_row) as cur:
        symbols = data.fetch_symbols(cur, roots)
        try:
            history = data.fetch_history(cur, portfolio_id, cfg["strategy_type"])
        except psycopg.Error:
            history = None  # a failed history read costs the charts, never the report
    payload = build_daily_payload(detail, portfolio_id, cfg["name"], day,
                                  symbols=symbols, history=history)
    return HtmlReportRenderer().render_daily(parse_daily_report(payload))


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--portfolio-id", required=True)
    ap.add_argument("--date", required=True)
    ap.add_argument("--to", help="send to this address only (no Cc)")
    ap.add_argument("--preview-dir", help="write the HTML and images here and send nothing")
    args = ap.parse_args(argv)

    try:
        day = data.report_day(args.date)
        settings = load_settings()
        with psycopg.connect(**settings.db.conninfo_kwargs(), row_factory=dict_row) as conn:
            email = build_email(conn, args.portfolio_id, day)
    except (data.ReportDataError, ConfigError, psycopg.Error) as exc:
        print(f"report: {type(exc).__name__}: {exc}", file=sys.stderr)
        return 1

    if args.preview_dir:
        out = Path(args.preview_dir)
        out.mkdir(parents=True, exist_ok=True)
        (out / "report.html").write_text(email.html, encoding="utf-8")
        for cid, blob in email.images.items():
            ext = "jpg" if blob[:3] == b"\xff\xd8\xff" else "png"
            (out / f"{cid}.{ext}").write_bytes(blob)
        print(f"report: preview written to {out}")
        return 0

    cmd = settings.commands
    try:
        pdir = resolve_portfolio_dir(cmd.config_dir, args.portfolio_id)
    except PortfolioConfigError as exc:
        print(f"report: {exc}", file=sys.stderr)
        return 1
    smtp, why = load_smtp(cmd, pdir)
    if smtp is None:
        print(f"report: e-mail is off: {why}", file=sys.stderr)
        return 1
    if args.to:
        to, cc = _clean([args.to], set()), []
    else:
        to, cc = book_recipients(cmd, pdir, os.environ.get("QT_MAIL_CC", ""))
    if not to:
        print("report: no recipients", file=sys.stderr)
        return 1
    try:
        send_message(smtp, to, cc, email)
    except Exception as exc:  # smtplib/ssl/OSError: the text never carries the password
        print(f"report: send failed: {type(exc).__name__}: {exc}", file=sys.stderr)
        return 1
    print(f"report: sent to {len(to)} recipient(s), {len(cc)} cc")
    return 0


if __name__ == "__main__":
    sys.exit(main())
