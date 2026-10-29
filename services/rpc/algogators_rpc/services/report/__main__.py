"""Send (or preview) one portfolio's daily book e-mail from the engine's report.

    python -m algogators_rpc.services.report --portfolio-id QT_CONSERVATIVE --date 2026-10-23 \\
        --manifest /tmp/tn_mail_1/manifest.json
    ... --to me@example.org            send to that address only (no Cc)
    ... --preview-dir /tmp/out         write the HTML and images there, send nothing

The engine writes the manifest (live/python_mail.hpp): the subject, the HTML body, the charts and
the CSV files of the day it built. Exit status: 0 sent (or previewed), 1 not sent (one line on
stderr, never a password), 2 bad arguments. SMTP settings are the portfolio's email.json
(desk/override_mail.load_smtp); QT_EMAIL_DISABLED=1 sends nothing and fails.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

from ..desk.config import ConfigError, load_settings
from ..desk.override_mail import ApproverError, load_smtp, parse_approvers
from ..desk.portfolios import PortfolioConfigError, resolve_portfolio_dir
from .engine_report import ReportInputError, build_report, load_manifest
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


def build_email(manifest: dict, portfolio_id: str, day: str):
    """The rendered e-mail of the engine's report for the portfolio's day."""
    if manifest["portfolio_id"] != portfolio_id or manifest["date"] != day:
        raise ReportInputError(
            f"the manifest is for {manifest['portfolio_id']} {manifest['date']}, "
            f"not {portfolio_id} {day}"
        )
    report = build_report(manifest)
    return report, HtmlReportRenderer().render(report)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--portfolio-id", required=True)
    ap.add_argument("--date", required=True)
    ap.add_argument("--manifest", required=True, help="the engine's report (live/python_mail.hpp)")
    ap.add_argument("--to", help="send to this address only (no Cc)")
    ap.add_argument("--preview-dir", help="write the HTML and images here and send nothing")
    args = ap.parse_args(argv)

    try:
        report, email = build_email(load_manifest(args.manifest), args.portfolio_id, args.date)
    except ReportInputError as exc:
        print(f"report: {exc}", file=sys.stderr)
        return 1

    if args.preview_dir:
        out = Path(args.preview_dir)
        out.mkdir(parents=True, exist_ok=True)
        (out / "report.html").write_text(email.html, encoding="utf-8")
        (out / "report.txt").write_text(email.text, encoding="utf-8")
        for cid, blob in email.images.items():
            ext = "jpg" if blob[:3] == b"\xff\xd8\xff" else "png"
            (out / f"{cid}.{ext}").write_bytes(blob)
        print(f"report: preview written to {out}")
        return 0

    try:
        cmd = load_settings().commands
        pdir = resolve_portfolio_dir(cmd.config_dir, args.portfolio_id)
    except (ConfigError, PortfolioConfigError) as exc:
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
        send_message(smtp, to, cc, email, report.attachments)
    except Exception as exc:  # smtplib/ssl/OSError: the text never carries the password
        print(f"report: send failed: {type(exc).__name__}: {exc}", file=sys.stderr)
        return 1
    print(f"report: sent to {len(to)} recipient(s), {len(cc)} cc")
    return 0


if __name__ == "__main__":
    sys.exit(main())
