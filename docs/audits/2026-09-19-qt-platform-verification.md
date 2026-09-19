# QT platform verification checkpoint

**Checkpoint:** 2026-09-19 (America/New_York)

**Release verdict:** local QT reporting gates pass; production evidence remains blocked. No production connection, query, migration, or write was attempted.

## Source under test

- Trade Ngin: branch `codex/qt-effective-position-reporting`, commit `95db40a`.
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
| Approved production read-only audit | blocked | No approved production PostgreSQL service/profile or credential source exists in the scoped local environment | Zero production connections and zero production SQL statements were attempted |

## Defects found and fixed during verification

1. Migration 012 was documented as idempotent but failed on its second application. PostgreSQL rejects `INSERT ... ON CONFLICT` on the append-only companion relation after its rules exist. The migration now serializes inference writes and excludes existing mappings explicitly. The real PostgreSQL harness changed from 20 pass / 1 fail to 21 pass / 0 fail.
2. CMake always preferred pkg-config libpqxx 7.8 even when a compatible package-provided target was installed. That version cannot compile the repository's `pqxx::params` calls. Package-target-first discovery now produces a complete build while preserving pkg-config fallback.
3. `test_credential_store.cpp` included GoogleMock without using it. Removing the unused include avoids imposing a false dependency on that test.
4. `StrategyMetrics` left all 17 scalar members uninitialized. Earlier heap activity therefore changed the starting P&L and trade-count values, which made `BaseStrategyTest.CheckRiskLimits_FailsOnMaxDrawdown` fail only in the monolithic run. The test then used a non-fatal assertion and dereferenced the absent error on its next line, causing the reported segmentation fault immediately rather than in a later suite. Every metric now has an explicit zero default, a sentinel-backed regression test proves default construction overwrites dirty storage, and the risk test uses fatal assertions before accessing an error. The regression was observed failing before the production fix and passing afterward.

## Production read-only audit status

The existing production audit remains authoritative: `docs/audits/2026-09-18-qt-platform-production-db-audit.md`.

The required first statements are:

```sql
BEGIN READ ONLY;
SELECT current_database(), current_user,
       current_setting('transaction_read_only') AS transaction_read_only,
       current_setting('default_transaction_read_only') AS default_read_only;
```

The audit must stop unless `transaction_read_only` is `on`. After that gate, only catalog and aggregate `SELECT` statements are permitted, followed by `ROLLBACK`. No raw investor identifiers, quantities, reasons, credentials, or position payloads may be copied into documentation.

To unblock this gate, provide the existing approved production PostgreSQL service/profile or approved connection procedure. Do not provide credentials in chat; identify the approved local profile, secret manager command, bastion procedure, or operator-run client path.

## Release interpretation

- The complete C++ build, the 1,277-test monolithic C++ suite, and the QT-specific C++/SQL/PostgreSQL behavior are verified locally.
- The migration and rollback behavior are verified on disposable PostgreSQL, not production.
- The daily report's existing email content/template remains unchanged; the verified code supplies a captured QT position snapshot as the position-number source.
- Production compatibility and production data integrity are not yet verified, so this checkpoint does not authorize deployment or production migration.
