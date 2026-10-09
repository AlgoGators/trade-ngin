"""The shared server's own settings, read from the environment.

    RPC_LISTEN        bind address (default 0.0.0.0:50051). No TLS: the server is reachable
                      only on the private Docker network `qt` and publishes no host port.
    RPC_MAX_WORKERS   gRPC worker threads (default 8).
    RPC_HEALTH_LISTEN a second, health-only port with its own 2 threads (default
                      127.0.0.1:50052), so a busy or stuck main pool never fails the container
                      healthcheck (R#9). "off" disables it.
    RPC_SERVICES      comma-separated services to run (default: desk). Names are the keys of
                      algogators_rpc.services.AVAILABLE.

DESK_AGENT_LISTEN and DESK_AGENT_MAX_WORKERS, the names used before the shared layer, are still
read when the RPC_ ones are unset. They go away in the next release.

Each service reads its own settings (e.g. services/desk/config.py).
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from typing import Mapping, Optional, Tuple

DEFAULT_LISTEN = "0.0.0.0:50051"
DEFAULT_HEALTH_LISTEN = "127.0.0.1:50052"
DEFAULT_MAX_WORKERS = 8
DEFAULT_SERVICES = ("desk",)


class ConfigError(RuntimeError):
    """The server cannot be configured; it must not start."""


@dataclass(frozen=True)
class ServerSettings:
    listen: str = DEFAULT_LISTEN
    max_workers: int = DEFAULT_MAX_WORKERS
    services: Tuple[str, ...] = DEFAULT_SERVICES
    # None: no separate health port.
    health_listen: Optional[str] = DEFAULT_HEALTH_LISTEN


def _first(env: Mapping[str, str], *names: str) -> Tuple[Optional[str], str]:
    for name in names:
        value = (env.get(name) or "").strip()
        if value:
            return value, name
    return None, names[0]


def load_server_settings(env: Optional[Mapping[str, str]] = None) -> ServerSettings:
    env = os.environ if env is None else env
    listen, _ = _first(env, "RPC_LISTEN", "DESK_AGENT_LISTEN")
    raw_workers, workers_name = _first(env, "RPC_MAX_WORKERS", "DESK_AGENT_MAX_WORKERS")
    try:
        max_workers = int(raw_workers or str(DEFAULT_MAX_WORKERS))
    except ValueError:
        raise ConfigError(f"{workers_name} must be an integer") from None
    if max_workers < 1:
        raise ConfigError(f"{workers_name} must be >= 1")
    raw_services, _ = _first(env, "RPC_SERVICES")
    services = (tuple(s.strip() for s in raw_services.split(",") if s.strip())
                if raw_services else DEFAULT_SERVICES)
    if not services:
        raise ConfigError("RPC_SERVICES names no service")
    health, _ = _first(env, "RPC_HEALTH_LISTEN")
    health_listen = None if (health or "").lower() == "off" else (health or DEFAULT_HEALTH_LISTEN)
    return ServerSettings(listen=listen or DEFAULT_LISTEN, max_workers=max_workers,
                          services=services, health_listen=health_listen)
