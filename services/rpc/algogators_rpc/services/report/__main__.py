"""Send (or preview) one portfolio's daily book e-mail: the only way the book e-mail is sent.

    python -m algogators_rpc.services.report --portfolio-id QT_CONSERVATIVE --date 2026-10-23
    ... --to me@example.org            send to that address only (no Cc)
    ... --preview-dir /tmp/out         write the HTML and charts there, send nothing
    ... --no-session                   the run found no session on the day before (engine flag)
    ... --alert TEXT                   the run's risk-module failure: the book was held (engine flag)
    ... --attach FILE                  attach a file the run exported (repeatable)

The engine runs it after the day is stored (live/python_mail.hpp); every figure is read for --date.

Exit status: 0 sent (or previewed), 1 not sent (one line on stderr, never a password), 2 bad
arguments. Database settings are the engine's (DB_* or defaults.json), SMTP settings the
portfolio's email.json (desk/override_mail.load_smtp); QT_EMAIL_DISABLED=1 sends nothing and fails.
"""

from __future__ import annotations

import argparse
import json
from dataclasses import replace
import os
import sys
from pathlib import Path

import psycopg
from psycopg.rows import dict_row

from ..desk.config import ConfigError, load_settings
from ..desk.override_mail import ApproverError, load_smtp, parse_approvers
from ..desk.portfolios import PortfolioConfigError, resolve_portfolio_dir
from . import data
from .builder import ReportFlags, build_daily_payload
from .calc import build_strategy_detail
from .daily import ReportPayloadError, parse_daily_report
from .mailer import send_message
from .renderer import HtmlReportRenderer

def _clean(addrs, seen):
    out = []
    for a in addrs:
        a = (a or "").strip()
        if a and a.lower() not in seen:
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


def report_conninfo(db) -> dict:
    """The engine's connection settings, in a UTC session: every timestamp's date is the UTC day
    the engine stored it under, whatever the server's or the role's time zone."""
    kwargs = dict(db.conninfo_kwargs())
    kwargs["options"] = (kwargs.get("options", "") + " -c TimeZone=UTC").strip()
    kwargs["application_name"] = "engine-rpc/report"
    return kwargs


RISK_PREFIX = "[RISK MODULE FAILED - BOOK HELD] "


def build_email(conn, portfolio_id: str, day, flags: ReportFlags = ReportFlags(),
                strategy_id: str | None = None):
    """The rendered e-mail for the portfolio's day, from the database and the run's flags."""
    with conn.cursor() as cur:
        cfg = data.fetch_registry(cur, portfolio_id, strategy_id)
        rows = data.fetch_detail_rows(cur, cfg["strategy_type"], portfolio_id, day)
    detail = build_strategy_detail(cfg, rows)
    if detail is None:
        raise data.ReportDataError(
            f"no {rows.get('book', 'system')} results stored for {portfolio_id} on {day}"
        )
    roots = [p["symbol"].replace(".v.0", "") for p in detail["positions"]]
    with conn.cursor(row_factory=psycopg.rows.tuple_row) as cur:
        symbols = data.fetch_symbols(cur, roots)
        try:
            history = data.fetch_history(cur, portfolio_id, cfg["strategy_type"],
                                         detail["book"], day)
        except psycopg.Error:
            history = None  # a failed history read costs the charts, never the report
    payload = build_daily_payload(detail, portfolio_id, cfg["name"], day,
                                  symbols=symbols, history=history, flags=flags)
    email = HtmlReportRenderer().render_daily(parse_daily_report(payload))
    if flags.alert:
        email = replace(email, subject=RISK_PREFIX + email.subject)
    return email


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--portfolio-id", required=True)
    ap.add_argument("--date", required=True)
    ap.add_argument("--strategy-id", help="the strategy the day is stored under (the engine's id); "
                    "default: the portfolio's first registered strategy")
    ap.add_argument("--to", help="send to this address only (no Cc)")
    ap.add_argument("--preview-dir", help="write the HTML and images here and send nothing")
    ap.add_argument("--no-session", action="store_true",
                    help="the run found no session on the day before and carried its book")
    ap.add_argument("--alert", default="", help="the run's risk-module failure, in its words")
    ap.add_argument("--attach", action="append", default=[], help="a file to attach")
    ap.add_argument("--equities", action="store_true", help="an equity book")
    ap.add_argument("--rolls", default="", help="the engine's roll texts, JSON (report_rolls_json.hpp)")
    args = ap.parse_args(argv)
    try:
        rolls = json.loads(args.rolls) if args.rolls.strip() else None
    except ValueError:
        print("report: --rolls is not JSON", file=sys.stderr)
        return 2
    flags = ReportFlags(no_session=args.no_session, alert=args.alert.strip(),
                        equities=args.equities, rolls=rolls or None)

    try:
        day = data.report_day(args.date)
        settings = load_settings()
        with psycopg.connect(**report_conninfo(settings.db), row_factory=dict_row) as conn:
            email = build_email(conn, args.portfolio_id, day, flags, args.strategy_id)
    except (data.ReportDataError, ConfigError, psycopg.Error, ReportPayloadError) as exc:
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
        send_message(smtp, to, cc, email, args.attach)
    except Exception as exc:  # smtplib/ssl/OSError: the text never carries the password
        print(f"report: send failed: {type(exc).__name__}: {exc}", file=sys.stderr)
        return 1
    print(f"report: sent to {len(to)} recipient(s), {len(cc)} cc")
    return 0


if __name__ == "__main__":
    sys.exit(main())
