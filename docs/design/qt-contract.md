# QT platform: engine ↔ AlgoLens contract (v1)

Status: binding for the 2026-10-09 build. The master document ("QT platform", 2026-10-08) is the source of truth. This file fixes the names both repos code against. Section 3b of `qt-platform-analysis-and-plan.md` explains the decisions behind it.

## 1. Where things run

| Piece | Where | Database | Notes |
|---|---|---|---|
| Live trading cron (`trade-ngin-trade-ngin-1`) | trade-ngin host | `algo_data` | **Never touched by QT work.** It runs the pre-stage-3 image at 09:30. |
| `qt-engine` container (trade-ngin image built from main) | trade-ngin host, Docker network `qt` | `new_algo_data` | Container `engine-rpc` (network alias `desk-agent` for one release): the engine's gRPC server (`docs/design/rpc.md`, :50051, not published) with the desk service on it. The desk service also runs the QT model runs, the 09:30 send and the 10:00 fallback (the scheduler, C6 and C7); the host cron is only a watchdog. |
| AlgoLens (edge, frontend, backend) | trade-ngin host, backend also on network `qt` | `new_algo_data` | The backend calls `engine-rpc:50051` (env `ENGINE_RPC_ADDR`). |

## 2. Portfolios

| portfolio_id | Desk editing | Purpose |
|---|---|---|
| `QT_CONSERVATIVE_PORTFOLIO` | on | The desk's book. Runs the conservative strategy and capital. Its first day has no prior book (ruling 27). |
| `QT_CONSERVATIVE_MODEL_PORTFOLIO` | off | The untouched model twin (ruling 19). Same strategy and capital, run by the same cron. It never gets qt_proposal rows. |

AlgoLens groups the two with the `strategy_registry.portfolio_group` column, value `qt_conservative`. Desk editing is gated by `strategy_registry.desk_editable` (boolean).

## 3. Books (`portfolio_type`)

`system`, `qt_proposal` and `qt` (migration 021).
- **The model run** writes `system`. On a desk-editable portfolio it also seeds `qt_proposal`, per symbol, as a copy of system. It writes `qt` as a copy of system too: positions, executions, live_results and equity_curve, with `live_results.book_source='model'`.
- **The model run's starting book** is yesterday's `qt` book. If the portfolio has never had a `qt` book, it starts from its own `system` book (the first desk day, ruling 27). After that, a missing `qt` book for yesterday makes the run refuse.
- **On a non-editable portfolio,** only `system` is written, and the run starts from `system`.
- **Each book is finalised into its own rows** (ruling 17).

## 4. Command log: `trading.position_overrides` (migration 023 rebuilds it; it has 0 rows today)

```sql
id            bigserial primary key
portfolio_id  text not null
date          date not null                       -- the book date being edited/published
kind          text not null check (kind in ('save','override_request','override_decision','publish'))
status        text not null default 'pending' check (status in ('pending','running','done','refused','failed'))
requested_by  text not null                       -- AlgoLens user email
reason        text                                -- required (non-blank) for save and override_request
payload       jsonb not null default '{}'         -- save: {"changes":[{"symbol":"ES","from":3,"to":1}]}
                                                   -- override_decision: {"approved":true}
parent_id     bigint references trading.position_overrides(id)   -- decision -> its override_request
approver_role text check (approver_role in ('vp','president'))   -- override_decision only
token_hash    text                                -- override_request: sha256 hex of the one-time link token
token_expires_at timestamptz
result        jsonb                               -- written by the engine (see §6)
message       text                                -- engine's human-readable outcome / refusal reason
created_at    timestamptz not null default now()
started_at    timestamptz
finished_at   timestamptz
```

Constraints and indexes:
- index (portfolio_id, date, kind)
- a CHECK that `reason` is non-blank for `save` and `override_request`
- a unique partial index allowing one `done` publish per (portfolio_id, date)
- 025 (C2): unique partial indexes for one `override_decision` per `parent_id` while pending, running or done; one open (pending or running) `publish` per (portfolio_id, date); one open `override_request` per (portfolio_id, date)

**Who writes what:**
- AlgoLens inserts the rows. An insert must be `pending` with `started_at`, `finished_at`, `result`, `message`, `token_hash` and `token_expires_at` NULL (025 trigger).
- The engine (the desk service on `engine-rpc`, and the binary it runs) moves `status`, `started_at`, `finished_at`, `result`, `message` and the token columns.
- Nobody deletes or truncates rows (triggers). Columns other than those are never updated (trigger).
- Status moves only as C4 says, and terminal rows are final.

## 5. Other schema changes

- **021** (E1): book columns, and `positions.moved_by text`, which names the step that moved a symbol. Its values are `cap`, `overlay`, `sign_close`, `hold`, `search`, `buffer`, `rounding`, `clip`, `trim` or `none`.
- **022** (E2):
  - `trading.strategy_config`, the desk's saved settings.
  - On `live_run_metadata`: `settings_used jsonb`, `published_by text` and `published_at timestamptz`.
- **023** (this contract):
  - Rebuild `position_overrides` as in §4.
  - Add `live_results.book_source text check in ('model','desk','override')`.
  - Add `strategy_registry.portfolio_group text` and `strategy_registry.desk_editable boolean not null default false`. (The engine never reads `strategy_registry`. It ships here only because trade-ngin owns the migration chain.)
- **024**: drop `trading.risk_limits`, `trading.portfolios`, `trading.strategy_book_memberships` and `trading.portfolio_assignments` (ruling 28). It is applied separately and is irreversible; the pre-QT backup on the host holds them. First confirm that AlgoLens main does not reference them.
  - Register the two QT portfolios in `strategy_registry`.

## 6. gRPC (`proto/algogators/desk.proto`, API `desk`)

The desk is one service on the engine's shared gRPC layer (`docs/design/rpc.md`): service `algogators.desk.DeskService` at `engine-rpc:50051`. Every call carries the metadata `x-algogators-api-version: desk=<version>`, where the version comes from the header block of `desk.proto` (now `1.1.0`: C7 added `publish_source` and `sent_at` to RunStatus). The server refuses a missing header or a different major version with `FAILED_PRECONDITION` and accepts any minor or patch. AlgoLens vendors `desk.proto` byte for byte at a pinned commit and sends the header from its shared client layer. The pre-versioning name `algogators.qt.v1.DeskService` is still served, with the same wire format and no header, until AlgoLens has switched; it is removed in the next release.

AlgoLens always inserts the `position_overrides` row first, then calls the RPC with its `audit_id`. The RPCs return `ACCEPTED` immediately: the engine works asynchronously, and AlgoLens polls the row (`status`, `result`) every 2 s. If gRPC is down, the row stays `pending`, and the desk service re-drives pending rows on startup and every 60 s. A row left `running` with no live job (a restart, a database error mid-command) goes back to `pending` and runs again (C4), so the engine must tolerate re-running a command whose earlier attempt died midway. gRPC is the channel AlgoLens uses to call the engine (decided 2026-10-09; it amends the master document's "never call each other"). The re-drive guarantees a dropped call loses nothing.

| RPC | Engine action | `result` written |
|---|---|---|
| RunDesk(audit_id) | Refuse if there is no qt_proposal for the portfolio+date (ruling 14). Otherwise run the one pass on the whole qt_proposal book and write qt (`book_source='desk'`, `moved_by` per symbol). Never e-mails (ruling 15). | `{"symbols":[{"symbol","asked","given","moved_by"}], "book_source":"desk"}` |
| RequestOverride(audit_id) | Generate a token, e-mail the VP and the President the link `https://algolens.algogators.com/qt/approve?token=<token>`, then store its sha256 in `token_hash` with a 48 h expiry and the result in one UPDATE (C1). The e-mail shows the model book next to the desk request. | `{"emailed":["vp","president"],"emailed_at":"<UTC ISO>"}` |
| RecordDecision(audit_id of the decision row) | Refused when the request is not `done`, its link had expired, it already has a done (or running) decision, or the day is published (C2, C3; the engine re-checks, plus the C1 snapshot hash). On approval: write qt = qt_proposal exactly (`book_source='override'`), with the one-pass run kept as a report in the qt live_results `risk_detail`. On rejection: nothing. | `{"approved":bool}` |
| Publish(audit_id) | The desk's approval ("Approve" in AlgoLens; C7). Freeze the qt book and set `live_run_metadata.published_by/published_at` and `publish_source='desk'`. Before 09:30 New York nothing is sent (the 09:30 send e-mails the day); from 09:30 to 10:00 the daily e-mail and CSV, built from the stored qt rows, go at once and `sent_at` is set. Refused when the row was created at or after 10:00 New York on its date, when the day is published, or when a previous day is unpublished or failed (ruling 29). | `{"published":true,"publish_source":"desk","emailed":bool}` (+ `published_at`, `email_sent_at`) |
| GetRunStatus(portfolio_id, date) | Model-run state for the desk page header; since 1.1.0 also `publish_source` and `sent_at` (approved, sent, fallback). | n/a |

Approver addresses come from the env var `QT_APPROVERS="vp=<email>,president=<email>"`, set on both the qt-engine container and AlgoLens (host env files, not the repo). The requester may not approve their own request: AlgoLens refuses it, and the engine re-checks.

## 7. AlgoLens rules (rulings 13, 14, 16, 20, 21, 22)

- **Opening state:** AlgoLens opens on the `qt` book. A portfolio switcher, grouped by `portfolio_group`, sits above everything; inside a portfolio the toggle flips system / qt_proposal / qt.
- **What can be edited:** quantity only, and only on futures portfolios with `desk_editable`. Zero means flatten, and is stored as a zero-quantity row. A new symbol takes its price from market data and is picked per asset class from `metadata.contract_metadata`. A reason is required.
- **Saving:** a save upserts the changed symbols into qt_proposal for the latest seeded date, inserts the `save` row, and calls RunDesk. If no qt_proposal rows exist for that portfolio and date, the save gets a 409.
- **The asked-vs-returned view:** qt_proposal against qt, per symbol, with `moved_by`.
- **Override:** a "Request override" button with a reason. The approval page sits behind login and shows both books.
- **Publish:** ~~a button, enabled whatever the edit state~~ the **Approve** button (C7): enabled whatever the edit state until 10:00 New York, refused after it and on a published day. The state comes from `live_run_metadata` (`published_at`, `publish_source`, `sent_at`).

## 8. Hardening amendments (2026-10-09)

The four QT reviews were triaged into these changes. They amend the sections above, and both repos code against them.

### C1. Override snapshot

- When AlgoLens inserts an `override_request`, it snapshots the qt_proposal book for (portfolio_id, date) in the same transaction, with the proposal rows locked FOR SHARE, into `payload`: `{"proposal": [{"strategy_name": s, "symbol": y, "quantity": q}, ...], "proposal_sha256": h}`. The list holds every qt_proposal row of the day (zero rows included), sorted by (strategy_name, symbol), with `quantity` an integer. `h` is the sha256 hex of `json.dumps(list, separators=(",", ":"), ensure_ascii=True)`; the engine reproduces the same bytes.
- On an approved decision the engine recomputes the hash from the current qt_proposal rows and refuses when it differs ("the proposal changed after the override was requested; request a new override"). AlgoLens runs the same check before inserting a decision and returns 409. The approval page shows the snapshot.
- **Token in the result.** The request row's `result` carries the plaintext token or link only when `QT_EMAIL_DISABLED=1` is set explicitly on `engine-rpc`. Any other e-mail misconfiguration (no `email.json`, `"enabled": false`, missing or `YOUR_...` credentials, no host) fails the row with the reason, and no token is stored.
- **Sent once.** The desk service sends the mail, then stores `token_hash`, `token_expires_at` and `result.emailed_at` in one UPDATE, then marks the row done. A re-driven request whose `token_hash` is set is marked done ("already e-mailed; not sent again") with no new token and no second mail.

### C2. One decision per request, one open publish, one open override request per day

- Migration 025's unique partial indexes (section 4). "Done, not yet decided and not expired" cannot be an index; AlgoLens enforces it under its lock.
- The desk service and the engine refuse a decision whose request is not `done`, already has a `done` decision, had an expired link when the decision was inserted, or whose day is published. The desk service also refuses while another decision of the request is `running`, and checks decisions one at a time, so two concurrent decisions never both run.
- AlgoLens maps a unique violation to 409.

### C3. Published days are frozen

- A day is published when `live_run_metadata.published_at IS NOT NULL` for (portfolio_id, date): by the desk's approval, by the 10:00 fallback or the catch-up of a past day (C7), or, on the model twin, as `system:model-only`. (~~including the non-trading days the engine publishes itself~~: amended by C7, the engine no longer does.)
- On a published day AlgoLens refuses save, override request, decision and publish, reading that state from live_run_metadata. The desk service refuses save, override request and decision rows for it before queueing. It leaves publish rows to the engine, which refuses `--desk`, `--override` and a second `--publish`. A model re-run does not rewrite qt or qt_proposal for that date.

### C4. Command row life cycle

- An insert is `pending` with the engine columns NULL (025 insert trigger).
- Allowed status moves: pending → running; running → done, refused or failed; running → pending only inside a transaction that set `algogators.recovery = 'on'`, which only the desk service's recovery of a stale or orphaned row does. Terminal rows are final: any UPDATE of one is refused. To retry, AlgoLens inserts a new row.
- TRUNCATE is refused by a statement trigger; DELETE already was (023).
- The engine's finish UPDATE carries `AND status IN ('pending','running')`.
- The desk service puts a `running` row back to `pending` when no job of its process owns it and the row is an orphan of an earlier process (at startup, retried until the database answers), was abandoned by this process after a database error, or is older than `QT_JOB_TIMEOUT_S` + `QT_STALE_GRACE_S` (1800 + 300 s). An RPC retry of such a row re-drives it at once.

### C5. Non-trading day means the calendar only

~~Auto-publish (`system:non-trading-day`) applies only when the runner's holiday and weekend calendar says T-1 was closed.~~ **Amended by C7:** the model run publishes no day; a weekend or holiday book is approved or falls back at 10:00 like any other. A feed hole (no T-1 prices on a calendar trading day) still refuses with QT_ALERT and a non-zero exit (ruling 29).

### C6. Catch-up scheduler and the weekend chain

- The single 10:15 cron run is replaced by the catch-up scheduler, a background task of the desk service on `engine-rpc` (`services/desk/catchup.py`). ~~A pass is due every 30 min from 06:00 to 22:00 New York time, every day. It holds a scheduler flock and, per QT portfolio, walks from the day after the newest book (qt on a desk-editable portfolio, else system) up to today. It stops at the first trading day that is not yet published (the desk publishes it first); otherwise it runs the model run for the next day (non-trading days are auto-published per C5) and continues. Today's run on a trading day waits until 10:15 for T-1 data. After the desk publishes Saturday's book on Monday morning, Sunday and Monday run in the next pass.~~ **Amended by C7:** the walk, the times and the sends are C7's.
- Each model run is the command line of `scripts/qt_model_run.sh`, under the per-portfolio flock the desk jobs take.
- A refusal or failure (anything but "waiting for the approval") raises an alert: an e-mail to the President address of `QT_APPROVERS` through the portfolio's `email.json` (not when `QT_EMAIL_DISABLED=1`), at most once per (portfolio, date, reason) per day, and a line in the catch-up log on the host (`/home/ubuntu/qt-engine/logs`, rotated daily into dated files). ~~A trading day still unpublished 24 h after its model run gets a daily reminder the same way.~~ (C7: the reminder is gone; the fallback publishes every day.)
- The host cron is a watchdog: it runs a pass through the same code when none ran in the last 75 minutes (15 minutes from 06:30 to 10:30, C7); it fires every 5 minutes.

### C7. The daily cutoff (decided by Dom 2026-10-09; amends rulings 18 and 29, C5 and C6)

Every calendar day D, weekends and holidays included, America/New_York (DST resolved by the zone):

| Time | What happens |
|---|---|
| from 06:45 | The model run for D (bars through D-1), retried every 15 min until it is done; it writes system, seeds qt_proposal and copies qt (`book_source='model'`), and publishes nothing. No model book at 08:30: an alert to the President (`model_not_done`). |
| until 09:30 | The desk edits (save), requests overrides, and approves. **Approval** is the Publish command (`kind='publish'`): it freezes the day (C3) and sets `published_by`, `published_at` and `publish_source='desk'`. Before 09:30 it sends nothing. |
| 09:30 | **Send:** every approved, unsent day D is e-mailed (daily e-mail and CSV from the stored rows) and `sent_at` is set, once. An approval from 09:30 to 10:00 is sent at once. |
| 10:00 | **Fallback:** a day D not approved is reset to the model's book (qt positions, executions, live_results and equity copied again from system, `book_source='model'`; an unapproved desk save or override is discarded), published (`published_by='system:fallback-10am'`, `publish_source='fallback'`) and sent at once; the President is alerted ("QT did not approve <portfolio> <D>; the model's book was sent at 10:00"). |
| after 10:00 | Approving D is refused (the row's `created_at` is at or after 10:00 New York on D); saving, override requests and decisions are refused because D is published. |

- **Rows.** The fallback is a `position_overrides` row of kind `publish`, `requested_by` `system:fallback-10am` (today) or `system:fallback-catchup` (a past day), inserted `pending` and moved to `running` in one transaction by the scheduler, then finished by the engine (`--fallback`). AlgoLens detects a fallback by `publish_source='fallback'`, or by `published_by` starting `system:fallback`.
- **Missed past days** (the engine was down): a day D < today is run by the model and published with the model's book as `system:fallback-catchup`, `publish_source='fallback'`; it is never e-mailed (`sent_at` NULL, `result.send_skipped='past day'`), and one alert is raised. An approved but unsent past day is not e-mailed either. The chain rule (ruling 29) still holds: every day is published by 10:00.
- **The model twin** (`desk_editable=false`) is never approved: each day its model run writes is published as `system:model-only` (`publish_source='model-only'`) and, today, sent at 09:30 when its `email.json` names recipients (`to_emails`); give it `to_emails: []` to stop that.
- **Schema** (migration 027): `live_run_metadata.publish_source text CHECK (publish_source IN ('desk','fallback','model-only'))` and `sent_at timestamptz`, both NULL for days published before 027. svc_trade_ngin reads and writes them; svc_algolens reads them only.
- **Engine modes:** `--publish --audit-id N [--send-now]` (approve; with `--send-now` also send), `--fallback --audit-id N [--send-now]` (reset, publish, send for today), `--send --date D` (no row: e-mail a published day from its stored rows; a day with `sent_at`, or a publish row's `email_sent_at`, is never sent again). Published days stay frozen and a partial book is never done (items 3 and 5).
- **Desk service:** the Publish RPC decides by the clock (`services/desk/cutoff.py`): `--publish` before 09:30, `--publish --send-now` from 09:30, refused from 10:00 (by the row's `created_at`) or on a published day. The scheduler (C6) passes every 5 min from 06:30 to 10:30, then every 30 min to 22:00; its steps are decided from the database and the clock, so a late pass (10:07) still does the fallback.
- **GetRunStatus** (desk.proto 1.1.0) adds `publish_source` and `sent_at`: approved = `publish_source` desk, sent = `sent_at` set, fallback = `publish_source` fallback.
