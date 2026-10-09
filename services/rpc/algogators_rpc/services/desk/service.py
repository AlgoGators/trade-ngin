"""Registers the desk service with the shared server.

The desk owns two background tasks:

* `desk.redrive` (recovery.py): at start and every QT_REDRIVE_INTERVAL_S (default 60 s), the
  recovery of orphaned and stale 'running' rows (retried until the startup pass succeeds), then
  the dispatch of every pending row of trading.position_overrides.
* `desk.catchup` (catchup.py): every minute it checks whether a catch-up pass is due (every
  QT_CATCHUP_EVERY_MIN inside QT_CATCHUP_WINDOW, New York time) and runs the QT model runs that
  are missing (contract C6). QT_CATCHUP_ENABLED=0 leaves it out.
"""

from __future__ import annotations

import logging
from typing import Mapping, Optional

from algogators import desk_pb2, desk_pb2_grpc
from algogators.versions import API_VERSIONS

from ...registry import BackgroundTask, Registry, Service
from . import API, LEGACY_SERVICE_NAME
from .catchup import CatchupScheduler, PostgresDayStore
from .command_store import PostgresCommandStore
from .commands import Dispatcher
from .config import load_settings
from .jobs import JobManager
from .recovery import redrive
from .servicer import DeskServicer
from .store import PostgresRunStatusStore, RunStatusStore

log = logging.getLogger("desk.service")

CATCHUP_TICK_S = 60.0


def build_service(store: RunStatusStore, dispatcher: Optional[Dispatcher] = None,
                  commands=None, redrive_interval_s: float = 60.0,
                  catchup: Optional[CatchupScheduler] = None) -> Service:
    """The desk as a registry entry. Without `commands` (tests) it has no re-drive task;
    without `catchup` no scheduler."""
    tasks = []
    if commands is not None and dispatcher is not None:
        tasks.append(BackgroundTask(
            name="desk.redrive", interval_s=redrive_interval_s,
            run=lambda: redrive(commands, dispatcher),
            on_start=lambda: redrive(commands, dispatcher)))
    if catchup is not None:
        tasks.append(BackgroundTask(name="desk.catchup", interval_s=CATCHUP_TICK_S,
                                    run=catchup.tick))
    return Service(
        api=API,
        version=API_VERSIONS[API],
        descriptor=desk_pb2.DESCRIPTOR.services_by_name["DeskService"],
        servicer=DeskServicer(store, dispatcher),
        add_to_server=desk_pb2_grpc.add_DeskServiceServicer_to_server,
        background_tasks=tuple(tasks),
        legacy_names=(LEGACY_SERVICE_NAME,),
    )


def register(registry: Registry, env: Mapping[str, str]) -> Service:
    settings = load_settings(env)  # ConfigError stops the server (ruling 24)
    store = PostgresRunStatusStore(settings.db)
    commands = PostgresCommandStore(settings.db)
    jobs = JobManager(commands, settings.commands)
    dispatcher = Dispatcher(commands, jobs, settings.commands)
    catchup = None
    if settings.catchup.enabled:
        catchup = CatchupScheduler(PostgresDayStore(settings.db), settings.commands,
                                   settings.catchup)
    service = registry.register(build_service(
        store, dispatcher, commands, settings.commands.redrive_interval_s, catchup))
    log.info("desk service registered",
             extra={"db": settings.db.describe(),
                    "services": ("catch-up on: " + ",".join(settings.catchup.portfolios)
                                 if catchup else "catch-up off")})
    return service
