# QT Effective Position Reporting and Audit Design

**Status:** Approved in design review on 2026-09-18  
**Primary repository:** `trade-ngin` (`qt-platform-preview`)  
**Companion repository:** `AlgoLens` (`qt-platform-preview`)  
**Production database rule:** Audit through a read-only session only. This work must not apply migrations or execute DDL/DML against production.

## 1. Purpose

Make the existing Daily Trading Report and its positions CSV show the quantities actually set by the QT team, while preserving the report's current layout, wording, recipients, and non-position content.

The QT position stream is the operational source of truth. A manual QT quantity replaces the strategy-computed quantity until QT clears or replaces it. A quantity of zero closes the position and removes it from the report and positions CSV.

This work also resolves the four defects identified during review of the two QT platform branches:

1. Ambiguous equity/futures symbol lookup in Trade Ngin.
2. Net leverage calculated from gross notional in AlgoLens.
3. Position-override audit records without portfolio identity.
4. The internal-only override-history API being requested by subscriber users.

## 2. Non-goals

- Do not add an email, section, banner, label, or explanation for manual changes.
- Do not change the Daily Trading Report's layout or wording.
- Do not change recipients or delivery timing.
- Do not replay the audit log to reconstruct positions.
- Do not change executions, P&L, risk metrics, or other report sections as part of the reporting fix.
- Do not expose internal override history to subscribers.
- Do not apply a migration, repair data, or write test records in production during the audit.

## 3. Verified Current State

### 3.1 AlgoLens write path

`AlgoLens` writes a manual edit to `trading.positions` with `portfolio_type='qt'` and inserts an append-only row in `trading.position_overrides` in the same transaction. The position row is keyed by portfolio, strategy, strategy name, date, symbol, and stream.

The current override row includes strategy and symbol but not `portfolio_id`, so two books using the same strategy and symbol cannot be distinguished reliably in audit history.

### 3.2 Trade Ngin report path

Trade Ngin stores the system position snapshot and seeds the QT stream without overwriting an existing QT snapshot. However, the Daily Trading Report and current-positions CSV are built from the runner's in-memory system maps rather than a fresh QT database snapshot.

This means a valid AlgoLens edit can exist in the database while the investor report still shows the strategy-computed quantity.

### 3.3 Additional defects

- The instrument registry normalizes a versioned futures symbol such as `ES.v.0` to `ES` and stores it in a symbol-only map. An equity with symbol `ES` can therefore win or lose based on load order.
- AlgoLens computes both gross and net leverage from an absolute-notional total, so an offsetting long/short book can be rejected as if its net exposure equaled its gross exposure.
- The AlgoLens UI renders override history for subscribers even though the corresponding API is internal-only, causing an expected 403.

## 4. Requirements

### 4.1 Effective-position semantics

- A QT quantity completely replaces the corresponding system quantity.
- The override remains effective until QT clears or replaces it.
- A zero QT quantity represents a closed position.
- Closed positions do not appear in the email or positions CSV.
- The email and CSV must use one identical, immutable report snapshot.

### 4.2 Report compatibility

- Preserve the existing email HTML and text.
- Preserve report recipients, subject, schedule, attachments, and non-position inputs.
- Replace only the current position maps supplied to the existing email renderer and positions CSV exporter.
- Quantity-dependent values inside existing position rows may update naturally from the QT quantity. Unrelated report metrics remain on their existing data path.

### 4.3 Safety

- Never silently substitute system positions when the QT snapshot is unavailable or inconsistent.
- Never calculate report positions by replaying audit events.
- Every new override record must identify its portfolio.
- Audit history queries must be scoped by portfolio and strategy.

## 5. Chosen Architecture

### 5.1 Canonical report snapshot

Immediately before report and positions-CSV generation, the live runner loads a fresh QT snapshot from `trading.positions`.

The query is scoped by:

- `portfolio_id`
- combined `strategy_id`
- `strategy_name`
- report date
- `portfolio_type='qt'`

The loader reads zero rows as evidence of explicit closures, then filters zero quantities when constructing the display maps. It returns one value object containing:

- per-strategy open positions for the email and CSV;
- combined open positions for existing aggregate consumers;
- per-strategy snapshot-evidence counts, including zero rows;
- the portfolio, strategy, stream, and date used for the load.

The runner loads this object once and passes the same per-strategy map to both the current-positions CSV exporter and the Daily Trading Report. The email renderer remains a presentation component and does not query the database itself.

### 5.2 Ordering

The live-run order becomes:

1. Calculate and persist the system snapshot.
2. Seed the day's QT snapshot where one does not already exist.
3. Load and validate the fresh QT report snapshot.
4. Generate the positions CSV from that snapshot.
5. Generate the unchanged Daily Trading Report using the same snapshot.
6. Send the existing email with the existing attachments.

The QT seed remains non-destructive. Re-running Trade Ngin must not overwrite a manual QT decision.

### 5.3 Failure behavior

Reporting fails closed when:

- the QT snapshot query fails;
- the database cannot establish a consistent report snapshot;
- a strategy has a current system snapshot but no QT snapshot evidence after seeding; or
- the loaded snapshot does not match the requested portfolio, strategy, date, and stream.

When reporting is blocked, Trade Ngin logs a structured error identifying the portfolio, strategy, date, and failure reason. It does not send the investor email or its positions attachment, and it does not silently fall back to in-memory system positions.

A genuinely flat book is valid. If neither system nor QT has an open position, the report may proceed with no open-position rows. Explicit QT zero rows count as snapshot evidence and are removed only when constructing displayed open positions.

## 6. Supporting Corrections

### 6.1 Type-safe instrument identity

Trade Ngin will keep exact identifiers separate from aliases and make aliases instrument-type aware.

- An exact versioned identifier such as `ES.v.0` is classified as a future and may resolve only to a future.
- An exact plain identifier such as `ES` may continue to resolve to the equity whose exact symbol is `ES`.
- A normalized futures root is stored in a futures-qualified alias index rather than the global exact-symbol map.
- Ambiguous untyped aliases are rejected instead of being resolved by insertion order.
- Registry behavior must be deterministic regardless of database row order.

### 6.2 Correct net leverage

AlgoLens will calculate:

- gross notional as the sum of absolute signed notionals;
- net notional as the signed sum of position notionals; and
- net leverage as `abs(net_notional) / portfolio_value`.

The gross-leverage limit continues to use gross notional. The net-leverage limit uses the corrected net calculation.

### 6.3 Portfolio-scoped override audit

A new Trade Ngin migration will add `portfolio_id TEXT` to `trading.position_overrides`, enforce it for new rows, and add a portfolio/strategy/time index suitable for the history endpoint.

AlgoLens will:

- insert `portfolio_id` in the same transaction as the QT position update;
- return and query history by both portfolio and strategy;
- update its schema contract to match the migration; and
- preserve the database's append-only guarantees.

Existing override rows are immutable evidence. They must not be updated in place. If legacy book scope is required, the migration may create an append-only companion mapping keyed by override ID and populate it only when the production audit proves exactly one possible portfolio. Rows with zero or multiple candidates remain explicitly unscoped. The audit report must count both categories before any migration is considered for production.

### 6.4 Subscriber-safe history access

The override-history API remains internal-only. AlgoLens will render the history panel only for roles authorized to call that endpoint. Subscriber pages must not issue the request and must not expose internal editor, reason, or risk-override data.

## 7. Database Contract

### 7.1 Authoritative tables

- `trading.positions` is the authoritative current-state source for report positions.
- `trading.position_overrides` is append-only evidence of manual changes.
- Audit records are not an event-sourced replacement for `trading.positions`.

### 7.2 Position keys

All report and edit queries must retain the full identity needed to prevent cross-book or cross-stream mixing:

`(portfolio_id, strategy_id, strategy_name, date, symbol, portfolio_type)`

No report query may use a latest-by-symbol lookup that omits portfolio, strategy, date, or stream scope.

### 7.3 Planned migration behavior

The planned audit-scope migration must be idempotent and transactional. It will:

1. add `portfolio_id` without rewriting existing audit evidence;
2. enforce non-null portfolio scope for newly inserted rows;
3. add an index beginning with `portfolio_id` and `strategy_id`, ordered by `created_at DESC`;
4. preserve the no-update and no-delete rules; and
5. keep legacy unresolved rows visible to internal audit tooling as unscoped.

The migration will be committed to source control but will not be applied to production by this task.

## 8. Read-only Production Database Audit

### 8.1 Guardrails

The production session must be read-only at connection or transaction level. The audit permits catalog queries and `SELECT` statements only.

Forbidden operations include:

- migrations and schema changes;
- `INSERT`, `UPDATE`, `DELETE`, `MERGE`, `TRUNCATE`, or `COPY FROM`;
- creating temporary or permanent objects;
- repair statements;
- explicit row/table locks; and
- test writes followed by rollback.

Credentials, investor data, reasons, user identifiers, and full position payloads must not be copied into the Markdown report. Report counts, schema facts, redacted samples when essential, and anomaly identifiers only.

### 8.2 Audit checks

The audit will verify:

1. Database identity and read-only posture.
2. Applied migration/version evidence available in production.
3. Column types, nullability, defaults, indexes, constraints, and append-only rules for `trading.positions` and `trading.position_overrides`.
4. Presence and population of `portfolio_type` values (`system`, `qt`, and other supported streams).
5. Uniqueness of position keys across portfolio, strategy, name, date, symbol, and stream.
6. Current-date system-to-QT snapshot coverage per portfolio and strategy.
7. QT/system quantity divergence, summarized by book and strategy without exposing investor-level detail.
8. Zero-quantity QT rows that represent explicit closures.
9. Override rows that can be linked to exactly one portfolio, no portfolio, or multiple portfolios.
10. Duplicate, orphaned, or cross-book-ambiguous audit records.
11. Whether the production schema can support the proposed code before any deployment.

### 8.3 Evidence format

Each audit requirement will be recorded as:

| Item | Owner/repository | Claimed state | Evidence | Verified state | Gap/next action |
|---|---|---|---|---|---|

Allowed verified states are `done`, `partial`, `missing`, `blocked`, and `not verifiable`.

The audit section will include the UTC audit timestamp, database identity in non-secret form, and exact Trade Ngin and AlgoLens commit identifiers.

## 9. Testing Strategy

### 9.1 Trade Ngin

- A manually changed QT quantity replaces the system quantity in the report snapshot.
- The same snapshot object drives email and CSV inputs.
- A zero QT quantity is accepted as closure evidence and omitted from displayed positions.
- A database error blocks reporting.
- A system snapshot without corresponding QT evidence blocks reporting.
- A genuinely flat book remains reportable.
- Cross-portfolio, cross-strategy, cross-date, and cross-stream rows are excluded.
- Exact equity/future collisions resolve deterministically and independently of insertion order.

### 9.2 AlgoLens

- Offset long/short positions produce gross leverage from absolute exposure and net leverage from signed exposure.
- New audit rows include the requested portfolio.
- History reads require and enforce portfolio plus strategy scope.
- The position upsert and audit insert remain atomic.
- Subscriber views do not render or request override history.
- Internal users retain authorized history access.

### 9.3 Integration and migration tests

- The audit-scope migration is idempotent.
- New audit inserts without `portfolio_id` are rejected.
- Append-only update/delete protection remains effective.
- Composite indexes and constraints match the schema contract.
- Focused tests run first; broader suites run after the focused tests pass.

## 10. Acceptance Criteria

The work is complete only when:

1. A QT manual quantity is the quantity shown in the existing Daily Trading Report.
2. The attached positions CSV shows the same quantity.
3. A QT zero closes and removes the position from both outputs.
4. Missing or inconsistent QT state blocks the email instead of falling back.
5. No email layout, wording, recipients, or unrelated metrics change.
6. Instrument collisions, net leverage, audit scoping, and subscriber authorization defects are fixed with regression tests.
7. Focused tests and appropriate broader verification pass.
8. The production database audit is completed through read-only queries only and its evidence is appended to this document or linked from it.
9. No production migration or data repair is performed by this task.

## 11. Implementation Ownership

### Trade Ngin

- QT report-snapshot loading and validation.
- Live-runner handoff to email and CSV.
- Instrument registry correction.
- Audit-scope migration and migration tests.

### AlgoLens

- Signed net-leverage calculation.
- Portfolio-scoped audit writes and reads.
- Schema-contract updates.
- Subscriber/internal history rendering behavior.

### Shared verification

- Cross-repository contract checks.
- Read-only production audit.
- Evidence matrix and final acceptance review.

## 12. Deployment Notes

The safe deployment order is:

1. Review the read-only production audit and confirm schema prerequisites.
2. Apply the approved audit-scope migration through the normal deployment process outside this task.
3. Deploy AlgoLens audit-scope changes.
4. Deploy Trade Ngin report-snapshot and registry changes.
5. Verify one non-investor test report or approved staging equivalent before the next investor distribution.

If the production audit finds a schema mismatch, ambiguous legacy scope, or missing QT coverage, mark the affected requirement `partial` or `blocked`; do not repair production as part of the audit.
