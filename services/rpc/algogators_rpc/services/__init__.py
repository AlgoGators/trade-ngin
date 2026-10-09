"""The services this server can run.

Each entry names a module with `register(registry, env)`, which reads the service's own
settings, builds its servicer and calls `registry.register(Service(...))`. RPC_SERVICES picks
which ones run (algogators_rpc.config). Adding a service: docs/design/rpc.md, "Adding a service".
"""

from __future__ import annotations

import importlib
import os

from ..config import ConfigError

AVAILABLE = {
    "desk": "algogators_rpc.services.desk.service",
}


def load(registry, names, env=None) -> None:
    env = os.environ if env is None else env
    unknown = [n for n in names if n not in AVAILABLE]
    if unknown:
        raise ConfigError("unknown service(s) in RPC_SERVICES: " + ", ".join(unknown)
                          + "; available: " + ", ".join(sorted(AVAILABLE)))
    for name in names:
        importlib.import_module(AVAILABLE[name]).register(registry, env)
