# QT reporting: cross-repository revalidation

Checkpoint: **2026-09-21 (UTC)**. This supersedes the September 19 readiness checkpoint, without replacing its historical evidence.

## Plain-language verdict

The local implementation has been verified from the QT edit through PostgreSQL into the existing CSV and Daily Trading Report renderer. Additional defects found during this review have been corrected locally. **Production is not ready for this release.** Its database is missing the scoped-audit migration and all QT position evidence, and its newest system positions are dated August 5. No production repair, deployment, seeding, or email delivery was attempted.

The intended behavior remains: the QT team's effective position quantities feed the report. The email template, wording, subject, sender, recipients, schedule, and unrelated metrics are unchanged. A zero quantity remains in the database as closure evidence but is omitted from the report's open-position rows. Missing QT evidence blocks reporting instead of substituting system proposals.

## Scope and source

- Local Trade Ngin branch: `codex/qt-effective-position-reporting`, starting at `7df5ad7`.
- Local AlgoLens branch: `codex/qt-effective-position-reporting`, starting at `ccd0571ad4b428b45c8e40de964b0ba81eca99be`.
- Verified Trade Ngin code/test commit: `5ee11f57ff4a5f2c7020d3ad34751c588399f2d6`.
- Verified AlgoLens code/test/demo commit: `e4469170f07e80b4319929ce85017cb7c1fb8148`.
- Both working trees were clean before this revalidation. No pull, push, merge, or deployment was performed.
- The earlier legacy C++ fix remains in place: initialized strategy metrics and fatal assertions prevent the order-dependent failure and null dereference.
- Read-only database fingerprints match the prior audit: `db#dcd5f3f8`, `role#e8a48653`. These fingerprints identify the inspected endpoint, not a deployed source version.

## Additional fixes

1. **CSV closure handling.** The snapshot correctly removed zero positions, but the CSV exporter could add them back from a strategy's complete instrument universe. The daily current-positions export now explicitly uses snapshot-only rows. Other export callers retain their existing default behavior. A regression uses a real strategy universe containing a closed instrument.
2. **Concurrent first edits.** Two writers creating the same absent QT position could both record an empty prior state. The repository now reserves the complete position identity through PostgreSQL's unique index before reading the prior state. A contending writer then locks and reads the winner's committed value. The position change and audit insertion remain one transaction.
3. **Invalid numeric inputs.** NaN, infinity, and integers too large for finite numeric conversion are rejected with stable HTTP 400 validation codes before a write is attempted. This avoids accepting invalid input and failing later in conversion or persistence; it is not evidence that production contained such values.
4. **Real database test coverage.** The older position-write integration fixture was missing migration-012 fields. It now models that contract. Additional PostgreSQL tests cover book separation, explicit zero closures, concurrent new identities, and atomic rollback when audit insertion fails.
5. **Local demo schema.** The complete PostgreSQL integration run found that the demo seed also lacked the migration-012 schema. The seed now includes portfolio scope, the companion relation, the scoped index/constraint, and append-only rules. This changes only local demo setup, not any production database.

The numeric tests were observed failing before the validation fix (16 failures) and passing afterward. The concurrent-new-identity regression reproduced two empty `before_state` values before the repository fix, then verified exactly one creation and one audit of the previous writer's value. Temporarily restoring the original CSV condition caused the new strict-snapshot test to fail because the closed instrument reappeared; restoring the fix returned the full suite to green. The demo-seed schema contract failed before its local correction.

## Verification results

| Check | Fresh result | What it establishes |
|---|---|---|
| Complete C++ Debug build | Passed | Shared library, live/backtest apps, tools, and test executable build together; the explicit test-only probe also builds |
| Full monolithic C++ executable | **1,278 passed, zero skipped**, final restored run 26.513 seconds | Original one-process order, including the legacy BaseStrategy fix and new strict CSV regression |
| Python SQL contract checks | **7 passed** | QT seed/snapshot SQL expectations |
| Migration 012 on disposable PostgreSQL 16 | **21 passed, zero failed** | Forward, repeat, append-only scope, and safe rollback behavior; not a production migration |
| Complete AlgoLens backend with disposable PostgreSQL | **317 passed, zero skipped/deselected**, 20.07 seconds | All unit/HTTP/schema/integration tests after the demo-seed fix, including both existing-row and new-row audit-failure rollback |
| AlgoLens frontend | **179 tests passed**, type-check and production build passed | Selected-book/role UI and frontend compilation remain healthy |
| Real cross-repository rendered report harness | **18 checks passed** on the final restored build | AlgoLens edit -> PostgreSQL -> C++ snapshot -> CSV and existing email HTML; no email sent |
| Independent reviews | Approved, no outstanding actionable findings | Trade Ngin/report harness, AlgoLens concurrency/numeric fixes, and demo-seed schema protection |
| Production audit | Completed, **release blocked** | Live schema/data evidence only; no deployed-runtime or delivery claim |

Heavy remote validation was attempted through the configured SSH host, but it was unreachable. Builds ran in local Ubuntu WSL using the existing dependencies instead; no source pull was needed. The final build directory is `/home/devcontainers/qt-validation-20260921`. The C++ suite ran from an isolated test directory with a symlink to the tracked `config_template`, not from a directory that loads the production `.env`. An earlier build-directory run skipped one config-discovery test; the final correctly configured full run skipped none. No test was disabled, reordered, or weakened.

The preliminary backend offline run passed 282 tests, skipped four database tests, and deselected 30 marked integration tests. Those are explicitly not a complete-backend pass; the final database-enabled result above supersedes them. The final backend run emitted 108 non-failing warnings from SQLite's deprecated date adapters and a short, test-only JWT signing key. That test key is not evidence about any production key. The frontend build emitted a non-failing large-chunk warning.

Final evidence retained locally (ignored, not committed):

- Trade Ngin `.cache/restored-full-build.log`, `.cache/restored-full-suite.log`, `.cache/csv-mutation-red.log`, and `.cache/qt-pipeline-final.log`; the pipeline harness also retains synthetic CSV/HTML and probe logs under `.cache/qt-pipeline-evidence/`.
- AlgoLens `algolens-api/.cache/backend-integration-seedfix-31fe082a-8556-4f06-bcad-efb55c92e50b.log` contains the full final backend output. Its UUID PostgreSQL container was confirmed removed after the run.
- Sanitized command/result summaries for the earlier SQL/migration/frontend runs are retained as Trade Ngin `.cache/sql-contract-7.log`, `.cache/migration-012-21.log`, and AlgoLens `.cache/frontend-validation-final.log`. These summaries are not raw execution transcripts.

Both repositories' final source changes passed independent review and `git diff --check`. Credentials and generated validation artifacts were excluded from commits. The email sender implementation/interface have no diff against `origin/qt-platform-preview`; no production binaries, configuration, recipients, or schedules were modified.

## What the cross-repository test actually exercises

`tests/integration/test_qt_report_pipeline.py` creates its own unique, loopback-only PostgreSQL 16 container. It accepts no external database connection string, disables dotenv loading, imports application components without starting Flask, and removes its exact container on exit. Its C++ probe accepts only a restricted loopback test-database connection.

The harness applies the real migrations 004, 005, and 012 to synthetic base tables. It calls the real C++ seed helper, the real AlgoLens edit use case/repository, the strict C++ snapshot loader, the actual CSV exporter, and the existing email HTML renderer. It checks changed quantities, a zero closure, isolation between two books, two strategies sharing a symbol, unchanged system proposals, idempotent reseeding, next-day carry-forward, and missing-evidence/schema failures. The test's UTC report date is also exercised while the C++ process uses a timezone still on the previous local date.

This proves component wiring against disposable PostgreSQL. It does **not** run the live portfolio process, exercise SMTP, send a report, prove the deployed binary version, or establish the health of production scheduling. The date assertion concerns database scope, not a change to existing email subject/date formatting.

Reproduction after installing the normal project build and Python dependencies:

```sh
cmake --build <build-directory> --target qt_report_probe trade_ngin_tests
python tests/integration/test_qt_report_pipeline.py \
  --algolens-root ../algolens-qt \
  --probe <build-directory>/bin/Debug/qt_report_probe \
  --output-dir .cache/qt-pipeline-evidence
```

Use a local Linux Docker daemon with `postgres:16-alpine` available. Generated artifacts are synthetic and live under a fresh UUID directory; no saved production connection material is passed to the probe.

## Fresh production read-only evidence

Every September 21 production session forced `default_transaction_read_only=on`, a 30-second statement timeout, and a 5-second lock timeout at connection startup. It then entered `BEGIN READ ONLY`, verified the read-only posture, executed catalog or aggregate `SELECT` statements, and ended with `ROLLBACK`. No DDL, DML, migration, seed, test write, stored procedure, or application startup ran against production.

The credential itself remains a superuser with broad write privileges. Session controls provided the read-only boundary; the account is **not** a read-only role. A dedicated read-only audit account remains an operational recommendation, not a change made by this task.

The actual AlgoLens schema-contract checker was run locally against catalog-column results captured read-only at **2026-09-21T20:10:53.591536Z**. Of its 13 required tables, 12 exist. It reported exactly two declared table/column gaps:

- `trading.position_overrides.portfolio_id` is absent.
- `trading.position_override_legacy_scopes` is absent.

Catalog checks also confirm the related migration-012 constraint/index/companion rules are absent. Passing the other declared column checks does not certify every type, index, deployment setting, or runtime dependency.

| Production observation | Result |
|---|---:|
| Total positions / system rows | 3,803 / 3,803 |
| QT rows / override rows | 0 / 0 |
| Duplicate complete position identities | 0 |
| Latest system scopes / symbol keys | 9 / 64 |
| Scopes / keys missing matching QT evidence | 9 / 64 |
| Legacy override candidates with zero / one / multiple books | 0 / 0 / 0 |
| Newest system position date | 2026-08-05 |
| Newest system `last_update` | 2026-08-05 00:00:00+00 |
| Registry rows with `is_active=true` | 4 |

The six-part positions primary key is intact. The stream constraint permits `system` and `qt`. Zero overrides means there is no current legacy audit history to infer; zero QT rows means effective quantities and closures cannot be verified in production.

### Additional producer/consumer linkage checks

A scope is `(portfolio_id, strategy_id, strategy_name)` at its latest system date. Registry matching requires matching `strategy_type`, `is_active=true`, and either the primary portfolio or a book membership. It does **not** require `lifecycle='live'`; these counts must not be described as a confirmed inventory of currently running investor reports.

| Aggregate linkage | All latest scopes | Active-flag registry-matched subset |
|---|---:|---:|
| Scopes examined | 9 | 5 |
| Scopes without a risk envelope | 9 | 5 |
| Scopes without a same-date, same-book, same-strategy live result | 4 | 1 |
| Distinct nonzero-position symbols | 30 | 23 |
| Symbols without matching contract metadata | 8 | 1 |
| Symbols without any exact-symbol daily OHLCV row | 8 | 1 |

Four of the nine scopes have no active-flag registry match. The broad set can include historical or retired scopes. The metadata check matches the symbol root against Databento or IB symbols; the price check establishes existence, **not freshness or a price on the report date**. These aggregate gaps require owner review before claiming end-to-end production readiness; they do not alone prove every missing symbol is an erroneous live instrument.

Missing risk envelopes are significant: current AlgoLens behavior records risk as not evaluated when no envelope is available; absence is not proof that an edit was checked and safe. The August 5 position date, 47 days before this checkpoint, also prevents a claim that current daily ingestion is working. This audit cannot identify the deployment/scheduler cause from database aggregates alone.

## What remains, and why it was not done

The remaining work requires a separately authorized production change process:

1. Apply migration 012 and verify its scoped-audit schema before deploying dependent AlgoLens code.
2. Deploy compatible Trade Ngin and AlgoLens versions through the normal release process. Confirm the intended live strategy/book configuration and investigate the stale system-position producer or scheduler.
3. Have the normal producer publish current system/QT snapshots and risk envelopes. Resolve applicable registry, live-result, instrument-metadata, and market-data gaps. Do not manufacture investor positions or audit entries as a test.
4. Repeat the read-only audit for the intended report scopes. Require schema compatibility, current data, complete QT evidence, and the required data dependencies.
5. Validate an approved staging/non-investor report and email delivery before investor distribution. No email was sent in this task.

These are release prerequisites, **not permission to perform them**. Under the user's read-only production boundary, leaving production untouched is the correct stopping point. Local verification can establish code behavior; it cannot honestly turn missing production schema/data into a working deployed pipeline.
