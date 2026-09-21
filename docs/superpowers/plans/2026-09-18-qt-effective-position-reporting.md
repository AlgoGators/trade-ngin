# QT Effective Position Reporting Implementation Plan

> Current status is recorded in the [September 21 revalidation](../../audits/2026-09-21-qt-pipeline-revalidation.md). Tasks 1-7 have implementations in the reviewed local branches; Task 8's read-only evidence collection is complete but blocks production readiness. Task 9's fresh checks and additional fixes are recorded there. The original step-by-step checkboxes below are planning history, not a current release-status ledger.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the existing Daily Trading Report and current-positions CSV use the QT team's effective quantities, while fixing all reviewed QT-platform defects and auditing production through read-only queries only.

**Architecture:** Trade Ngin loads and validates one portfolio-, strategy-, date-, and stream-scoped QT snapshot immediately before current-position reporting, then supplies that immutable snapshot to both existing consumers. AlgoLens remains the atomic writer of QT positions and audit evidence; its leverage math, audit scoping, and role-aware history UI are corrected without changing the investor email template.

**Tech Stack:** C++20, CMake, GoogleTest, libpqxx/PostgreSQL, Python 3.11, Flask, pytest, React 18, TypeScript 5, Vitest.

**Spec:** `docs/superpowers/specs/2026-09-18-qt-effective-position-reporting-design.md`

## Global Constraints

- Preserve the Daily Trading Report's layout, wording, recipients, subject, schedule, and non-position content.
- Use `trading.positions` with `portfolio_type='qt'` as current state; never replay `position_overrides` to calculate positions.
- A QT zero is closure evidence and is omitted from email and CSV open-position rows.
- Email and current-positions CSV use one identical immutable snapshot.
- Never silently fall back to system positions when QT loading or validation fails.
- Keep override history internal-only; subscribers must not request or see it.
- Production DB work is read-only: catalog queries and `SELECT` only; no migration, DDL, DML, locks, repair, temporary objects, or test writes.
- Do not expose credentials, investor data, reasons, user IDs, or full position payloads in docs or command output.
- Preserve unrelated user changes in both repositories.

---

## File Map

### Trade Ngin

- `include/trade_ngin/apps/live_portfolio_helpers.hpp`: report-snapshot type and loader declaration.
- `src/apps/live_portfolio_helpers.cpp`: load, validate, zero-filter, and aggregate QT rows.
- `tests/apps/test_live_portfolio_helpers.cpp`: isolated report-snapshot tests.
- `include/trade_ngin/data/postgres_database.hpp`, `src/data/postgres_database.cpp`: strict date/stream report read with no legacy fallback.
- `apps/strategies/live_portfolio_runner.cpp`: one-time QT handoff to CSV/email and send gate.
- `include/trade_ngin/instruments/instrument_registry.hpp`, `src/instruments/instrument_registry.cpp`, `tests/instruments/test_instrument_registry.cpp`: type-qualified symbol resolution.
- `migrations/012_position_overrides_portfolio_scope.sql` and related rollback/test/docs: portfolio-scoped audit schema.
- `docs/audits/2026-09-18-qt-platform-production-db-audit.md`: sanitized production evidence.

### AlgoLens

- `algolens-api/algolens/domain/portfolio/position_edit.py` and `tests/test_position_edit.py`: gross/signed-net leverage.
- `algolens-api/algolens/application/portfolio/{ports.py,use_cases.py}`: book-scoped history contract.
- `algolens-api/algolens/infrastructure/portfolio/repositories.py`: portfolio audit insert and read.
- `algolens-api/algolens/adapters/http/portfolio.py`: required history book parameter.
- `algolens-api/algolens/infrastructure/db/schema_contract.py` and tests: schema contract.
- `algolens-frontend/src/{infrastructure/api/portfolioApi.ts,components/OverrideHistory.tsx,components/StrategyDetail.tsx}` and component tests: role and book scoping.

---

### Task 1: Build a Testable QT Report Snapshot Loader

**Repository:** Trade Ngin

**Files:**
- Modify: `include/trade_ngin/apps/live_portfolio_helpers.hpp`
- Modify: `src/apps/live_portfolio_helpers.cpp`
- Modify: `tests/apps/test_live_portfolio_helpers.cpp`
- Modify: `include/trade_ngin/data/postgres_database.hpp`
- Modify: `src/data/postgres_database.cpp`

**Interfaces:**
- Consumes: a new strict database read plus the runner's system strategy map:

```cpp
virtual Result<std::unordered_map<std::string, Position>>
load_report_positions_by_date(
    const std::string& strategy_id,
    const std::string& strategy_name,
    const std::string& portfolio_id,
    const Timestamp& report_date,
    const std::string& portfolio_type);
```

  The implementation queries the `date` column, requires `portfolio_type` to exist, and returns an error rather than dropping the stream predicate on a legacy schema.
- Produces:

```cpp
using StrategyPositionRows =
    std::unordered_map<std::string, std::unordered_map<std::string, Position>>;

struct ReportPositionSnapshot {
    StrategyPositionRows by_strategy;
    std::unordered_map<std::string, Position> combined;
};

Result<ReportPositionSnapshot> load_qt_report_position_snapshot(
    PostgresDatabase& db,
    const std::string& strategy_id,
    const std::vector<std::string>& strategy_names,
    const std::string& portfolio_id,
    const Timestamp& report_date,
    const StrategyPositionRows& system_rows);
```

- [ ] **Step 1: Write the failing tests**

Add a `ReportSnapshotDatabase` test double that overrides `load_report_positions_by_date`, records all scope arguments, and returns configured rows/errors. Pin replacement, zero closure, DB failure, missing QT evidence, partial per-symbol QT coverage, a genuinely flat strategy, all-zero QT evidence, and aggregation across strategies.

```cpp
TEST(LivePortfolioHelpers, QtReportSnapshotReplacesSystemQuantityAndFiltersClosures) {
    ReportSnapshotDatabase db;
    db.rows["TREND"] = {
        {"ES", position("ES", 7.0)},
        {"NQ", position("NQ", 0.0)},
    };
    StrategyPositionRows system{{"TREND", {{"ES", position("ES", 12.0)}}}};

    auto result = load_qt_report_position_snapshot(
        db, "LIVE_TREND", {"TREND"}, "INVESTOR_A", report_date(), system);

    ASSERT_TRUE(result.is_ok());
    EXPECT_DOUBLE_EQ(
        result.value().by_strategy.at("TREND").at("ES").quantity.as_double(), 7.0);
    EXPECT_EQ(result.value().by_strategy.at("TREND").count("NQ"), 0u);
    EXPECT_EQ(db.calls.front().portfolio_type, "qt");
}
```

- [ ] **Step 2: Verify red state**

Run: `make build && ./build/bin/trade_ngin_tests --gtest_filter='LivePortfolioHelpers.*QtReport*'`

Expected: compile failure because the new type/function do not exist.

- [ ] **Step 3: Implement minimal loader**

Implement `load_report_positions_by_date` with an exact `date = $4` and `portfolio_type = $5` predicate. If the stream column is absent, return `DATABASE_ERROR`; never use the legacy no-stream query for report data.

Call that method once per strategy. Propagate a scoped error on query failure. Return an error when any system symbol lacks a corresponding raw QT row; a zero QT row satisfies coverage and proves closure. Treat empty system+QT as flat. Count zero rows as evidence, then omit quantities with `abs(quantity) <= 1e-10` from returned maps.

```cpp
if (loaded.value().empty() && system_rows.contains(strategy_name) &&
    !system_rows.at(strategy_name).empty()) {
    return make_error<ReportPositionSnapshot>(
        ErrorCode::DATABASE_ERROR,
        "System positions exist without QT snapshot evidence for portfolio=" +
            portfolio_id + ", strategy=" + strategy_name);
}
```

- [ ] **Step 4: Verify green state**

Run the same filter. Expected: all snapshot tests pass.

- [ ] **Step 5: Commit**

Run: `git add include/trade_ngin/apps/live_portfolio_helpers.hpp src/apps/live_portfolio_helpers.cpp tests/apps/test_live_portfolio_helpers.cpp include/trade_ngin/data/postgres_database.hpp src/data/postgres_database.cpp && git commit -m "feat: load validated QT report snapshots"`

---

### Task 2: Feed One QT Snapshot to the Existing Email and CSV

**Repository:** Trade Ngin

**Files:**
- Modify: `apps/strategies/live_portfolio_runner.cpp:3020-3050`
- Modify: `apps/strategies/live_portfolio_runner.cpp:3399-3435`
- Modify: `tests/apps/test_live_portfolio_helpers.cpp`

**Interfaces:**
- Consumes Task 1.
- Produces report-only `report_strategy_positions`, `report_positions`, and `reporting_blocked`. The EmailSender API and template do not change.

- [ ] **Step 1: Add an immutability regression**

Copy the returned snapshot into two consumer inputs, mutate the test DB fixture, and assert both inputs retain the original quantity. This proves one load feeds two consumers.

- [ ] **Step 2: Integrate before current-position CSV generation**

Keep system maps unchanged for trading/P&L/metrics.

```cpp
auto report_snapshot = load_qt_report_position_snapshot(
    *db, combined_strategy_id, strategy_names, portfolio_id, now,
    strategy_positions_map);
if (report_snapshot.is_error()) {
    reporting_blocked = true;
    ERROR("INVESTOR_REPORT_BLOCKED: " +
          std::string(report_snapshot.error()->what()));
} else {
    report_strategy_positions = report_snapshot.value().by_strategy;
    report_positions = report_snapshot.value().combined;
}
```

Generate the current-position CSV only from `report_strategy_positions`. If export fails, set `reporting_blocked = true`.

- [ ] **Step 3: Replace only email position arguments**

Pass `report_strategy_positions` and `report_positions` as the first two arguments to the existing email renderer. Preserve every other argument. Guard generation/sending with `!reporting_blocked && !today_filename.empty()`.

- [ ] **Step 4: Verify**

Run:
- `make build`
- `./build/bin/trade_ngin_tests --gtest_filter='LivePortfolioHelpers.*QtReport*:CSVExporter*'`
- `git diff -- src/core/email_sender.cpp include/trade_ngin/core/email_sender.hpp`
- `git diff --check`

Expected: tests pass; email sender/template diff is empty.

- [ ] **Step 5: Commit**

Run: `git add apps/strategies/live_portfolio_runner.cpp tests/apps/test_live_portfolio_helpers.cpp && git commit -m "fix: report QT effective positions"`

---

### Task 3: Make Instrument Resolution Type-safe

**Repository:** Trade Ngin

**Files:**
- Modify: `include/trade_ngin/instruments/instrument_registry.hpp:92-112`
- Modify: `src/instruments/instrument_registry.cpp:35-70,145-175`
- Modify: `tests/instruments/test_instrument_registry.cpp:65-165`

**Interfaces:** Type-qualified future/equity/option maps; generic plain-symbol lookup remains deterministic.

- [ ] **Step 1: Write failing collision tests**

Install both an `ES` equity and future in both insertion orders and require:

```cpp
EXPECT_EQ(registry.get_instrument("ES"), es_equity);
EXPECT_EQ(registry.get_instrument("ES.v.0"), es_future);
EXPECT_EQ(registry.get_equity_instrument("ES"), es_equity);
EXPECT_EQ(registry.get_futures_instrument("ES"), es_future);
EXPECT_EQ(registry.get_futures_instrument("ES.v.0"), es_future);
```

Retain the full-size-versus-micro regression.

- [ ] **Step 2: Verify red state**

Run: `./build/bin/trade_ngin_tests --gtest_filter='InstrumentRegistryTest.*'`

Expected: variant/collision tests fail under the single map.

- [ ] **Step 3: Implement type-qualified indexes**

Add separate maps for futures, equities, and options. A `.v.` input resolves only through the futures map after root normalization. Typed getters query their own maps directly. For a colliding plain generic ticker, exact equity wins; otherwise the only exact instrument wins. Clear/swap all maps together.

- [ ] **Step 4: Verify and commit**

Run the registry filter, then:
`git add include/trade_ngin/instruments/instrument_registry.hpp src/instruments/instrument_registry.cpp tests/instruments/test_instrument_registry.cpp && git commit -m "fix: disambiguate futures and equity symbols"`

---

### Task 4: Add Portfolio Scope to the Append-only Audit Schema

**Repository:** Trade Ngin

**Files:**
- Create: `migrations/012_position_overrides_portfolio_scope.sql`
- Create: `migrations/012_position_overrides_portfolio_scope_rollback.sql`
- Create: `migrations/test_012_position_overrides_portfolio_scope.sh`
- Modify: `migrations/README.md`

**Interfaces:** Produces `position_overrides.portfolio_id`, `position_override_legacy_scopes`, and a portfolio/strategy/time index for Task 6.

- [ ] **Step 1: Write the failing migration harness**

Create pre-012 tables and insert one uniquely attributable legacy override and one multi-book ambiguous override. Assert unique mapping only, new unscoped inserts rejected, scoped inserts accepted, update/delete protection retained, and a second migration run remains idempotent.

- [ ] **Step 2: Verify red state**

Run: `bash migrations/test_012_position_overrides_portfolio_scope.sh`

Expected: failure because migration 012 is absent.

- [ ] **Step 3: Implement the migration**

```sql
BEGIN;

ALTER TABLE trading.position_overrides
    ADD COLUMN IF NOT EXISTS portfolio_id TEXT;

ALTER TABLE trading.position_overrides
    DROP CONSTRAINT IF EXISTS position_overrides_new_rows_require_portfolio;
ALTER TABLE trading.position_overrides
    ADD CONSTRAINT position_overrides_new_rows_require_portfolio
    CHECK (portfolio_id IS NOT NULL) NOT VALID;

CREATE TABLE IF NOT EXISTS trading.position_override_legacy_scopes (
    override_id BIGINT PRIMARY KEY REFERENCES trading.position_overrides(id),
    portfolio_id TEXT NOT NULL,
    inference_basis JSONB NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS idx_position_overrides_portfolio_strategy_created
    ON trading.position_overrides (portfolio_id, strategy_id, created_at DESC);

COMMIT;
```

Populate the companion table with `INSERT ... SELECT ... GROUP BY override_id HAVING count(DISTINCT positions.portfolio_id)=1 ON CONFLICT DO NOTHING`. Never update an original audit row. Add append-only rules to the companion.

- [ ] **Step 4: Verify and document**

Run the harness twice. Update `migrations/README.md`. Expected: both passes, no duplicate mappings.

- [ ] **Step 5: Commit**

Run: `git add migrations/012_position_overrides_portfolio_scope.sql migrations/012_position_overrides_portfolio_scope_rollback.sql migrations/test_012_position_overrides_portfolio_scope.sh migrations/README.md && git commit -m "feat: scope position override audits by portfolio"`

---

### Task 5: Correct Gross and Net Leverage

**Repository:** AlgoLens

**Files:**
- Modify: `algolens-api/algolens/domain/portfolio/position_edit.py:287-300,345-366`
- Modify: `algolens-api/tests/test_position_edit.py:150-175`

**Interfaces:** `_gross_notional` sums absolute values; `_net_notional` sums signed values; net leverage is `abs(net)/portfolio_value`.

- [ ] **Step 1: Write failing tests**

```python
def test_net_leverage_uses_signed_exposure_for_an_offsetting_book():
    envelope = {"max_gross_leverage": 3.0, "max_net_leverage": 0.5}
    book = [{"symbol": "LONG", "quantity": 1, "notional": 100.0}]
    proposed = {"symbol": "SHORT", "quantity": -1, "notional": -100.0}

    verdict = evaluate_risk(envelope, book, proposed, portfolio_value=100.0)

    assert verdict["passed"] is True
    assert set(verdict["checked"]) >= {
        "max_gross_leverage", "max_net_leverage"
    }
```

Also test negative net exposure reports a positive ratio and unknown exposure disables both leverage checks.

- [ ] **Step 2: Verify red state**

Run: `cd algolens-api && python -m pytest tests/test_position_edit.py -q`

Expected: offsetting book reports 2.0x net and fails.

- [ ] **Step 3: Implement**

```python
def _gross_notional(book):
    values = [position.get("notional") for position in book]
    if any(value is None for value in values):
        return None
    return sum(abs(float(value)) for value in values)


def _net_notional(book):
    values = [position.get("notional") for position in book]
    if any(value is None for value in values):
        return None
    return sum(float(value) for value in values)
```

Use gross for gross limits and `abs(net)` for net limits.

- [ ] **Step 4: Verify and commit**

Run tests, then:
`git add algolens-api/algolens/domain/portfolio/position_edit.py algolens-api/tests/test_position_edit.py && git commit -m "fix: calculate signed net leverage"`

---

### Task 6: Scope AlgoLens Audit Writes and Reads by Portfolio

**Repository:** AlgoLens

**Files:**
- Modify: `algolens-api/algolens/application/portfolio/ports.py`
- Modify: `algolens-api/algolens/application/portfolio/use_cases.py:686-700`
- Modify: `algolens-api/algolens/infrastructure/portfolio/repositories.py:843-885`
- Modify: `algolens-api/algolens/adapters/http/portfolio.py:465-480`
- Modify: `algolens-api/algolens/infrastructure/db/schema_contract.py:167-180`
- Modify: `algolens-api/tests/{test_position_edit.py,test_position_edit_routes.py,test_schema_contract.py}`

**Interfaces:**
- `fetch_overrides(strategy_type, portfolio_id, limit=100)`
- `ListPositionOverrides.execute(strategy_id, portfolio_id, limit=100)`
- `GET /portfolio/overrides/<strategy_id>?portfolio_id=<book>`

- [ ] **Step 1: Write failing use-case and route tests**

Require the selected portfolio to reach the reader; missing query parameter returns stable 400; non-member book is rejected; subscriber remains 403.

```python
def test_overrides_are_scoped_to_the_selected_book():
    reader = _Reader()
    ListPositionOverrides(_Registry(_STRATEGY), reader).execute(
        "trendfollowing", "INVESTOR_A"
    )
    assert reader.override_scope == (
        "LIVE_TREND_FOLLOWING", "INVESTOR_A", 100
    )
```

- [ ] **Step 2: Add failing SQL/schema assertions**

Require the audit insert to include `portfolio_id`. Require history SQL to filter:

```sql
WHERE COALESCE(o.portfolio_id, legacy.portfolio_id) = %s
  AND o.strategy_id = %s
```

Update schema tests to require `portfolio_id` in reads/writes.

- [ ] **Step 3: Verify red state**

Run: `cd algolens-api && python -m pytest tests/test_position_edit.py tests/test_position_edit_routes.py tests/test_schema_contract.py -q`

- [ ] **Step 4: Implement use case, adapter, and repository**

Resolve/validate the book with strategy membership, require the route query parameter, insert `portfolio_id` atomically with the QT update, and query direct or uniquely inferred legacy scope with the composite filter.

- [ ] **Step 5: Verify and commit**

Run the focused backend tests, then:
`git add algolens-api/algolens/application/portfolio/ports.py algolens-api/algolens/application/portfolio/use_cases.py algolens-api/algolens/infrastructure/portfolio/repositories.py algolens-api/algolens/adapters/http/portfolio.py algolens-api/algolens/infrastructure/db/schema_contract.py algolens-api/tests/test_position_edit.py algolens-api/tests/test_position_edit_routes.py algolens-api/tests/test_schema_contract.py && git commit -m "fix: scope position override history by book"`

---

### Task 7: Gate Override History by Role and Selected Book

**Repository:** AlgoLens

**Files:**
- Modify: `algolens-frontend/src/infrastructure/api/portfolioApi.ts:465-470`
- Modify: `algolens-frontend/src/components/OverrideHistory.tsx:16-40`
- Modify: `algolens-frontend/src/components/StrategyDetail.tsx:1-25,450-467`
- Modify: `algolens-frontend/src/components/StrategyDetail.test.tsx`

**Interfaces:**
- `getPositionOverrides(strategyId: string, portfolioId: string)`
- `OverrideHistoryProps = {strategyId: string; portfolioId: string}`
- `isInternalRole(user?.role)` controls whether history is mounted.

- [ ] **Step 1: Write failing component tests**

Mock `useAuth` and render the history mock's props.

```tsx
vi.mock('./OverrideHistory', () => ({
  OverrideHistory: (p: { strategyId: string; portfolioId: string }) => (
    <div data-testid="override-history">
      {p.strategyId}:{p.portfolioId}
    </div>
  ),
}));
```

Require no element for `subscriber_individual`, the correct element for `general_member`, and the new selected book after switching.

- [ ] **Step 2: Verify red state**

Run: `cd algolens-frontend && npm test -- --run src/components/StrategyDetail.test.tsx`

Expected: subscriber test fails and book prop is absent.

- [ ] **Step 3: Implement gate and scoped API**

```tsx
const { user } = useAuth();
const canReadOverrideHistory = isInternalRole(user?.role);
const shownPortfolioId = shown.portfolio_id ?? book;
```

Render only when internal and `shownPortfolioId` exists. Encode portfolio as a query parameter in the API request.

- [ ] **Step 4: Verify and commit**

Run `npm test -- --run src/components/StrategyDetail.test.tsx` and `npm run typecheck`. Return to repo root, then:
`git add algolens-frontend/src/infrastructure/api/portfolioApi.ts algolens-frontend/src/components/OverrideHistory.tsx algolens-frontend/src/components/StrategyDetail.tsx algolens-frontend/src/components/StrategyDetail.test.tsx && git commit -m "fix: gate override history by role and book"`

---

### Task 8: Conduct the Read-only Production DB Audit

**Repository:** Trade Ngin docs; production PostgreSQL read-only session

**Files:**
- Create: `docs/audits/2026-09-18-qt-platform-production-db-audit.md`
- Modify: `docs/superpowers/specs/2026-09-18-qt-effective-position-reporting-design.md`

**Interfaces:** Produces sanitized schema/count evidence; produces no DB state.

- [ ] **Step 1: Prove read-only posture**

Use the existing approved production client. Execute only:

```sql
BEGIN READ ONLY;
SELECT current_database(), current_user,
       current_setting('transaction_read_only') AS transaction_read_only,
       current_setting('default_transaction_read_only') AS default_read_only;
```

Stop if `transaction_read_only` is not `on`.

- [ ] **Step 2: Audit schema facts**

Use `SELECT` against `information_schema.columns`, `pg_indexes`, `pg_constraint`, and `pg_rules` for positions, overrides, and legacy scope. Record types, nullability, keys, checks, indexes, and append-only rules.

- [ ] **Step 3: Audit position integrity**

Use grouped counts only for stream values, duplicate full keys, latest system/QT coverage, missing QT evidence, QT zero closures, and summarized QT/system divergence. Scope with the complete key. Redact or hash identifiers.

- [ ] **Step 4: Classify legacy overrides**

Count rows with exactly one, zero, and multiple candidate portfolios by strategy/symbol and available date evidence. Do not insert mappings or update rows.

- [ ] **Step 5: End and document**

Run `ROLLBACK;`. Write the evidence matrix using only `done`, `partial`, `missing`, `blocked`, or `not verifiable`. Include UTC time and exact repo SHAs, no sensitive values. Link the audit from the design spec.

- [ ] **Step 6: Commit**

Run: `git add docs/audits/2026-09-18-qt-platform-production-db-audit.md docs/superpowers/specs/2026-09-18-qt-effective-position-reporting-design.md && git commit -m "docs: record read-only QT production audit"`

---

### Task 9: Cross-repository Verification and Review

**Repositories:** Trade Ngin and AlgoLens

- [ ] **Step 1: Trade Ngin focused checks**

Run:
- `make build`
- `./build/bin/trade_ngin_tests --gtest_filter='LivePortfolioHelpers.*QtReport*:InstrumentRegistryTest.*:CSVExporter*'`
- `bash migrations/test_012_position_overrides_portfolio_scope.sh`
- `git diff --check origin/qt-platform-preview...HEAD`

- [ ] **Step 2: AlgoLens focused checks**

Run:
- `cd algolens-api && python -m pytest tests/test_position_edit.py tests/test_position_edit_routes.py tests/test_schema_contract.py -q`
- `cd ../algolens-frontend && npm test -- --run src/components/StrategyDetail.test.tsx`
- `npm run typecheck`

- [ ] **Step 3: Broader suites**

Trade Ngin full build/tests are a heavy workload; use the configured remote `tmux` workflow if local artifacts are not warm. Run `make test` in Trade Ngin and `make test && make build` in AlgoLens. Record environmental skips as skips, not passes.

- [ ] **Step 4: Requirement review**

Confirm no email-template diff; shared QT snapshot for email/CSV; zero and fail-closed tests; both symbol load orders; signed net leverage; book-scoped audits; no subscriber history request; read-only production audit; and no production migration.

- [ ] **Step 5: Request code review**

Invoke `superpowers:requesting-code-review` on both final diffs. Fix accepted high/medium findings with regression tests and atomic commits.

- [ ] **Step 6: Capture handoff state**

Run `git status --short --branch` and `git log --oneline origin/qt-platform-preview..HEAD` in both repos. Report exact commits and tests. Do not push, merge, deploy, or apply production migrations without separate authorization.
