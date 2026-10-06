# QT platform verification checkpoint

> Superseded for current readiness by the [September 21 cross-repository revalidation](2026-09-21-qt-pipeline-revalidation.md). The evidence below remains the historical September 19 checkpoint.

**Checkpoint:** 2026-09-19 (America/New_York)

**Release verdict:** local QT reporting gates pass and the production read-only audit is complete, but production is not ready for the reviewed QT reporting release. Migration 012 and QT position evidence are absent. No production migration or write was attempted.

## Source under test

- Trade Ngin: branch `codex/qt-effective-position-reporting`, implementation through commit `a74f1af`.
- AlgoLens: branch `codex/qt-effective-position-reporting`, commit `ccd0571`.
- The daily trading report email template and sender were not changed by this work. The intended behavior change remains limited to the position quantities supplied to the existing CSV/email reporting path.

## Verification matrix

| Gate | Result | Evidence | Notes |
|---|---|---|---|
| Complete Trade Ngin C++ build | pass | CMake/Ninja Debug build completed for the shared library, live portfolio binaries, backtest binaries, benchmark tools, and test binary in Ubuntu WSL | CMake now prefers the package-provided `libpqxx::pqxx` target and falls back to pkg-config, preventing selection of system libpqxx 7.8 for code that uses the newer parameter API |
| QT/report-focused C++ tests | pass | 54 tests from `LivePortfolioHelpers`, `InstrumentRegistryTest`, and `CSVExporterTest` passed | Covers QT seeding/snapshot behavior, fail-closed coverage, stable copies for CSV/email, exact portfolio/strategy/date/stream scope, symbol collisions, contract multipliers, and CSV position helpers |
| SQL contract tests | pass | 7/7 Python tests passed | Covers manual quantity carry-forward, zero closures, missing/new symbols, atomic report snapshots, and rollback locking expectations |
| Migration 012 on real PostgreSQL | pass | 21/21 checks passed on disposable PostgreSQL 16 | Covers forward application, immutable legacy rows, unique/ambiguous inference, new-row enforcement, append-only rules, second application, rollback refusal, lock posture, safe rollback, and repeated safe rollback |
| QT seed lifecycle on real PostgreSQL | pass | 10/10 checks passed on a separate disposable PostgreSQL 16 container | Manual ES quantity remained `99`, NG zero closure remained `0`, system rows were unchanged, three reruns inserted nothing, and the next date carried both manual states |
| Disposable database cleanup | pass | Both harness containers were removed | No production endpoint or data was involved |
| Full legacy C++ test executable | pass | The monolithic executable passed 1,277/1,277 tests three times in one-process mode: 27.485, 26.155, and 27.259 seconds | No test was disabled, filtered, reordered, or weakened. The former order-dependent BaseStrategy failure and immediate segfault are fixed as described below |
| Approved production read-only audit | complete — release blocked | The live database was inspected through catalog and aggregate `SELECT`s in explicitly read-only transactions; every transaction rolled back | Production has 3,803 system rows, zero QT rows, zero overrides, no duplicate full position keys, and no migration-012 schema. The supplied login is a superuser, so session controls—not account privileges—provided the read-only boundary |

## Defects found and fixed during verification

1. Migration 012 was documented as idempotent but failed on its second application. PostgreSQL rejects `INSERT ... ON CONFLICT` on the append-only companion relation after its rules exist. The migration now serializes inference writes and excludes existing mappings explicitly. The real PostgreSQL harness changed from 20 pass / 1 fail to 21 pass / 0 fail.
2. CMake always preferred pkg-config libpqxx 7.8 even when a compatible package-provided target was installed. That version cannot compile the repository's `pqxx::params` calls. Package-target-first discovery now produces a complete build while preserving pkg-config fallback.
3. `test_credential_store.cpp` included GoogleMock without using it. Removing the unused include avoids imposing a false dependency on that test.
4. `StrategyMetrics` left all 17 scalar members uninitialized. Earlier heap activity therefore changed the starting P&L and trade-count values, which made `BaseStrategyTest.CheckRiskLimits_FailsOnMaxDrawdown` fail only in the monolithic run. The test then used a non-fatal assertion and dereferenced the absent error on its next line, causing the reported segmentation fault immediately rather than in a later suite. Every metric now has an explicit zero default, a sentinel-backed regression test proves default construction overwrites dirty storage, and the risk test uses fatal assertions before accessing an error. The regression was observed failing before the production fix and passing afterward.

## Production read-only audit status

The completed production audit is authoritative: `docs/audits/2026-09-18-qt-platform-production-db-audit.md`.

The required first statements are:

```sql
BEGIN READ ONLY;
SELECT current_database(), current_user,
       current_setting('transaction_read_only') AS transaction_read_only,
       current_setting('default_transaction_read_only') AS default_read_only;
```

The initial gate reported `transaction_read_only=on`. Subsequent connections additionally forced the session default to read-only. Only catalog and aggregate `SELECT` statements were executed, followed by `ROLLBACK`. No raw investor identifiers, quantities, reasons, credentials, or position payloads were copied into documentation.

The connected login is not a read-only role: it is a superuser with full audited-table write privileges. A dedicated read-only audit profile remains an operational follow-up even though this session was safely constrained.

## Release interpretation

- The complete C++ build, the 1,277-test monolithic C++ suite, and the QT-specific C++/SQL/PostgreSQL behavior are verified locally.
- The migration and rollback behavior are verified on disposable PostgreSQL, not production.
- The daily report's existing email content/template remains unchanged; the verified code supplies a captured QT position snapshot as the position-number source.
- Production schema and aggregate data were verified read-only and are missing the migration-012 contract and all QT snapshot evidence, so this checkpoint does not authorize deployment or production migration.
