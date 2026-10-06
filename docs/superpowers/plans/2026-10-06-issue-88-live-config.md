# Issue 88 Governed Live Configuration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal:** Complete the agreed engine override and audit workflow without adding a config editor or changing trading behavior absent an approved override.
**Architecture:** C++ owns validation and canonical configuration identities. PostgreSQL stores approved versions and attempt evidence; authenticated AlgoLens endpoints and a small CLI manage versions, while existing read-only views consume versioned publications.
**Tech Stack:** C++20, nlohmann JSON, OpenSSL SHA256, libpqxx/PostgreSQL, Python/Flask, TypeScript/React.
**Spec:** ../specs/2026-10-06-issue-88-live-config-design.md

## Global Constraints
- User decisions Q1–Q11 and the linked spec are binding.
- No production connections/mutations, deployments, broker/email actions, issue comments/closures or pushes.
- Work only in the two issue-88-config-contract worktrees; preserve other worktrees.
- Fail closed on unavailable/invalid overrides; successful no-row selects files.
- Retry latest config only for failed/aborted attempts; completed days immutable; unsafe partial writes require verified recovery.
- No risk assignment removal or protected/unsupported edits. No implicit config authority grants.
- Two people total: authenticated submitter and one distinct current approver.
- Preserve old report readers; backtests file-only. Use fresh meaningful tests and explicit evidence.

## Review Focus
- Config activation racing admission must not change a running attempt (Tasks 2/4).
- Partial T-1 or corporate-action writes before failure must not be hidden by final-transaction rollback (Task 4).
- Revoked/retired/alias identities must not self-approve or activate stale candidates (Task 3).
- JSON pointer escaping, arrays, float canonicalization and coupled risk fields must not bypass policy (Task 1).
- Old reports and unsupported profiles must not be mistaken for newly proven effective configuration (Task 5).

---

### Task 1: Native override contract and complete runtime snapshots

**Files:** Create include/trade_ngin/core/live_config_override.hpp, src/core/live_config_override.cpp, apps/tools/live_config_validate.cpp, tests/core/test_live_config_override.cpp. Modify include/trade_ngin/core/config_loader.hpp, src/core/config_loader.cpp, src/apps/live_portfolio_helpers.cpp, apps/tools/CMakeLists.txt, tests/CMakeLists.txt, tests/core/test_config_loader.cpp, tests/apps/test_live_portfolio_helpers.cpp (locate existing equivalent if named differently).
**Interfaces:** Produce Result<AppConfig> apply_live_config_override(const AppConfig& base, const nlohmann::json& changes); Result<nlohmann::json> validate_live_config_request(const nlohmann::json& request); Result<nlohmann::json> build_runtime_trading_snapshot(const AppConfig&) emits v2. Protocol and hashes as spec. Expose a pure snapshot-to-config parser for the validation tool without credential placeholders.

- [ ] Write tests for conservative 0.3/2.0 unrelated-edit preservation, fractional 12.75, all eligible leaves, unknown/protected/structural paths, no-op/null/arrays, malformed pointer escapes, coupled risk constraints and complete snapshot identity.
- [ ] Run focused tests and observe failures caused by missing contract/v2 fields.
- [ ] Implement minimal native policy and canonical SHA256; enumerate eligibility from actual supported consumer fields, not every serialized numeric key. Default deny unknown fields. Native source handles scalar/array type rules and full parser validation.
- [ ] Add bounded stdin protocol executable, operator-only --export-base --config-root PATH --portfolio KEY, and CMake target; reject duplicate keys and nonfinite/oversize input; redact error values.
- [ ] Run fresh focused native tests/tool protocol tests. Expected all pass; record exact commands/build inputs and outputs.
- [ ] Commit task changes.

### Task 2: Governed version storage and race-safe attempt selection

**Files:** Create migrations/031_live_config_overrides.sql and rollback; include/trade_ngin/data/live_config_selection.hpp; src/data/postgres_live_config.cpp; tests/integration/test_live_config_overrides.py and native probe. Modify include/trade_ngin/data/postgres_database.hpp, src/data/postgres_runtime.cpp and build registration.
**Interfaces:** Consume Task 1 protocol. Produce ConfigSelection {AppConfig config; json receipt;} and Result<ConfigSelection> select_live_configuration(PostgresDatabase&, const AppConfig&, const std::string& engine_build). Receipt has source/version_id/base_sha256/effective_sha256. Extend admission with immutable receipt and active-version comparison under scope locks. SQL table names and transaction ownership are documented in migration and shared with Task 3.

- [ ] Write disposable-PostgreSQL tests for no-row file selection, lookup/schema failure refusal, invalid/stale approved row refusal, immutable candidates, one active pointer, activation race, source/book isolation and publisher cannot activate.
- [ ] Observe missing migration/selection behavior failures using owned disposable database only.
- [ ] Implement version/activation/attempt tables and constraints, immutable audit triggers, hashes/source identity and scoped locking. Supply additive config capability grants support without assigning users.
- [ ] Implement native read/validation selection with no fallback on errors; baseline and exact validated effective snapshot must match stored approved data. Ensure admission verifies the selection still current.
- [ ] Run migration apply/reapply/rollback and DB/native tests. Expected explicit safety assertions pass.
- [ ] Commit task changes and record the precise SQL interface for application task.

### Task 3: Authenticated submit/approve workflow and administrative CLI

**Files:** AlgoLens: create application/live_config.py, infrastructure/portfolio/live_config.py, infrastructure/config/live_config.py, adapters/http/live_config.py, scripts/live_config_admin.py and corresponding tests. Modify domain/identity/capabilities.py, capability registration/dependencies/app_factory, infrastructure/config/runtime_control.py, application/runtime_control.py, deployment/database/qt_runtime_role_contract.sql and relevant rollback/role-contract tests. Paths relative to algolens-api/algolens unless prefixed scripts/deployment.
**Interfaces:** Consume native live-config-validation/v1 and Task 2 SQL schema. HTTP /portfolio/strategies/<id>/config/preview, /config/requests, /config/requests/<id>/approve, /config status. Bodies strictly contain portfolio_id, changes, reason, expected_active_version as applicable. Identity exclusively from authenticated session. Native validator path/hash/build are provisioned server-side. Scope loader supplies current effective v2 snapshot for subsequent runtime approval.

- [ ] Write unit/HTTP/DB tests for authenticated scope, explicit capabilities, one distinct approver, alias/self/retired/revoked identity, stale baseline/active version, malformed native reply, expiry/change of provisioned validator, fail-closed subprocess and CLI CSRF/secret handling.
- [ ] Observe expected failures before implementation.
- [ ] Implement small pure validation/application layer plus repository atomic activation and current-authority locks. Add config_submit/config_approve backed edit_config/approve_config without granting existing users automatically.
- [ ] Implement CLI as thin authenticated HTTP client; no direct DB actor assertions and no UI. Keep config activation separate from execution permission.
- [ ] Extend runtime snapshot validation for v2 and active selection; retain legacy historical reads. New governed approvals require complete v2 snapshot.
- [ ] Run API/CLI/authority/schema tests; expected pass, no externally sent requests except owned loopback test server.
- [ ] Commit AlgoLens task changes; record native/SQL interoperability evidence.

### Task 4: Live-run integration and safe changed-config retries

**Files:** Trade: apps/strategies/live_portfolio_runner.cpp, apps/strategies/live_equity_mean_reversion.cpp, src/data/postgres_runtime.cpp, src/apps/book_tail.cpp, src/data/postgres_database.cpp, src/data/postgres_database_extensions.cpp; add tests/integration/test_live_config_retry.py; extend runtime_publication_probe.
**Interfaces:** Consume ConfigSelection and version tables. Admission receives immutable receipt; publication writes configuration_selection with actual consumption. Durable retry-safety marker precedes independently committed writes. Recovery verification compares restored state to recorded pre-write evidence; no plain Boolean bypass.

- [ ] Write failure-injection tests at admission, T-1 update, corporate-action/write boundaries and final publish: safe retry uses new version; unsafe changed retry returns config_retry_recovery_required; completed date rejects changes; unresolved running attempt refuses.
- [ ] Observe expected pre-fix failures.
- [ ] Select/validate override before configurable work or mutation in both live profiles. Freeze receipt per attempt; concurrent active change aborts admission and retry reselects. In-flight change never changes that attempt.
- [ ] Fence all independently committed financial mutation paths before their transaction; preserve existing calculations and transaction contents. A failure leaves marker; success seals it with publication. Record verified recovery through restricted administrative/recovery interface only when state proof matches.
- [ ] Publish final config selection/effective identity atomically with existing results and consumption. Avoid a second success manifest.
- [ ] Run native and disposable DB failure-injection/parity tests, including no-override behavior. Expected all acceptance assertions pass.
- [ ] Commit task changes.

### Task 5: Versioned inspection readers and cross-repository acceptance

**Files:** Trade src/core/config_loader.cpp plus fixtures/tests. AlgoLens domain/portfolio/configuration_inspection.py, application/portfolio configuration reader as needed, frontend src/domain/portfolio/configurationInspection.ts and matching API/parser/panel tests. Create versioned native handoff fixtures and docs/operations/live-config-overrides.md in both repos.
**Interfaces:** New projection_version 2 and publication_schema_version 4 (futures)/5 (equity) as spec; keep existing actual-consumption schemas. Freeze legacy v1/v2 futures and v3 equity fixture behavior. Scope exact book/attempt/date and selection hashes.

- [ ] Write native producer -> JSONB -> API -> frontend parser acceptance covering futures/equity supported profiles, legacy history, wrong book/hash/version, unsupported profile and no false success at startup.
- [ ] Observe missing new-version reader behavior and stale-path failures.
- [ ] Correct schema-2 field catalog and classifiers; separate historical catalog dispatch from current payload; publish explicit read-only/unsupported metadata. Do not add editing controls.
- [ ] Align release inventory/provisioning for native validator, least-privilege role probes, migrations and safe recovery operational instructions. Avoid automatically applying production grants or changing immutable deployment pins.
- [ ] Run fresh native suite, Python suites/DB integration, frontend tests/typecheck/build and diff checks. Record baseline failures separately, never claim skipped DB tests passed.
- [ ] Commit both repository changes, build combined review packages and run final independent review. Fix load-bearing findings with regression tests.

