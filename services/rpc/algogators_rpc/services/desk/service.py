"""Registers the desk service with the shared server.

The desk owns two background tasks:

* `desk.redrive` (recovery.py): at start and every QT_REDRIVE_INTERVAL_S (default 60 s), the
  recovery of orphaned and stale 'running' rows (retried until the startup pass succeeds), then
  the dispatch of every pending row of trading.position_overrides.
* `desk.catchup` (catchup.py): every minute it checks whether a pass is due (every 5 minutes
  from 06:30 to 10:30 New York time, then every 30 until 22:00) and runs the QT model runs that
  are missing, the 09:30 send and the 10:00 fallback (contracts C6, C7). QT_CATCHUP_ENABLED=0
  leaves it out (then nothing runs the model, sends at 09:30 or falls back at 10:00).
  With QT_EVENING_RUN=1 (the default; the data-driven run) it also polls ops.data_ready, which
  data-ngin writes after its 04:30 ET pull (the vendor's historical data is ~8 h delayed), and
  today's model run waits for the previous trading day's data. That polling is the interim
  of https://github.com/AlgoGators/trade-ngin/issues/191 (event-driven strategy runs) and
  https://github.com/AlgoGators/trade-ngin/issues/192 (NATS JetStream in place of the polling).
"""

from __future__ import annotations

import logging
from typing import Mapping, Optional

from algogators import desk_pb2, desk_pb2_grpc
from algogators.versions import API_VERSIONS

from ...registry import BackgroundTask, Registry, Service
from . import API, LEGACY_SERVICE_NAME
from .catchup import CatchupScheduler, PostgresDayStore, read_portfolio
from .command_store import PostgresCommandStore
from .commands import Dispatcher
from .config import ConfigError, load_settings
from .jobs import JobManager
from .override_mail import ApproverError, parse_approvers
from .recovery import redrive
from .servicer import DeskServicer
from .settings_check import SettingsChecker
from .store import PostgresRunStatusStore, RunStatusStore, StoreError

log = logging.getLogger("desk.service")

# Polling, the interim of issues #191 and #192 (a NATS readiness event replaces it):
# https://github.com/AlgoGators/trade-ngin/issues/191 and /192
CATCHUP_TICK_S = 60.0


def build_service(store: RunStatusStore, dispatcher: Optional[Dispatcher] = None,
                  commands=None, redrive_interval_s: float = 60.0,
                  catchup: Optional[CatchupScheduler] = None,
                  config_dir: Optional[str] = None, settings_checker=None) -> Service:
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
        servicer=DeskServicer(store, dispatcher, config_dir=config_dir,
                              settings_checker=settings_checker),
        add_to_server=desk_pb2_grpc.add_DeskServiceServicer_to_server,
        background_tasks=tuple(tasks),
        legacy_names=(LEGACY_SERVICE_NAME,),
    )


def check_desk_editable(store, config_dir: str, portfolio_dirs) -> None:
    """D-52: "desk editing on" is stored twice, in portfolio.json's qt.desk_editable (the engine)
    and strategy_registry.desk_editable (AlgoLens). The server does not start while they
    disagree for a scheduled portfolio. A database it cannot read yet is logged, not fatal: the
    runs themselves fail on it."""
    for portfolio_dir in portfolio_dirs:
        info = read_portfolio(config_dir, portfolio_dir)
        try:
            registered = store.registry_desk_editable(info.portfolio_id)
        except StoreError as exc:
            log.warning("desk-editing flags not compared: the registry cannot be read",
                        extra={"portfolio_id": info.portfolio_id, "error": str(exc)})
            continue
        if registered is not None and registered != info.desk_editable:
            raise ConfigError(
                f"desk editing disagrees for {info.portfolio_id}: portfolio.json qt.desk_editable "
                f"is {str(info.desk_editable).lower()}, strategy_registry.desk_editable is "
                f"{str(registered).lower()}; set them alike before engine-rpc starts (D-52)")


def register(registry: Registry, env: Mapping[str, str]) -> Service:
    settings = load_settings(env)  # ConfigError stops the server (ruling 24)
    # D-39: with no approvers no override can be decided and every alert goes nowhere; the
    # server does not start without both.
    try:
        parse_approvers(settings.commands.approvers)
    except ApproverError as exc:
        raise ConfigError(f"engine-rpc does not start without the approvers: {exc}") from None
    store = PostgresRunStatusStore(settings.db)
    commands = PostgresCommandStore(settings.db)
    jobs = JobManager(commands, settings.commands)
    dispatcher = Dispatcher(commands, jobs, settings.commands, risk_reader=store.desk_risk_row,
                            margin_reader=store.margin_inputs)
    catchup = None
    if settings.catchup.enabled:
        check_desk_editable(store, settings.commands.config_dir, settings.catchup.portfolios)
        catchup = CatchupScheduler(PostgresDayStore(settings.db), settings.commands,
                                   settings.catchup, dispatcher=dispatcher)
    service = registry.register(build_service(
        store, dispatcher, commands, settings.commands.redrive_interval_s, catchup,
        config_dir=settings.commands.config_dir,
        settings_checker=SettingsChecker(settings.commands)))
    log.info("desk service registered",
             extra={"db": settings.db.describe(),
                    "services": ("catch-up on: " + ",".join(settings.catchup.portfolios)
                                 if catchup else "catch-up off")})
    return service
