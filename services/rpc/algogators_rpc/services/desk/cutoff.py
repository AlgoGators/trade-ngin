"""The QT daily cutoff (contract C7, decided 2026-10-09; amends rulings 18 and 29).

Every calendar day D, weekends and holidays included, America/New_York:

    ~07:00  the model run for D (the scheduler starts it at 06:45, retries every 15 min)
    09:30   SEND: a day the desk approved is e-mailed (from its stored rows), once
    10:00   CUTOFF: a day not approved is reset to the model's book, published
            (system:fallback-10am, publish_source 'fallback') and e-mailed at once

The desk's Publish command is the approval ("Approve" in AlgoLens). Its row's created_at decides
whether it was in time: before 09:30 it freezes the day and sends nothing (the 09:30 send does);
from 09:30 to 10:00 it freezes and sends at once; from 10:00 on it is refused. The times are fixed
by the decision, not configuration.
"""

from __future__ import annotations

import datetime as dt
from typing import Optional

TIMEZONE = "America/New_York"
SEND_AT = dt.time(9, 30)
CUTOFF_AT = dt.time(10, 0)

FALLBACK_10AM = "system:fallback-10am"
FALLBACK_CATCHUP = "system:fallback-catchup"
FALLBACK_PREFIX = "system:fallback"
MODEL_ONLY = "system:model-only"

# Approval windows of a book date.
BEFORE_SEND, SEND_NOW, CLOSED = "before_send", "send_now", "closed"


def zone():
    from zoneinfo import ZoneInfo

    return ZoneInfo(TIMEZONE)


def at(day: dt.date, time: dt.time) -> dt.datetime:
    """`time` New York on `day`, as an aware datetime (DST resolved by the zone)."""
    return dt.datetime.combine(day, time, tzinfo=zone())


def send_time(day: dt.date) -> dt.datetime:
    return at(day, SEND_AT)


def cutoff_time(day: dt.date) -> dt.datetime:
    return at(day, CUTOFF_AT)


def window(day: dt.date, when: dt.datetime) -> str:
    """Where `when` (aware) falls for an approval of book date `day`."""
    if when >= cutoff_time(day):
        return CLOSED
    if when >= send_time(day):
        return SEND_NOW
    return BEFORE_SEND


def is_fallback(requested_by: Optional[str]) -> bool:
    return (requested_by or "").startswith(FALLBACK_PREFIX)


def deadline_message(day: dt.date) -> str:
    return (f"the approval deadline for {day} passed at {CUTOFF_AT:%H:%M} New York; the 10:00 "
            "fallback publishes and sends the model's book")
