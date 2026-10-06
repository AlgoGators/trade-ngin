# QT certification: no-send implementation and evidence

Checkpoint: 2026-09-21 23:46 UTC test run. Status: **local no-send implementation complete; tests and both independent review gates passed. Production is not certified or changed.**

The user authorized isolated certification work and explicitly prohibited **all email sending**. This supersedes the SMTP-sink and canary steps in the earlier [certification plan](../superpowers/plans/2026-09-21-qt-investor-email-certification.md). The [migration rollout plan](../superpowers/plans/2026-09-21-migration-012-production-rollout.md) remains gated by production-read-only authority.

## Non-negotiable execution boundary

- No `EmailSender::send_email`, SMTP sink/client, provider mail call, internal canary, investor message, or live portfolio runner.
- No production database connection, migration, seed, repair, deployment, service restart or schedule change in this implementation phase.
- Existing report renderer, sender, recipients, subject, template, schedule and unrelated report code remain unchanged.
- Real integration writes are allowed only inside the uniquely owned, synthetic disposable PostgreSQL fixture.
- Actual outgoing MIME, SMTP acceptance, recipient delivery and mailbox receipt remain unverified. We do not fabricate zero-attempt telemetry where no counter exists.

## Safety design

The C++ unit suite and focused backend unit tests run with cleared environments under Linux `unshare -Urn`. The no-send integration runner uses that same network isolation for Python/C++, plus a PostgreSQL container with `--network none --pull=never`, no published ports and `listen_addresses=''`. It communicates through a private temporary Unix socket. The local Docker control socket is used only to manage the fixture; it is not an email transport.

The harness rejects production/transport environment inputs before side effects, uses an empty owned Docker configuration, and cleans up only the container ID returned by its own successful creation, that container's anonymous test volume, and its exact temporary directory. Before resource creation it verifies exactly the loopback interface and zero IPv4 routes. Failure to verify is fatal. The final run's container and database socket directory were independently confirmed absent afterward; Docker event evidence also confirmed destruction of its one anonymous test volume.

The snapshot synchronization requires a Linux-native output directory supporting POSIX FIFOs (not Windows-mounted `/mnt/c`). The final run used `/tmp/qt-no-send-certification`; its regular artifact files were subsequently copied to the ignored workspace cache and their hashes rechecked. FIFOs are synchronization controls, not retained report evidence.

The actual MIME builder is embedded in the production transport method. A shared-library curl interception is not a sufficient portable no-network guarantee, so MIME testing is deliberately omitted rather than invoking that method. This preserves the production sender and the user's absolute no-send restriction.

## Fresh verification

| Check | Result | Evidence / limits |
|---|---|---|
| SQL contract regressions | 7 passed | In-memory, no production connection |
| Migration harness cleanup regressions | 3 passed | Fake Docker boundary; no actual Docker resource deletion |
| Full C++ test executable | 1,278 passed, zero skips/errors/disabled | Network namespace, isolated cwd/config template; 28.646 seconds |
| Focused AlgoLens edit/auth/identity/schema | 117 passed | Network namespace; 42 existing SQLite date-adapter deprecation warnings |
| Migration 012 real disposable PostgreSQL | 21 passed, zero failed | Network-disabled synthetic container; removal independently confirmed |
| New harness safety regressions | 7 passed | Environment rejection, keyed quantities, content mutation, duplicate rejection, manifest, bounded FIFO failure, no-egress guard; root rerun 0.35 seconds |
| Enhanced API → PostgreSQL → C++ HTML/CSV | 36 passed, zero failed | Real route/repository/DB/renderer, final 3.34-second run; no SMTP/MIME/delivery claim |
| Retained artifact integrity | 12 SHA-256 hashes matched | CSV/HTML copies independently checked against the generated manifest |
| Independent task review | Approved after fixes | Static-content masking tightened; malformed/duplicate rows rejected |
| Final overall review | Approved | No Critical/Important findings; one nonblocking comparison-precision limitation documented below |

The initial C++ run had one config-discovery skip because of an incorrectly quoted temporary working-directory command. A standalone checked helper fixed the invocation; the final 1,278/zero-skip run above supersedes it. No test was weakened or disabled.

Remote compute was checked first. The host was reachable, but Docker and required C++ development dependencies were absent, and its older QT CMake cache had `GTest_DIR-NOTFOUND`. Validation therefore used the existing isolated WSL build and virtualenv; no dependency installation or production runtime change was made.

## Verified enhanced coverage

- Real authenticated Flask route with JWT/role/CSRF protection, real use case/repository and real disposable PostgreSQL, rather than a mocked repository.
- Exact quantities keyed by strategy and symbol in both CSV and rendered HTML; other books and system rows unchanged; zero closures absent from open-position rows.
- Baseline-versus-edit comparison for unchanged non-position content and position-table structure.
- Deterministic synchronization after the C++ snapshot load: a later edit must not change either output from that frozen snapshot, but must appear in the next snapshot.
- Reseed/carry-forward and schema/missing-evidence failure checks; no report artifacts on blocked paths.
- Sanitized machine-readable evidence with scope/date, individual checks, artifact hashes and explicit unverified transport/delivery fields.

Concrete fixture result: BOOK_A/TREND_FOLLOWING ES changed from system proposal **12** to QT **7**; TREND_FOLLOWING_FAST ES remained **-3**; combined ES was **4**. BOOK_B remained **77**. NG's QT zero row remained in PostgreSQL but was absent from open-position HTML/CSV. After a captured snapshot of 7, an authenticated edit committed 9: both outputs from that captured snapshot stayed 7, and the next snapshot read 9.

The unchanged-content assertion compares all HTML text except validated position data rows and one numeric/currency token after each of six known position-derived summary labels. It validates the five-cell row shape and value formats before masking, rejects duplicate identities, and compares strategy headings and column headers separately. Mutations inserting unrelated paragraphs, summary prose or a one-cell banner row are rejected. CSV metadata/header whitespace is retained by the comparison helper. Production renderer/template code did not change.

**Precision limit identified in final review:** artifact reads use `Path.read_text()`, which normalizes disk line endings before comparison. This proves newline-normalized text equality, not raw file-byte or original CRLF/LF equality. The retained manifest/check label saying "byte-stable" must be read with this explicit qualification. Legitimately variable position rows are not claimed identical. Separately, the 12 SHA-256 checks do verify exact bytes of each retained artifact copy against its source manifest; they are not before/after report-equality checks.

The integration run exposed and resolved three test-environment/fixture issues: inaccessible PID-1 metadata (replaced with direct no-egress verification), a nonnumeric synthetic user ID (corrected to numeric JWT identity without changing the real audit schema), and unsupported FIFOs on `/mnt/c` (native Linux output selected). The final passing run supersedes those failures. The rejected-role warning is expected evidence of the negative authorization test, not an unexpected production error.

## Local evidence locations

Raw logs and generated synthetic artifacts are ignored, not deployment artifacts:

- `.cache/no-send-cpp-build.log`, `.cache/no-send-cpp-suite.log`, `.cache/no-send-cpp-suite.json`
- `.cache/no-send-backend-unit.log`
- `.cache/no-send-migration012.log`
- `.cache/qt-no-send-certification.log`
- `.cache/qt-no-send-certification/qt-pipeline-7426bd0ffa39/evidence-manifest.json` and its 12 synthetic CSV/HTML artifacts

The retained manifest preserves the original Linux artifact paths; the mirrored files have the same relative paths under the cache directory above. No production payloads or credentials are included.

## Files changed by this implementation

- `tests/integration/test_qt_report_pipeline.py`: authenticated route coverage, isolated fixture, scoped assertions, snapshot synchronization and evidence manifest.
- `tests/integration/qt_report_probe.cpp`: exact synthetic Unix socket validation, snapshot barrier and report-date identity.
- `tests/test_qt_report_pipeline_safety.py`: seven fast offline safety regressions.
- This audit and the certification plan's explicit no-send override. Previously present rollout/inventory documents are preserved.

No production C++, AlgoLens application code, migration SQL, email template, sender or runtime configuration changed in this phase. No pull, deployment, production write, SMTP sink, internal email or investor email was performed. Changes remain local and uncommitted.

Cleanup was also strengthened: future fixture removal includes its own anonymous PostgreSQL volume. Three volumes from earlier runs were individually attributed using Docker events, timestamps and absence of container references, then removed by exact ID. No broad prune or unknown-resource deletion occurred. Their synthetic data can be recreated; report evidence was retained.

## Remaining certification limits

Authenticated route coverage does not by itself certify the real browser/UI, deployed API, running scheduler, production database freshness or investor receipt. Production still needs the separately approved migration/application rollout and current data verification. The no-send restriction means actual delivery cannot be certified by this work.

Any pre-existing SMTP certificate-verification issue remains documented in the plan and unchanged; there is no SMTP activity in this phase. No implementation or test result here authorizes a live send.
