# Issues #121 and #124 completion audit — 2026-10-01

Branch: `codex/qt-exact-choice-continuation`

Committed base: `e9f34d9` (`feat: schedule live books sequentially`)

Audited state: the committed base plus the current uncommitted implementation in this worktree.

## Verdict

The implementation and its functional completion gates for issues #121 and #124 are complete on
the audited worktree. It is ready for normal code review. It is not yet a mergeable changeset only
because the audited work is uncommitted and remote CI/reviewer approval have not run.

The product rulings applied throughout are:

- issue #123 is out of scope;
- investor books use only the `system` model stream;
- successful investor publication is automatic and atomic with the day's writes.

No Critical or Important correctness finding remains open from the branch review.

## What is now implemented

| Requirement | Final state |
|---|---|
| Multi-strategy equity book | One strategy instance per configured sleeve, deterministic combined strategy ID, owner-only positions/signals/executions, and combined book results. |
| Sleeve and portfolio risk | Sleeve modules run on their own gross books before account-level aggregation; the configured portfolio module sees the complete book. |
| Netting | Gross owner fills remain attributable while one symbol-level account execution is generated. Account cost is allocated back with `netting_adjustment`, including full internal crosses. |
| Daily accounting | T-1 is finalized before T; cumulative realized P&L and costs carry forward; daily/total realized and unrealized values, equity, returns, notional, leverage, and margin reconcile. A missing intervening trading day fails closed. |
| Corporate actions | Class 1/2/3 actions run independently for each `(portfolio_id, combined_strategy_id, strategy_name)` owner. Split, rename, spinoff, and termination state cannot leak between books or owners. Dedup rows participate in the publication transaction. |
| Portfolio identity | Runtime reads, deletes, replacements, priors, anchors, actions, and publications are explicitly portfolio-scoped. The old broad-date/`BASE_PORTFOLIO` compatibility retry was removed. |
| Investor onboarding | Immutable book identity, immutable opening anchors per strategy, idempotent identical onboarding, and atomic refusal of conflicting replay. |
| Investor publication | Registered investor books select `SystemInvestor` mode automatically, reject QT dependencies, publish only `system` rows, return the same UUID for an identical completed day, and refuse changed content. |
| Scheduler | One global lock, one explicit date, deterministic manifest order, and fail-fast execution. |
| AlgoLens boundary | Authentication, current user, active grant, active book, durable publication, and literal `system` filters are all required; unauthorized and unpublished books are non-disclosing. |

## P0-P3 completion evidence

### P0 — exact legacy compatibility

The frozen OLD and final NEW one-sleeve artifacts contain stable, full-row JSON for positions,
executions, signals, live results, equity, metadata, inputs, limits, corporate actions, investor
registration/publication, and trading-day anchors.

The OLD side is the frozen pre-change application and migration-015 house fence; the NEW side is
the audited worktree. Both binaries were built with the same explicit `e9f34d9` runtime identity.
The dump intentionally omits only the database-assigned `created_at` field from immutable QT
publication rows; every economic and identity field is compared exactly. Both files produced this
SHA-256:

`713d67e8f0f66cda013b3a5afac54c483ca22e6a3aeadee01ff8ff21709ce955`

`compare_issues_121_124_dumps.sh` reported `zero differing cells`. The P0 substrate explicitly pins
the frozen fixture's serial and UUID starting points, clock, and runtime build identity, so the
comparison does not depend on earlier test order. A second NEW house run with two onboarded
system-model investor books produced the same hash and zero differing cells, proving that investor
registration does not perturb house output.

### P1 — deterministic multi-sleeve math

The deterministic six-symbol fixture produces `+7` and `-3` gross sleeve trades for every symbol:
gross turnover is `10`, the account order is `+4`, and the complete book equals the per-sleeve sum.
The focused aggregation/netting run passed 8/8 tests.

### P2 — real ten-day live chain

The disposable PostgreSQL test runs two investor books across ten source days from 2026-11-04
through 2026-11-17. It
covers owner-scoped split application, opposing owner trades, one account execution per netted
symbol/day, opening on day two, closing to flat on day three, next-day finalization, automatic
publication, identical latest-day replay, changed-content refusal, and clean reset/replay table
digests.

The database-backed test passed 1/1. The independent SQL ledger passed all 16 invariants, including:

- twenty system-only publications and twenty distinct publication IDs;
- ten distinct published dates per book;
- two independent book IDs and opening anchors;
- four owner-scoped split audit rows;
- four day-two opens and four day-three closes, ending at zero quantity;
- W12 owner-realized residual exactly zero;
- W12 total-P&L identity residual `0.000000000005` (numeric rounding scale).

### P3 — two-book isolation

The same P2/P3 run uses `INVESTOR_P2_A` and `INVESTOR_P2_B` with different capital and the same
combined strategy ID. Priors, positions, executions, action dedup, anchors, live results, and
publications remain portfolio-scoped. The SQL ledger proves distinct immutable identities and no QT
position or publication rows.

## Additional verification

| Gate | Result |
|---|---|
| Frozen OLD all-target build | Passed, 202/202 build steps. |
| NEW all-target native build | Passed before and after the controlled test-identity reconfigure; only existing warnings remain. |
| Full serial CTest | 2243/2243 passed in 92.08 seconds after the final fail-fast capability fix. |
| Real PostgreSQL backtest | 1/1 passed; `+7/-3` sleeves, account net `+4`, and distinct portfolio run IDs. |
| Real PostgreSQL live chain | 1/1 passed across ten days and two books. |
| Independent SQL ledger | 16/16 invariants passed. |
| Accounting and corporate-action focus | 5/5 passed. |
| Investor PostgreSQL transaction focus | 5/5 passed on a disposable clone of the current full schema. |
| Migration 026 | `investor-book onboarding migration: PASS` on an empty owned database. |
| Migration 028 | `strategy-id-width migration: PASS` on an empty owned database. |
| AlgoLens investor unit boundary | 9/9 passed. |
| Connected AlgoLens read | Granted user received `system-investor-book/v1` for 2026-11-04 with two owner positions; foreign user received no book. |
| AlgoLens frontend | 1370/1370 tests, typecheck, and production build passed. |
| AlgoLens backend portable non-integration suite | 2134/2134 passed with `DEV_MODE=false`; the host-only evaluator-isolation file is excluded. |
| Trade-ngin portable Python focus | 91 tests plus 54 subtests passed; one `pidfd_open` test and five `memfd_create` isolation tests are unavailable on this host. |
| Diff hygiene | `git diff --check` passed. |

The full CTest includes the scheduler regression. Migration and PostgreSQL targets are deliberately
excluded from ordinary CTest and were run separately against owned local databases.

## Reproduction commands

The commands below were run from the trade-ngin worktree unless a different directory is shown.
The PostgreSQL DSNs point only at the owned Unix-socket test cluster under `/tmp`.

```bash
env TRADE_NGIN_GIT_SHA=local-qt-controlled cmake -S . -B .superpowers/sdd/2026-09-30-issues-121-124-completion/build
cmake --build .superpowers/sdd/2026-09-30-issues-121-124-completion/build -j2
ctest --test-dir .superpowers/sdd/2026-09-30-issues-121-124-completion/build --output-on-failure -j1

cmake --build .superpowers/sdd/2026-09-30-issues-121-124-completion/build \
  --target equity_multi_backtest_pg_tests equity_multi_live_pg_tests -j2
env TRADE_NGIN_121124_TEST_DSN='host=/tmp/algolens-repair-pg-codexqt121124/socket port=55432 dbname=trade_ngin_121124_p2_test user=postgres ' \
  .superpowers/sdd/2026-09-30-issues-121-124-completion/build/bin/Debug/equity_multi_backtest_pg_tests
env TRADE_NGIN_121124_TEST_DSN='host=/tmp/algolens-repair-pg-codexqt121124/socket port=55432 dbname=trade_ngin_121124_p2_test user=postgres ' \
  .superpowers/sdd/2026-09-30-issues-121-124-completion/build/bin/Debug/equity_multi_live_pg_tests

.superpowers/sdd/2026-09-30-issues-121-124-completion/env/bin/psql \
  -h /tmp/algolens-repair-pg-codexqt121124/socket -p 55432 -U postgres \
  -d trade_ngin_121124_p2_test -v ON_ERROR_STOP=1 \
  -v book_a="'INVESTOR_P2_A'" -v book_b="'INVESTOR_P2_B'" \
  -v strategy_id="'LIVE_EQUITY_ALPHA_BETA'" \
  -v source_day="'2026-11-04'" -v final_day="'2026-11-17'" \
  -f scripts/verification/issues_121_124_invariants.sql

bash scripts/verification/compare_issues_121_124_dumps.sh \
  /tmp/issues-121-124-p0-old-house.dump /tmp/issues-121-124-p0-new-house.dump
bash scripts/verification/compare_issues_121_124_dumps.sh \
  /tmp/issues-121-124-p0-new-house.dump /tmp/issues-121-124-p0-house-with-investors.dump
```

AlgoLens verification used:

```bash
cd /home/john-riley/projects/Algo/.worktrees/AlgoLens/codex-qt-exact-choice-continuation/algolens-api
env DEV_MODE=false .venv/bin/python -m pytest -q tests \
  --ignore=tests/integration --ignore=tests/test_qt_evaluator_process.py
.venv/bin/python -m pytest -q tests/test_system_investor_books.py

cd ../algolens-frontend
npm test -- --reporter=dot
npm run typecheck
npm run build
```

## Review fixes made during the final audit

- Removed the remaining schema-error fallback that retried position writes with a broad date delete
  and `BASE_PORTFOLIO`; schema mismatch now rolls the scoped transaction back.
- Removed a redundant dynamically assembled trading-day gap query. The typed accounting-context
  read already supplies the latest prior day, and the runner requires it to equal the exact previous
  trading day.
- Added `PortfolioManager` teardown unregistration so repeated per-book construction cannot leave a
  stale StateManager identity.
- Added an early completed-publication replay check so a byte-identical investor rerun returns
  without applying corporate actions or T-1 finalization twice.
- Made non-legacy backtest run IDs portfolio-scoped while preserving the exact legacy
  `EQUITY_MR_PORTFOLIO` identity; the two-book database proof exposed the collision.
- Removed storage-generated surrogate `id` values from the investor publication digest while
  retaining economic event identities; reset/replay now produces the same digest.
- Corrected migration 026's runtime fence so house `qt_proposal` positions retain migration-015
  behavior while registered investor scopes still reject every non-system stream.
- Removed the stale hash for the broken pre-fix migration-026 fence from the runtime capability
  allowlist, so an environment that missed the correction fails at capability validation.
- Hardened runtime-fence capability inspection with `search_path=pg_catalog`, preventing a
  synthetic verification clock from changing catalog-expression rendering.
- Rebuilt the P0 fixture as a true house path. The earlier fixture had accidentally registered the
  house as an investor and therefore did not exercise the legacy QT path.
- Extended the database proof to ten genuine trading days and added explicit open/close/flat,
  capital-scaling, idempotency, conflict, and reset/replay assertions.

## Environment limitations (not issue regressions)

Three broader repository groups cannot be made fully green on this host without capabilities or
fixtures outside this branch:

1. One trade-ngin synchronization test requires `os.pidfd_open`, which this Python runtime does not
   expose. Five native-bundle isolation tests require `os.memfd_create`, which is also absent.
2. The 21-test AlgoLens evaluator-isolation module requires the host sealing primitives. The
   remaining 2,134 non-integration backend tests pass with production authorization mode enabled.
3. Database-heavy QT suites require their own dedicated `ALGOLENS_TEST_DB`, historical fixed build
   layout, or separate QT fixture stack. The issue #121/#124 database gates use their own owned
   PostgreSQL databases and all pass.

Some trade-ngin integration modules also depend on an untracked `synthetic_sql_clock`, a historical
absolute validation-build path, or the separate QT AlgoLens fixture stack. Those external fixture
dependencies are not supplied by this branch; the native, migration, disposable-PostgreSQL, and
portable Python gates above cover the changed behavior directly.

## Remaining handoff work

No additional feature code is required for #121 or #124. Before merge:

1. review and commit the audited worktree changes;
2. push the branch and let remote CI run on its supported Linux host;
3. obtain normal code-owner review and merge approval.

No push, merge, deployment, or production database action was performed by this audit.
