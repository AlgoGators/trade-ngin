"""Who the book e-mail goes to, and the command line the engine runs."""

import json
from types import SimpleNamespace

from algogators_rpc.services.report.__main__ import book_recipients, main


def _settings(tmp_path, to_emails, approvers="vp=vp@x.org,president=pres@x.org"):
    d = tmp_path / "portfolios" / "qt"
    d.mkdir(parents=True)
    (d / "email.json").write_text(json.dumps({"to_emails": to_emails}), encoding="utf-8")
    return SimpleNamespace(config_dir=str(tmp_path), approvers=approvers)


def test_to_is_the_book_list_and_the_approvers_are_cc(tmp_path):
    to, cc = book_recipients(_settings(tmp_path, ["a@x.org", "b@x.org"]), "qt")
    assert to == ["a@x.org", "b@x.org"]
    assert cc == ["vp@x.org", "pres@x.org"]


def test_excluded_and_duplicate_addresses_are_never_addressed(tmp_path):
    s = _settings(tmp_path, ["a@x.org", "DHebrank@msn.com", "A@x.org"])
    to, cc = book_recipients(s, "qt", extra_cc="dhebrank@msn.com, a@x.org, c@x.org")
    assert to == ["a@x.org"]
    assert cc == ["c@x.org", "vp@x.org", "pres@x.org"]


def test_an_approver_who_is_also_in_the_book_is_not_mailed_twice(tmp_path):
    to, cc = book_recipients(_settings(tmp_path, ["vp@x.org", "a@x.org"]), "qt")
    assert to == ["vp@x.org", "a@x.org"]
    assert cc == ["pres@x.org"]


def test_missing_approvers_or_book_file_does_not_raise(tmp_path):
    to, cc = book_recipients(_settings(tmp_path, ["a@x.org"], approvers=""), "qt")
    assert (to, cc) == (["a@x.org"], [])
    to, cc = book_recipients(SimpleNamespace(config_dir=str(tmp_path), approvers=""), "nope")
    assert (to, cc) == ([], [])


def _manifest(tmp_path, **over):
    m = {
        "version": 1,
        "portfolio_id": "QT_CONSERVATIVE",
        "date": "2026-10-23",
        "subject": "[QT QT_CONSERVATIVE] Daily Trading Report - 2026-10-23",
        "body_html": '<div class="container"><h2>Today\'s Positions</h2><p>No positions.</p></div>',
        "images": [],
        "attachments": [],
        **over,
    }
    p = tmp_path / "manifest.json"
    p.write_text(json.dumps(m), encoding="utf-8")
    return str(p)


def test_preview_writes_the_rendered_report_and_sends_nothing(tmp_path, capsys):
    out = tmp_path / "out"
    rc = main(["--portfolio-id", "QT_CONSERVATIVE", "--date", "2026-10-23",
               "--manifest", _manifest(tmp_path), "--preview-dir", str(out)])
    assert rc == 0
    assert "No positions." in (out / "report.html").read_text(encoding="utf-8")
    assert (out / "report.txt").exists() and (out / "brandmark.jpg").exists()


def test_a_manifest_for_another_day_is_refused(tmp_path, capsys):
    rc = main(["--portfolio-id", "QT_CONSERVATIVE", "--date", "2026-10-24",
               "--manifest", _manifest(tmp_path), "--preview-dir", str(tmp_path / "o")])
    assert rc == 1
    assert "not QT_CONSERVATIVE 2026-10-24" in capsys.readouterr().err


def test_an_unreadable_manifest_is_a_failure_with_one_line(tmp_path, capsys):
    rc = main(["--portfolio-id", "P", "--date", "2026-10-23",
               "--manifest", str(tmp_path / "absent.json")])
    assert rc == 1
    assert capsys.readouterr().err.startswith("report: the manifest could not be read")
