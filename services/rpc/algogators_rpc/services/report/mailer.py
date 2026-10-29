"""Build the report e-mail's MIME message and send it over SMTP."""

import smtplib
import ssl
import sys
from email.message import EmailMessage
from email.utils import formatdate, make_msgid
from pathlib import Path


def build_message(sender, to, cc, email, attachments=()):
    """multipart/alternative (text, then HTML), with the charts inline on the HTML part and the
    run's exported files attached. A file that cannot be read is left off with one line on
    stderr, never a failed send: the report itself is the message."""
    msg = EmailMessage()
    msg["Subject"] = email.subject
    msg["From"] = sender
    msg["To"] = ", ".join(to)
    if cc:
        msg["Cc"] = ", ".join(cc)
    msg["Date"] = formatdate(localtime=False)
    msg["Message-ID"] = make_msgid(domain="algogators.org")
    msg.set_content(email.text)
    msg.add_alternative(email.html, subtype="html")
    html_part = msg.get_body(preferencelist=("html",))
    assert html_part is not None
    for cid, data in email.images.items():
        subtype = "jpeg" if data[:3] == b"\xff\xd8\xff" else "png"
        html_part.add_related(
            data,
            maintype="image",
            subtype=subtype,
            cid=f"<{cid}>",
            filename=f"{cid}.{'jpg' if subtype == 'jpeg' else 'png'}",
            disposition="inline",
        )
    for path in attachments:
        p = Path(path)
        try:
            data = p.read_bytes()
        except OSError as exc:
            print(f"report: attachment {p.name} left off: {exc.strerror}", file=sys.stderr)
            continue
        maintype, subtype = ("text", "csv") if p.suffix.lower() == ".csv" else ("application", "octet-stream")
        msg.add_attachment(data, maintype=maintype, subtype=subtype, filename=p.name)
    return msg


def send_message(cfg, to, cc, email, attachments=()):
    """Send over SMTP with an SmtpConfig (desk/override_mail.py: host, port, username, password,
    from_email, use_tls). Port 465 is implicit TLS; otherwise STARTTLS when use_tls."""
    message = build_message(cfg.from_email, list(to), list(cc), email, attachments)
    context = ssl.create_default_context()
    server: smtplib.SMTP
    if cfg.port == 465:
        server = smtplib.SMTP_SSL(cfg.host, cfg.port, context=context, timeout=30)
    else:
        server = smtplib.SMTP(cfg.host, cfg.port, timeout=30)
    with server:
        if cfg.port != 465 and cfg.use_tls:
            server.starttls(context=context)
        server.login(cfg.username, cfg.password)
        server.send_message(message, to_addrs=list(to) + list(cc))
