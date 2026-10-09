"""Runtime settings for desk-agent, read from the environment.

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
from typing import Mapping, Optional

DEFAULT_LISTEN = "0.0.0.0:50051"
DEFAULT_CONFIG_DIR = "/app/config"


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

    def conninfo_kwargs(self, connect_timeout: int = 10) -> dict:
        return {
            "host": self.host,
            "port": self.port,
            "user": self.user,
            "password": self.password,
            "dbname": self.dbname,
            "connect_timeout": connect_timeout,
            "application_name": "desk-agent",
        }

    def describe(self) -> str:
        """Safe for logs: no password."""
        return f"{self.user}@{self.host}:{self.port}/{self.dbname} (from {self.source})"


@dataclass(frozen=True)
class Settings:
    listen: str
    db: DbConfig
    max_workers: int


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
    try:
        max_workers = int(env.get("DESK_AGENT_MAX_WORKERS") or "4")
    except ValueError:
        raise ConfigError("DESK_AGENT_MAX_WORKERS must be an integer") from None
    if max_workers < 1:
        raise ConfigError("DESK_AGENT_MAX_WORKERS must be >= 1")
    return Settings(listen=env.get("DESK_AGENT_LISTEN") or DEFAULT_LISTEN, db=db,
                    max_workers=max_workers)
