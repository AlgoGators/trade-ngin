"""Server interceptors shared by every service: the version header and one log line per call.

Order (server.py): logging is outermost, so a call refused by the version check is logged too.
Health checks (grpc.health.v1) are neither version-checked nor logged.
"""

from __future__ import annotations

import logging
import time
from typing import Callable, Optional

import grpc

from .registry import Registry
from .versioning import HEADER, check

log = logging.getLogger("rpc")

HEALTH_SERVICE = "grpc.health.v1.Health"


def service_of(method: str) -> str:
    """'/algogators.desk.DeskService/RunDesk' -> 'algogators.desk.DeskService'."""
    return method.lstrip("/").rsplit("/", 1)[0]


def _metadata_value(details, key: str) -> Optional[str]:
    values = [v for k, v in (details.invocation_metadata or ()) if k == key]
    return ",".join(values) if values else None


def _wrap(handler, wrap_behavior: Callable):
    """The same handler with its behavior function passed through `wrap_behavior`."""
    kinds = (("unary_unary", grpc.unary_unary_rpc_method_handler),
             ("unary_stream", grpc.unary_stream_rpc_method_handler),
             ("stream_unary", grpc.stream_unary_rpc_method_handler),
             ("stream_stream", grpc.stream_stream_rpc_method_handler))
    for attr, factory in kinds:
        behavior = getattr(handler, attr)
        if behavior is not None:
            return factory(wrap_behavior(behavior),
                           request_deserializer=handler.request_deserializer,
                           response_serializer=handler.response_serializer)
    return handler


class VersionInterceptor(grpc.ServerInterceptor):
    """Refuses a call whose x-algogators-api-version header is missing or names another major
    version of the called API (FAILED_PRECONDITION), and echoes the server's '<api>=<version>' in
    the response's initial metadata. Health checks, legacy names and unknown services pass."""

    def __init__(self, registry: Registry):
        self._registry = registry

    def intercept_service(self, continuation, handler_call_details):
        handler = continuation(handler_call_details)
        name = service_of(handler_call_details.method)
        if handler is None or name == HEALTH_SERVICE:
            return handler
        found = self._registry.lookup(name)
        if found is None or found[1]:
            return handler
        service = found[0]
        refusal = check(service.api, service.version,
                        _metadata_value(handler_call_details, HEADER))
        echo = ((HEADER, f"{service.api}={service.version}"),)

        def wrap(behavior):
            def checked(request, context):
                context.send_initial_metadata(echo)
                if refusal is not None:
                    context.abort(grpc.StatusCode.FAILED_PRECONDITION, refusal)
                return behavior(request, context)
            return checked

        return _wrap(handler, wrap)


def _code_name(context, default: str) -> str:
    code_fn = getattr(context, "code", None)
    code = code_fn() if callable(code_fn) else None
    return code.name if isinstance(code, grpc.StatusCode) else default


class LoggingInterceptor(grpc.ServerInterceptor):
    """One structured line per call: method, final code, elapsed time, the version header the
    client sent. Never the request or reply (they can carry credentials, e.g. a link token)."""

    def intercept_service(self, continuation, handler_call_details):
        handler = continuation(handler_call_details)
        method = handler_call_details.method
        if handler is None or service_of(method) == HEALTH_SERVICE:
            return handler
        sent = _metadata_value(handler_call_details, HEADER)

        def wrap(behavior):
            def logged(request, context):
                started = time.monotonic()
                code = "UNKNOWN"
                try:
                    reply = behavior(request, context)
                    code = _code_name(context, "OK")
                    return reply
                except Exception:
                    code = _code_name(context, "UNKNOWN")
                    raise
                finally:
                    log.info("rpc call", extra={
                        "method": method, "code": code, "api_version": sent,
                        "elapsed_ms": round((time.monotonic() - started) * 1000, 1)})
            return logged

        return _wrap(handler, wrap)
