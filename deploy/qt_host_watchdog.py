#!/usr/bin/env python3
"""The QT host watchdog (D-39): alerts from OUTSIDE engine-rpc when nothing inside it can.

Every alert the QT books raise (a model run not done, the 10:00 fallback, a missed send) comes
from inside the engine-rpc container. With the container stopped, crash-looping or wedged,
nothing fires and nobody hears. This script runs on the host from cron (deploy/qt-engine.cron),
needs only python3's standard library and the docker CLI, and e-mails the President when:

* the container is not running, or its health check says unhealthy;
* inside the scheduler's window (New York time) the scheduler's heartbeat file
  (<logs>/qt-catchup.heartbeat, written after every pass) is older than its limit: 15 minutes
  in the busy window (06:30-10:30), 75 minutes after it (the in-container watchdog's limits).

One e-mail per problem at most every WATCHDOG_REPEAT_MIN (60) minutes, and one "recovered"
e-mail when the problems clear (state in WATCHDOG_STATE). Exit 0 when healthy, 1 when it found a
problem, 2 on a configuration error.

Configuration (environment, usually from an env file the cron line sources, mode 600):

    WATCHDOG_CONTAINER   engine-rpc
    WATCHDOG_HEARTBEAT   /home/ubuntu/qt-engine/logs/qt-catchup.heartbeat
    WATCHDOG_STATE       /home/ubuntu/qt-engine/logs/qt-host-watchdog.json
    WATCHDOG_WINDOW      06:30-22:00     WATCHDOG_BUSY_WINDOW  06:30-10:30
    WATCHDOG_BUSY_MAX_MIN 15             WATCHDOG_MAX_MIN      75
    WATCHDOG_REPEAT_MIN  60
    ALERT_TO             the President's address (QT_APPROVERS president=)
    SMTP_HOST, SMTP_PORT (587), SMTP_USER, SMTP_PASSWORD, SMTP_STARTTLS (1), MAIL_FROM
"""

from __future__ import annotations

import datetime as dt
import json
import os
import smtplib
import subprocess
import sys
from email.message import EmailMessage
from pathlib import Path
from typing import Callable, Dict, List, Mapping, Optional, Tuple
from zoneinfo import ZoneInfo

NY = ZoneInfo("America/New_York")


class ConfigError(ValueError):
    pass


def _span(text: str, name: str) -> Tuple[dt.time, dt.time]:
    try:
        a, b = (dt.time.fromisoformat(p.strip()) for p in text.split("-"))
    except ValueError:
        raise ConfigError(f"{name} must be HH:MM-HH:MM") from None
    return a, b


def _minutes(env: Mapping[str, str], name: str, default: int) -> int:
    try:
        value = int(env.get(name) or default)
    except ValueError:
        raise ConfigError(f"{name} must be a whole number of minutes") from None
    if value <= 0:
        raise ConfigError(f"{name} must be > 0")
    return value


def container_state(name: str, run=subprocess.run) -> Tuple[str, str]:
    """(status, health) from docker inspect: ("missing", "") when there is no such container."""
    res = run(["docker", "inspect", "-f",
               "{{.State.Status}} {{if .State.Health}}{{.State.Health.Status}}{{end}}", name],
              capture_output=True, text=True, timeout=30)
    if res.returncode != 0:
        return "missing", ""
    parts = res.stdout.split()
    return (parts[0] if parts else "unknown"), (parts[1] if len(parts) > 1 else "")


def problems(env: Mapping[str, str], now: dt.datetime,
             state: Callable[[str], Tuple[str, str]]) -> Dict[str, str]:
    """{key: sentence} of what is wrong now; {} when healthy."""
    name = env.get("WATCHDOG_CONTAINER") or "engine-rpc"
    found: Dict[str, str] = {}
    status, health = state(name)
    if status != "running":
        found["container"] = (f"the {name} container is {status}: no model run, 09:30 send, "
                              "10:00 fallback or alert can happen")
    elif health == "unhealthy":
        found["container"] = f"the {name} container is running but unhealthy"
    local = now.astimezone(NY)
    lo, hi = _span(env.get("WATCHDOG_WINDOW") or "06:30-22:00", "WATCHDOG_WINDOW")
    if lo <= local.time() < hi and "container" not in found:
        blo, bhi = _span(env.get("WATCHDOG_BUSY_WINDOW") or "06:30-10:30", "WATCHDOG_BUSY_WINDOW")
        limit = (_minutes(env, "WATCHDOG_BUSY_MAX_MIN", 15) if blo <= local.time() < bhi
                 else _minutes(env, "WATCHDOG_MAX_MIN", 75))
        path = Path(env.get("WATCHDOG_HEARTBEAT")
                    or "/home/ubuntu/qt-engine/logs/qt-catchup.heartbeat")
        try:
            beat = dt.datetime.fromisoformat(path.read_text(encoding="utf-8").strip())
            if beat.tzinfo is None:
                beat = beat.replace(tzinfo=dt.timezone.utc)
            age = (now - beat).total_seconds() / 60
            if age > limit:
                found["heartbeat"] = (f"the QT scheduler has not finished a pass for "
                                      f"{age:.0f} minutes (limit {limit}) at {local:%H:%M} New "
                                      "York")
        except (OSError, ValueError):
            found["heartbeat"] = f"the QT scheduler's heartbeat {path} cannot be read"
    return found


def load_state(path: Path) -> dict:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def save_state(path: Path, data: dict) -> None:
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(data, indent=1), encoding="utf-8")
    os.replace(tmp, path)


def to_send(found: Dict[str, str], state: dict, now: dt.datetime,
            repeat_min: int) -> Tuple[List[str], bool, dict]:
    """(problem keys to e-mail now, whether to send "recovered", the new state)."""
    last: Dict[str, str] = dict(state.get("alerted") or {})
    due = []
    for key in found:
        sent = last.get(key)
        if sent is None or (now - dt.datetime.fromisoformat(sent)).total_seconds() \
                >= repeat_min * 60:
            due.append(key)
            last[key] = now.isoformat()
    recovered = bool(state.get("alerted")) and not found
    new_state = {"alerted": {k: v for k, v in last.items() if k in found}}
    return due, recovered, new_state


def send(env: Mapping[str, str], subject: str, body: str, smtp=smtplib.SMTP) -> None:
    to = env.get("ALERT_TO") or ""
    host = env.get("SMTP_HOST") or ""
    if not to or not host:
        raise ConfigError("ALERT_TO and SMTP_HOST must be set")
    msg = EmailMessage()
    msg["Subject"] = subject
    msg["From"] = env.get("MAIL_FROM") or env.get("SMTP_USER") or to
    msg["To"] = to
    msg.set_content(body)
    with smtp(host, int(env.get("SMTP_PORT") or 587), timeout=30) as s:
        if (env.get("SMTP_STARTTLS") or "1") != "0":
            s.starttls()
        if env.get("SMTP_USER"):
            s.login(env["SMTP_USER"], env.get("SMTP_PASSWORD") or "")
        s.send_message(msg)


def main(env: Optional[Mapping[str, str]] = None, now: Optional[dt.datetime] = None,
         state=None, smtp=smtplib.SMTP, out=sys.stdout) -> int:
    env = dict(os.environ if env is None else env)
    now = now or dt.datetime.now(dt.timezone.utc)
    state = state or (lambda name: container_state(name))
    path = Path(env.get("WATCHDOG_STATE") or "/home/ubuntu/qt-engine/logs/qt-host-watchdog.json")
    try:
        found = problems(env, now, state)
        due, recovered, new_state = to_send(found, load_state(path), now,
                                            _minutes(env, "WATCHDOG_REPEAT_MIN", 60))
        host = os.uname().nodename if hasattr(os, "uname") else "host"
        if due:
            lines = [found[k] for k in due]
            send(env, f"QT watchdog: {lines[0]}",
                 "\n".join(lines) + f"\n\n(qt_host_watchdog on {host}, "
                 f"{now.astimezone(NY):%Y-%m-%d %H:%M} New York)", smtp)
        elif recovered:
            send(env, "QT watchdog: recovered",
                 f"engine-rpc and the QT scheduler look healthy again (qt_host_watchdog on "
                 f"{host}, {now.astimezone(NY):%Y-%m-%d %H:%M} New York)", smtp)
    except ConfigError as exc:
        print(f"qt-host-watchdog: configuration: {exc}", file=out)
        return 2
    save_state(path, new_state)
    stamp = now.astimezone(NY).strftime("%Y-%m-%d %H:%M")
    if found:
        print(f"{stamp} qt-host-watchdog: " + "; ".join(found.values())
              + (f" (alerted: {', '.join(due)})" if due else " (alerted earlier)"), file=out)
        return 1
    print(f"{stamp} qt-host-watchdog: ok" + (" (recovered; alerted)" if recovered else ""),
          file=out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
