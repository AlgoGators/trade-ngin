# desk service

The QT desk's command channel from AlgoLens to the engine. Decided 2026-10-08 (QT plan, section
3b, "gRPC command channel"). `docs/design/qt-contract.md` sections 4 and 6 are binding.

It is one service on the engine's shared gRPC server (`rpc.md`), which provides the port, the
version header check, health, call logs and shutdown. Before 2026-10-09 it ran as its own
`desk-agent` process; the code moved unchanged into `services/rpc/algogators_rpc/services/desk/`.

## Contract

`proto/algogators/desk.proto`, package `algogators.desk`, API `desk`, version in the file's
header block (`Version: 1.0.0`). Every call sends `x-algogators-api-version: desk=1.0.0` (any
`1.x.y` is accepted; see `rpc.md`). The engine owns the file. AlgoLens vendors it byte for byte
at a pinned trade-ngin commit and commits the stubs it generates.

Until AlgoLens moves over, the server also answers the old name `algogators.qt.v1.DeskService`
(same messages and field numbers, no header needed). That legacy name is removed in the next
release.

| RPC | Row kind (`audit_id`) | What the desk does |
|---|---|---|
| `GetRunStatus` | n/a | Reads model-run state for one book and day |
| `RunDesk` | `save` | Engine `--desk` |
| `RequestOverride` | `override_request` | In Python: token, 48 h expiry, e-mail to `QT_APPROVERS` |
| `RecordDecision` | `override_decision` (its `parent_id` is the request) | Checks; rejection -> `done {"approved":false}`; approval -> engine `--override` |
| `Publish` | `publish` (`audit_id = 0`: newest pending publish row for the day) | Engine `--publish` |
| `grpc.health.v1.Health/Check` | n/a | Liveness, for `""` and `algogators.desk.DeskService` (no version header) |

How replies work:

- Malformed input, such as a bad date, an `audit_id <= 0` or an empty reason, gets gRPC status
  `INVALID_ARGUMENT` on every RPC. That is a caller bug.
- A command the engine declines returns `status = COMMAND_STATUS_REFUSED` with a reason in
  `message`. A command that broke returns `COMMAND_STATUS_FAILED`, and can be retried with the
  same `audit_id`.
- No RPC reports success for work it did not do. A command whose row is pending is claimed
  (`UPDATE ... SET status='running' WHERE id=%s AND status='pending'`) and queued, and the reply
  is `ACCEPTED` at once (`RunDesk` also says `source = BOOK_SOURCE_DESK`). The outcome lands on
  the row; AlgoLens polls it. A row that is already running, done, refused or failed is never
  run again: the reply is `ACCEPTED` with its current status in `message`.
- A call that does not match its row (unknown id, another kind, another portfolio or date) is
  `REFUSED` and the row is left alone. A rejected decision answers `DONE`.
- `GetRunStatus` returns `UNAVAILABLE` when the database cannot answer; so do the commands.

### Commands

- **Engine jobs.** The desk maps `portfolio_id` to the directory under
  `$TRADING_CONFIG_DIR/portfolios/` (default `/app/config`) whose `portfolio.json` names it (none
  or several: the row is refused), then runs, in `QT_ENGINE_CWD` (default `/app`):

  ```
  flock $QT_LOCK_DIR/<portfolio_id>.lock $QT_ENGINE_BINARY --desk|--override|--publish \
        --portfolio-config <dir> --date YYYY-MM-DD --audit-id <row id>
  ```

  `QT_ENGINE_BINARY` defaults to `/app/build/bin/Release/live_portfolio_conservative` and
  `QT_LOCK_DIR` to `/tmp/qt-locks`. The daily cron model run takes the same lock. The binary
  writes the row's status, result, message and finished_at itself (exit 0 = done, 2 = refused,
  else failed). If the row is still `running` when it exits, the desk marks it `failed` with
  `engine exited <rc> without recording an outcome: <last stderr line>`. After
  `QT_JOB_TIMEOUT_S` (default 1800) the process group is killed and the row failed. The last 40
  lines of stdout and stderr go to the server log.
- **One job at a time per portfolio.** Each portfolio has a FIFO queue and a worker thread;
  different portfolios run concurrently.
- **Override e-mail.** `token = secrets.token_urlsafe(32)`; the row gets
  `token_hash = sha256(token)` and `token_expires_at = now() + 48 h`. The e-mail goes to
  `QT_APPROVERS="vp=<email>,president=<email>"` (both required, else the row fails) with the link
  `$QT_APPROVE_URL_BASE?token=<token>` (default `https://algolens.algogators.com/qt/approve`) and
  a per-symbol table: model book (system), desk request (qt_proposal), engine desk result (qt)
  and `moved_by`. It is sent through the portfolio's `email.json`. E-mail is disabled by
  `QT_EMAIL_DISABLED=1`, a missing `email.json`, `"enabled": false`, or empty or `YOUR_...`
  credentials; then the row's result carries the mail instead
  (`{"emailed":[], "email_disabled":true, "email":{"to","subject","body"}}`) so an operator can
  still approve. Sent: `{"emailed":["vp","president"]}`.
- **Decision checks** (the engine re-checks): the parent is an `override_request` for the same
  portfolio and date; the decider is not the requester (case-insensitive) ->
  `the requester may not approve their own request`; `approver_role` is vp or president; the
  parent's link had not expired when the decision row was created; `payload.approved` is a
  boolean.

### GetRunStatus

It reads `trading.live_run_metadata` (and `trading.live_results`) for `portfolio_id` and `date`.

| State | Condition |
|---|---|
| `NOT_STARTED` | No metadata row |
| `FAILED` | A row's `portfolio_config` has `strict_assertion` |
| `REFUSED` | A row's `portfolio_config` has `risk_refusal` (portfolio, sleeve or sizing hold; the reason goes in `message`) |
| `SUCCEEDED` | No mark, and the day's `live_results` row exists. A completed run writes that row last. |
| `RUNNING` | No mark and no `live_results` row yet. A run that died midway also reads as RUNNING; the watchdog is what alerts on that. |

These marks are the same ones `scripts/check_live_trading.py` reads (`run_metadata_marks.hpp`).

- `started_at` is the earliest `created_at` of the day's metadata rows.
- `finished_at` is the `created_at` of the day's `live_results` row.
- `published_by` and `published_at` come from columns that only arrive with the publish
  migration (plan E8). The desk checks `information_schema` on every call. Until those columns
  exist, `published` is false and the other two fields are unset.

## Idempotency and recovery

- Every mutating command carries `audit_id`, the `trading.position_overrides.id` row that
  AlgoLens writes before it calls. That table is both the log and the queue; there is no job
  table (ruling 26).
- A repeated call with the same `audit_id` must not repeat the work. It returns the outcome
  already recorded.
- On startup, rows still `running` are orphans of the previous server process: they go back to
  `pending` (message `re-driven after agent restart`). Then every pending row is dispatched by
  kind, oldest first, and the sweep repeats every `QT_REDRIVE_INTERVAL_S` (default 60 s), so a
  call dropped by a restart or a gRPC outage loses nothing (`services/desk/recovery.py`). The
  sweep is the desk's registered background task (`services/desk/service.py`): the startup pass
  is its `on_start` and the shared server runs the loop. The claim
  makes double dispatch impossible. The engine must therefore tolerate re-running a command
  whose earlier attempt died midway.
- Postgres stays the record. A reply is a convenience, never the record of anything.

## Deployment

- **One image, two entrypoints.** The default command (none) runs cron and the live runner,
  exactly as before. `command: rpc` runs the gRPC server with the desk on it
  (`scripts/docker-entrypoint.sh`; `desk-agent` is an alias for one release).
- **Container and port.** Container `engine-rpc`, listening on `0.0.0.0:50051` (`RPC_LISTEN`),
  plaintext, on the private Docker network `qt` shared with `algolens-backend`, with the network
  alias `desk-agent` for one release. No host port is published.
- **Compose.** See `deploy/engine-rpc.compose.yml`: `mem_limit 512m` (the engine runs inside
  the container), `./config:/app/config:ro`, `QT_APPROVERS` passed through, a `qt-locks` volume
  for the flock files, `restart: unless-stopped`, and a gRPC healthcheck in place of the image's
  cron healthcheck. The cron model run must take its lock in the same directory.
- **Database credentials.** The desk reads `DB_HOST`, `DB_PORT`, `DB_USER`, `DB_PASSWORD` and
  `DB_NAME`, the variables the watchdog reads and the entrypoint passes to cron. Without them it
  reads the `database` block of `$TRADING_CONFIG_DIR/defaults.json` (default `/app/config`), the
  same file the C++ runners load. If neither is complete, the server refuses to start. The
  password is never logged.
- **Logs.** One JSON object per line on stdout (`docker logs engine-rpc`). The approval token
  and the SMTP password are never logged.
- **Image.** The server is built in its own Docker stage (`rpc-build`): a venv installed from
  wheels only, with stubs generated by `services/rpc/gen.sh`. The runtime stage adds `python3`
  and copies `/opt/rpc`. The C++ build stage is unchanged.

## Calling it from AlgoLens

AlgoLens uses its shared client layer (`algolens/infrastructure/rpc/`), which caches one
channel per address and adds the version header. The equivalent by hand:

```python
from algogators import desk_pb2 as pb, desk_pb2_grpc
from algogators_rpc.client import versioned_channel     # adds x-algogators-api-version

channel = versioned_channel("engine-rpc:50051")          # the `qt` network only
desk = desk_pb2_grpc.DeskServiceStub(channel)

status = desk.GetRunStatus(pb.RunStatusRequest(portfolio_id="CONSERVATIVE_PORTFOLIO",
                                               date="2026-10-07"), timeout=5)
if status.state != pb.RUN_STATE_SUCCEEDED:
    ...  # D18: no desk window

# Commands: write the position_overrides row first, then call with its id.
reply = desk.RunDesk(pb.RunDeskRequest(portfolio_id="CONSERVATIVE_PORTFOLIO",
                                       date="2026-10-07", audit_id=row_id,
                                       requested_by=user_email), timeout=60)
```

Always set a deadline. If a call times out or comes back `UNAVAILABLE`, retry with the same
`audit_id`. Treat `REFUSED` as final. `FAILED_PRECONDITION` is a version mismatch: fix the
client.

From the host, `scripts/qt_grpc_call.py` makes one call the same way (`scripts/qt_e2e.sh` uses
it).

## Regenerating stubs

See `rpc.md`, "Local development". The stubs are build output: gitignored here, generated by
the Docker build and by CI. Never edit generated files.

## References

- Plan section 3b: `qt-platform-analysis-and-plan.md`.
- Decisions D1 to D22: `qt-platform-design.md`.
- Numbered rulings: the QT platform master document of 2026-10-08.

The proto's comments cite these. The first two files are not yet committed to this repo.
