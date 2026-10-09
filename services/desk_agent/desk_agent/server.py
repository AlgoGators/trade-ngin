"""Server wiring: DeskService + grpc.health.v1, plaintext on the private `qt` network."""

from __future__ import annotations

import logging
import signal
import threading
from concurrent import futures

import grpc
from grpc_health.v1 import health, health_pb2, health_pb2_grpc

from qt.v1 import desk_pb2_grpc

from . import SERVICE_NAME
from . import log as logsetup
from .config import ConfigError, load_settings
from .recovery import redrive_pending
from .servicer import DeskServicer
from .store import PostgresRunStatusStore, RunStatusStore

log = logging.getLogger("desk_agent")


def build_server(store: RunStatusStore, address: str, max_workers: int = 4):
    """Returns (server, health_servicer, bound_port). The server is not started."""
    server = grpc.server(futures.ThreadPoolExecutor(max_workers=max_workers))
    desk_pb2_grpc.add_DeskServiceServicer_to_server(DeskServicer(store), server)
    health_servicer = health.HealthServicer()
    health_pb2_grpc.add_HealthServicer_to_server(health_servicer, server)
    port = server.add_insecure_port(address)
    if port == 0:
        raise RuntimeError(f"cannot bind {address}")
    for name in ("", SERVICE_NAME):
        health_servicer.set(name, health_pb2.HealthCheckResponse.SERVING)
    return server, health_servicer, port


def main() -> int:
    logsetup.setup()
    try:
        settings = load_settings()
    except ConfigError as exc:
        log.error("refusing to start: configuration", extra={"error": str(exc)})
        return 2
    store = PostgresRunStatusStore(settings.db)
    server, health_servicer, _ = build_server(store, settings.listen, settings.max_workers)
    redrive_pending(store)
    server.start()
    log.info("desk-agent serving", extra={"listen": settings.listen,
                                          "db": settings.db.describe()})

    stop = threading.Event()
    for sig in (signal.SIGTERM, signal.SIGINT):
        signal.signal(sig, lambda *_: stop.set())
    stop.wait()
    log.info("shutting down")
    for name in ("", SERVICE_NAME):
        health_servicer.set(name, health_pb2.HealthCheckResponse.NOT_SERVING)
    server.stop(grace=10).wait()
    return 0
