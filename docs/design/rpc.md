# rpc: the engine's gRPC layer

One gRPC server in the trade-ngin image, shared by every AlgoGators service that talks to the
engine. The QT desk (`desk-service.md`) is the first service on it. This document covers what
all services share: layout, versioning, the header, ports, health, and how to add a service.

## Layout

| Path | What |
|---|---|
| `proto/algogators/<api>.proto` | One file per API, package `algogators.<api>`. No version in the path or package. |
| `proto/algogators/common.proto` | The conventions below, and the home of message types several APIs share (none yet). |
| `services/rpc/gen.sh` | Generates `algogators/<api>_pb2*.py` and `algogators/versions.py` (build output, gitignored). |
| `services/rpc/gen_versions.py` | Parses each .proto's version header into `algogators/versions.py`. |
| `services/rpc/algogators_rpc/` | The shared server: `server.py` (bootstrap, background tasks, shutdown), `registry.py`, `interceptors.py` (version check, call log), `versioning.py`, `client.py`, `config.py`, `log.py`, `healthcheck.py`. |
| `services/rpc/algogators_rpc/services/<name>/` | One package per service. `services/__init__.py` lists them. |
| `services/rpc/tests/` | Tests for the layer and every service. |
| `.github/workflows/rpc.yml` | Generates the stubs and runs the tests. |

## Versioning

The version of an API lives in the header block at the top of its .proto, and only there:

```proto
// AlgoGators gRPC API
// API: desk
// Version: 1.0.0
// Compatibility: a client must send x-algogators-api-version: desk=<major>.<minor>.<patch>; the
// server refuses a different major and accepts any minor/patch (docs/design/rpc.md).
```

- `API` equals the file name, and the package is `algogators.<API>`. `gen_versions.py` refuses
  anything else, so the stubs fail to build.
- Stub generation writes `algogators/versions.py`, e.g. `API_VERSIONS = {"common": "1.0.0",
  "desk": "1.0.0"}`. The server registers each service with `API_VERSIONS[api]` and clients
  send the same constant, so the header is the single source on both sides. AlgoLens vendors the
  .proto files byte for byte and generates the same `versions.py`.
- **Minor** (or patch): an older peer still works. Examples: a new field, a new RPC, a new enum
  value an old client can treat as unknown, a comment.
- **Major**: anything else. Examples: removing or renumbering a field, changing a field's type
  or meaning, removing an RPC. Field numbers are never reused.
- Every change bumps the version and adds a line to the file's "Version history" comment.

## The header at runtime

Every call carries gRPC metadata:

```
x-algogators-api-version: desk=1.0.0
```

A client that speaks several APIs sends them comma-separated (`common=1.0.0,desk=1.0.0`). Both
shared clients (trade-ngin `algogators_rpc.client.versioned_channel`, AlgoLens
`algolens.infrastructure.rpc`) send every API in their `versions.py`.

The server's `VersionInterceptor` finds the API of the called service in the registry and then:

| Header | Result |
|---|---|
| names the API with the server's major (any minor/patch) | served; the reply's initial metadata carries `x-algogators-api-version: desk=1.0.0` |
| missing | `FAILED_PRECONDITION`: `missing x-algogators-api-version metadata: this server speaks desk=1.0.0; send 'x-algogators-api-version: desk=<major>.<minor>.<patch>'` |
| another major | `FAILED_PRECONDITION`: `API version mismatch for 'desk': client speaks desk=2.0.0, server speaks desk=1.0.0; the major versions must match` |
| does not name the API, or malformed | `FAILED_PRECONDITION`, with the reason |

A refused call never reaches the servicer. `FAILED_PRECONDITION` means "fix the client"; do not
retry it.

### Exemptions

- **Health.** `grpc.health.v1.Health` keeps its standard name and needs no header. It is the one
  service outside `algogators.*` and it is not versioned by us: Docker healthchecks,
  `grpc_health_probe` and load balancers expect exactly that name.
- **Legacy names.** A service may list earlier full names (`Service.legacy_names`), served by
  the same servicer with no version check, for clients that predate a rename. The desk keeps
  `algogators.qt.v1.DeskService` until AlgoLens has moved to `algogators.desk.DeskService`; then
  it is deleted.

## Server

- **Bind.** `RPC_LISTEN`, default `0.0.0.0:50051`, plaintext. The container is `engine-rpc` on the
  private Docker network `qt`; no host port is published. The network alias `desk-agent` keeps
  the old address for one release.
- **Settings.** `RPC_MAX_WORKERS` (default 8), `RPC_HEALTH_LISTEN` (default
  `127.0.0.1:50052`; `off` disables it) and `RPC_SERVICES` (default `desk`).
  `DESK_AGENT_LISTEN` and `DESK_AGENT_MAX_WORKERS` are still read when the `RPC_` names are
  unset, for one release.
- **Entrypoint.** `command: rpc` (`scripts/docker-entrypoint.sh`); `desk-agent` is an alias for
  one release. Compose: `deploy/engine-rpc.compose.yml`.
- **Health.** `grpc.health.v1` reports `""` (the whole server) and each service's full name
  (and legacy names). Everything is `SERVING` once all services are registered and goes
  `NOT_SERVING` at shutdown. The same health service is also served alone on
  `RPC_HEALTH_LISTEN` (loopback, its own two threads), so calls that occupy every main worker
  never starve it. The container healthcheck, `python -m algogators_rpc.healthcheck`, dials that
  port.
- **Background tasks.** A service's `BackgroundTask`s run in daemon threads: `on_start` once
  before the server serves, then `run` every `interval_s`. Exceptions are logged and swallowed,
  so a task whose start hook failed (a database down at startup) must retry that work in `run`
  (the desk's recovery does). A long `run` (the desk's catch-up pass runs engines for minutes)
  delays only its own thread.
- **Processes.** Services may spawn processes (the desk runs the engine). Run the container
  with `init: true` so they are reaped, and never read a child's output into memory unbounded.
- **Logs.** One JSON object per line on stdout. Each call gets one `rpc call` line with
  `method`, `code`, `elapsed_ms` and the `api_version` header the client sent, never the request
  or reply. Health checks are not logged. Compose caps `docker logs` with json-file
  `max-size: 10m`, `max-file: 3` per service (never in the Docker daemon's config).
- **Startup.** Read settings. Each service in `RPC_SERVICES` registers itself; a service that
  cannot be configured stops the server (exit 2). Start the health-only server. Run every
  background task's `on_start`. Start the periodic loops. Serve.
- **Shutdown.** On SIGTERM or SIGINT: health goes `NOT_SERVING`, background loops stop, and
  in-flight calls get 10 s.

## Adding a service

1. Write `proto/algogators/<api>.proto` with the header block, `Version: 1.0.0`, package
   `algogators.<api>`. Shared message types go in `common.proto`.
2. Create `services/rpc/algogators_rpc/services/<api>/` with a servicer and a `register(registry,
   env)` that reads the service's own settings and calls:

   ```python
   from algogators import <api>_pb2, <api>_pb2_grpc
   from algogators.versions import API_VERSIONS
   from algogators_rpc.registry import BackgroundTask, Service

   registry.register(Service(
       api="<api>",
       version=API_VERSIONS["<api>"],
       descriptor=<api>_pb2.DESCRIPTOR.services_by_name["<Name>Service"],
       servicer=<Name>Servicer(...),
       add_to_server=<api>_pb2_grpc.add_<Name>ServiceServicer_to_server,
       background_tasks=(BackgroundTask("<api>.sweep", 60, run=..., on_start=...),),
   ))
   ```
3. Add it to `AVAILABLE` in `algogators_rpc/services/__init__.py`, and to the default
   `RPC_SERVICES` if it runs everywhere.
4. Add tests under `services/rpc/tests/`. Build clients with `versioned_channel`.
5. Clients (e.g. AlgoLens) vendor the new .proto byte for byte, regenerate their stubs and
   `versions.py`, and update their pinned commit.

Nothing in the shared layer or in other services changes.
`tests/test_rpc_layer.py::test_second_service_alongside_desk` shows a second service running
next to the desk.

## Local development

```sh
python3 -m venv .venv && .venv/bin/pip install -r services/rpc/requirements-dev.txt
PYTHON=.venv/bin/python services/rpc/gen.sh     # writes services/rpc/algogators/
cd services/rpc && ../../.venv/bin/python -m pytest -q
```
