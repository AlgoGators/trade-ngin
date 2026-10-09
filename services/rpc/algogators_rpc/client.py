"""Client side of the version header, for Python callers inside trade-ngin (tests,
scripts/qt_grpc_call.py). AlgoLens has its own copy in algolens/infrastructure/rpc/.

    with versioned_channel("engine-rpc:50051") as channel:
        desk_pb2_grpc.DeskServiceStub(channel).GetRunStatus(...)

Every call on the channel carries x-algogators-api-version with every API in
algogators.versions.API_VERSIONS (the versions parsed from the .proto headers).
"""

from __future__ import annotations

import collections
from typing import Mapping, Optional

import grpc

from .versioning import HEADER, format_header


class _CallDetails(collections.namedtuple(
        "_CallDetails", ("method", "timeout", "metadata", "credentials", "wait_for_ready",
                         "compression")), grpc.ClientCallDetails):
    pass


class VersionHeaderInterceptor(grpc.UnaryUnaryClientInterceptor,
                               grpc.UnaryStreamClientInterceptor,
                               grpc.StreamUnaryClientInterceptor,
                               grpc.StreamStreamClientInterceptor):
    def __init__(self, value: str):
        self.value = value

    def _details(self, details):
        metadata = [(k, v) for k, v in (details.metadata or ()) if k != HEADER]
        metadata.append((HEADER, self.value))
        return _CallDetails(details.method, details.timeout, metadata, details.credentials,
                            getattr(details, "wait_for_ready", None),
                            getattr(details, "compression", None))

    def intercept_unary_unary(self, continuation, details, request):
        return continuation(self._details(details), request)

    def intercept_unary_stream(self, continuation, details, request):
        return continuation(self._details(details), request)

    def intercept_stream_unary(self, continuation, details, request_iterator):
        return continuation(self._details(details), request_iterator)

    def intercept_stream_stream(self, continuation, details, request_iterator):
        return continuation(self._details(details), request_iterator)


def default_versions() -> Mapping[str, str]:
    from algogators.versions import API_VERSIONS
    return API_VERSIONS


def versioned_channel(target: str, versions: Optional[Mapping[str, str]] = None) -> grpc.Channel:
    """An insecure channel whose calls all carry the version header."""
    value = format_header(versions if versions is not None else default_versions())
    return grpc.intercept_channel(grpc.insecure_channel(target), VersionHeaderInterceptor(value))
