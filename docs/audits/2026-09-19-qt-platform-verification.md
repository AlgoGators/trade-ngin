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
| Full legacy C++ test executable | partial | The complete executable builds, but a monolithic all-tests run is not green | The all-tests process reports an order-dependent `BaseStrategyTest.CheckRiskLimits_FailsOnMaxDrawdown` failure and later segfaults; the BaseStrategy group passes 12/12 in isolation. This is outside the QT-focused gate but must not be represented as a full-suite pass |
| Approved production read-only audit | blocked | No approved production PostgreSQL service/profile or credential source exists in the scoped local environment | Zero production connections and zero production SQL statements were attempted |

## Defects found and fixed during verification

1. Migration 012 was documented as idempotent but failed on its second application. PostgreSQL rejects `INSERT ... ON CONFLICT` on the append-only companion relation after its rules exist. The migration now serializes inference writes and excludes existing mappings explicitly. The real PostgreSQL harness changed from 20 pass / 1 fail to 21 pass / 0 fail.
2. CMake always preferred pkg-config libpqxx 7.8 even when a compatible package-provided target was installed. That version cannot compile the repository's `pqxx::params` calls. Package-target-first discovery now produces a complete build while preserving pkg-config fallback.
3. `test_credential_store.cpp` included GoogleMock without using it. Removing the unused include avoids imposing a false dependency on that test.

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

- The C++ build and the QT-specific C++/SQL/PostgreSQL behavior are verified locally.
- The migration and rollback behavior are verified on disposable PostgreSQL, not production.
- The daily report's existing email content/template remains unchanged; the verified code supplies a captured QT position snapshot as the position-number source.
- Production compatibility and production data integrity are not yet verified, so this checkpoint does not authorize deployment or production migration.
