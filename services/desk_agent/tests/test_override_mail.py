import json

import pytest

from conftest import PDIR, make_row
from desk_agent.command_store import BookLine
from desk_agent.config import CommandSettings, ConfigError, command_settings
from desk_agent.override_mail import (ApproverError, approve_link, build_email, load_smtp,
                                      parse_approvers)
from desk_agent.portfolios import PortfolioConfigError, resolve_portfolio_dir


def test_parse_approvers():
    assert parse_approvers(" vp = a@x.com , president=b@y.com") == {"vp": "a@x.com",
                                                                   "president": "b@y.com"}
    for raw in ("", "vp=a@x.com", "president=b@y.com,cfo=c@z.com", "vp=,president=b@y.com"):
        with pytest.raises(ApproverError, match="QT_APPROVERS"):
            parse_approvers(raw)


GOOD = {"smtp_host": "smtp.example.com", "smtp_port": 465, "username": "bot@example.com",
        "password": "pw-123456", "from_email": "qt@example.com", "use_tls": False}


@pytest.mark.parametrize("email,disabled_env,why", [
    (None, False, "no "),
    ({**GOOD, "enabled": False}, False, "enabled=false"),
    ({**GOOD, "username": "YOUR_SMTP_USERNAME"}, False, "no SMTP credentials"),
    ({**GOOD, "password": "YOUR_SMTP_APP_PASSWORD"}, False, "no SMTP credentials"),
    ({**GOOD, "password": ""}, False, "no SMTP credentials"),
    (GOOD, True, "QT_EMAIL_DISABLED=1"),
])
def test_email_disabled_cases(tmp_path, email, disabled_env, why):
    d = tmp_path / "portfolios" / PDIR
    d.mkdir(parents=True)
    if email is not None:
        (d / "email.json").write_text(json.dumps(email))
    s = CommandSettings(config_dir=str(tmp_path), email_disabled=disabled_env)
    cfg, reason = load_smtp(s, PDIR)
    assert cfg is None and why in reason
    assert "pw-123456" not in reason


def test_email_enabled(tmp_path):
    d = tmp_path / "portfolios" / PDIR
    d.mkdir(parents=True)
    (d / "email.json").write_text(json.dumps(GOOD))
    cfg, reason = load_smtp(CommandSettings(config_dir=str(tmp_path)), PDIR)
    assert reason == "" and cfg.port == 465 and not cfg.use_tls
    assert "pw-123456" not in repr(cfg)


def test_build_email_escapes_and_shows_three_books():
    row = make_row(1, "override_request", reason="<b>why</b>")
    books = {"system": {"ES": BookLine(1)}, "qt_proposal": {"ZN": BookLine(2.5)},
             "qt": {"ES": BookLine(0, "sign_close")}}
    subject, body = build_email(row, books, approve_link("https://x/approve", "T0K"))
    assert subject == "QT override request: QT_CONSERVATIVE_PORTFOLIO 2026-10-07"
    assert "&lt;b&gt;why&lt;/b&gt;" in body and "<b>why</b>" not in body
    assert "https://x/approve?token=T0K" in body
    assert "<td>ZN</td><td align=\"right\">&ndash;</td><td align=\"right\">2.5</td>" in body
    assert "sign_close" in body


def test_portfolio_dir_resolution(tmp_path):
    with pytest.raises(PortfolioConfigError, match="no portfolio config directory"):
        resolve_portfolio_dir(str(tmp_path), "P")
    for name, pid in (("a", "P"), ("b", "Q"), ("c", "P")):
        (tmp_path / "portfolios" / name).mkdir(parents=True)
        (tmp_path / "portfolios" / name / "portfolio.json").write_text(
            json.dumps({"portfolio_id": pid}))
    (tmp_path / "portfolios" / "broken").mkdir()
    (tmp_path / "portfolios" / "broken" / "portfolio.json").write_text("{not json")
    assert resolve_portfolio_dir(str(tmp_path), "Q") == "b"
    with pytest.raises(PortfolioConfigError, match="a, c"):
        resolve_portfolio_dir(str(tmp_path), "P")
    with pytest.raises(PortfolioConfigError, match="no portfolio config under"):
        resolve_portfolio_dir(str(tmp_path), "Z")


def test_command_settings_from_env():
    s = command_settings({})
    assert s.engine_binary == "/app/build/bin/Release/live_portfolio_conservative"
    assert (s.engine_cwd, s.config_dir, s.lock_dir) == ("/app", "/app/config", "/tmp/qt-locks")
    assert (s.job_timeout_s, s.redrive_interval_s, s.email_disabled) == (1800, 60, False)
    s = command_settings({"QT_ENGINE_BINARY": "/b", "QT_ENGINE_CWD": "/c", "QT_LOCK_DIR": "/l",
                          "QT_JOB_TIMEOUT_S": "5", "QT_REDRIVE_INTERVAL_S": "2",
                          "QT_EMAIL_DISABLED": "1", "QT_APPROVERS": "vp=a,president=b",
                          "QT_APPROVE_URL_BASE": "https://t/a", "TRADING_CONFIG_DIR": "/cfg"})
    assert (s.engine_binary, s.engine_cwd, s.lock_dir, s.config_dir) == ("/b", "/c", "/l", "/cfg")
    assert (s.job_timeout_s, s.redrive_interval_s, s.email_disabled) == (5, 2, True)
    assert s.approvers == "vp=a,president=b" and s.approve_url_base == "https://t/a"
    with pytest.raises(ConfigError, match="QT_JOB_TIMEOUT_S"):
        command_settings({"QT_JOB_TIMEOUT_S": "soon"})
