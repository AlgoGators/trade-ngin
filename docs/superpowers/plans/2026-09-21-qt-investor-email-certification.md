# QT-to-Investor-Email Certification Implementation Plan

## Current execution override — NO EMAILS

The user's subsequent instruction is **"DO NOT SEND OUT ANY EMAILS AT ALL COST"**. This supersedes every SMTP-sink, canary, live-send, and delivery-execution step below. Only isolated no-send certification work is approved: synthetic database/API tests and offline report artifacts. No SMTP server/client connection, local test email, internal message, investor message, live runner, production mutation, or deployment is permitted. Older transport/delivery steps remain a record of what evidence would be needed, not executable instructions. Actual delivery must remain **unverified** under this boundary.

The [no-send implementation evidence](../../audits/2026-09-21-qt-no-send-certification.md) records the actual execution scope and results. Passing its HTML/CSV checks is **offline content verification only**, not the broader MIME-inclusive content or delivery certification defined below. The production rollout and live-delivery gates remain closed.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkboxes. Never execute a production mutation or external email send under the current read-only authorization.

**Goal:** Produce an evidence-backed, time-bounded sign-off that legitimate QT edits accepted before a report's snapshot cutoff appear in the correct existing Daily Trading Report and attached positions CSV, and establish delivery separately from rendering.

**Architecture:** Trace the real AlgoLens UI/API through the atomic PostgreSQL QT-position/audit write, the Trade Ngin immutable QT report snapshot, both report outputs, the actual MIME message, and delivery evidence. Test destructive/failure scenarios only in owned disposable infrastructure. After an independently authorized rollout, observe genuine production QT activity and normal scheduled reports using read-only evidence collection.

**Tech Stack:** React/TypeScript, Python/Flask, PostgreSQL 16, C++, existing HTML/CSV renderer and libcurl SMTP sender, test-only loopback STARTTLS SMTP capture.

**Spec:** [Approved QT reporting design](../specs/2026-09-18-qt-effective-position-reporting-design.md). This plan supplements, and does not replace, the [migration-012 rollout plan](2026-09-21-migration-012-production-rollout.md). Baseline evidence: [pipeline revalidation](../../audits/2026-09-21-qt-pipeline-revalidation.md) and [database inventory](../../audits/2026-09-21-database-table-inventory-and-migration-checks.md).

## Global Constraints

Copied from the approved design:

- Preserve the existing email HTML and text.
- Preserve report recipients, subject, schedule, attachments, and non-position inputs.
- Replace only the current position maps supplied to the existing email renderer and positions CSV exporter.
- Quantity-dependent values inside existing position rows may update naturally from the QT quantity. Unrelated report metrics remain on their existing data path.
- A zero QT quantity represents a closed position.
- Closed positions do not appear in the email or positions CSV.
- The email and CSV must use one identical, immutable report snapshot.
- Never silently substitute system positions when the QT snapshot is unavailable or inconsistent.
- Never calculate report positions by replaying audit events.

Additional execution boundaries:

- This request authorizes a plan, not its execution. Production remains read-only. No migration, production seed/edit, deployment, service pause/restart, trading run, or email send is authorized here.
- Later migration/application changes require explicit, scoped production authorization and the existing rollout plan's recovery/maintenance/writer-control gates. Mailbox/provider access and any non-investor canary send also need appropriate authority.
- Do not create fake investor positions to obtain a passing result. The agent observes legitimate QT changes made through normal authorized operations.
- Do not change normal investor recipients, schedule, subject, wording, or the production sender as a testing shortcut. Keep test transport/configuration isolated.
- Database integration fixtures can DROP/CREATE schemas. They may run only against a uniquely owned disposable database, never production or a shared staging database.
- Do not place credentials, raw emails, investor addresses, edit reasons, or full production position payloads in Git or the Markdown certificate. Retain restricted evidence separately; publish sanitized IDs, dates, counts, hashes, and mismatches.

## What will and will not be certified

```text
Authorized QT edit → same-book/date QT row + atomic audit
                  → frozen QT report snapshot
                  → CSV + unchanged report HTML
                  → captured outgoing MIME
                  → provider acceptance → recipient evidence
```

| Sign-off level | Required evidence | Insufficient evidence |
|---|---|---|
| Migration ready | Full 012 catalog contract, compatible serving applications, recovery controls | 21 disposable assertions by themselves |
| Content verified | Exact expected QT quantities/closures match the actual CSV, HTML and outgoing MIME for the identified scope/date/cutoff | API success, audit row, or rendered HTML alone |
| Provider accepted | SMTP/provider event tied to the actual message and intended recipient set | Sender success log without message correlation |
| Recipient delivery verified | Provider per-recipient delivered/accepted-by-destination evidence, with its precise meaning recorded | SMTP submission acceptance, lack of an immediate bounce |
| Mailbox receipt verified | Authorized mailbox copy/raw message or recipient confirmation tied to the report | Delivery event alone; no claim the human read it |

Use only the level actually evidenced. An internal canary proves the internal address/path, not delivery to every investor. Never claim universal inbox placement, human reading, or permanent future correctness.

### Meaning of “current”

- Each report has an explicitly identified business/report date, portfolio, strategy set, and snapshot cutoff.
- Every legitimate edit committed and visible before that snapshot, for that report's complete position identity, must be reflected.
- An edit after the snapshot must not make the CSV and email disagree. Verify it in the next eligible run under the existing carry-forward rules; do not resend or change the schedule automatically.
- An earlier edit superseded by a later accepted edit is not the expected final quantity. Compare the authoritative QT state, not a replay of audit events.
- `created_at` on an audit record alone is not proof of transaction commit ordering. Use the committed API response plus snapshot evidence; if that ordering cannot be established, mark the individual trace not verifiable.
- Certification covers the recorded releases, database, report scopes, recipients/evidence level, and observed runs. A code/config/schema/recipient change or freshness/mismatch failure invalidates that sign-off until rechecked.

## Baseline, not a new production check

At the September 21 read-only checkpoint, the audited database had 3,803 system position snapshots, no QT rows, no overrides, no risk-limit rows, and latest system date 2026-08-05. Migration 012 was absent. These facts must be refreshed; they are not proof of the active application's database target.

The recorded local harness already passed 18 component/rendering checks; the migration harness recorded 21 passes. Neither was rerun while writing this plan.

| Requirement | Owner/repository | Claimed state | Evidence | Verified state | Gap / next action |
|---|---|---|---|---|---|
| Portfolio-scoped atomic QT edit | AlgoLens | Implemented locally | Repository, HTTP/unit/PostgreSQL tests; prior revalidation | partial | Verify exact deployed artifact and authenticated UI/API against same disposable DB as renderer |
| QT state to CSV/HTML | Trade Ngin | Component path verified locally | Existing real PostgreSQL/C++ rendering harness | partial | Refresh exact-artifact evidence; include outgoing MIME and real scheduler path |
| Migration 012 live | DB operator | Prepared, not applied | Read-only catalog checkpoint | missing | Complete rollout gates and later authorized execution |
| Correct API deployment target | Release operator / AlgoLens | Local configuration mismatch | Nginx targets 5000; workflow restarts 5001 | not verifiable | Inspect actual runtime and pin artifact provenance |
| Fresh production data and risk coverage | QT/data owner / both repos | No current readiness evidence | Empty QT/risk tables and August system date | missing | Confirm intended DB/scopes, restore normal authorized producer/configuration, then read-only recheck |
| Email transport/content capture | Trade Ngin | HTML tested; MIME not tested | Sender uses real libcurl path, no current capture test | missing | Add test-only STARTTLS sink and real sender probe |
| Investor delivery | Mail/report operator | Not established | No observed delivery/receipt evidence | not verifiable | Authorized provider/mailbox evidence for normal production reports |

## Source and planned file map

Paths prefixed `AlgoLens/` mean the sibling `../../../algolens-qt` checkout, not a new directory in Trade Ngin.

| File | Responsibility / intended treatment |
|---|---|
| `migrations/012_position_overrides_portfolio_scope.sql` and rollback | Existing reviewed schema change; execute only through separate approved rollout |
| `tests/integration/test_qt_report_pipeline.py` | Existing disposable DB orchestration and 18 rendering checks; extend shared fixture/real API path where useful without accepting external DB URLs |
| `tests/integration/qt_report_probe.cpp` | Existing seed/snapshot/HTML/CSV probe; add test-only snapshot barrier and evidence output if required |
| `tests/integration/test_qt_email_delivery_capture.py` (new) | Own disposable test resources, loopback STARTTLS sink, real MIME parsing, safety and content assertions |
| `tests/integration/qt_email_transport_probe.cpp` (new) | Explicit synthetic EmailSenderConfig; real initialize/send_email; loopback-only destination validation; no dotenv or CredentialStore |
| `tests/CMakeLists.txt` | Register the new probe as EXCLUDE_FROM_ALL; normal application targets unchanged |
| `src/apps/live_portfolio_helpers.cpp` | Existing `load_qt_report_position_snapshot`; use unchanged unless a reproducible scoped defect requires a separately reviewed fix |
| `src/data/postgres_database.cpp` | Existing QT seed and `load_report_positions_by_date`; compare exact scopes |
| `apps/strategies/live_portfolio_runner.cpp` | Existing shared snapshot handoff and report/send gates; verify deployed code and staging scheduler invocation |
| `src/core/email_sender.cpp`, `include/trade_ngin/core/email_sender.hpp` | Existing renderer/MIME/sender; no production rewrite or interface change planned |
| `AlgoLens/algolens-api/routes/portfolio.py` | Authenticated `POST /portfolio/positions` and scoped history route |
| `AlgoLens/algolens-api/algolens/application/portfolio/use_cases.py` | Existing `UpsertQtPosition`, scope/risk evaluation |
| `AlgoLens/algolens-api/algolens/infrastructure/portfolio/repositories.py` | Existing atomic `write_qt_position` and before/after audit persistence |
| `AlgoLens/algolens-frontend/src/components/EditPositionModal.tsx` and `src/infrastructure/api/portfolioApi.ts` | Real selected-book edit/acknowledgement path; verify unchanged behavior |
| `docs/audits/qt-email-certification/` (new, sanitized results only) | Run-specific gate matrix and final certificate after evidence exists, not a pre-filled pass |
| `.cache/qt-email-certification/` (ignored, synthetic local artifacts) | Disposable fixture results/MIME/CSV/HTML; production evidence belongs in restricted operator-approved storage, not here by default |

## Task 1: Freeze the release, scopes, date contract, and evidence prerequisites

**Owner:** release operator + QT/data owner; agent prepares and reviews evidence. **Interfaces:** consumes the existing rollout/audit documents; produces a scoped release manifest and a gate matrix with every unknown explicitly blocked.

- [ ] Record exact Trade Ngin and AlgoLens source SHAs, clean/dirty status, migration file hash, built binary/image digests and CI results. Earlier source checkpoints are Trade Ngin `5ee11f57ff4a5f2c7020d3ad34751c588399f2d6` and AlgoLens `e4469170f07e80b4319929ce85017cb7c1fb8148`; do not assume those are deployed or the eventual final artifacts.
- [ ] Identify every intended investor-report book and strategy from the actual report configuration and business owner. Record full `(portfolio_id, strategy_id, strategy_name, date, symbol, portfolio_type)` matching rules. Do not derive the release scope from all historical database rows or `is_active` alone.
- [ ] Inspect active Nginx upstream and the process/container actually receiving traffic; record its immutable image/process checksum and database identity without dumping environment secrets. Reconcile the port-5000/port-5001 inconsistency. `/health` only checks connectivity and is not a version/schema attestation.
- [ ] Inspect the actual Trade Ngin container/binary, cron/job command, mounts, timezone and email enablement. The local cron file says 09:30 America/New_York weekdays, but this is not runtime proof. Preserve the existing schedule; record market holidays and the expected latest completed business date per report.
- [ ] Resolve the date contract using actual clocks: AlgoLens `write_qt_position` uses Python `date.today()` while report loading takes an explicit date/timestamp. Record API timezone, scheduler timezone, report-date resolution, and UTC snapshot time. If they target different dates for a supported editing window, block certification and specify a narrowly reviewed correction before continuing; do not conceal the mismatch by relabeling an email.
- [ ] Identify restricted evidence storage, approved read-only provider/mailbox access, retention, and the person allowed to acknowledge a received message. No new recipient is added to production distribution.
- [ ] Carry forward the rollout plan's backup/restore evidence, maintenance window, writer inventory, old-writer restart prevention, and explicit production approval requirements. Snapshot-named DB tables are not backup verification.

**Acceptance:** all serving artifacts, DB targets, enabled report scopes, clock/date rules and owners are known. Unknown target, scope, date semantics, recovery control or delivery-evidence access is a recorded blocker, not an assumption.

## Task 2: Prove the chain in owned disposable infrastructure

**Files:** existing pipeline/probe and new capture test/probe listed above; existing AlgoLens tests below. **Interfaces:** consumes synthetic QT API edits and the real C++ snapshot; produces synthetic CSV, HTML, complete MIME, exact quantity comparisons and failure evidence. This is the only task that proposes new test code.

### 2A. Preserve and rerun existing regression evidence

- [ ] Review the safety wrappers before execution. Require uniquely owned loopback PostgreSQL, no production `.env`, explicit disposable credentials, no external database URL accepted by the cross-repository harness, no real SMTP, and cleanup by owned resource ID only.
- [ ] Run the existing local SQL and harness-safety tests:

```sh
python -m unittest discover -s tests -p 'test_qt_position_sql.py'
python -m unittest discover -s tests -p 'test_migration_012_harness_safety.py'
```

- [ ] Run the real migration-012 harness only after verifying its Docker endpoint is a local isolated test daemon, with a unique container name and synthetic credentials. Require all 21 checks and cleanup to pass again for the release artifact.
- [ ] Rebuild the real C++ probe and run the no-send pipeline harness. `QT_CERT_BUILD` must be the inspected isolated build of the recorded SHA, not an arbitrary installed binary:

```sh
cmake --build "$QT_CERT_BUILD" --target qt_report_probe trade_ngin_tests
python tests/integration/test_qt_report_pipeline.py \
  --algolens-root ../algolens-qt \
  --probe "$QT_CERT_BUILD/bin/Debug/qt_report_probe" \
  --output-dir .cache/qt-email-certification/component
```

- [ ] In the AlgoLens API test environment, run `python -m pytest tests/test_position_edit.py tests/test_position_edit_routes.py tests/test_position_identity.py tests/test_schema_contract.py -q`. Run `tests/integration/test_qt_position_override_postgres.py` and `tests/integration/test_position_write_postgres.py` only after `ALGOLENS_TEST_DB` is independently checked to identify a newly created disposable database: these fixtures drop/recreate schemas.
- [ ] Run frontend `npm test`, `npm run typecheck`, and `npm run build` from `AlgoLens/algolens-frontend`. Refresh full C++ and backend regressions on the exact release candidate, with zero unexplained skips/failures. Offload heavy builds/full suites through the configured SSH/tmux compute workflow after verifying an isolated workspace; do not transfer production credentials. If unavailable, record the limitation and use the previously established safe fallback only within local capacity.

### 2B. Close the authenticated UI/API integration gap

- [ ] Start only a test AlgoLens app against the owned disposable fixture, with synthetic users/keys, controlled timezone/date, and external service egress denied. Do not import the default production-configured app into the test.
- [ ] Using the real UI, select synthetic `BOOK_A`, edit a position, enter a synthetic reason, and submit. Observe the real authenticated `POST /portfolio/positions` and committed response. Verify internal-role/JWT/CSRF behavior, explicit book selection and deliberate second-click risk acknowledgement. Reuse real route/app test factories rather than bypassing auth with a mocked use case.
- [ ] Verify a real QT position and portfolio-scoped audit row commit atomically on the same database consumed by the C++ probe. A route test against a mocked repository plus a separate direct-use-case test is not one integrated pass.
- [ ] Exercise the scenario matrix below through this shared fixture; preserve the existing direct-use-case harness as a fast lower-level check.

| Synthetic scenario | Exact required outcome |
|---|---|
| BOOK_A/strategy A/ES system=10; accepted QT edit=7 | QT DB, immutable snapshot, CSV, HTML and captured MIME all show 7; system remains 10 |
| BOOK_B/same strategy and symbol/system=20 | BOOK_A edit does not alter BOOK_B; its report remains 20 after normal synthetic seeding |
| BOOK_A/strategy B/same symbol/QT=-3 | Strategy A stays 7, strategy B stays -3; an existing combined view, if present, is 4, without row mixing |
| Existing NG position edited to 0 | Zero stays in QT DB as closure evidence; symbol absent from open-position CSV/HTML/MIME |
| New permitted symbol, negative quantity, repeated edit | Accepted final state appears only in correct scope; before/after audit chain and risk behavior are correct |
| Accepted no-op | Quantity stays unchanged; preserve the existing documented audit/response behavior rather than changing API semantics for certification |
| Validation error, risk rejection, missing acknowledgement | No position/audit commit and no successful-edit claim |
| Two first edits race; audit insert forced to fail | Existing concurrency tests prove serial before-state correctness; audit failure leaves no partial QT reservation/update |
| Reseed/rerun and next report day | Manual value and closure survive the existing seed/carry-forward behavior |
| Other book/strategy/date/stream records | All excluded from requested output |
| UTC/local-midnight, DST transition, weekend/holiday boundary | API edit date and report scope follow the documented date contract; no accidental next/previous-day inclusion |
| QT state missing, inconsistent, non-finite, schema absent, DB failure | Report generation fails closed; SMTP sink receives zero messages; no system fallback |
| Genuinely flat valid book | Empty open-position report succeeds under existing flat-book rules; missing data is not treated as proof of flatness |

### 2C. Verify the actual outgoing MIME without modifying production sender behavior

- [ ] First add failing safety/MIME tests proving that the old renderer-only harness cannot supply captured SMTP DATA. Introduce the two test-only files and CMake target from the file map; keep production sender code/interface unchanged.
- [ ] Start a loopback-only STARTTLS SMTP sink on an ephemeral port with synthetic credentials, a test certificate and no relay capability. Use a test-only dependency declaration if a sink package is needed; do not add it to production dependencies. No plaintext-only sink: the existing sender unconditionally requires TLS.
- [ ] The new probe must construct the existing explicit `EmailSenderConfig`, call `initialize()`, then call the real `send_email` with the already-rendered report and the actual generated attachment paths. Permit only a numeric loopback SMTP host and owned ephemeral port, synthetic sender/recipients under `.invalid`, and synthetic files inside the test output directory. Reject production credentials, external hosts, symlink escapes and externally supplied DB URLs before connecting.

Concrete probe configuration contract:

```cpp
trade_ngin::EmailSenderConfig cfg{};
cfg.smtp_host = "127.0.0.1";
cfg.smtp_port = owned_sink_port;  // Returned by this test's bound sink.
cfg.username = "qt-certification";
cfg.password = "synthetic-test-only";
cfg.from_email = "sender@qt-certification.invalid";
cfg.to_emails = {"recipient-a@qt-certification.invalid", "recipient-b@qt-certification.invalid"};
cfg.use_tls = true;
trade_ngin::EmailSender sender(cfg);
```

`owned_sink_port` is a value passed by the owning capture harness after binding its socket, never a production configuration value. Use the return/error handling of the existing sender interface; a successful process exit without captured DATA is a failure.

- [ ] Parse captured bytes with Python's standard `email` MIME parser. Require exactly one expected message in the nominal test, expected envelope/subject, readable HTML, all existing attachment filenames/MIME types, decoded current-position CSV equal to the on-disk CSV, and unchanged unrelated attachments. Compare decoded data, not unstable MIME boundaries.
- [ ] Assert SMTP envelope recipients separately from the MIME `To:` header. The current sender sends RCPT for all configured recipients but writes only the first recipient into `To:`. Preserve that existing behavior; the two-recipient synthetic fixture must prove both envelope recipients without requiring a new header layout. A visible `To:` header or one mailbox copy is not the distribution list or proof of delivery to all recipients.
- [ ] Fix all non-position inputs and the clock, render the baseline and edited message with the same existing renderer, and compare DOM/text/attachments. Allow only the approved position-row quantities and dependent values, plus addition/removal of open-position rows under existing semantics. Require zero differences elsewhere, including unrelated P&L/risk/performance sections, subject, wording, layout, sender and recipients.
- [ ] Add a test-only synchronization barrier immediately after snapshot load: capture QT=7, then commit QT=9 through the API before generating the second output. Both outputs from that frozen snapshot must remain 7; the next eligible snapshot must show 9. Never refresh DB separately for CSV and email.
- [ ] Exercise sink refusal, connection failure, missing attachment, and report-blocked paths. Record actual failure semantics, require no false success and no fallback email. A retry or crash must not silently create an unobserved duplicate distribution; record intended retry behavior and block release on an unexplained duplicate.
- [ ] Run new tests red then green, rerun the existing harness and broader suites, and have an independent reviewer check resource ownership, network guards, full-key scoping, MIME assertions and the no-report-change contract. Commit only scoped test changes and sanitized results after review.

**Acceptance:** exact-artifact component/API/MIME tests pass, including immutable-snapshot race and zero-message failure assertions; output compatibility has zero unauthorized differences. A local SMTP sink proves capture/acceptance by that sink, not internet delivery.

## Task 3: Complete migration 012 and compatible deployment through the approved rollout

**Owner:** authorized DB/release operator. **Files:** existing migration rollout plan and actual reviewed deployment definitions. **Interfaces:** consumes Tasks 1-2 evidence plus explicit production approval; produces a verified schema/application pairing. This task is blocked under current read-only authority.

- [ ] Execute the existing rollout plan's recovery, maintenance, writer-drain and restart-prevention gates. Inspect actual definitions; do not stop unrelated trading/data services or follow guessed deployment commands.
- [ ] Apply only reviewed migration 012 with the plan's lock/statement timeouts and stop-on-error transaction behavior. If historical audit rows appeared since the empty baseline, review unique/ambiguous attribution before execution.
- [ ] Verify all intended column types, constraint validation/enforcement, index definition, companion PK/FK and append-only rules through read-only catalogs. A migration tracking row alone is insufficient.
- [ ] Deploy compatible pinned AlgoLens and Trade Ngin artifacts to the actual serving API and actual scheduled report process before resuming dependent writers/reporting. Restarting systemd 5001 or an old mutable Docker image is not proof of the 5000 API release.
- [ ] Confirm every instance's artifact/DB identity and resolve deployment automation/restart races. Preserve current investor email configuration and schedule.
- [ ] Record migration, API deployment, report-engine deployment and report readiness as four distinct outcomes. Do not mark content/delivery gates passed here.

**Acceptance:** schema and every live dependent artifact match the tested release; recovery controls and required approvals are recorded. Never delete audit evidence to force rollback. After scoped audit data exists, prefer a reviewed forward fix; a downgrade must remain compatible with schema 012.

## Task 4: Re-establish and verify current production data, read-only from the agent

**Owner:** QT/data operators perform any separately authorized normal producer/configuration work; agent performs SELECT/catalog verification. **Interfaces:** consumes the approved report-scope/date manifest; produces current per-scope readiness evidence.

- [ ] Confirm the serving API and report job use the same intended database audited by the agent. The prior fingerprint alone does not prove either application's connection.
- [ ] Have authorized operators investigate the stale producer/scheduler and populate normal required state through legitimate workflows, not hand-written certification rows. Migration 012 does not seed QT positions, create risk envelopes, register portfolios or refresh prices.
- [ ] Recheck through startup `default_transaction_read_only=on`, `BEGIN READ ONLY`, endpoint/role checks, 30-second statement and 5-second lock timeout, SELECT/catalog queries only, and `ROLLBACK`. A dedicated read-only role is preferred; do not infer the supplied broad-privilege login is read-only.
- [ ] For every enabled report scope, require the expected business-date system snapshot and matching QT snapshot evidence under the complete key. Count zero closures as evidence, not as missing rows. Treat a flat report as valid only when the underlying scope is genuinely flat.
- [ ] Verify published risk envelopes and actual risk-evaluated edit verdicts for intended books, correct strategy registry/membership/lifecycle, same-scope/date live results, resolvable instrument metadata and appropriately dated prices. Use the actual report's dependencies; exclude retired/historical scopes from live pass/fail scope.
- [ ] Count duplicate identities, wrong-scope rows, missing dependencies, schema mismatches and unexpected portfolio/date/stream values. Require zero unexplained anomalies. Every scoped audit row must agree with its legitimate committed edit; use audit only as trace evidence, never as the report-state source.
- [ ] Record precise timestamps and refresh immediately before the observation run. If data is stale, scope is unknown, risk was not evaluated where required, or QT coverage is missing, stop short of content certification; do not repair from the audit session.

**Acceptance:** all manifest scopes satisfy schema, freshness, identity, risk and dependency requirements, with no unexplained gap. There must be at least one genuine QT-edited position to claim an observed manual-edit path; unchanged seeded positions alone do not prove it.

## Task 5: Verify the real staging scheduler and an approved non-investor delivery

**Owner:** report/mail operator with agent verification. **Interfaces:** consumes tested artifacts and synthetic stage state; produces actual job/MIME/provider/mailbox evidence. Any external email send requires separate explicit authorization.

- [ ] Run the real scheduler/runner entry point only in a verified isolated staging environment with synthetic data, no production DB credentials, and no broker/order or real-investor network access. Verify its command, environment, timezone, report date and `--send-email` behavior. Do not run the live production runner to make evidence.
- [ ] First route that staging instance to the owned STARTTLS capture sink. Prove that the actual job, not only the probe, uses the immutable snapshot and sends the intended attachment bytes. If complete safe staging fixtures/runtime isolation cannot be established, record this gate blocked.
- [ ] Compare stage scheduler output to the component/API/MIME acceptance matrix, including edit-before-cutoff, edit-after-cutoff, reseed, next eligible run, and failure suppression. Verify expected duplicate/retry behavior without changing the existing schedule.
- [ ] Before any external canary, obtain approval for the named internal recipient and provider, synthetic content, one-message limit and storage/access. Configure only the isolated staging instance; never replace or augment production investor recipients.
- [ ] Record provider acceptance ID/event, per-recipient status and bounce/defer outcomes; obtain an authorized mailbox copy and compare its decoded HTML/attachments to the captured payload. Record hashes and zero quantity/attachment mismatches. A preview, send-success log or lack of a bounce is insufficient.

### Existing SMTP security finding — separate approval boundary

Source review found that `src/core/email_sender.cpp` currently requires TLS but disables peer/hostname verification; `use_tls` is not consulted by `send_email`. This is an existing transport-security issue, not introduced by QT reporting. Do not claim that encrypted transport authenticates the intended SMTP server under this setting.

Before an external certification canary or unrestricted release recommendation, require the mail/security owner to resolve this through a separately authorized, reviewed transport fix or explicitly document an accepted compensating control/risk. Do not silently alter sender behavior under the user's position-numbers-only constraint. An accepted risk is recorded in the certificate; it is not labeled a security pass. Loopback synthetic sink testing can proceed without an external send.

**Acceptance:** the actual staging job produces the correct unchanged report, SMTP/provider evidence correlates to it, and the approved internal mailbox copy matches. This verifies staging delivery only, not investor delivery.

## Task 6: Observe legitimate production runs and issue a scoped certificate

**Owner:** QT/report/mail operators carry out existing authorized business operations; agent reads approved evidence. **Interfaces:** consumes the approved deployment and production-readiness gates; produces the final evidence matrix/certificate. No new live send or synthetic edit is performed by the agent.

- [ ] Identify a real QT edit made through the deployed UI/API before the next eligible scheduled report, with QT-team confirmation of intended scope/quantity. Record a sanitized correlation ID, committed response evidence, full-key match, and corresponding portfolio-scoped audit evidence privately.
- [ ] Correlate the committed state to the actual report snapshot. Prefer a retained snapshot artifact/run log. If no trustworthy cutoff/artifact exists, use an explicitly coordinated business observation window with no intervening QT changes plus before/after read-only comparisons; if the ordering or intermediate changes remain ambiguous, mark this trace not verifiable rather than treating audit timestamps as commit times.
- [ ] Observe two consecutive eligible scheduled reports without triggering extra sends. In the first, prove at least one genuine manual edit; in the next, prove persistence/carry-forward or the later legitimately superseding edit. Compare every emitted position row in every enabled scope, not merely one sampled symbol. Books with no observed edit get coverage labeled as output reconciliation, not observed manual-edit certification.
- [ ] Obtain actual production outgoing/received message evidence through authorized provider/mailbox access or an operator-supplied original message, not a newly rendered reconstruction. Match the current-position CSV bytes and decoded HTML quantities to the cutoff state. Verify zero closures stay absent and unrelated content/configuration remains unchanged.
- [ ] Reconcile the intended recipient set against provider events. Record accepted, delivered-to-destination, deferred, bounced and unknown counts separately. Unresolved delivery outcomes block an all-recipient delivery claim; the agent does not retry, resend, change recipients or suppress scheduled mail on its own.
- [ ] For any recipient/observer whose actual mailbox copy is available, verify its content and identify exactly that receipt coverage. Do not imply everyone received/read it from one internal copy.
- [ ] Write the sanitized certificate and independent review result only after all mandatory gates pass. If a gate lacks evidence, issue a NOT CERTIFIED/PARTIAL report with the precise missing evidence, rather than a blanket assurance.

### Required certificate fields and pass criteria

| Field | Required content / pass rule |
|---|---|
| Release identity | Trade Ngin SHA + artifact digest; AlgoLens SHA + serving artifact digest; migration checksum/schema attestation |
| Runtime identity | Verified API upstream/process, report job, same intended DB fingerprint and approved configuration identity |
| Scope | All enabled books/strategies, business dates/timezones, UTC cutoff or proven observation interval, two report run identities |
| QT trace | Legitimate edit correlation and atomic DB/audit proof; evidence of inclusion or documented next-run exclusion |
| Position reconciliation | Expected and observed row counts, quantity mismatch count=0, wrong-scope count=0, missing/extra open-row count=0 |
| Output consistency | HTML/CSV/MIME agreement; closure checks; decoded attachment hashes match |
| Compatibility | Unauthorized differences outside approved position rows=0; recipient/subject/schedule/non-position configuration unchanged |
| Failure behavior | Passing isolated missing-data/DB-error/race tests; no fallback sends |
| Delivery | Correlated message/provider IDs in restricted evidence; recipient-set match; accepted/delivered/deferred/bounced/unknown counts; mailbox-copy coverage explicitly identified |
| Exceptions | Any accepted security/operational risk stated plainly; no unresolved content or scope mismatch treated as a pass |
| Time bounds | Verification UTC time, observed report dates, evidence references, verifier/reviewer and invalidation conditions |

Suggested final assurance, only when supported:

> For the recorded releases, database, books and report runs, all QT edits committed before the verified snapshot cutoff were reflected correctly in the existing Daily Trading Report and positions CSV. All compared position quantities matched; zero closures were omitted; unrelated report content and production recipient/schedule settings were unchanged. Delivery and mailbox receipt were verified only to the recipient/evidence scope listed in this certificate.

## Stop, escalation, and rollback rules

- Content mismatch, missing QT coverage, stale inputs, ambiguous date/book identity, wrong deployed process or missing provenance: NOT CERTIFIED; notify the operator with sanitized evidence.
- No silent system fallback, audit replay, fabricated QT rows, destructive rollback, recipient change, retry/resend, scheduler suppression or production repair by the agent.
- Ask the authorized operator to decide containment for an actual live mismatch. Do not infer permission to stop scheduled reports or trading from this plan.
- Pre-commit migration failure follows transactional rollback. Post-commit rollback follows the existing guarded-empty-attribution rule; never remove audit rows to enable it.
- Any necessary application correction follows a separate failing regression, minimal scoped fix, review and refreshed artifact verification. Any correction that changes sender behavior, dates/business policy, report content beyond allowed position values, or production operations requires explicit scope approval.

## Execution handoff

The next safe implementation phase is Task 2's isolated test pack plus Task 1's read-only evidence preparation. Use subagent-driven execution for independent API/capture tests with review between tasks; inline execution with the same gates is an alternative. Tasks 3-6 must not be started merely because this plan exists.

Plan completion does not certify production. Production remains read-only until explicitly changed, and investor delivery remains unverified until the corresponding evidence gates pass.
