"""Server bootstrap: every registered service + grpc.health.v1 on one plaintext port.

    python -m algogators_rpc          (the image: `docker run ... trade-ngin rpc`)

Startup: read the server settings, let each configured service register itself, run every
background task's on_start hook (the desk re-drives pending rows here), start the periodic
background threads, then serve. SIGTERM/SIGINT: health goes NOT_SERVING, background loops stop,
in-flight calls get 10 s to finish.
"""

from __future__ import annotations

import logging
import signal
import threading
from concurrent import futures
from typing import List, Optional

import grpc
from google.protobuf import message_factory
from grpc_health.v1 import health, health_pb2, health_pb2_grpc

from . import log as logsetup
from . import services as service_modules
from .config import load_server_settings
from .interceptors import LoggingInterceptor, VersionInterceptor
from .registry import BackgroundTask, Registry, Service
from .versioning import format_header

log = logging.getLogger("rpc.server")

SHUTDOWN_GRACE_S = 10


def legacy_handler(service: Service, legacy_name: str):
    """A generic handler serving `service` under an earlier full name (same messages, so the
    wire is identical). Unary-unary RPCs only."""
    handlers = {}
    for method in service.descriptor.methods:
        if method.client_streaming or method.server_streaming:
            raise ValueError(f"{service.full_name}.{method.name}: legacy names support "
                             "unary-unary RPCs only")
        request = message_factory.GetMessageClass(method.input_type)
        reply = message_factory.GetMessageClass(method.output_type)
        handlers[method.name] = grpc.unary_unary_rpc_method_handler(
            getattr(service.servicer, method.name),
            request_deserializer=request.FromString,
            response_serializer=reply.SerializeToString)
    return grpc.method_handlers_generic_handler(legacy_name, handlers)


def set_health(health_servicer, registry: Registry, status) -> None:
    for name in ("", *registry.health_names()):
        health_servicer.set(name, status)


def build_server(registry: Registry, address: str, max_workers: int = 4):
    """Returns (server, health_servicer, bound_port). The server is not started."""
    server = grpc.server(futures.ThreadPoolExecutor(max_workers=max_workers),
                         interceptors=[LoggingInterceptor(), VersionInterceptor(registry)])
    for service in registry.services():
        service.add_to_server(service.servicer, server)
        for name in service.legacy_names:
            server.add_generic_rpc_handlers((legacy_handler(service, name),))
    health_servicer = health.HealthServicer()
    health_pb2_grpc.add_HealthServicer_to_server(health_servicer, server)
    port = server.add_insecure_port(address)
    if port == 0:
        raise RuntimeError(f"cannot bind {address}")
    set_health(health_servicer, registry, health_pb2.HealthCheckResponse.SERVING)
    return server, health_servicer, port


def _tasks(registry: Registry) -> List[BackgroundTask]:
    return [t for s in registry.services() for t in s.background_tasks]


def run_start_hooks(registry: Registry) -> None:
    for task in _tasks(registry):
        if task.on_start is None:
            continue
        try:
            task.on_start()
        except Exception as exc:
            log.error("background task start hook failed; its loop will retry",
                      exc_info=True, extra={"task": task.name, "error": type(exc).__name__})


def start_background_tasks(registry: Registry, stop: threading.Event) -> List[threading.Thread]:
    threads = []
    for task in _tasks(registry):
        def loop(task=task):
            while not stop.wait(task.interval_s):
                try:
                    task.run()
                except Exception as exc:
                    log.error("background task failed", exc_info=True,
                              extra={"task": task.name, "error": type(exc).__name__})

        thread = threading.Thread(target=loop, name=task.name, daemon=True)
        thread.start()
        threads.append(thread)
    return threads


def load_registry(names, env=None) -> Registry:
    registry = Registry()
    service_modules.load(registry, names, env)
    return registry


def main(env=None) -> int:
    logsetup.setup()
    try:
        settings = load_server_settings(env)
        registry = load_registry(settings.services, env)
    except Exception as exc:
        log.error("refusing to start: configuration", extra={"error": str(exc)})
        return 2
    server, health_servicer, _ = build_server(registry, settings.listen, settings.max_workers)
    stop = threading.Event()
    run_start_hooks(registry)
    start_background_tasks(registry, stop)
    server.start()
    log.info("rpc server serving", extra={"listen": settings.listen,
                                          "services": format_header(registry.versions())})

    for sig in (signal.SIGTERM, signal.SIGINT):
        signal.signal(sig, lambda *_: stop.set())
    stop.wait()
    log.info("shutting down")
    set_health(health_servicer, registry, health_pb2.HealthCheckResponse.NOT_SERVING)
    server.stop(grace=SHUTDOWN_GRACE_S).wait()
    return 0
