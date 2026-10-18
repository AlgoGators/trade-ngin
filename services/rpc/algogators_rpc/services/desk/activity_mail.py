"""The VP and President's log of desk actions (Dominick 2026-10-18; catchup.py).

The VP and the President act as the fund's PMs, and an e-mail per desk action is their log. On
each scheduler pass, every desk command that reached a final status since the last pass (a save,
an approval, a Send, a rescind, an override request or decision; trading.position_overrides) and
every new desk settings version (trading.strategy_config, saved or reverted from AlgoLens) is
e-mailed to both addresses of QT_APPROVERS, one e-mail each, oldest first. The scheduler's own
rows (requested_by system:...) are not desk actions: they raise their own alerts.

What was e-mailed is kept in <QT_LOG_DIR>/qt-activity-mail.json (the review e-mail's pattern), so
a restart sends nothing twice; with no state yet (the first start) it starts from now, so history
is not e-mailed. QT_EMAIL_DISABLED=1: nothing is sent; the bodies go to qt-activity-mail.log.
"""

from __future__ import annotations

import datetime as dt
import html
import json
import os
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Mapping, Optional, Tuple

STATE = "qt-activity-mail.json"
BODY_LOG = "qt-activity-mail.log"
SYSTEM_PREFIX = "system:"

# A command's action in plain words, by kind (a publish by its C10 flow).
_VERBS = {"save": "saved edits to", "rescind": "rescinded the approval of",
          "override_request": "requested an override of"}


@dataclass(frozen=True)
class SettingsVersion:
    id: int
    portfolio_id: str
    version: int
    reason: str
    created_by: str
    created_at: dt.datetime
    overrides: Mapping[str, Any] = field(default_factory=dict)
    previous: Mapping[str, Any] = field(default_factory=dict)  # the version before (none: {})


@dataclass(frozen=True)
class Mark:
    """How far the log has e-mailed: the newest command (finished_at, id) and settings id."""
    command_at: Optional[dt.datetime]
    command_id: int
    settings_id: int

    def to_json(self) -> Dict[str, Any]:
        return {"command_at": self.command_at.isoformat() if self.command_at else None,
                "command_id": self.command_id, "settings_id": self.settings_id}

    @classmethod
    def from_json(cls, data: Any) -> Optional["Mark"]:
        try:
            at = data.get("command_at")
            return cls(dt.datetime.fromisoformat(at) if at else None,
                       int(data["command_id"]), int(data["settings_id"]))
        except (AttributeError, KeyError, TypeError, ValueError):
            return None


def is_desk_action(requested_by: Optional[str]) -> bool:
    return not (requested_by or "").startswith(SYSTEM_PREFIX)


def command_verb(kind: str, payload: Mapping[str, Any]) -> str:
    if kind == "publish":
        return "sent the e-mail of" if payload.get("flow") == "send" else "approved"
    if kind == "override_decision":
        return ("rejected the override of" if payload.get("approved") is False
                else "approved the override of")
    return _VERBS.get(kind, f"{kind} on")


def _when(at: Optional[dt.datetime], tz) -> str:
    return at.astimezone(tz).strftime("%Y-%m-%d %H:%M %Z") if at else "-"


def build_command(row, tz) -> Tuple[str, str]:
    """(subject, html body) of one desk command row (command_store.CommandRow)."""
    verb = command_verb(row.kind, row.payload or {})
    who = row.requested_by or "someone"
    subject = f"QT log: {who} {verb} {row.portfolio_id} {row.date} ({row.status})"
    lines = [("Who", who), ("Did", f"{verb} the {row.date} book"),
             ("Outcome", row.status), ("When", _when(row.finished_at, tz)),
             ("Reason given", row.reason or "-"), ("Result", row.message or "-"),
             ("Command", str(row.id))]
    return subject, _body(row.portfolio_id, lines)


def _flatten(node: Any, prefix: Tuple[str, ...] = ()) -> Dict[Tuple[str, ...], Any]:
    if isinstance(node, Mapping) and node:
        out: Dict[Tuple[str, ...], Any] = {}
        for k, v in node.items():
            out.update(_flatten(v, prefix + (str(k),)))
        return out
    return {prefix: node}


def settings_changes(previous: Mapping[str, Any], overrides: Mapping[str, Any]) -> List[str]:
    """One line per setting the version changed, against the version before it."""
    before, after = _flatten(previous or {}), _flatten(overrides or {})
    out = []
    for path in sorted(set(before) | set(after)):
        if not path:
            continue
        old, new = before.get(path, "(default)"), after.get(path, "(default)")
        if old != new:
            out.append(f"{'.'.join(path)}: {json.dumps(old)} -> {json.dumps(new)}")
    return out


def build_settings(v: SettingsVersion, tz) -> Tuple[str, str]:
    subject = (f"QT log: {v.created_by} changed the {v.portfolio_id} settings "
               f"(version {v.version})")
    changes = settings_changes(v.previous, v.overrides)
    lines = [("Who", v.created_by), ("Did", f"saved settings version {v.version}"),
             ("When", _when(v.created_at, tz)), ("Reason given", v.reason or "-"),
             ("Changes", "\n".join(changes) or "none against the version before")]
    return subject, _body(v.portfolio_id, lines)


def _body(portfolio_id: str, lines) -> str:
    rows = "".join(f"<tr><td><b>{html.escape(k)}</b></td><td><pre style='margin:0'>"
                   f"{html.escape(str(v))}</pre></td></tr>" for k, v in lines)
    return ("<html><body>"
            f"<p>A desk action on <b>{html.escape(portfolio_id)}</b>.</p>"
            f"<table cellpadding='4'>{rows}</table>"
            "<p>The QT desk's log for the VP and the President: one e-mail per desk action, from "
            "the QT scheduler on engine-rpc.</p></body></html>")


class ActivityState:
    """The mark in <log_dir>/qt-activity-mail.json."""

    def __init__(self, log_dir: str):
        self._dir = Path(log_dir)

    @property
    def path(self) -> Path:
        return self._dir / STATE

    def load(self) -> Optional[Mark]:
        try:
            return Mark.from_json(json.loads(self.path.read_text(encoding="utf-8")))
        except (OSError, ValueError):
            return None

    def save(self, mark: Mark) -> bool:
        try:
            self._dir.mkdir(parents=True, exist_ok=True)
            tmp = self.path.with_suffix(".tmp")
            tmp.write_text(json.dumps(mark.to_json(), sort_keys=True), encoding="utf-8")
            os.replace(tmp, self.path)
            return True
        except OSError:
            return False

    def log_body(self, now: dt.datetime, to, subject: str, body: str) -> None:
        try:
            self._dir.mkdir(parents=True, exist_ok=True)
            with open(self._dir / BODY_LOG, "a", encoding="utf-8") as f:
                f.write(f"{now.isoformat()} to {', '.join(to)}: {subject}\n{body}\n\n")
        except OSError:
            pass
