"""The futures trading calendar of the QT scheduler: the engine's own.

A trading day is a weekday that is not a market holiday of the engine's calendar, the
holidays.json HolidayChecker loads (include/trade_ngin/core/holiday_checker.hpp), found the same
way: TRADE_NGIN_HOLIDAYS_JSON, else include/trade_ngin/core/holidays.json, holidays.json and
/etc/trade_ngin/holidays.json under the engine's working directory (QT_ENGINE_CWD). As in the
engine, a date outside the years the file covers is an error, never a guess (BA-1, C-5 B4).

The data-driven run (catchup.py; issues #191, #192) builds the books of the next trading day after
a bar date: Friday's, Saturday's and Sunday's bars all build Monday, a holiday is skipped.
"""

from __future__ import annotations

import datetime as dt
import json
import os
from pathlib import Path
from typing import Dict, FrozenSet, Iterable, Mapping, Optional

ENV_PATH = "TRADE_NGIN_HOLIDAYS_JSON"
CANDIDATES = ("include/trade_ngin/core/holidays.json", "holidays.json",
              "/etc/trade_ngin/holidays.json")
# A sanity bound: no exchange closes for longer than this.
MAX_GAP_DAYS = 14


class CalendarError(RuntimeError):
    """The calendar is missing, unreadable or does not cover the date asked about."""


class TradingCalendar:
    def __init__(self, holidays: Iterable[dt.date], years: Iterable[int], source: str = ""):
        self.holidays: FrozenSet[dt.date] = frozenset(holidays)
        self.years: FrozenSet[int] = frozenset(years)
        self.source = source

    def covers(self, day: dt.date) -> bool:
        return day.year in self.years

    def is_trading_day(self, day: dt.date) -> bool:
        if not self.covers(day):
            raise CalendarError(f"the holiday calendar {self.source or ''} does not cover "
                                f"{day.year}; extend it with scripts/generate_market_holidays.py")
        return day.weekday() < 5 and day not in self.holidays

    def next_trading_day(self, day: dt.date) -> dt.date:
        """The first trading day after `day` (Friday and Sunday -> Monday)."""
        d = day
        for _ in range(MAX_GAP_DAYS):
            d += dt.timedelta(days=1)
            if self.is_trading_day(d):
                return d
        raise CalendarError(f"no trading day within {MAX_GAP_DAYS} days after {day}")

    def previous_trading_day(self, day: dt.date) -> dt.date:
        """The last trading day before `day` (Monday -> Friday)."""
        d = day
        for _ in range(MAX_GAP_DAYS):
            d -= dt.timedelta(days=1)
            if self.is_trading_day(d):
                return d
        raise CalendarError(f"no trading day within {MAX_GAP_DAYS} days before {day}")


def resolve_holidays_path(engine_cwd: str, env: Optional[Mapping[str, str]] = None) -> str:
    env = os.environ if env is None else env
    if env.get(ENV_PATH):
        return str(env[ENV_PATH])
    for candidate in CANDIDATES:
        path = Path(candidate) if os.path.isabs(candidate) else Path(engine_cwd) / candidate
        if path.is_file():
            return str(path)
    return str(Path(engine_cwd) / CANDIDATES[0])


def parse_holidays(data: object, source: str = "") -> TradingCalendar:
    """{"2026": [{"date": "2026-01-01", ...}, ...], ...} (holidays.json)."""
    if not isinstance(data, dict) or not data:
        raise CalendarError(f"{source or 'holidays.json'} is not a non-empty object of years")
    holidays, years = set(), set()
    for year, entries in data.items():
        try:
            years.add(int(year))
        except ValueError:
            raise CalendarError(f"{source}: {year!r} is not a year") from None
        if not isinstance(entries, list):
            raise CalendarError(f"{source}: year {year} is not a list")
        for entry in entries:
            raw = entry.get("date") if isinstance(entry, dict) else None
            try:
                holidays.add(dt.date.fromisoformat(str(raw)))
            except ValueError:
                raise CalendarError(f"{source}: bad holiday date {raw!r}") from None
    return TradingCalendar(holidays, years, source)


def load_calendar(path: str) -> TradingCalendar:
    try:
        data = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise CalendarError(f"cannot read the holiday calendar {path}: "
                            f"{type(exc).__name__}") from None
    return parse_holidays(data, path)


def calendar_from_dates(holidays: Iterable[dt.date], years: Iterable[int]) -> TradingCalendar:
    """For tests and callers that already hold the dates."""
    return TradingCalendar(holidays, years, "inline")


def per_contract_rolls(config_dir: str, portfolio_dir: str) -> bool:
    """portfolio.json's top-level per_contract_rolls as the engine reads it (ConfigLoader: the
    portfolio's file over defaults.json; absent means false). On, the model run reads the
    per-contract bars (rolls step 3), so it also waits for their ops.data_ready row. A value that
    is not true or false is left to the engine, which refuses it."""
    from .portfolios import portfolios_root

    value = False
    for path in (Path(config_dir) / "defaults.json",
                 portfolios_root(config_dir) / portfolio_dir / "portfolio.json"):
        try:
            data: Dict = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        raw = data.get("per_contract_rolls") if isinstance(data, dict) else None
        if isinstance(raw, bool):
            value = raw
    return value


def staleness_tolerance_days(config_dir: str, portfolio_dir: str, default: int = 4) -> int:
    """live.data_staleness_tolerance_days as the engine reads it: the portfolio's portfolio.json
    `live` block over defaults.json's (ConfigLoader), else the engine's default of 4."""
    from .portfolios import portfolios_root

    value = default
    for path in (Path(config_dir) / "defaults.json",
                 portfolios_root(config_dir) / portfolio_dir / "portfolio.json"):
        try:
            data: Dict = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        live = data.get("live") if isinstance(data, dict) else None
        raw = live.get("data_staleness_tolerance_days") if isinstance(live, dict) else None
        if isinstance(raw, int) and not isinstance(raw, bool) and raw >= 0:
            value = raw
    return value
