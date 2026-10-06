# Migration 012 Production Rollout Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to execute this plan task-by-task. Never bypass an unchecked production gate.

**Goal:** Apply the existing portfolio-scoped audit migration and compatible application release only after authority, recoverability, and writer coordination are established.

**Architecture:** Migration 012 prepares the audit schema; AlgoLens validates portfolio selection and atomically writes the QT position plus audit record. The reporting release reads QT positions, not audit history. Schema readiness is not report readiness.

**Tech Stack:** PostgreSQL, SQL migrations, Python/Flask, C++ reporting, existing deployment infrastructure.

**Spec:** `../specs/2026-09-18-qt-effective-position-reporting-design.md`; evidence: `../../audits/2026-09-21-qt-pipeline-revalidation.md`.

## Current authorization and outcome

The user explicitly selected **"Keep production read-only; prepare the rollout"** on September 21. This document is preparation, not production execution approval. No migration, deployment, service pause, backup export, position write, or email send has been performed. The unexecuted production steps below are intentionally gated.

The migration is already implemented locally. Production's unchanged schema and the currently unknown deployed application/control state mean that applying SQL by itself is not a safe completion of the rollout.

## Global Constraints

- Current authorization restricts production to read-only. Obtain separate explicit confirmation that production rollout replaces that restriction before any production mutation; ambiguous approval does not bypass this gate.
- No investor emails, position edits, QT seeding, trading-process execution, unrelated migrations, or changes to the Daily Trading Report template/sender/schedule as part of migration application.
- Never write credentials, raw investor records, connection strings, or individual positions into logs or documentation.
- No old audit writer may resume after the new mandatory portfolio constraint is committed.
- Abort on missing backup/recovery evidence, unknown deployment control, or inability to coordinate writers.
- Rollback 012 is allowed only when no new scoped audit rows or inferred legacy-scope rows exist. Never delete evidence to make rollback possible.

## File and responsibility map

- Existing `migrations/012_position_overrides_portfolio_scope.sql`: reviewed forward schema change; no changes planned.
- Existing `migrations/012_position_overrides_portfolio_scope_rollback.sql`: guarded rollback; not a general undo command.
- `migrations/test_012_position_overrides_portfolio_scope.sh`: disposable local PostgreSQL verification, never a production command. Cleanup now uses only the ID returned by a successful container creation, never an unowned requested name.
- New `tests/test_migration_012_harness_safety.py`: runs the actual harness with a controlled Docker boundary to prove failed creation cannot trigger cleanup of another container and owned-container cleanup remains intact.
- Existing AlgoLens `algolens-api/algolens/infrastructure/portfolio/repositories.py`: scoped atomic writer and history reader.
- This plan and the linked audit: sanitized readiness/rollout record.
- Ignored `.cache/prod_migration012_preflight.sh`: aggregate-only production recheck with startup and transaction read-only enforcement and expected endpoint fingerprints.

## Task 1: Recheck code and production prerequisites

- [x] Independently review forward/rollback SQL, old-writer compatibility, historical inference, and the actual AlgoLens implementation. No local code/SQL correctness blocker found for the observed empty audit history; the missing production schema and operational gates still block application compatibility and execution.
- [x] Rerun the migration's disposable PostgreSQL harness and require all checks to pass. Fresh local run: **21 passed, zero failed**, with container removal independently confirmed after exit.
- [x] Run the read-only preflight. Require matching endpoint fingerprints and read-only settings; record schema presence, audit counts, position/QT aggregate counts, and transient activity/lock counts. Completed at `2026-09-21T21:32:42.699353Z`; facts below.
- [ ] Identify the deployed application version, deployment control, and every audit writer from authoritative runtime/release evidence. Also verify the serving application's configured database matches the approved audited endpoint, without exporting secrets. Repository configuration and database access alone do not prove this.
- [ ] Verify an existing backup/restore or point-in-time recovery mechanism, restore-point timestamp, tested restore target/result, and the approved maintenance window. An empty audit table or idle database is not equivalent evidence.
- [ ] Obtain explicit authorization replacing the earlier production-read-only boundary.

### Fresh preflight evidence

The connection forced `default_transaction_read_only=on`, `statement_timeout=30000`, and `lock_timeout=5000`; its transaction opened `BEGIN READ ONLY`, checked both read-only settings and expected endpoint fingerprints, executed aggregate/catalog SELECTs, and ended with `ROLLBACK`.

| Check | Observed result | Interpretation |
|---|---|---|
| Database/role fingerprints | `dcd5f3f8` / `e8a48653` | Same approved audit endpoint, no cleartext connection details retained |
| PostgreSQL server version | 16.14 | Migration tests use the same major version |
| Transaction/default read-only | on / on | This preflight did not authorize writes |
| Portfolio column / companion / scope constraint / scope index | all absent | Migration 012 is not applied |
| Existing override records | 0 | No historical overrides to attribute at this checkpoint |
| Positions / QT positions | 3,803 / 0 | Migration alone cannot make the report ready |
| Latest system-position date | 2026-08-05 | Current daily ingestion remains unverified |
| Existing audit no-update/no-delete rules | 2 | Original audit protections are present |
| Other client sessions / audit-table locks / custom audit triggers | 0 / 0 / 0 | A momentary observation, **not** proof of a maintenance pause or future quiescence |

Recheck immediately before any later approved execution. If override records appear, review their inference candidates and historical limitations before proceeding; do not silently reuse the empty-history assumption. The legacy inference uses all currently stored position rows matching strategy ID and symbol, not an as-of reconstruction of historical ownership.

### Frozen source references

- Trade Ngin starting checkpoint: `488a3613469eb8896ce40fdd609ffe755f751d7d`; verified reporting implementation: `5ee11f57ff4a5f2c7020d3ad34751c588399f2d6`.
- Compatible local AlgoLens implementation: `e4469170f07e80b4319929ce85017cb7c1fb8148`.
- Reviewed local harness-safety fix: `167f3494c84c677bfcf46a86b90d146261761386` (test tooling only; forward/rollback SQL unchanged).
- Forward SQL Git blob: `f70c82bc7c0a697b8fd72bcf484e01908702726f`.
- Rollback SQL Git blob: `ae334ef2b17f754735ab0c5639049d3cc58c95c2`.
- Current checkout forward-file SHA-256: `27b474912c48c6eae7815c615ac68fd11fe5bd18de84c9fe7b0d7269e4cd7f5a`.
- Current checkout rollback-file SHA-256: `17bc9224d35c2fcb327d4e89ebbf5de20274824708c2abb2a8eeca728019d601`.

Git blob identities refer to tracked content. Checkout SHA-256 values depend on exact file bytes, including line endings; compare the actual approved deployment artifact rather than assuming a Windows checkout and Linux checkout have identical byte hashes. Local source references do not establish which release is currently serving production.

### Operational blockers discovered during preparation

| Requirement | Authoritative local evidence | Verified state / required next action |
|---|---|---|
| Actual AlgoLens deployment target | AlgoLens `DEPLOYMENT.md`, `deployment/algolens.conf`, `.github/workflows/deploy.yml`, `deployment/algolens-backend.service`, `docker-compose.prod.yml` | **Blocked:** the docs/checked-in Nginx route API traffic to Docker on port 5000; the workflow restarts legacy systemd on port 5001 and does not replace the API container. Verify active Nginx upstream, serving container/process, and its deployed image before choosing a rollout command. Do not assume the existing workflow deploys the new writer. |
| Deployed application identity | AlgoLens `algolens-api/algolens/infrastructure/config/app_factory.py`; Trade Ngin `cmake/git_version.hpp.in` and `.github/workflows/ci-cd-pipeline.yml` | **Not verifiable locally:** AlgoLens `/health` checks database reachability but does not return a commit/build identity. Record the serving container image digest or process artifact checksum with commit provenance as a fallback. Trade Ngin embeds a Git SHA and tags images, but its workflow also uses a moving `latest` tag. No current production artifact was inspected. |
| Writer pause and restart prevention | AlgoLens `deployment/algolens-backend.service`, `docker-compose.prod.yml`, deployment docs | **Blocked:** Docker and legacy systemd both have restart behavior. Identify all serving instances and direct/manual audit writers, drain writes, and prevent the old code from returning until the compatible release is verified. No service was stopped in this task. |
| Trade Ngin runtime controls | Trade Ngin `.github/workflows/ci-cd-pipeline.yml` | **Not verifiable locally:** the production Compose/runtime definition is on the remote host rather than in this checkout. Confirm its mounts, scheduler, image, and restart controls without running the trading process. |
| Backup/recovery evidence | Deployment documentation search | **Not verifiable:** no backup/PITR/restore runbook or verified recovery artifact was found locally. This does not prove backups are absent; require operational evidence from the database owner. No production backup export was made. |
| Maintenance window and production authority | User's explicit read-only selection | **Blocked for execution:** preparation only is approved; no production mutation or outage window is authorized. |

The API routing/workflow mismatch is a concrete repository inconsistency, not a verified diagnosis of the running server. Resolving it may require changes to deployment configuration after runtime inspection; guessing which service is authoritative would risk updating the wrong process. Database access alone also does not prove the serving application connects to that endpoint; verify that association before any later production change.

An idle database snapshot cannot replace these controls: an old service may reconnect or restart after migration commit. If it writes an audit record without `portfolio_id`, PostgreSQL will reject that insert. AlgoLens's transaction should also roll back the corresponding position write; losing edit availability is still a release failure.

### Secondary review observations

- Historical attribution: inference is conservative but not historical proof. With zero audit rows this has no current migration effect. If rows appear before rollout, stop for explicit review rather than silently attributing them.
- History-query performance: the reader filters `COALESCE(o.portfolio_id, legacy.portfolio_id)`. Do not assume the new portfolio-leading index guarantees efficient use for this combined predicate. A realistic-data staging `EXPLAIN` is an optional performance follow-up, not a reason to rewrite the production query during this read-only preparation.
- Local test safety: review found that the disposable migration harness originally installed cleanup by an environment-overridable container name even before successful creation. A failed `docker run` on an existing name could therefore remove a container it did not create. This was fixed locally with regression tests; no existing user container was removed during reproduction.

### Completed local preparation checks

- **3/3 harness safety regressions:** failed creation/collision does not remove the requested name; failure with stdout does not establish cleanup ownership; successful creation followed by fixture failure cleans up only the returned owned ID. The regressions were observed failing before their fixes, then passing. They execute the real shell script with a fake Docker command, not a real conflicting container.
- **7/7 SQL contract checks:** in-memory tests, no production connection.
- **Shell syntax:** `bash -n migrations/test_012_position_overrides_portfolio_scope.sh` passed.
- **21/21 real migration checks:** fresh PostgreSQL 16 container, pinned to the local Unix Docker socket with a unique name and explicit disposable credentials. Forward migration, future-row enforcement, preserved originals, inference, append-only rules, repeat application, rollback refusal, lock posture, and safe empty rollback all passed. The exact test container was absent afterward.
- Local raw migration output: ignored `.cache/migration012-preparation-test.log`. Only synthetic fixture data was involved.
- Independent SQL/application review, deployment-inventory review, and final harness code review are complete. No critical or important harness findings remain. The local implementation is approved only with the unfulfilled production gates below.

No C++/AlgoLens application code, forward migration SQL, rollback SQL, or deployment configuration was changed during this preparation. The broader passing suites in the linked audit remain the earlier checkpoint; only the preparation checks listed here were rerun in this turn.

## Task 2: Coordinated production change — all Task 1 gates required

- [ ] Use the verified deployment controls to pause/drain manual-edit/audit writers and prevent an old instance from restarting during rollout. Coordinate GitHub Actions automatic/manual deploy jobs, Docker `restart: unless-stopped`, systemd `Restart=always`, and any remote scheduler job that can write audits or restart incompatible code. Inspect the remote Compose/cron definitions to determine which jobs require coordination; do not indiscriminately stop trading or market-data services.
- [ ] Record fresh baseline aggregate counts and migration file checksum; confirm the backup/recovery reference and exact release artifacts. If the schema is partially present or differs from the reviewed definitions, stop for reconciliation rather than relying on `IF NOT EXISTS` to repair it.
- [ ] Verify successful CI for the exact approved AlgoLens commit and its artifact provenance. The current manual `workflow_dispatch` path can bypass the preceding CI event gate and pulls mutable `main`; do not use it as evidence of a tested, pinned backend deployment. Prepare a verified immutable backend artifact first.
- [ ] In the approved migration session, configure a 5-second lock timeout and 30-second statement timeout, use a client that stops on SQL errors, and execute the existing forward migration once. Its own BEGIN/COMMIT define the transaction; do not run selected fragments manually in pgAdmin.
- [ ] Verify column, future-row constraint, companion table, foreign key, scoped index, and append-only rules through read-only catalog queries. Confirm position quantities were not a target of the change and compare aggregate counts.
- [ ] Deploy the verified compatible AlgoLens artifact before resuming writers. Use an immutable digest or pinned SHA artifact, not an unverified `latest` tag or a restart of an old container. Confirm every serving instance has the new code and passes the schema contract; `/health` alone does not establish that. Any separately approved Trade Ngin rollout must likewise bind its image digest to the approved embedded/source SHA.
- [ ] Resume only the intended writers and inspect health/error signals. Do not perform synthetic investor-position edits in production.

If migration fails before commit, verify transaction rollback and keep the prior application/schema pairing. If it commits, do not resume an incompatible application. A rollback after commit requires the guarded rollback's empty-attribution conditions; otherwise preserve the schema/evidence and use a forward fix.

## Task 3: Report readiness — separate from migration success

The [QT-to-investor-email certification plan](2026-09-21-qt-investor-email-certification.md) expands this gate into authenticated UI/API, snapshot/CSV/HTML/MIME, data-freshness, runtime-identity, and delivery evidence. It preserves the current production-read-only restriction and distinguishes migration success from verified report content and recipient delivery.

- [ ] Verify a staging edit changes only its selected book, commits history atomically, survives reseeding, and renders the intended quantity/zero closure in CSV and email HTML without sending investor mail.
- [ ] Re-audit intended production report scopes read-only for current system/QT positions, published risk envelopes, registry membership, live-result and market/metadata dependencies.
- [ ] Record separate outcomes: migration applied, application deployed, and reporting ready/not ready. Never infer one from another.

Production data population, scheduler repairs, live runner execution, and investor email delivery remain outside this migration execution unless separately scoped and authorized.
