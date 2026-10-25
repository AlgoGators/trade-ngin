#!/usr/bin/env python3
"""scripts/send_book_email.py: the message it builds, who it mails, and how it fails.

The SMTP connection is replaced by a recorder, so nothing leaves the machine.
Run directly (python3 tests/scripts/test_send_book_email.py) or through ctest.
"""

import base64
import importlib.util
import json
import os
import sys
import tempfile
import unittest
from io import StringIO
from unittest import mock

sys.dont_write_bytecode = True

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SCRIPT = os.path.join(REPO_ROOT, "scripts", "send_book_email.py")
spec = importlib.util.spec_from_file_location("send_book_email", SCRIPT)
sbe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sbe)

PNG = b"\x89PNG\r\n\x1a\nfake"
PASSWORD = "s3cret-pw"


class FakeSMTP:
    sent = []
    fail_login = False
    last = None

    def __init__(self, host, port, timeout=None, context=None):
        FakeSMTP.last = (host, port)
        self.tls = False

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False

    def starttls(self, context=None):
        self.tls = True

    def login(self, user, pw):
        if FakeSMTP.fail_login:
            import smtplib
            raise smtplib.SMTPAuthenticationError(535, b"bad credentials")

    def send_message(self, msg, to_addrs=None):
        FakeSMTP.sent.append((msg, list(to_addrs)))


class SendBookEmail(unittest.TestCase):
    def setUp(self):
        FakeSMTP.sent = []
        FakeSMTP.fail_login = False
        FakeSMTP.last = None
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)

    def write(self, name, data, mode="w"):
        p = os.path.join(self.dir.name, name)
        with open(p, mode) as f:
            f.write(data)
        return p

    def manifest(self, **over):
        m = {
            "smtp": {"host": "smtp.example.com", "port": 587, "username": "u@example.com",
                     "password": PASSWORD, "from_email": "fund@example.com", "use_tls": True},
            "subject": "[QT p] Daily Trading Report - 2026-10-23",
            "to": ["a@x.org", "b@x.org"],
            "cc": ["vp@x.org"],
            "body_html_path": self.write("body.html", '<html><img src="cid:equity_chart"></html>'),
            "inline": [{"cid": "equity_chart", "path": self.write("eq.b64",
                        base64.b64encode(PNG).decode()), "base64": True}],
            "attachments": [{"path": self.write("pos.csv", "sym,qty\nES,1\n"),
                             "filename": "positions.csv", "mime": "text/csv"}],
        }
        m.update(over)
        return self.write("manifest.json", json.dumps(m))

    def run_main(self, path):
        err = StringIO()
        with mock.patch.object(sbe.smtplib, "SMTP", FakeSMTP), mock.patch("sys.stderr", err):
            rc = sbe.main(["--manifest", path])
        return rc, err.getvalue()

    def test_sends_one_message_with_inline_chart_and_csv(self):
        rc, _ = self.run_main(self.manifest())
        self.assertEqual(rc, 0)
        (msg, envelope), = FakeSMTP.sent
        self.assertEqual(envelope, ["a@x.org", "b@x.org", "vp@x.org"])
        self.assertEqual(msg["To"], "a@x.org, b@x.org")
        self.assertEqual(msg["Cc"], "vp@x.org")
        self.assertEqual(msg["From"], "fund@example.com")
        self.assertEqual(FakeSMTP.last, ("smtp.example.com", 587))
        cids = [p["Content-ID"] for p in msg.walk() if p["Content-ID"]]
        self.assertEqual(cids, ["<equity_chart>"])
        png = [p for p in msg.walk() if p.get_content_type() == "image/png"][0]
        self.assertEqual(png.get_payload(decode=True), PNG)
        csv = [p for p in msg.walk() if p.get_filename() == "positions.csv"]
        self.assertEqual(len(csv), 1)
        self.assertEqual(csv[0].get_content_type(), "text/csv")

    def test_excluded_and_duplicate_recipients_are_dropped(self):
        rc, _ = self.run_main(self.manifest(to=["a@x.org", "DHebrank@msn.com", "A@x.org"],
                                            cc=["a@x.org", "vp@x.org"]))
        self.assertEqual(rc, 0)
        (msg, envelope), = FakeSMTP.sent
        self.assertEqual(envelope, ["a@x.org", "vp@x.org"])
        self.assertNotIn("msn.com", msg.as_string())

    def test_no_recipients_fails_without_connecting(self):
        rc, err = self.run_main(self.manifest(to=["dhebrank@msn.com"], cc=[]))
        self.assertEqual(rc, 1)
        self.assertIn("no recipients", err)
        self.assertEqual(FakeSMTP.sent, [])
        self.assertIsNone(FakeSMTP.last)

    def test_login_failure_exits_1_and_never_prints_the_password(self):
        FakeSMTP.fail_login = True
        rc, err = self.run_main(self.manifest())
        self.assertEqual(rc, 1)
        self.assertIn("SMTPAuthenticationError", err)
        self.assertNotIn(PASSWORD, err)
        self.assertEqual(FakeSMTP.sent, [])

    def test_missing_chart_file_is_skipped_not_fatal(self):
        m = json.load(open(self.manifest()))
        m["inline"][0]["path"] = os.path.join(self.dir.name, "gone.b64")
        rc, _ = self.run_main(self.write("m2.json", json.dumps(m)))
        self.assertEqual(rc, 0)
        (msg, _), = FakeSMTP.sent
        self.assertEqual([p for p in msg.walk() if p["Content-ID"]], [])

    def test_unusable_manifest_exits_2(self):
        rc, err = self.run_main(self.write("bad.json", "{not json"))
        self.assertEqual(rc, 2)
        self.assertIn("bad manifest", err)
        rc, _ = self.run_main(os.path.join(self.dir.name, "missing.json"))
        self.assertEqual(rc, 2)


if __name__ == "__main__":
    unittest.main()
