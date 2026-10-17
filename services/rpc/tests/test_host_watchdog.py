"""D-39: the host watchdog (deploy/qt_host_watchdog.py) alerts from outside engine-rpc."""

import datetime as dt
import importlib.util
import io
from pathlib import Path

import pytest

SPEC = importlib.util.spec_from_file_location(
    "qt_host_watchdog", Path(__file__).resolve().parents[3] / "deploy" / "qt_host_watchdog.py")
wd = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(wd)
NY = wd.NY


def at(hh, mm=0):
    return dt.datetime(2026, 10, 14, hh, mm, tzinfo=NY).astimezone(dt.timezone.utc)


class Smtp:
    sent = []

    def __init__(self, host, port, timeout):
        self.host, self.port = host, port

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False

    def starttls(self):
        pass

    def login(self, user, password):
        pass

    def send_message(self, msg):
        Smtp.sent.append((msg["To"], msg["Subject"], msg.get_content()))


@pytest.fixture
def env(tmp_path):
    Smtp.sent = []
    beat = tmp_path / "qt-catchup.heartbeat"
    return {"WATCHDOG_HEARTBEAT": str(beat), "WATCHDOG_STATE": str(tmp_path / "state.json"),
            "ALERT_TO": "pres@x.org", "SMTP_HOST": "smtp.x.org", "SMTP_STARTTLS": "0"}


def run(env, now, status=("running", "healthy")):
    out = io.StringIO()
    rc = wd.main(env, now, state=lambda name: status, smtp=Smtp, out=out)
    return rc, out.getvalue()


def beat(env, when):
    Path(env["WATCHDOG_HEARTBEAT"]).write_text(when.isoformat(), encoding="utf-8")


def test_healthy_is_quiet(env):
    beat(env, at(8, 55))
    assert run(env, at(9, 0))[0] == 0 and Smtp.sent == []


@pytest.mark.parametrize("status", [("exited", ""), ("missing", ""), ("restarting", ""),
                                    ("running", "unhealthy")])
def test_a_stopped_or_unhealthy_container_alerts_at_any_hour(env, status):
    rc, out = run(env, at(3, 0), status)
    assert rc == 1 and len(Smtp.sent) == 1
    to, subject, _ = Smtp.sent[0]
    assert to == "pres@x.org" and "engine-rpc container is" in subject


@pytest.mark.parametrize("now, last, alerts", [
    (at(9, 0), at(8, 44), True),     # busy window: 16 min > 15
    (at(9, 0), at(8, 46), False),
    (at(14, 0), at(12, 44), True),   # after it: 76 min > 75
    (at(14, 0), at(12, 46), False),
    (at(23, 0), at(12, 0), False),   # outside the window: only the container is checked
])
def test_a_stale_heartbeat_alerts_inside_the_window(env, now, last, alerts):
    beat(env, last)
    rc, _ = run(env, now)
    assert (rc == 1) == alerts and bool(Smtp.sent) == alerts


def test_a_missing_heartbeat_alerts_in_the_window(env):
    rc, _ = run(env, at(9, 0))
    assert rc == 1 and "cannot be read" in Smtp.sent[0][1]


def test_one_mail_an_hour_then_one_recovered(env):
    down = ("exited", "")
    run(env, at(9, 0), down)
    run(env, at(9, 30), down)
    assert len(Smtp.sent) == 1
    run(env, at(10, 1), down)
    assert len(Smtp.sent) == 2
    beat(env, at(10, 5))
    assert run(env, at(10, 6))[0] == 0
    assert Smtp.sent[-1][1] == "QT watchdog: recovered"
    run(env, at(10, 7))
    assert len(Smtp.sent) == 3


def test_a_configuration_error_is_exit_2(env):
    del env["SMTP_HOST"]
    rc, out = run(env, at(3, 0), ("exited", ""))
    assert rc == 2 and "SMTP_HOST" in out
