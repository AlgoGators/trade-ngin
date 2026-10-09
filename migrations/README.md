# Migrations

Apply in number order. Each `NNN_name.sql` has a `NNN_name_rollback.sql`; where a
`test_NNN_*.sh` exists it applies and rolls back the migration on a throwaway database
(`MIGRATION_TEST_DB` = `PGDATABASE`).

| No. | File | What it does |
|-----|------|--------------|
| 022 | `022_strategy_config.sql` | `trading.strategy_config` (desk settings, versioned, one active row per portfolio); `settings_used`, `published_by`, `published_at` on `trading.live_run_metadata` (QT plan E2) |
