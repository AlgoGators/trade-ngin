# QT platform: engine ↔ AlgoLens contract (v1)

Status: binding for the 2026-10-09 build. The master document ("QT platform", 2026-10-08) is the source of truth. This file fixes the names both repos code against. Section 3b of `qt-platform-analysis-and-plan.md` explains the decisions behind it.

## 1. Where things run

| Piece | Where | Database | Notes |
|---|---|---|---|
| Live trading cron (`trade-ngin-trade-ngin-1`) | trade-ngin host | `algo_data` | **Never touched by QT work.** It runs the pre-stage-3 image at 09:30. |
| `qt-engine` container (trade-ngin image built from main) | trade-ngin host, Docker network `qt` | `new_algo_data` | Runs `desk-agent` (gRPC on :50051, not published) plus a daily cron for the QT model run. |
| AlgoLens (edge, frontend, backend) | trade-ngin host, backend also on network `qt` | `new_algo_data` | The backend calls `desk-agent:50051`. |

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

**Who writes what:**
- AlgoLens inserts the rows (status `pending`).
- The engine (desk-agent) moves `status`, `started_at`, `finished_at`, `result` and `message`.
- Nobody deletes rows. Rows other than status and result are never updated (enforced by a trigger).

## 5. Other schema changes

- **021** (E1): book columns, and `positions.moved_by text`, which names the step that moved a symbol. Its values are `cap`, `overlay`, `sign_close`, `hold`, `search`, `buffer`, `rounding`, `clip`, `trim` or `none`.
- **022** (E2):
  - `trading.strategy_config`, the desk's saved settings.
  - On `live_run_metadata`: `settings_used jsonb`, `published_by text` and `published_at timestamptz`.
- **023** (this contract):
  - Rebuild `position_overrides` as in §4.
  - Add `live_results.book_source text check in ('model','desk','override')`.
  - Add `strategy_registry.portfolio_group text` and `strategy_registry.desk_editable boolean not null default false`. (The engine never reads `strategy_registry`. It ships here only because trade-ngin owns the migration chain.)
  - Drop `trading.risk_limits`, `trading.portfolios`, `trading.strategy_book_memberships` and `trading.portfolio_assignments` (ruling 28). First confirm that AlgoLens main does not reference them.
  - Register the two QT portfolios in `strategy_registry`.

## 6. gRPC (`proto/qt/v1/desk.proto`, #160)

AlgoLens always inserts the `position_overrides` row first, then calls the RPC with its `audit_id`. The RPCs return `ACCEPTED` immediately: the engine works asynchronously, and AlgoLens polls the row (`status`, `result`) every 2 s. If gRPC is down, the row stays `pending`, and desk-agent re-drives pending rows on startup and every 60 s. So the RPC is a fast path, never the only path.

| RPC | Engine action | `result` written |
|---|---|---|
| RunDesk(audit_id) | Refuse if there is no qt_proposal for the portfolio+date (ruling 14). Otherwise run the one pass on the whole qt_proposal book and write qt (`book_source='desk'`, `moved_by` per symbol). Never e-mails (ruling 15). | `{"symbols":[{"symbol","asked","given","moved_by"}], "book_source":"desk"}` |
| RequestOverride(audit_id) | Generate a token, store its sha256 in `token_hash` with a 48 h expiry, and e-mail the VP and the President the link `https://algolens.algogators.com/qt/approve?token=<token>`. The e-mail shows the model book next to the desk request. | `{"emailed":["vp","president"]}` |
| RecordDecision(audit_id of the decision row) | On approval: write qt = qt_proposal exactly (`book_source='override'`), with the one-pass run kept as a report in the qt live_results `risk_detail`. On rejection: nothing. | `{"approved":bool}` |
| Publish(audit_id) | Finalise the qt book, send the daily e-mail and CSV built from the qt book, and set `live_run_metadata.published_by/published_at`. Refuse if a previous day is unpublished or failed: catch it up first (ruling 29). | `{"emailed":true}` |
| GetRunStatus(portfolio_id, date) | Model-run state for the desk page header. | n/a |

Approver addresses come from the env var `QT_APPROVERS="vp=<email>,president=<email>"` on the qt-engine container. The requester may not approve their own request: AlgoLens refuses it, and the engine re-checks.

## 7. AlgoLens rules (rulings 13, 14, 16, 20, 21, 22)

- **Opening state:** AlgoLens opens on the `qt` book. A portfolio switcher, grouped by `portfolio_group`, sits above everything; inside a portfolio the toggle flips system / qt_proposal / qt.
- **What can be edited:** quantity only, and only on futures portfolios with `desk_editable`. Zero means flatten, and is stored as a zero-quantity row. A new symbol takes its price from market data and is picked per asset class from `metadata.contract_metadata`. A reason is required.
- **Saving:** a save upserts the changed symbols into qt_proposal for the latest seeded date, inserts the `save` row, and calls RunDesk. If no qt_proposal rows exist for that portfolio and date, the save gets a 409.
- **The asked-vs-returned view:** qt_proposal against qt, per symbol, with `moved_by`.
- **Override:** a "Request override" button with a reason. The approval page sits behind login and shows both books.
- **Publish:** a button, enabled whatever the edit state. The publish state comes from `live_run_metadata`.
