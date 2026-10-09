"""Container healthcheck: exit 0 iff the local agent answers grpc.health.v1 SERVING."""

from __future__ import annotations

import os
import sys

import grpc
from grpc_health.v1 import health_pb2, health_pb2_grpc

from . import SERVICE_NAME


def main() -> int:
    port = os.environ.get("DESK_AGENT_LISTEN", "0.0.0.0:50051").rsplit(":", 1)[-1]
    try:
        with grpc.insecure_channel(f"127.0.0.1:{port}") as channel:
            reply = health_pb2_grpc.HealthStub(channel).Check(
                health_pb2.HealthCheckRequest(service=SERVICE_NAME), timeout=3)
    except grpc.RpcError as exc:
        print(f"unhealthy: {exc.code().name}", file=sys.stderr)
        return 1
    if reply.status != health_pb2.HealthCheckResponse.SERVING:
        print(f"unhealthy: {health_pb2.HealthCheckResponse.ServingStatus.Name(reply.status)}",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
