"""Registers the desk service with the shared server.

The desk owns one background task: the re-drive of trading.position_overrides (recovery.py).
On start it resets orphaned 'running' rows and dispatches every pending row; then it sweeps
pending rows every QT_REDRIVE_INTERVAL_S (default 60 s).
"""

from __future__ import annotations

import logging
from typing import Mapping, Optional

from algogators import desk_pb2, desk_pb2_grpc
from algogators.versions import API_VERSIONS

from ...registry import BackgroundTask, Registry, Service
from . import API, LEGACY_SERVICE_NAME
from .command_store import PostgresCommandStore
from .commands import Dispatcher
from .config import load_settings
from .jobs import JobManager
from .recovery import redrive_pending, sweep_pending
from .servicer import DeskServicer
from .store import PostgresRunStatusStore, RunStatusStore

log = logging.getLogger("desk.service")


def build_service(store: RunStatusStore, dispatcher: Optional[Dispatcher] = None,
                  commands=None, redrive_interval_s: float = 60.0) -> Service:
    """The desk as a registry entry. Without `commands` (tests) it has no background task."""
    tasks = ()
    if commands is not None and dispatcher is not None:
        tasks = (BackgroundTask(
            name="desk.redrive", interval_s=redrive_interval_s,
            run=lambda: sweep_pending(commands, dispatcher),
            on_start=lambda: redrive_pending(commands, dispatcher)),)
    return Service(
        api=API,
        version=API_VERSIONS[API],
        descriptor=desk_pb2.DESCRIPTOR.services_by_name["DeskService"],
        servicer=DeskServicer(store, dispatcher),
        add_to_server=desk_pb2_grpc.add_DeskServiceServicer_to_server,
        background_tasks=tasks,
        legacy_names=(LEGACY_SERVICE_NAME,),
    )


def register(registry: Registry, env: Mapping[str, str]) -> Service:
    settings = load_settings(env)  # ConfigError stops the server (ruling 24)
    store = PostgresRunStatusStore(settings.db)
    commands = PostgresCommandStore(settings.db)
    jobs = JobManager(commands, settings.commands)
    dispatcher = Dispatcher(commands, jobs, settings.commands)
    service = registry.register(build_service(
        store, dispatcher, commands, settings.commands.redrive_interval_s))
    log.info("desk service registered", extra={"db": settings.db.describe()})
    return service
