"""Container healthcheck: exit 0 iff the local server answers grpc.health.v1 SERVING.

    python -m algogators_rpc.healthcheck [service full name]

With no argument it checks "" (the server as a whole, SERVING once every service registered).
It dials the health-only port (RPC_HEALTH_LISTEN, default 127.0.0.1:50052; server.py), or the
main port when that is "off".
"""

from __future__ import annotations

import os
import sys

import grpc
from grpc_health.v1 import health_pb2, health_pb2_grpc


def main(argv=None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    service = argv[0] if argv else ""
    health = (os.environ.get("RPC_HEALTH_LISTEN") or "127.0.0.1:50052").strip()
    if health.lower() == "off":
        health = (os.environ.get("RPC_LISTEN") or os.environ.get("DESK_AGENT_LISTEN")
                  or "0.0.0.0:50051")
    port = health.rsplit(":", 1)[-1]
    try:
        with grpc.insecure_channel(f"127.0.0.1:{port}") as channel:
            reply = health_pb2_grpc.HealthStub(channel).Check(
                health_pb2.HealthCheckRequest(service=service), timeout=3)
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
