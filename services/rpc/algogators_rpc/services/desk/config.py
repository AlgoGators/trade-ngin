"""Runtime settings of the desk service, read from the environment.

The server's own settings (listen address, worker threads) are algogators_rpc.config.

Database credentials are found the same two ways the rest of trade-ngin finds them:

1. DB_HOST / DB_PORT / DB_USER / DB_PASSWORD / DB_NAME, the variables the watchdog
   (scripts/check_live_trading.py) reads and docker-entrypoint.sh snapshots for cron.
2. Otherwise the `database` block of <TRADING_CONFIG_DIR>/defaults.json (default
   /app/config), the file the C++ live runners load through ConfigLoader::load("./config", ...)
   and DatabaseConfig::get_connection_string() (host, port, username, password, name).

If neither yields a complete set the agent refuses to start (ruling 24: no silent fall-back).
The password is never logged: describe() redacts it.
"""

from __future__ import annotations

import json
import os
from dataclasses import dataclass, field
from pathlib import Path
import datetime as dt
from typing import Mapping, Optional, Tuple

DEFAULT_CONFIG_DIR = "/app/config"
DEFAULT_ENGINE_BINARY = "/app/build/bin/Release/live_portfolio_conservative"
DEFAULT_ENGINE_CWD = "/app"
DEFAULT_LOCK_DIR = "/tmp/qt-locks"
DEFAULT_APPROVE_URL_BASE = "https://algolens.algogators.com/qt/approve"
DEFAULT_LOG_DIR = "/var/log/qt-engine"
DEFAULT_CATCHUP_PORTFOLIOS = ("qt_conservative", "qt_conservative_model")
# Every connection of the desk (both stores) gives up after these (R#9): a database that does
# not answer never pins a gRPC worker for long.
CONNECT_TIMEOUT_S = 5
STATEMENT_TIMEOUT_MS = 30000


class ConfigError(RuntimeError):
    """The agent cannot be configured; it must not start."""


@dataclass(frozen=True)
class DbConfig:
    host: str
    port: str
    user: str
    dbname: str
    password: str = field(repr=False)
    source: str = "env"

    def conninfo_kwargs(self, connect_timeout: int = CONNECT_TIMEOUT_S) -> dict:
        return {
            "host": self.host,
            "port": self.port,
            "user": self.user,
            "password": self.password,
            "dbname": self.dbname,
            "connect_timeout": connect_timeout,
            "options": f"-c statement_timeout={STATEMENT_TIMEOUT_MS}",
            "application_name": "engine-rpc/desk",
        }

    def describe(self) -> str:
        """Safe for logs: no password."""
        return f"{self.user}@{self.host}:{self.port}/{self.dbname} (from {self.source})"


@dataclass(frozen=True)
class CommandSettings:
    """How commands are run: the engine binary, its lock and the override e-mail.

    Every field has the production default, so tests can build one with only what they change.
    """

    engine_binary: str = DEFAULT_ENGINE_BINARY
    engine_cwd: str = DEFAULT_ENGINE_CWD
    config_dir: str = DEFAULT_CONFIG_DIR
    lock_dir: str = DEFAULT_LOCK_DIR
    flock: str = "flock"
    job_timeout_s: float = 1800.0
    redrive_interval_s: float = 60.0
    # "vp=<email>,president=<email>"; parsed when an override e-mail is built, so a missing value
    # fails that row (with a message) instead of stopping the agent.
    approvers: str = ""
    approve_url_base: str = DEFAULT_APPROVE_URL_BASE
    # QT_EMAIL_DISABLED=1, and only that: the override request then stores the mail (link and
    # token included) in the row's result. Any other e-mail misconfiguration fails the row (C1).
    email_disabled: bool = False
    # A 'running' row with no live job in this process is re-driven once it is older than
    # job_timeout_s + stale_grace_s (it may belong to an engine started by hand).
    stale_grace_s: float = 300.0
    # Dated catch-up and alert logs (bind mount on the host; deploy/qt-engine.logrotate).
    log_dir: str = DEFAULT_LOG_DIR


@dataclass(frozen=True)
class CatchupSettings:
    """The scheduler (contract C6, C7; catchup.py). Times are America/New_York. The 09:30 send
    and the 10:00 cutoff are fixed (cutoff.py), not settings."""

    enabled: bool = True
    # Portfolio config directories under <TRADING_CONFIG_DIR>/portfolios/, as the old cron
    # passed them to qt_model_run.sh.
    portfolios: Tuple[str, ...] = DEFAULT_CATCHUP_PORTFOLIOS
    window_start: dt.time = dt.time(6, 30)
    window_end: dt.time = dt.time(22, 0)
    every_minutes: int = 30
    # Around the model run, the 09:30 send and the 10:00 fallback a pass is due more often.
    busy_start: dt.time = dt.time(6, 30)
    busy_end: dt.time = dt.time(10, 30)
    busy_every_minutes: int = 5
    # Today's model run waits for the day's data (data-ngin's DAG at 06:45), every day.
    # Earlier days run at once.
    today_not_before: dt.time = dt.time(6, 45)
    # A failed model run, fallback or send is retried after this many minutes.
    model_retry_minutes: float = 15.0
    # No model book for today by then: an alert to the President.
    model_alert_at: dt.time = dt.time(8, 30)
    # How long a pass waits for a portfolio's flock (a desk job may hold it) before it skips
    # that portfolio until the next pass.
    lock_wait_s: float = 600.0
    timezone: str = "America/New_York"


@dataclass(frozen=True)
class Settings:
    db: DbConfig
    commands: CommandSettings = field(default_factory=CommandSettings)
    catchup: CatchupSettings = field(default_factory=CatchupSettings)


def _db_from_env(env: Mapping[str, str]) -> Optional[DbConfig]:
    keys = ("DB_HOST", "DB_USER", "DB_PASSWORD", "DB_NAME")
    present = [k for k in keys if env.get(k)]
    if not present:
        return None
    missing = [k for k in keys if not env.get(k)]
    if missing:
        # A partial set is a mistake, not a reason to try the next source.
        raise ConfigError("database env vars incomplete; missing: " + ", ".join(missing))
    return DbConfig(host=env["DB_HOST"], port=env.get("DB_PORT") or "5432", user=env["DB_USER"],
                    password=env["DB_PASSWORD"], dbname=env["DB_NAME"], source="env")


def _db_from_defaults_json(config_dir: str) -> Optional[DbConfig]:
    path = Path(config_dir) / "defaults.json"
    if not path.is_file():
        return None
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise ConfigError(f"cannot read {path}: {type(exc).__name__}") from None
    db = data.get("database") if isinstance(data, dict) else None
    if not isinstance(db, dict):
        raise ConfigError(f"{path} has no `database` object")
    missing = [k for k in ("host", "username", "password", "name") if not db.get(k)]
    if missing:
        raise ConfigError(f"{path} database block missing: " + ", ".join(missing))
    return DbConfig(host=str(db["host"]), port=str(db.get("port") or "5432"),
                    user=str(db["username"]), password=str(db["password"]),
                    dbname=str(db["name"]), source=str(path))


def load_settings(env: Optional[Mapping[str, str]] = None) -> Settings:
    env = os.environ if env is None else env
    db = _db_from_env(env) or _db_from_defaults_json(
        env.get("TRADING_CONFIG_DIR") or DEFAULT_CONFIG_DIR)
    if db is None:
        raise ConfigError("no database credentials: set DB_HOST/DB_USER/DB_PASSWORD/DB_NAME or "
                          "mount defaults.json under TRADING_CONFIG_DIR")
    return Settings(db=db, commands=command_settings(env), catchup=catchup_settings(env))


def _positive(env: Mapping[str, str], name: str, default: float) -> float:
    raw = env.get(name)
    if not raw:
        return default
    try:
        value = float(raw)
    except ValueError:
        raise ConfigError(f"{name} must be a number of seconds") from None
    if value <= 0:
        raise ConfigError(f"{name} must be > 0")
    return value


def command_settings(env: Optional[Mapping[str, str]] = None) -> CommandSettings:
    env = os.environ if env is None else env
    return CommandSettings(
        engine_binary=env.get("QT_ENGINE_BINARY") or DEFAULT_ENGINE_BINARY,
        engine_cwd=env.get("QT_ENGINE_CWD") or DEFAULT_ENGINE_CWD,
        config_dir=env.get("TRADING_CONFIG_DIR") or DEFAULT_CONFIG_DIR,
        lock_dir=env.get("QT_LOCK_DIR") or DEFAULT_LOCK_DIR,
        flock=env.get("QT_FLOCK") or "flock",
        job_timeout_s=_positive(env, "QT_JOB_TIMEOUT_S", 1800.0),
        redrive_interval_s=_positive(env, "QT_REDRIVE_INTERVAL_S", 60.0),
        approvers=env.get("QT_APPROVERS") or "",
        approve_url_base=env.get("QT_APPROVE_URL_BASE") or DEFAULT_APPROVE_URL_BASE,
        email_disabled=(env.get("QT_EMAIL_DISABLED") or "").strip() == "1",
        stale_grace_s=_positive(env, "QT_STALE_GRACE_S", 300.0),
        log_dir=env.get("QT_LOG_DIR") or DEFAULT_LOG_DIR,
    )


def _clock(env: Mapping[str, str], name: str, default: dt.time) -> dt.time:
    raw = (env.get(name) or "").strip()
    if not raw:
        return default
    try:
        hh, mm = raw.split(":")
        return dt.time(int(hh), int(mm))
    except ValueError:
        raise ConfigError(f"{name} must be HH:MM (America/New_York)") from None


def _window(env: Mapping[str, str], name: str, default: str) -> Tuple[dt.time, dt.time]:
    raw = (env.get(name) or default).strip()
    try:
        start_raw, end_raw = raw.split("-")
    except ValueError:
        raise ConfigError(f"{name} must be HH:MM-HH:MM") from None
    start = _clock({name: start_raw}, name, dt.time(0, 0))
    end = _clock({name: end_raw}, name, dt.time(0, 0))
    if end <= start:
        raise ConfigError(f"{name} must end after it starts")
    return start, end


def _every(env: Mapping[str, str], name: str, default: int) -> int:
    every = _positive(env, name, default)
    if int(every) != every or 60 % int(every):
        raise ConfigError(f"{name} must divide 60 (e.g. 5, 15, 30, 60)")
    return int(every)


def catchup_settings(env: Optional[Mapping[str, str]] = None) -> CatchupSettings:
    """QT_CATCHUP_ENABLED (default 1), QT_CATCHUP_PORTFOLIOS (comma-separated config dirs),
    QT_CATCHUP_WINDOW ("06:30-22:00"), QT_CATCHUP_EVERY_MIN (30), QT_CATCHUP_BUSY_WINDOW
    ("06:30-10:30"), QT_CATCHUP_BUSY_EVERY_MIN (5), QT_CATCHUP_TODAY_NOT_BEFORE ("06:45"),
    QT_MODEL_RETRY_MIN (15), QT_MODEL_ALERT_AT ("08:30"), QT_CATCHUP_LOCK_WAIT_S (600).
    QT_UNPUBLISHED_REMINDER_H is no longer read (the 10:00 fallback replaced the reminder)."""
    env = os.environ if env is None else env
    enabled = (env.get("QT_CATCHUP_ENABLED") or "1").strip() != "0"
    raw = env.get("QT_CATCHUP_PORTFOLIOS")
    portfolios = (tuple(p.strip() for p in raw.split(",") if p.strip()) if raw is not None
                  else DEFAULT_CATCHUP_PORTFOLIOS)
    if enabled and not portfolios:
        raise ConfigError("QT_CATCHUP_PORTFOLIOS names no portfolio; set QT_CATCHUP_ENABLED=0 "
                          "to turn the catch-up scheduler off")
    start, end = _window(env, "QT_CATCHUP_WINDOW", "06:30-22:00")
    busy_start, busy_end = _window(env, "QT_CATCHUP_BUSY_WINDOW", "06:30-10:30")
    return CatchupSettings(
        enabled=enabled, portfolios=portfolios, window_start=start, window_end=end,
        every_minutes=_every(env, "QT_CATCHUP_EVERY_MIN", 30),
        busy_start=busy_start, busy_end=busy_end,
        busy_every_minutes=_every(env, "QT_CATCHUP_BUSY_EVERY_MIN", 5),
        today_not_before=_clock(env, "QT_CATCHUP_TODAY_NOT_BEFORE", dt.time(6, 45)),
        model_retry_minutes=_positive(env, "QT_MODEL_RETRY_MIN", 15.0),
        model_alert_at=_clock(env, "QT_MODEL_ALERT_AT", dt.time(8, 30)),
        lock_wait_s=_positive(env, "QT_CATCHUP_LOCK_WAIT_S", 600.0),
    )
