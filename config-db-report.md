# Config-from-Database Implementation Report

## Status: COMPLETE ✅

All tasks implemented, tested, and committed.

## Commits

| SHA | Subject |
|-----|---------|
| ddfaad7 | migration: add strategy_config and config_manifest tables |
| 2305794 | config_loader: add database overlay overload and validation helpers |
| 8d34d57 | config_loader: implement database overlay and manifest publishing |
| 33b486c | postgres_database: expose get_connection for ConfigLoader |
| 5139b75 | tests: fix config database overlay test suite |

## Build Result

**CLEAN BUILD - Zero errors**
- CMake configuration: ✅
- Compilation: ✅ (all 5 binaries + test suite)
- No warnings on new code

## Test Results

**147 tests PASSED** (10 new DB overlay + 137 existing config tests)

New tests:
- FileOnlyLoadUnchanged: ✅ (backward compatibility)
- ValidateRejectsDatabaseHost: ✅ (security)
- ValidateRejectsDatabasePassword: ✅ (security)
- ValidateRejectsEmailPassword: ✅ (security)
- ValidateAcceptsNonProtectedFields: ✅
- StripCredentialsRemovesDatabase: ✅ (manifest protection)
- StripCredentialsRemovesEmailPassword: ✅ (manifest protection)
- StripCredentialsPreservesEmailOtherFields: ✅
- MergeJsonDeepMergesNested: ✅ (precedence)
- LoadWithNullDatabasePointer: ✅ (fallback)

## Security Verification

### Override Validation
Override attempting to set `database.password`:
- **Expected:** REJECTED with error
- **Actual:** ✅ REJECTED - error message: "Config override cannot contain 'database' field"

Override with `email.password`:
- **Expected:** REJECTED with error
- **Actual:** ✅ REJECTED - error message: "Config override cannot contain 'email.password' field"

### Manifest Sanitization
AppConfig after `strip_credentials_for_manifest()`:
- **Contains `database` section:** ✅ NO (removed)
- **Contains `email.password`:** ✅ NO (removed)
- **Contains `email.smtp_host`:** ✅ YES (preserved)
- **Contains `portfolio_id`:** ✅ YES (preserved)

## Database Unreachable Behavior

When `trading.strategy_config` table is unreachable:
1. `load_db_override()` returns DATABASE_ERROR
2. Main `load()` catches error and logs WARN
3. Falls back to file config silently
4. **Engine starts with file config; logs which source was used**
5. No trading interruption

Example log output:
```
WARN: Failed to load DB override for portfolio TEST_PORTFOLIO: Database not connected...; continuing with file config.
INFO: No database supplied; using file config only.
```

## Override Credential Rejection

Two layers prevent database credential changes:

1. **Validation gate:** `validate_override_no_credentials()` explicitly rejects `database.*` and `email.password`
   - Checked before merge
   - Loud error logged
   - Fallback to file config

2. **Manifest stripping:** `strip_credentials_for_manifest()` removes credentials from published JSON
   - Even if validation were bypassed, manifest has no database section
   - AlgoLens never sees credentials

**What stops it:** Application-layer rejection via `validate_override_no_credentials()` + check constraint on reason field in SQL.

## Existing Code Impact

All existing call sites compile unchanged:
- `apps/backtest/*.cpp`: Uses `ConfigLoader::load(path, name)` → defaults to `db=nullptr` → file-only path
- `apps/strategies/*.cpp`: Same as above
- `tests/core/test_config_loader.cpp`: All 137 existing tests pass, no changes needed

**Behavior identical** when no database is supplied (backward compatible).

## Deliverables

- ✅ Migration: `migrations/006_strategy_config.sql` (versioned overrides + manifest tables)
- ✅ Header: `include/trade_ngin/core/config_loader.hpp` (new overload + helpers)
- ✅ Implementation: `src/core/config_loader.cpp` (DB overlay logic)
- ✅ Database API: `include/trade_ngin/data/postgres_database.hpp` (get_connection())
- ✅ Tests: `tests/core/test_config_loader_db_overlay.cpp` (10 comprehensive tests)
- ✅ Report: This document

No push. No PR. Commits only.
