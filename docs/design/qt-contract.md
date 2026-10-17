# QT platform: engine ↔ AlgoLens contract (v1)

Status: binding for the 2026-10-09 build. The master document ("QT platform", 2026-10-08) is the source of truth. This file fixes the names both repos code against. Section 3b of `qt-platform-analysis-and-plan.md` explains the decisions behind it.

## 1. Where things run

| Piece | Where | Database | Notes |
|---|---|---|---|
| Old live trading cron (`trade-ngin-trade-ngin-1`) | trade-ngin host | `algo_data` | **Never touched by QT work, and being retired (D-4, 2026-10-16).** It runs a pre-stage-3 image and still e-mails daily; `algo_data` is no longer used. Once the model book (`CONSERVATIVE_PORTFOLIO`) runs daily on `new_algo_data` with the current engine, this cron is stopped. |
| `qt-engine` container (trade-ngin image built from main) | trade-ngin host, Docker network `qt` | `new_algo_data` | Container `engine-rpc` (network alias `desk-agent` for one release): the engine's gRPC server (`docs/design/rpc.md`, :50051, not published) with the desk service on it. The desk service also runs the QT model runs, the 09:30 send and the 10:00 fallback (the scheduler, C6 and C7); the host cron is only a watchdog. |
| AlgoLens (edge, frontend, backend) | trade-ngin host, backend also on network `qt` | `new_algo_data` | The backend calls `engine-rpc:50051` (env `ENGINE_RPC_ADDR`). |

## 2. Portfolios

| portfolio_id | Desk editing | Purpose |
|---|---|---|
| `QT_CONSERVATIVE_PORTFOLIO` | on | The desk's book (the QT book). Runs the conservative strategy and capital. It starts from the model book (`CONSERVATIVE_PORTFOLIO`): the model book's history up to S-1 is copied into it, and day S takes the model book's S-1 positions and equity (D-2, 2026-10-16; replaces ruling 27). |
| `QT_CONSERVATIVE_MODEL_PORTFOLIO` | off | **Retired (D-1, 2026-10-16).** The model twin (ruling 19) is no longer run: not scheduled, not e-mailed, `strategy_registry.is_active = false`. Its rows stay. The desk's book is compared with the desk portfolio's own model book (`system`) instead (D-3). |

AlgoLens groups the QT portfolios with the `strategy_registry.portfolio_group` column, value `qt_conservative`; the desk's value is drawn as the QT book against its own model book (`system`, D-3). Desk editing is gated by `strategy_registry.desk_editable` (boolean).

## 3. Books (`portfolio_type`)

`system`, `qt_proposal` and `qt` (migration 021).
- **The model run** writes `system`. On a desk-editable portfolio it also seeds `qt_proposal`, per symbol, as a copy of system. It writes `qt` as a copy of system too: positions, executions, live_results and equity_curve, with `live_results.book_source='model'`.
- **The model run's starting book** is yesterday's `qt` book. If the portfolio has never had a `qt` book, it starts from its own `system` book (the first desk day, ruling 27). After that, a missing `qt` book for yesterday makes the run refuse.
- **On a non-editable portfolio,** only `system` is written, and the run starts from `system`.
- **Each book is finalised into its own rows** (ruling 17).

## 4. Command log: `trading.position_overrides` (migration 023 rebuilt it; kind `rescind` since 029)

```sql
id            bigserial primary key
portfolio_id  text not null
date          date not null                       -- the book date being edited/published
kind          text not null check (kind in ('save','override_request','override_decision','publish','rescind'))
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
- ~~a unique partial index allowing one `done` publish per (portfolio_id, date)~~: dropped by 029 (a rescinded approval leaves its row done, C8; a second approval of a published day is refused by `published_at`, C3)
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

The desk is one service on the engine's shared gRPC layer (`docs/design/rpc.md`): service `algogators.desk.DeskService` at `engine-rpc:50051`. Every call carries the metadata `x-algogators-api-version: desk=<version>`, where the version comes from the header block of `desk.proto` (now `1.6.0`: C7 added `publish_source` and `sent_at` to RunStatus in 1.1.0, C8 added Rescind in 1.2.0, C9 added GetDeskRisk in 1.3.0 and its per-limit `checks` in 1.4.0, C10 its `multipliers` in 1.5.0, decision 5's publish gate `blocks_publish`/`blockers` in 1.6.0). The server refuses a missing header or a different major version with `FAILED_PRECONDITION` and accepts any minor or patch. AlgoLens vendors `desk.proto` byte for byte at a pinned commit and sends the header from its shared client layer. The pre-versioning name `algogators.qt.v1.DeskService` is still served, with the same wire format and no header, until AlgoLens has switched; it is removed in the next release.

AlgoLens always inserts the `position_overrides` row first, then calls the RPC with its `audit_id`. The RPCs return `ACCEPTED` immediately: the engine works asynchronously, and AlgoLens polls the row (`status`, `result`) every 2 s. If gRPC is down, the row stays `pending`, and the desk service re-drives pending rows on startup and every 60 s. A row left `running` with no live job (a restart, a database error mid-command) goes back to `pending` and runs again (C4), so the engine must tolerate re-running a command whose earlier attempt died midway. gRPC is the channel AlgoLens uses to call the engine (decided 2026-10-09; it amends the master document's "never call each other"). The re-drive guarantees a dropped call loses nothing.

| RPC | Engine action | `result` written |
|---|---|---|
| RunDesk(audit_id) | Refuse if there is no qt_proposal for the portfolio+date (ruling 14), or when the row was created at or after 10:00 New York on its date, even before the fallback publishes the day (C7, 2026-10-17). Otherwise run the one pass on the whole qt_proposal book and write qt (`book_source='desk'`, `moved_by` per symbol). Never e-mails (ruling 15). | `{"symbols":[{"symbol","asked","given","moved_by"}], "book_source":"desk"}` |
| RequestOverride(audit_id) | Generate a token, e-mail the VP and the President the link `https://algolens.algogators.com/qt/approve#token=<token>` (the fragment keeps the token out of server logs, D-44), then store its sha256 in `token_hash` with its expiry, 10:00 New York on the book date (the approval cutoff, D-13; the e-mail states it), and the result in one UPDATE (C1). Refused, with nothing e-mailed, when the row was created at or after 10:00 New York on its date, for a future day (D-43) or over the margin cap (decision 10). The e-mail shows the model book next to the desk request. | `{"emailed":["vp","president"],"emailed_at":"<UTC ISO>"}` |
| RecordDecision(audit_id of the decision row) | Refused when the request is not `done`, the decider is the requester or is not the address `QT_APPROVERS` names for the row's `approver_role` (the desk service checks both, 2026-10-17; AlgoLens checks the link's token), its link had expired, it already has a done, refused or running decision (the link works once, D-75: a refused decision uses it up; only a failed one, which booked nothing, may be tried again), or the day is published (C2, C3; the engine re-checks the requester, a done decision and the C1 snapshot hash; a refused decision is stopped by the desk service). On approval: write qt = qt_proposal exactly (`book_source='override'`), with the one-pass run kept as a report in the qt live_results `risk_detail`. On rejection: nothing. | `{"approved":bool}` |
| Publish(audit_id) | The desk's approval ("Approve" in AlgoLens; C7). Freeze the qt book and set `live_run_metadata.published_by/published_at` and `publish_source='desk'`. Before 09:30 New York nothing is sent (the 09:30 send e-mails the day); from 09:30 to 10:00 the daily e-mail and CSV, built from the stored qt rows, go at once and `sent_at` is set. Refused when the row was created at or after 10:00 New York on its date, when the day is published, or when a previous day is unpublished or failed (ruling 29). | `{"published":true,"publish_source":"desk","emailed":bool}` (+ `published_at`, `email_sent_at`) |
| Rescind(audit_id) | Since 1.2.0 (C8): take back the desk's own approval of the day, before 09:30 New York and while unsent. Clears `published_by/published_at/publish_source`; the day takes edits and a new approval again. Refused on a day not approved by the desk (fallback, model-only, unpublished), a sent day, and from 09:30 (the clock when it runs). | `{"rescinded":{"published_by","published_at"}}` |
| GetDeskRisk(portfolio_id, date) | Since 1.3.0 (C9): read-only risk report of the day's qt book (the engine's `risk_detail`, the configured limits) and whether the desk's positions breach a risk limit. | n/a |
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

- A day is published while `live_run_metadata.published_at IS NOT NULL` for (portfolio_id, date) (a done publish row alone does not publish it: the desk's Rescind clears `published_at`, C8): by the desk's approval, by the 10:00 fallback or the catch-up of a past day (C7), or, on the model twin, as `system:model-only`. (~~including the non-trading days the engine publishes itself~~: amended by C7, the engine no longer does.)
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
- **Weekends wait like weekdays** (D-63, decision 6, 2026-10-16): with the data-driven run, a day after today is never published ahead of its own day. The build stops at the first unpublished day after today; that day, once it is today, waits for the desk's approval or its 10:00 fallback with the alert, and the build goes on (Monday's books are built on Sunday once Sunday is out). `system:fallback-nontrading` is no longer written; rows written before stay.
- **Approve with or without the e-mail** (D-66, 2026-10-16): an approval (plain publish, Approve on B, Approve on C) whose publish row has `payload.email: false` freezes the day and e-mails nothing: no `--send-now`, the 09:30 send skips it and the missed-send alert does not count it. The desk e-mails it later with **Send**: a publish row `{"flow": "send"}` that the desk service runs on the portfolio's queue as the engine's `--send` (the 09:30 send's command line), for today's book only, once it is published and while `sent_at` is NULL; the row ends `done` with `{"sent": true, "sent_at"}` and never carries `published_at`, so it is not an approval (C8 still reads the approving row). An override's approval always e-mails.
- **A missed send** (D-40, 2026-10-16): a day published (approved, or by its 10:00 fallback) whose e-mail never went out before the day passed is not sent late, since a past day's book must not reach anyone as today's. After every pass the scheduler alerts the President, once a day for three days (`missed_send`). A caught-up day is never e-mailed and is alerted when it is caught up. Nothing is alerted while e-mail is off.
- **The model twin** (`desk_editable=false`) is never approved: each day its model run writes is published as `system:model-only` (`publish_source='model-only'`) and, today, sent at 09:30 when its `email.json` names recipients (`to_emails`); give it `to_emails: []` to stop that.
- **Schema** (migration 027): `live_run_metadata.publish_source text CHECK (publish_source IN ('desk','fallback','model-only'))` and `sent_at timestamptz`, both NULL for days published before 027. svc_trade_ngin reads and writes them; svc_algolens reads them only.
- **Engine modes:** `--publish --audit-id N [--send-now]` (approve; with `--send-now` also send), `--fallback --audit-id N [--send-now]` (reset, publish, send for today), `--send --date D` (no row: e-mail a published day from its stored rows; a day with `sent_at`, or a publish row's `email_sent_at`, is never sent again). Published days stay frozen and a partial book is never done (items 3 and 5).
- **Desk service:** the Publish RPC decides by the clock (`services/desk/cutoff.py`): `--publish` before 09:30, `--publish --send-now` from 09:30, refused from 10:00 (by the row's `created_at`) or on a published day. The scheduler (C6) passes every 5 min from 06:30 to 10:30, then every 30 min to 22:00; its steps are decided from the database and the clock, so a late pass (10:07) still does the fallback.
- **GetRunStatus** (desk.proto 1.1.0) adds `publish_source` and `sent_at`: approved = `publish_source` desk, sent = `sent_at` set, fallback = `publish_source` fallback.

### C8. Rescind (decided by Dom 2026-10-13, who may amended 2026-10-16; amends C3)

The desk may take back an approval of day D before the 09:30 send, edit again and approve again. **Who may (Dom 2026-10-16):** anyone on QT takes back a desk approval (a plain publish, Approve on B, Approve on C); a day published through an approved override (the approving publish row's `payload.flow` is `override_b`) only the VP or the President (`QT_APPROVERS`).

- **Command.** AlgoLens inserts a `position_overrides` row of kind `rescind` (migration 029) and calls `Rescind` (desk.proto 1.2.0) with its id. AlgoLens refuses it unless D is approved by the desk (`publish_source='desk'`), unsent and before 09:30 New York, from anyone without the QT screen, on an override's day from anyone but the VP or the President, and while a publish or another rescind of D is open.
- **Desk service.** It runs the row in Python on the portfolio's job queue, under the portfolio's flock (the lock of the model runs and the 09:30 send), so a rescind and the send never overlap. It refuses from 09:30 New York on D (by the clock when it runs, not the row's `created_at`), when D was published through an override and the row's `requested_by` is neither the VP nor the President (`QT_APPROVERS`, e-mails compared without case; with no approvers set nobody takes an override's day back), when D is not published, when any published row of D has another `publish_source` (`fallback`, `model-only`), and when any row of D has `sent_at`. Otherwise, in one transaction with D's `live_run_metadata` rows locked, it sets `published_by`, `published_at` and `publish_source` back to NULL and finishes the row `done` with `result = {"rescinded": {"published_by", "published_at", "flow"}}`.
- **After a rescind** D is unpublished again (C3 reads `published_at` only): the desk saves, requests overrides and approves as before 09:30; not approved by 10:00, the fallback publishes and sends the model's book (C7). The approval's publish row stays `done` (C4), so a day may hold several done publish rows: migration 029 drops `position_overrides_one_publish`, and a second approval of a published day is still refused by `published_at` (C3).
- **After an override's day is rescinded** the qt book is still the override's booking of B. A plain publish of it is refused (`result.reason` `override_booking`): it is published only through its own approval, once (decisions 10 and 21). The desk saves the book it wants and approves it, or asks for a new override; at 10:00 the fallback sends the model's book.
- **Not covered.** A fallback or model-only publish, a sent day and a past day cannot be rescinded.

### C9. Desk risk check (decided by Dom 2026-10-14)

A desk save runs the engine's whole pass on the desk's positions and books what it gives back (`--desk`); the pass records what it did in `live_results.risk_detail.desk` of the day's qt row. The desk sees that record, and a desk book that breaches a risk limit needs an approved override before it can be approved.

- **Every check, breach or not (1.4.0).** `checks` lists R, R_jump, R_shock, L_g, L_n, the per-name cap and the trim, each with the stored book's reading, its max in the same unit, a status (within, bound, over, not measured) and a sentence. The engine writes the readings and the limits in force into `risk_detail.desk.checks` on every desk or override save (`desk_checks_json`, `include/trade_ngin/risk/risk_detail.hpp`; read only, the pass is unchanged and risk_detail's nine keys are untouched). On a day the engine did not measure (the model's book, a refusal, an older row) the maxes come from `risk.json` and R, R_jump and R_shock are given as multiples of the risk target (`tau`). trade-ngin owns the labels, units and maxes; AlgoLens hardcodes none.
- **Read.** `GetDeskRisk(portfolio_id, date)` (desk.proto 1.3.0, checks since 1.4.0) is read-only. It reads the day's qt row (`book_source`, `risk_detail`, `portfolio_leverage`, `net_leverage`, `margin_cushion`, `equity_to_margin_ratio`) and the portfolio's `risk.json` carver limits. The rule is `services/desk/risk.py`.
- **Breach**, i.e. a risk limit bound on the desk's positions:
  - a symbol moved by `cap`, `overlay`, `clip` or `trim`;
  - the overlay cut the book (m < 1 with a binding term R, R_jump, R_shock, L_g, L_n);
  - a term still over its limit after rounding or through a held position;
  - the overlay refused.
- **Not a breach:** the trading rules `sign_close` (no long-to-short flip in a day), `rounding`, `buffer`, `search` and `hold`. They are reported with their symbols.
- **States.**
  - `model`: no desk save that day; within limits by construction.
  - `desk`: the report of the last save, with its `audit_id`.
  - `override`: an approved override. The ask trades as asked; breaches are reported but waived (`approved_override`).
  - `no_book`.
- **Gate.** Amended by C10 for the A -> B -> C flows. For a plain publish (no flow), AlgoLens refuses the desk's approval while the report of the latest done save says `within_limits = false` and no override was approved, and while the report cannot be read (fail closed). The desk then either saves within the limits or requests an override (C1, C2). The engine's publish is unchanged.

### C10. The A -> B -> C approval (decided by Dom 2026-10-14)

The model's book is **A**. The QT edits it into **B** (`qt_proposal`) and presses Approve. The engine runs B through dynamic optimisation and the risk system (the desk pass, as for a save) and stores **C** (`qt`).

- **Auto.** AlgoLens inserts the save row and a publish row with payload `{"flow": "auto", "after": <save id>}` in one transaction, then calls RunDesk and Publish. The publish runs on the portfolio's queue after the save (`commands.py` `_queue_flow`; a pending save is queued first). When C equals B on every symbol (`risk_detail.desk.symbols`, asked == given) and the report is within the limits (C9), the engine publishes, as a plain publish (`publish_source` desk). Otherwise the row is refused with `result {"reason": "changed" | "outside_limits", "differs": [{symbol, asked, given, moved_by, category}], "within_limits", "breaches"}` and nothing is published. Any difference counts, a trading rule's (`sign_close`, `buffer`, ...) included.
- **Approve C.** `{"flow": "accept_c", "after": <save id>, "acknowledged": [symbols]}`: publishes C when the day's book is still that save's (else `stale`) and C itself is over no limit (no check `over`, no term over after rounding or by a hold, the overlay not refused). A limit that bound (`bound`) is fine: C is the book cut to fit it.
- **Insist on B.** The override flow (C1, C2): the request e-mails the VP and the President, either decides alone. On approval AlgoLens inserts the decision and a publish row `{"flow": "override_b", "after": <decision id>}`; the engine books B exactly (`--override`) and publishes it. A rejected or failed decision publishes nothing (`rejected`, `incomplete`). The publish is refused unless the day's book is still that decision's booking (`stale`), and an approval publishes once (decision 21, D-84): a second `override_b` publish after the same decision, as after a rescind, is refused (`approval_used`); any book but the model's then needs a new override.
- **Cutoffs** are C7's, by the publish row's `created_at` (the moment Approve was pressed): before 09:30 the day is frozen and sent at 09:30, from 09:30 it is sent at once, from 10:00 it is refused. A failed save or decision approves nothing (`incomplete`); an unreadable risk check fails the row (retry with a new row).
- **A future day (D-43, 2026-10-16).** A publish or an override request for a book date after today in New York is refused (`result.reason` `future_day`), by AlgoLens and by the desk service: it would freeze the day on prices its own morning run has not seen. Saves to it are taken. The scheduler's fallback rows are exempt.
- **Publish gate (1.6.0, decision 5 of 2026-10-16).** Publishing is blocked only when the final book (C) is over a cap: gross leverage, net leverage, the per-name cap or the fund's margin cap (100% of equity). A desk book whose caps were not measured, or whose overlay refused, is blocked too. So is an incomplete book (state `incomplete`, 1.6.0): a qt row whose `book_source` a desk or override run cleared and did not set again, as an override refused by the margin cap leaves it; the engine refuses to publish it as well, and the desk saves again or the 10:00 fallback restores the model's book. An approved override is blocked by the margin cap only: the VP or President may approve a book past gross, net and per-name, never past margin. A bound limit, R, R_jump, R_shock, the trim and every trading rule are reported and never block. Every publish path (plain publish, Approve on B, Approve on C) applies this one gate in the desk service, which fails closed when the risk check cannot be read; `GetDeskRisk.blocks_publish` and `blockers` carry it to AlgoLens, which gates Approve on it and does not recompute it.
- **Multipliers (1.5.0).** `GetDeskRisk.multipliers` reports the overlay's m and the risk scale of the last run, and each live strategy's configured IDM and FDM (the IDM is a setting, not estimated per run; the FDM is the lookup by the number of EMA windows).

### C11. The fund's hard cap (decided by Dom 2026-10-14)

A desk or override book may use at most **100% of the fund's equity** as initial margin. Equity is the book's value after today's transaction costs (`live_results.current_portfolio_value`; buys and sells both pay a cost, `trading.executions.total_transaction_costs`), so initial margin plus the day's costs never exceed the equity the day started with.

- **Enforced by the engine** on `--desk` and `--override`, where the day is marked (`qt::fund_cap_refusal`, `src/live/qt_desk.cpp`): a book over the cap is refused (exit 2) with the amounts, and left unmarked (`book_source` NULL), so it cannot be published; the 10:00 fallback sends the model's book. An approved override is refused the same way: an approval cannot take the fund past its money.
- **Checked first by the desk service on an override** (decision 10, Dom 2026-10-16), from the margin data the database holds: the request's proposal snapshot, each symbol's net quantity × `metadata.contract_metadata."Overnight Initial Margin"` (the figure the engine's instrument registry posts), against the day's equity (the qt row's `current_portfolio_value`, else the model's). `RequestOverride` refuses a request over the cap before anything is e-mailed (`result.reason` `outside_limits`, `over` `["MARGIN"]`), and an approving `RecordDecision` measures it again and refuses before the engine runs, so the day's book is never cleared by a refused override. A request whose margin cannot be measured (a symbol with no margin on record, no equity yet, no proposal snapshot) is refused too; an unreadable database fails it for a retry. The engine's own check above stays the last word: it measures the booked book with that day's costs.
- **Reported** by `GetDeskRisk` as the check `MARGIN` (unit `usd`: `margin_posted` against `current_portfolio_value` of the day's qt row), on every state; over the cap it is a breach. The model run is not refused by it (its book is sized by the overlay on the sizing capital); a model book over the cap shows as `over`.
- The desk record (`risk_detail.desk.fund_cap`) carries the amounts the engine checked.

## Credits

John Riley's QT work is the basis of these parts of the engine side:

- **The three books (#165).** Keeping a model book and a desk book side by side, seeding the desk book from the model, and reading yesterday's book from the desk side is his F1 design (issue #47, PRs #54 and #55); so is the untouched model portfolio's purpose, measuring whether desk edits add or lose value. The rulings changed how the seeding works.
- **Book-aware storage (#163, migration 021).** The book-scoped storage approach of #55, built on his merged #54.
- **The command log (#161, migration 023).** The `position_overrides` audit table and its no-edit, no-delete rule, from #55.
- **Desk settings from the database (#162, migration 022).** The settings table (from #60's migration 008), the overlay tests (#60), and the secret-key and parse-and-rebuild checks (#151).
