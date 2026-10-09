"""algogators_rpc: the engine's shared gRPC server (docs/design/rpc.md).

One process, one port, many services. Each service (services/<name>/) registers itself with
the registry: its API name and version, servicer, background tasks. The shared layer adds what
every service needs: the version header check, structured call logs, grpc.health.v1 and a
graceful shutdown. The QT desk (services/desk/) is the first service.
"""
