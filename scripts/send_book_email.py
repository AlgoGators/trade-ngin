#!/usr/bin/env python3
"""Send the daily book email from a manifest written by the engine (stdlib only).

    send_book_email.py --manifest /path/manifest.json

The C++ engine builds the report (HTML body, chart PNGs, CSVs) and hands this script the files
and the SMTP settings; the script builds the MIME message and sends it. Exit status:

    0  the SMTP server accepted the message
    1  the message could not be sent (one line on stderr; the password is never printed)
    2  the manifest is unusable

Manifest (JSON):
    smtp:        {host, port, username, password, from_email, use_tls}
    subject, to[], cc[], body_html_path
    inline[]:       {cid, path, mime?, base64?}   images referenced as cid: in the body
    attachments[]:  {path, filename, mime?}
"""
from __future__ import annotations

import argparse
import base64
import json
import mimetypes
import smtplib
import ssl
import sys
from email.message import EmailMessage
from email.utils import formatdate, make_msgid
from pathlib import Path

# Never mailed, whatever the config says.
EXCLUDED = {"dhebrank@msn.com"}


class MailError(Exception):
    pass


def recipients(to: list[str], cc: list[str]) -> tuple[list[str], list[str], list[str]]:
    """(to, cc, envelope): trimmed, de-duplicated, with the excluded addresses removed."""
    seen: set[str] = set()

    def clean(addrs: list[str]) -> list[str]:
        out = []
        for a in addrs:
            a = (a or "").strip()
            k = a.lower()
            if not a or k in EXCLUDED or k in seen:
                continue
            seen.add(k)
            out.append(a)
        return out

    to_c = clean(to)
    cc_c = clean(cc)
    return to_c, cc_c, to_c + cc_c


def build_message(m: dict) -> tuple[EmailMessage, list[str]]:
    smtp = m["smtp"]
    to, cc, envelope = recipients(m.get("to", []), m.get("cc", []))
    if not envelope:
        raise MailError("no recipients")

    msg = EmailMessage()
    msg["Subject"] = m["subject"]
    msg["From"] = smtp.get("from_email") or smtp["username"]
    msg["To"] = ", ".join(to) if to else "undisclosed-recipients:;"
    if cc:
        msg["Cc"] = ", ".join(cc)
    msg["Date"] = formatdate(localtime=True)
    msg["Message-ID"] = make_msgid(domain="algogators.org")

    html = Path(m["body_html_path"]).read_text(encoding="utf-8")
    msg.set_content("This report is best viewed in an HTML-capable mail client.")
    msg.add_alternative(html, subtype="html")
    html_part = msg.get_payload()[-1]

    for img in m.get("inline", []):
        p = Path(img["path"])
        if not p.is_file():
            continue  # a chart that was not drawn: the body copes without it
        raw = p.read_bytes()
        if img.get("base64"):
            raw = base64.b64decode(raw)
        mime = img.get("mime") or "image/png"
        main, _, sub = mime.partition("/")
        html_part.add_related(raw, maintype=main, subtype=sub or "png",
                              cid=f"<{img['cid']}>", disposition="inline",
                              filename=f"{img['cid']}.png")

    for att in m.get("attachments", []):
        p = Path(att["path"])
        if not p.is_file():
            continue
        mime = att.get("mime") or mimetypes.guess_type(p.name)[0] or "application/octet-stream"
        main, _, sub = mime.partition("/")
        msg.add_attachment(p.read_bytes(), maintype=main, subtype=sub,
                           filename=att.get("filename") or p.name)
    return msg, envelope


def deliver(msg: EmailMessage, envelope: list[str], smtp: dict) -> None:
    host, port = smtp["host"], int(smtp.get("port") or 587)
    ctx = ssl.create_default_context()
    try:
        if port == 465:
            with smtplib.SMTP_SSL(host, port, timeout=30, context=ctx) as s:
                s.login(smtp["username"], smtp["password"])
                s.send_message(msg, to_addrs=envelope)
            return
        with smtplib.SMTP(host, port, timeout=30) as s:
            if smtp.get("use_tls", True):
                s.starttls(context=ctx)
            s.login(smtp["username"], smtp["password"])
            s.send_message(msg, to_addrs=envelope)
    except (smtplib.SMTPException, OSError, ssl.SSLError) as e:
        # the exception text can echo the server's reply but never the password
        raise MailError(f"{type(e).__name__}: {e}") from None


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--manifest", required=True)
    args = ap.parse_args(argv)
    try:
        m = json.loads(Path(args.manifest).read_text(encoding="utf-8"))
        msg, envelope = build_message(m)
    except MailError as e:
        print(f"send_book_email: {e}", file=sys.stderr)
        return 1
    except (OSError, ValueError, KeyError) as e:
        print(f"send_book_email: bad manifest ({type(e).__name__}: {e})", file=sys.stderr)
        return 2
    try:
        deliver(msg, envelope, m["smtp"])
    except MailError as e:
        print(f"send_book_email: {e}", file=sys.stderr)
        return 1
    print(f"send_book_email: sent to {len(envelope)} recipient(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
