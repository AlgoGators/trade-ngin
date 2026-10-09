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
  `message`. A command that broke returns `COMMAND_STATUS_FAILED`. Both are final: the row never
  runs again, and to try again AlgoLens inserts a new row (contract C4). Repeating a call with
  the same `audit_id` (after a timeout or `UNAVAILABLE`) is always safe.
- No RPC reports success for work it did not do. A command whose row is pending is claimed
  (`UPDATE ... SET status='running' WHERE id=%s AND status='pending'`) and queued, and the reply
  is `ACCEPTED` at once (`RunDesk` also says `source = BOOK_SOURCE_DESK`). The outcome lands on
  the row; AlgoLens polls it. A row that is done, refused or failed is never run again: the
  reply is `ACCEPTED` with its current status in `message`. A row that is running is left alone
  while a job of this process owns it or it may belong to an engine started by hand; a
  `running` row nobody is working on is put back to `pending` and run (see "Idempotency and
  recovery").
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
  `QT_LOCK_DIR` to `/tmp/qt-locks`. The catch-up scheduler's model runs take the same lock. The binary
  writes the row's status, result, message and finished_at itself (exit 0 = done, 2 = refused,
  else failed). If the row is still `running` when it exits, the desk marks it `failed` with
  `engine exited <rc> without recording an outcome: <last stderr line>`. After
  `QT_JOB_TIMEOUT_S` (default 1800) the process group is killed and the row failed. The
  process's stdout and stderr go to temporary files, never to memory; the last 40 lines of each
  (read from the last 64 KiB) go to the server log.
- **One job at a time per portfolio.** Each portfolio has a FIFO queue and a worker thread;
  different portfolios run concurrently.
- **Override e-mail** (contract C1). `token = secrets.token_urlsafe(32)`. The e-mail goes to
  `QT_APPROVERS="vp=<email>,president=<email>"` (both required, else the row fails) with the link
  `$QT_APPROVE_URL_BASE?token=<token>` (default `https://algolens.algogators.com/qt/approve`) and
  a per-symbol table: model book (system), desk request (qt_proposal), engine desk result (qt)
  and `moved_by`. It is sent through the portfolio's `email.json`. Then one UPDATE stores
  `token_hash = sha256(token)`, `token_expires_at = now() + 48 h` and the result
  `{"emailed":["vp","president"], "emailed_at":"<UTC ISO>"}`, and only then is the row marked
  done. A request re-driven with `token_hash` already set is marked done
  `already e-mailed; not sent again`: no new token, no second mail. A crash between the send and
  that UPDATE leaves no token (the mailed link cannot work) and the re-drive mails a fresh one.
- **E-mail disabled.** Only `QT_EMAIL_DISABLED=1` disables the mail. Then the row's result
  carries it, link and token included
  (`{"emailed":[], "email_disabled":true, "email":{"to","subject","body"}}`), so an operator can
  still approve. Any other problem (no `email.json`, `"enabled": false`, empty or `YOUR_...`
  credentials, no `smtp_host`) fails the row with `override e-mail not sent: <why>. ...` and
  stores no token.
- **Published days** (contract C3). A `save`, `override_request` or `override_decision` row for
  a day whose `live_run_metadata.published_at` is set is refused before anything runs
  (`... was published at ...; a published day is frozen`). `publish` rows go to the engine,
  which refuses a second publish.
- **Decision checks** (contract C2; the engine re-checks): the parent is an `override_request`
  for the same portfolio and date; the decider is not the requester (case-insensitive) ->
  `the requester may not approve their own request`; `approver_role` is vp or president;
  `payload.approved` is a boolean; the request has its token and is `done`; its link had not
  expired when the decision row was created; no other decision of the request is `done`
  (`override request N was already decided (decision M)`) or `running`
  (`... is being decided by decision M`); the day is not published. Decisions are claimed and
  checked one at a time, so of two concurrent decisions on one request only one can run.
  Migration 025 also allows only one pending, running or done decision per request.

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
- **Status moves** (contract C4, migration 025): pending -> running (the claim); running ->
  done, refused or failed (the outcome); running -> pending only by this service's recovery,
  inside a transaction that set `algogators.recovery = 'on'`. Terminal rows are final.
- **Recovery** (`services/desk/recovery.py`, the background task `desk.redrive`, at startup and
  every `QT_REDRIVE_INTERVAL_S`, default 60 s):
  1. Startup: every `running` row no job of this process owns is an orphan of the previous
     process and goes back to `pending` (`re-driven after agent restart`). If the database is
     down at startup, every later pass retries this first, and nothing else runs until it has
     succeeded.
  2. Stale rows: a `running` row no job here owns goes back to `pending`
     (`re-driven: stale running row with no live job`) when this process abandoned it (a
     database error after the claim, or when the outcome could not be written) or when it is
     older than `QT_JOB_TIMEOUT_S` + `QT_STALE_GRACE_S` (1800 + 300 s, by the database clock).
     The age limit spares an engine run started by hand with `--audit-id`.
  3. Every pending row is dispatched by kind, oldest first, so a call dropped by a restart or a
     gRPC outage loses nothing.
- An RPC on a `running` row that step 1 or 2 would requeue requeues it and runs it at once, so
  a retry after a database error is not answered "already running" forever.
- The claim makes double dispatch impossible. The engine must therefore tolerate re-running a
  command whose earlier attempt died midway.

## Catch-up scheduler (contract C6)

The QT model runs are scheduled by the desk service itself: the background task `desk.catchup`
(`services/desk/catchup.py`), not a host cron. It already has the database, the SMTP settings
and the locks.

- **When.** The task ticks every minute; a pass is due once per `QT_CATCHUP_EVERY_MIN` (30)
  slot inside `QT_CATCHUP_WINDOW` (`06:00-22:00`), America/New_York, every day. A pass holds
  `$QT_LOCK_DIR/qt-catchup.lock`; a second pass (the watchdog) skips while one runs.
- **What.** For each config directory in `QT_CATCHUP_PORTFOLIOS`
  (`qt_conservative,qt_conservative_model`): `last` is the newest date with a book in
  `trading.live_results` (`qt` on a desk-editable portfolio, `system` otherwise; a desk-editable
  portfolio with no qt book yet starts after its newest system day, ruling 27). While
  `last < today`:
  - desk-editable and `last` unpublished: on a trading day, wait for the desk's publish (no
    alert); on a non-trading day, alert (the model run should have auto-published it);
  - the next day is today, a trading day, and it is before `QT_CATCHUP_TODAY_NOT_BEFORE`
    (10:15, the old cron time, for T-1 data): wait;
  - otherwise run the model run for the next day; exit 0 with the book written moves on, and
    anything else is an alert and ends this portfolio's walk until the next pass.

  A trading day is a book date whose T-1 is an open session on the runner's calendar
  (weekends and `holidays.json`, found as HolidayChecker finds it, from `QT_ENGINE_CWD`). A
  calendar that does not cover the dates is an alert.
- **How.** Exactly `scripts/qt_model_run.sh`'s command,
  `$QT_ENGINE_BINARY --portfolio-config <dir> --date <day>` in `QT_ENGINE_CWD`, bounded by
  `QT_JOB_TIMEOUT_S`. The scheduler takes `$QT_LOCK_DIR/<portfolio_id>.lock` itself (flock(2),
  the lock flock(1) takes for desk jobs; it waits up to `QT_CATCHUP_LOCK_WAIT_S`, 600 s, then
  skips the portfolio until the next pass) and re-reads the database under it, so a day another
  run has just written is never run twice.
- **Alerts.** A refusal, failure, timeout, missing book, calendar or configuration problem
  writes `ALERT <portfolio> <date> <reason>: <message>` to `$QT_LOG_DIR/qt-catchup.log` and
  e-mails the president of `QT_APPROVERS` through the portfolio's `email.json` (not when
  `QT_EMAIL_DISABLED=1` or the SMTP settings are unusable; the log line says why). At most one
  alert per (portfolio, date, reason) per New York day (`$QT_LOG_DIR/qt-alerts.json`); the run
  is still retried every pass. A trading day unpublished more than `QT_UNPUBLISHED_REMINDER_H`
  (24) hours after its model run gets the `unpublished_reminder` alert, daily.
- **Logs.** `$QT_LOG_DIR` (default `/var/log/qt-engine`, the host's
  `/home/ubuntu/qt-engine/logs`): `qt-catchup.log` (passes, decisions, alerts),
  `qt-engine-runs.log` (the full output of every model run), `qt-catchup.heartbeat` (the last
  pass) and `qt-alerts.json`. `deploy/qt-engine.logrotate` rotates them daily into dated files.
- **Watchdog.** `deploy/qt-engine.cron` runs
  `docker exec engine-rpc /app/scripts/qt_catchup.sh watchdog` at :07 and :37. Inside the
  window, if the heartbeat is older than 75 minutes, it runs one pass through the same code and
  locks. `qt_catchup.sh run-once` runs a pass by hand at any hour.
- `QT_CATCHUP_ENABLED=0` turns the scheduler off (then nothing runs the model).
- Postgres stays the record. A reply is a convenience, never the record of anything.

## Deployment

- **One image, two entrypoints.** The default command (none) runs cron and the live runner,
  exactly as before. `command: rpc` runs the gRPC server with the desk on it
  (`scripts/docker-entrypoint.sh`; `desk-agent` is an alias for one release).
- **Container and port.** Container `engine-rpc`, listening on `0.0.0.0:50051` (`RPC_LISTEN`),
  plaintext, on the private Docker network `qt` shared with `algolens-backend`, with the network
  alias `desk-agent` for one release. No host port is published.
- **Compose.** See `deploy/engine-rpc.compose.yml`: `init: true` (tini reaps the engine and
  flock children), `mem_limit 512m` (the engine runs inside the container),
  `./config:/app/config:ro`, `QT_APPROVERS` passed through, a `qt-locks` volume for the flock
  files, `./logs:/var/log/qt-engine` for the catch-up logs, json-file log limits (10 MB x 3;
  never the Docker daemon's config), `restart: unless-stopped`, and a gRPC healthcheck in place
  of the image's cron healthcheck.
- **Host files.** `deploy/qt-engine.cron` -> `/etc/cron.d/algogators-qt` (the watchdog; it
  replaces the 10:15 model run) and `deploy/qt-engine.logrotate` ->
  `/etc/logrotate.d/algogators-qt`.
- **Health.** The container healthcheck dials a health-only port (`RPC_HEALTH_LISTEN`, default
  `127.0.0.1:50052`) served by its own two threads, so slow calls on the main pool
  (`RPC_MAX_WORKERS`, default 8) never fail it. Every database connection of the desk has a 5 s
  connect timeout and a 30 s statement timeout.
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
`audit_id`. Treat `REFUSED` and `FAILED` as final; insert a new row to try again. `FAILED_PRECONDITION` is a version mismatch: fix the
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
