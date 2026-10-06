# Governed live configuration — issue 88 design

Date: 2026-10-06. User decisions Q1–Q11 are binding. This is implementation scope, not production rollout authorization.

## Outcome and decisions
Engine serialization, governed database overrides, per-attempt configuration identity and final consumption evidence work together. No configuration editor UI. Existing AlgoLens readers are updated compatibly.
- A successful lookup with no active override uses file configuration. Failed lookup, invalid active row, stale base, or invalid effective configuration refuses the run; never silently fall back.
- Supported strategy parameters, risk numerical settings within the existing module assignment, and optimizer settings may change. Portfolio identity/capital/ownership, strategy membership/type/allocation, symbols/universe, credentials, email, operational controls and unsupported cost settings cannot.
- Risk module identities, types, order, count, scope, activation and control predicates are immutable under an override. No disabling checks or replacing protection with none. Numerical tuning still passes the schema-2 parser and its coupled reporting/module constraints.
- Every attempt freezes exact configuration at admission. Failed/aborted attempts may retry with the currently active version. Successfully published days remain immutable.
- A failed attempt with durable side effects blocks a changed-config retry until verified recovery. No automatic cleanup, invented successful recovery, or whole-run accounting rewrite.
- Submitter plus ONE DISTINCT authorized approver activate a version; this is two people total, not two approvers. Nonempty reasons and immutable actor/version audit required.
- CLI uses authenticated AlgoLens HTTP and CSRF; no caller-supplied actor identity and no direct SQL operational workflow.
- New explicit config_submit/config_approve authority is provisioned separately; do not silently turn position approval into configuration approval or auto-grant production users. Resolve current non-retired identities and canonical approver identity using existing authority machinery.
- Configuration activation does not grant execution authority. Existing run/stop authorization remains required and must bind the new effective snapshot.
- Existing completed-run publication owns evidence. A startup preview/selection is never described as successful execution.
- Existing v1/v2 futures and v3 equity reports stay readable. New runtime snapshots and inspection payloads are explicitly versioned.
- Backtests remain file-only. No broker execution, production DB changes, deployment, issue closing, public messages or shared-branch pushes as part of implementation.

## Related work
Pair trade #88 with #49 and PR #60's loading intent (do not import PR60's unsafe fallback/early-publication implementation). Reuse narrow AlgoLens #94 authorization work and update its config readers. Coordinate stable refusal codes with trade #125/AlgoLens #97; do not implement their monitoring product. Keep trade #87 ledger/publication boundaries intact. Do not fold #138/#149 behavior-preserving refactors into behavior-changing configuration work. No data-ngin implementation dependency.

## Baselines and isolation
Trade checkout /home/john-riley/projects/Algo/.worktrees/trade-ngin/issue-88-config-contract at 70aa2764; AlgoLens checkout /home/john-riley/projects/Algo/.worktrees/AlgoLens/issue-88-config-contract at 7f4831e6. Both branch codex/issue-88-config-contract. Other production worktrees contain ongoing changes and must remain untouched. Native worktree tool reported Not a git repository at parent Algo workspace; manual isolated worktrees were created instead.

## Contract
Native C++ owns accepted setting paths, types, schema-2 extraction, and the effective snapshot.
Create a pure one-request tool live_config_validate reading bounded JSON from stdin:
request {schema: "live-config-validation/v1", base_snapshot: <v2 snapshot>, changes: {<RFC6901 pointer>: <scalar or supported array value>}}.
No file paths or credentials in protocol. Native output contains schema, base_sha256, effective_sha256, effective_snapshot, and sorted changed_paths. Hashes are SHA-256 of native canonical serialized snapshots; Python/TS must not recompute with different float serialization. No-op requests are rejected.
Unknown, missing, malformed, nonfinite, secret, derived, unsupported or protected paths fail with stable redacted codes. Apply changes only to existing eligible paths. Structural risk fields, removal/null, arbitrary strategy keys and metadata changes are refused. Both sides of coupled Carver/reporting changes must be explicit and equal; no silent secondary mutation.
Runtime snapshot_version 2 includes schema-2 risk, sleeve_risk_modules, use_optimization and covariance_history_prices alongside existing credential-free trading fields. Legacy v1 is read-only history; newly governed approvals require v2.
The supplied inspection projection_version 2 describes schema-2 paths with truthful classification and current values; consumers retain the frozen projection v1 catalog for history. New final futures publication version 4 and equity publication version 5 carry v2 supplied projection, the applicable existing actual consumption, and configuration_selection {source, version_id, base_sha256, effective_sha256}. Final evidence links selection to exact attempt/book/date and persists atomically with results.

## Storage and lifecycle
Use additive trade migration 031_live_config_overrides.sql, with guarded rollback. Recheck collisions before implementation.
Persist immutable version candidates (book/registry scope, registry revision, native build/validator identity, base/effective hashes, changes, effective snapshot, previous active version, submitter, reason, timestamp), a single active pointer per scope, immutable activation audit (approver, current authority versions, reason), and immutable attempt selection plus a durable retry-safety marker.
All reads validate shape, current baseline and source identity; all activation changes lock scope and current authority and compare expected active version. Concurrent activation has one winner; loser is stale. Submitted payload cannot be edited, deleted, or reused with different contents. Approver revocation before activation refuses it.
Select configuration before any strategy construction, input fetch depending on configurable settings, or financial write. Recheck selected identity at admission under the book lock. Changes after admission affect the next attempt only.
Completed-day check and unresolved attempt/side-effect checks occur before financial writes. Record an unsafe marker BEFORE a potentially independently committed financial mutation; clear only after successful atomic publication or verified recovery with evidence. Crash between marker and mutation safely refuses until recovery. Never clear on a failure merely because an exception was caught.
Recovery record verifies restoration against the attempt's recorded pre-write state (or explicit reviewed restored snapshot); a reason or boolean alone is not recovery proof. If a legacy failure cannot be classified safe, refuse changed-config retry.
The system publisher cannot activate versions; API cannot fabricate financial completion/recovery. Update least-privilege grants explicitly.

## CLI and application
Thin CLI in AlgoLens scripts/live_config_admin.py supports preview, submit, approve, status through cookie-authenticated HTTPS (loopback HTTP allowed for tests) with CSRF. Password/token/cookie values are never logged; use an explicit cookie jar, no actor flag.
Config-specific capability annotations are edit_config and approve_config, backed by explicit config_submit/config_approve grants. New capabilities do not widen old role bundles. Current identity retirement and canonical approver rules apply. No self-approval, including aliases mapping to same person.
Config-specific request/approval endpoints use existing Flask layering, per-book registry scope and stable redacted errors. Server obtains the provisioned v2 baseline and native validator identity, never trusts client baseline/hashes.
Preview and submission use the same native pure validation. Activation rechecks baseline, registry revision, expected active version, current authority and identical native validation output under appropriate locks. Authority revocation and changed baseline require resubmission.
Runtime approval obtains the active effective snapshot rather than stale static manifest fields; keep base provisioning separate from selected configuration. Changing active configuration requires an execution approval for that exact snapshot before a controlled attempt can proceed.

## Acceptance and critique
Fresh native tests reproduce and pin conservative 0.3/2.0 preservation under unrelated changes, 12.75 preservation, all editable emitted leaf paths, coupled risk settings, strategies, structural rejection and finite bounds.
A disposable PostgreSQL end-to-end test proves submit -> distinct approve -> active selection -> runtime approval -> native attempt -> JSONB publication -> AlgoLens API -> frontend parser, for supported futures and equity profiles.
Failure injection proves no-row vs lookup failure, malformed overrides, stale baseline, self/revoked approval, races, crash after side-effect fence, safe changed retry, unsafe changed retry refusal, completed-day refusal, immutable prior evidence and old-report readability.
No production state is touched. Remaining deployment prerequisites (explicit grants, validator provisioning, migrations, current release pins) are documented, not fabricated.


## Critique resolved before implementation
- Publication version 3 already identifies equity; use new 4=futures and 5=equity, preserving old 1/2/3 readers.
- Native canonical bytes, not Python float formatting, define hashes.
- Provisioning must export a complete v2 baseline from the actual schema-2 file set; legacy snapshots cannot synthesize missing risk assignments. Add operator-only live_config_validate --export-base --config-root PATH --portfolio KEY; the HTTP validation stdin protocol still accepts no paths.
- No direct actor-ID CLI; use current authenticated identity. Config grants are explicit and separate, with no automatic production enrollment.
- Configuration activation alone never bypasses runtime approval; both bind the effective snapshot.
- Every native attempt needs selection/retry tracking even when legacy runtime control is disabled; an active override requires controlled execution. No-row legacy runs retain their existing behavior plus truthful evidence.
- Recovery proves state restoration against pre-write evidence; a human reason alone cannot clear the guard.
- Current unrelated production edits are excluded by pinned clean worktrees.
