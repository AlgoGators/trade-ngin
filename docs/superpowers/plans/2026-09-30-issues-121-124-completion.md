# Issues #121 and #124 Completion Plan

> **Execution:** Implement inline with `executing-plans`, using strict RED→GREEN TDD and the durable SDD ledger.

**Goal:** Complete every agreed completion gate for multi-strategy/multi-portfolio equity books and automatic system-model investor books without changing existing house-book results.

**Authoritative specification:**

- AlgoGators/trade-ngin issue #121, including W1–W12, A1–A7, P0–P3, testing rules, and protected behavior.
- AlgoGators/trade-ngin issue #124.
- Product decisions supplied by the user on 2026-09-30: investor books use only the `system` model/stream and publication is automatic after a successful full run.
- The completion-gate list in the 2026-09-30 branch audit.

**Primary workspace:** `/home/john-riley/projects/Algo/.worktrees/trade-ngin/codex-qt-exact-choice-continuation`

**Related consumer workspace:** `/home/john-riley/projects/Algo/.worktrees/AlgoLens/codex-qt-exact-choice-continuation` when the published-only investor read boundary is implemented.

**Technical context:** C++20, CMake, GoogleTest, libpqxx/PostgreSQL, Bash, SQL migrations, Python/pytest integration checks, and AlgoLens Python/TypeScript tests where required.

## Global constraints

- Never use production data or credentials. Database tests use only `TRADE_NGIN_TEST_DSN` and a disposable UTC database.
- Every live invocation in tests supplies an explicit date. Runners are always serial.
- Existing house books must be byte-identical across every table they write.
- Preserve the legacy one-sleeve equity identity `LIVE_EQUITY_MEAN_REVERSION` / `EQUITY_MEAN_REVERSION`.
- Sleeve books remain gross; only the account execution is netted.
- Investor books read and write `portfolio_type='system'` only and never require or create QT desk state.
- Investor visibility is created automatically only in the same transaction that successfully commits the entire run.
- Empty portfolio identity is an error at production boundaries; it must never silently become `BASE_PORTFOLIO`.
- Initial capital, opening date, portfolio identity, and investor stream are immutable after book onboarding.
- Do not send email from any test or verification run.
- No push, merge to shared branches, deployment, production migration, or external publication without explicit user authorization.

## Completion matrix

| Gate | Required evidence |
|---|---|
| Clean build | Fresh configure and all-target build exit 0 on the final tree |
| Full tests | CTest plus repository Python tests exit 0, serially where database/process state is shared |
| House parity | OLD/NEW UTC ordered dumps have zero differing cells for every house-book table |
| Multi-sleeve equity | Two enabled deterministic sleeves retain separate positions, fills, costs, P&L, and risk assignments; combined rows reconcile |
| Multi-portfolio isolation | Two portfolio IDs using the same strategy key have distinct run IDs and no cross-book read/write/delete/anchor effect |
| System-only investor model | Investor rows and priors use only `system`; no QT proposal, seed, decision, or report state is required |
| Automatic atomic publication | Success creates exactly one durable `(portfolio_id, source_day)` publication; failure creates none |
| Reruns | Same publication digest is idempotent; a conflicting digest fails closed |
| Immutable onboarding | Capital, opening date, identity, and stream cannot be changed after creation; anchor is seeded transactionally |
| Scheduler | One global lock, deterministic order, explicit date, sequential execution, fail-fast, failed-book logging |
| Published-only reads | Investor consumption requires both portfolio authorization and a durable publication; out-of-scope/unpublished data is not returned |
| Final review | Independent whole-branch review has no unresolved Critical or Important findings |

## Rulings

- **Migration filename:** this continuation line already owns migrations 013–025. Use 026 for the investor-book/publication schema, keeping the SQL self-contained so review can renumber it without changing behavior. Cost if wrong: a mechanical migration rename and test-fixture update.
- **Integration style:** do not merge the Stage 3 branch wholesale because it deletes continuation/QT files and conflicts broadly. Port the required risk/backtest/netting behavior in coherent slices, preserving both test suites. Cost if wrong: more manual conflict resolution, but no loss of either branch's behavior.
- **Scheduler question:** issue #124 explicitly requires the cron wrapper to loop over books sequentially; implement a deterministic driver with one combined result and fail-fast behavior. Cost if wrong: wrapper contract adjustment, not engine data changes.

## Task 1: Reproducible baseline and integration harness

**Files:**

- Modify: `CMakeLists.txt`, only if required to make the documented dependency path reproducible.
- Create/modify: build/test setup artifacts only when the current documented path cannot configure.
- Create: the SDD ledger under the ignored `.superpowers/sdd/` workspace.

**Steps:**

1. Record branch SHA, merge bases, installed toolchain, and dependency versions in the ledger.
2. Configure a fresh Debug build using the repository-supported dependencies.
3. Build all targets and run the existing CTest/Python baseline.
4. If baseline failures exist, characterize them before feature code; fix only failures that prevent the requested work and preserve a RED→GREEN record.
5. Record the exact baseline commands and results.

**Completion:** A reproducible build/test command exists and baseline failures are either green or explicitly isolated with evidence.

## Task 2: Safe portfolio selection and config containment

**Files:**

- Modify: `include/trade_ngin/apps/live_portfolio_helpers.hpp`
- Modify: `src/apps/live_portfolio_helpers.cpp`
- Modify: `apps/strategies/live_portfolio_runner.cpp`
- Modify: `apps/strategies/live_equity_mean_reversion.cpp`
- Modify/create equity backtest entrypoints as integrated in Task 3.
- Modify: `src/core/config_loader.cpp`
- Modify: `tests/apps/test_live_portfolio_helpers.cpp`
- Modify: `tests/core/test_config_loader.cpp`

**Interface:**

- `PortfolioSelection resolve_portfolio_selection(argv, environment, default_name)` with precedence CLI > environment > default.
- A validated config key permits only lowercase ASCII letters, digits, `_`, and `-`, begins with an alphanumeric character, and is at most 64 bytes.
- Config loading proves the resolved portfolio directory remains under `<base>/portfolios`.

**RED cases:** default behavior, environment fallback, CLI precedence, duplicate flag, missing value, unknown flag, absolute path, separators, `..`, empty value, oversized key, and date/`--send-email` preservation.

**GREEN behavior:** every live and backtest equity/futures entrypoint accepts the same `--portfolio NAME` contract while preserving existing defaults.

## Task 3: Stage 3 equity/backtest/risk foundation

**Files:**

- Add/reconcile: `apps/backtest/bt_equity_mean_reversion.cpp`
- Add/reconcile: `apps/backtest/bt_equity_validation.cpp`
- Add/reconcile: `config_template/portfolios/equity_mr/{portfolio,risk,email}.json`
- Add/reconcile: `include/trade_ngin/risk/*.hpp`, `src/risk/*.cpp`
- Modify: `include/trade_ngin/portfolio/portfolio_manager.hpp`
- Modify: `src/portfolio/portfolio_manager.cpp`
- Modify: CMake target/source lists and focused risk/backtest tests.

**Steps:**

1. Port the Stage 3 `RiskModule` contract, fail-closed factory, portfolio/sleeve assignment, and net-leverage behavior without deleting continuation components.
2. Port the equity backtest and validation runners plus template config.
3. Add failing focused tests for unknown modules, failed gatekeepers, sleeve key mismatch, signed net leverage, and the equity `none` assignment.
4. Implement only the behavior needed by the authoritative Stage 3 contract and make the focused plus existing suites green.

**Completion:** Continuation and Stage 3 risk/backtest behaviors coexist and all relevant focused tests pass.

## Task 4: Backtest correctness and deterministic second sleeve

**Files:**

- Modify: equity/futures backtest runners and `src/backtest/backtest_coordinator.cpp`.
- Modify: PostgreSQL backtest position writers.
- Add: a trivial deterministic equity test strategy and tests.

**RED cases:**

- Backtest config snapshot omits a second enabled strategy.
- Re-running position persistence leaves stale rows.
- A fill is delivered to a strategy that did not generate it.
- Slow-trend futures config cannot dispatch.

**GREEN behavior:** W2, W3, W4, W4b, and W5 hold; futures outputs remain byte-identical where the optional slow strategy is absent.

## Task 5: N-sleeve live equity and fail-closed isolation

**Files:**

- Modify: `apps/strategies/live_equity_mean_reversion.cpp`
- Modify shared portfolio/risk/netting/storage code only where necessary.
- Modify: library callers that silently substitute `BASE_PORTFOLIO`.
- Add focused unit, database, and runner tests.
- Add A1–A7 analysis under `docs/audits/` with exact source/evidence references.

**Behavior:**

- Build one sleeve object per enabled equity strategy.
- Preserve the exact legacy identity for the one-sleeve Mean Reversion book; use the deterministic combined ID otherwise.
- Key sleeve rows by config strategy name; key combined results by combined strategy ID.
- Route signals, fills, positions, realized/unrealized P&L, corporate actions, risk modules, and deletes to the owning sleeve.
- Apply existing account-netting design after per-sleeve risk; retain gross sleeve records and reconcile costs to the net account order.
- Key/import legacy corporate-action state only for the legacy portfolio/sleeve pair.
- Reject empty portfolio IDs at every production persistence/query boundary reached by these runners.

**Completion:** W6, W8, W9, W10, A1–A7, and focused P0–P3 automated invariants pass.

## Task 6: Immutable investor-book onboarding and annualization anchor

**Files:**

- Add: `migrations/026_investor_books_and_publications.sql`
- Modify: database API/header/source.
- Add: migration safety and disposable-PostgreSQL tests.

**Schema/behavior:**

- `trading.investor_books` records validated config key, `portfolio_id`, fixed positive initial capital, opening date, fixed `system` stream, active status, creator, and timestamps.
- Database constraints/triggers reject changes to identity, capital, opening date, and stream.
- Onboarding inserts the matching `strategy_trading_days_metadata` rows in the same transaction and is idempotent only for identical input.
- A conflicting replay fails without partial rows.

**Completion:** onboarding success, identical replay, conflict rollback, and anchor scope tests pass.

## Task 7: System-only automatic atomic publication

**Files:**

- Modify: `include/trade_ngin/data/postgres_database.hpp`
- Modify: `src/data/postgres_runtime.cpp`
- Modify: `include/trade_ngin/apps/book_tail.hpp`
- Modify: `src/apps/book_tail.cpp`
- Modify: live futures/equity runners.
- Add: unit and disposable-PostgreSQL publication tests.

**Interface:**

- Add an explicit publication/execution mode distinguishing `QtHouse` from `SystemInvestor`.
- `SystemInvestor` loads prior positions from `system`, requires fresh system evidence for every member, never seeds/reads/writes QT proposal or QT model-publication state, and automatically records investor visibility.
- `trading.investor_book_publications` is unique on `(portfolio_id, source_day)` and stores publication ID, strategy ID, immutable digest, producer identity, and publication timestamp.

**RED cases:** success without visibility row, partial run visibility, QT dependency in system mode, duplicate same digest, duplicate conflicting digest, wrong portfolio/date, and transaction failure after queued writes.

**GREEN behavior:** same-digest replay returns the existing publication; conflicting replay fails closed; the visibility row commits atomically with all daily rows; failure leaves neither rows nor visibility.

## Task 8: Deterministic sequential multi-book scheduler

**Files:**

- Modify: `scripts/run_live_portfolio.sh` or replace it with a shared driver while retaining the deployed entrypoint.
- Modify: cron/container wiring only as required.
- Add: shell behavior tests using real executable fixtures, not source-text assertions.

**Behavior:**

- One global lock covers the whole cycle.
- A validated manifest supplies ordered runner/config pairs.
- Every invocation uses the same explicit date and `--portfolio`.
- Runs are strictly sequential.
- First nonzero exit stops the cycle, logs the failed portfolio, and propagates the exit code.
- Tests never enable email.

## Task 9: Published-only investor consumption boundary

**Files:**

- Trade Ngin: add a stable published-day query/view/API contract if required by the consumer.
- AlgoLens continuation: add portfolio authorization plus publication existence to investor reads; return non-disclosing not-found behavior for unauthorized or unpublished data.
- Add backend query/service tests and frontend behavior tests only where the read flow changes.

**Completion:** an investor sees only their own published system-model days; another portfolio, an unpublished date, QT-only data, and an unknown identity return no data.

## Task 10: P0–P3 and final completion audit

**Artifacts:**

- Add reset, ordered-dump, SQL-invariant, and comparison scripts under `scripts/verification/`.
- Add a dated audit report under `docs/audits/` containing commands, input snapshot identity, declared allowed differences, and results.

**Verification:**

1. Fresh OLD and NEW all-target builds.
2. Full serial CTest and Python/AlgoLens suites.
3. P0 legacy one-sleeve parity at tolerance zero.
4. P1 multi-strategy backtest and P2 multi-sleeve live chain, with hand-derived expectations.
5. P3 two portfolios sharing the same strategy key, proving read/write/delete/corporate-action/anchor isolation and distinct run IDs.
6. House cycle with and without two investor books, byte-identical for all house rows.
7. Different-capital investor positions scale within rounding.
8. Automatic publication success, failure rollback, same-digest idempotency, and conflicting-digest refusal.
9. Scheduler ordering/lock/fail-fast behavior.
10. Published-only consumer authorization.
11. Independent whole-branch review; fix all Critical and Important findings with RED→GREEN tests.

**Completion:** Every row in the completion matrix has direct, fresh evidence. No gate may be inferred from a narrower test.
