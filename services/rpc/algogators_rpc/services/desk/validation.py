"""Input checks shared by every RPC. A failed check is a caller bug: INVALID_ARGUMENT."""

from __future__ import annotations

import datetime as dt
import re

# Same shape the C++ side accepts for identifiers (CredentialStore name pattern, strategy ids).
_PORTFOLIO_ID = re.compile(r"^[A-Za-z0-9_-]{1,64}$")
_DATE = re.compile(r"^\d{4}-\d{2}-\d{2}$")
_MAX_TEXT = 2000


class InvalidArgument(ValueError):
    pass


def portfolio_id(value: str) -> str:
    if not _PORTFOLIO_ID.match(value or ""):
        raise InvalidArgument("portfolio_id must match [A-Za-z0-9_-]{1,64}")
    return value


def date(value: str) -> dt.date:
    if not _DATE.match(value or ""):
        raise InvalidArgument("date must be YYYY-MM-DD")
    try:
        return dt.date.fromisoformat(value)
    except ValueError:
        raise InvalidArgument(f"date {value!r} is not a calendar date") from None


def audit_id(value: int) -> int:
    if value <= 0:
        raise InvalidArgument("audit_id must be a trading.position_overrides id (> 0)")
    return value


def text(name: str, value: str) -> str:
    if not (value or "").strip():
        raise InvalidArgument(f"{name} is required")
    if len(value) > _MAX_TEXT:
        raise InvalidArgument(f"{name} is longer than {_MAX_TEXT} characters")
    return value
