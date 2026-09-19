# QT platform production database audit

> Related local build and disposable-PostgreSQL evidence is recorded in
> [`2026-09-19-qt-platform-verification.md`](2026-09-19-qt-platform-verification.md).

**Verdict: the live read-only audit completed successfully, but production is not compatible with the proposed QT reporting release.** Migration 012 is absent, the QT position stream is empty, and every latest system scope lacks QT evidence. No production change was made.

## Evidence scope and provenance

- Live audit checkpoint: **2026-09-19T06:19:52Z (UTC)**. The filename retains the design task's 2026-09-18 date.
- Trade Ngin source under review: `a74f1afc45cb5d94aa5bc5d30c18d1a710825fe4`.
- AlgoLens source under review: `ccd0571ad4b428b45c8e40de964b0ba81eca99be`.
- Database and role identities are retained only as redacted fingerprints: `db#dcd5f3f8` and `role#e8a48653`.
- The approved local connection material matched exactly one pgAdmin server profile. Its maintenance database was used only to discover the single accessible database containing both `trading.positions` and `trading.position_overrides`.
- Runtime database observations prove the live schema and aggregate data state at the checkpoint. Source SHAs prove only the local implementations inspected; they do not prove those binaries are deployed.

## Safety posture

The first successful connection opened `BEGIN READ ONLY` before the posture query. PostgreSQL reported `transaction_read_only=on`; the login's default was `off`. Every subsequent audit connection additionally forced `default_transaction_read_only=on` through the connection options and still opened an explicit `BEGIN READ ONLY` transaction. Those sessions reported both settings `on`.

The saved pgAdmin login itself is **not** a read-only account. Catalog checks show that it is a superuser with create-database, create-role, replication, and row-security-bypass attributes. It also has schema creation plus `SELECT`, `INSERT`, `UPDATE`, `DELETE`, and `TRUNCATE` privileges on both audited tables. Neither audited table has row-level security enabled. The session controls—not the credential—provided the read-only boundary for this audit.

Only catalog and aggregate `SELECT` statements were executed. Every successful audit transaction ended with `ROLLBACK`. No migration, DDL, DML, procedure, temporary object, explicit lock, repair, raw-row export, or test write was attempted. No credential, investor identifier, portfolio identifier, strategy identifier, symbol, user identifier, reason, individual quantity, or JSON payload is retained here.

## Highest-impact findings

1. **Migration 012 is not applied.** `trading.position_overrides` has no `portfolio_id` column. `trading.position_override_legacy_scopes` is missing. The new-row portfolio constraint, portfolio/strategy/time index, and append-only rules for the companion table are therefore also missing.
2. **The QT stream has no production data.** All 3,803 position rows are in the `system` stream. Across nine latest system book/strategy scopes and 64 latest system symbol keys, there are zero QT evidence rows. All nine scopes lack QT evidence, and all 64 system keys lack a matching QT key.
3. **There is no production override history to migrate.** `trading.position_overrides` contains zero rows. The zero-, one-, and multiple-portfolio legacy candidate categories are therefore all zero, as are exact duplicate override groups. This removes legacy-row ambiguity at this checkpoint but does not substitute for applying migration 012.
4. **The position primary key is healthy.** It covers portfolio, strategy ID, strategy name, date, symbol, and stream. The audit found zero duplicate complete-key groups.
5. **The supplied credential is operationally over-privileged.** The audit was safely constrained, but future routine audits should use a dedicated read-only role or approved service profile rather than relying on client-enforced session settings around a superuser.

## Evidence matrix

| Item | Owner/repository | Claimed state | Evidence | Verified state | Gap/next action |
|---|---|---|---|---|---|
| Read-only audit session | Production PostgreSQL | Required before any live query | Initial transaction reported read-only `on`; subsequent audit sessions also forced default read-only `on`; every successful session used `BEGIN READ ONLY` and rolled back | done | Preserve this transaction gate for any repeat audit |
| Read-only credential | Production access | Prefer a credential that cannot write | Connected role is a superuser with full audited-table write privileges | missing | Provision or identify a dedicated read-only role/profile |
| Core position schema | Production PostgreSQL | Complete six-part identity and stream discriminator | Full six-column primary key is present; no duplicate full keys; `portfolio_type` is present | done | No correction required for this feature's position identity |
| Position stream constraint | Production PostgreSQL | Support the streams expected by deployed source | Live constraint permits only `system` and `qt`; later benchmark stream values in repository migrations are absent | partial | Reconcile deployed migration level separately from the QT release decision |
| Base override append-only schema | Production PostgreSQL | Existing override evidence is append-only | Base table, primary key, supporting indexes, and no-update/no-delete rules are present | done | Retain these rules during migration 012 |
| Portfolio-scoped override schema | Trade Ngin migration 012 / AlgoLens contract | New audit rows identify portfolio and history is portfolio-scoped | Portfolio column, companion table, constraint, index, and companion rules are absent | missing | Apply migration 012 only in a separately authorized write/change window before deploying the dependent AlgoLens code |
| Stream population | Production PostgreSQL | QT state exists alongside system proposals | 3,803 system rows; zero QT rows | missing | Deploy/run the approved seed lifecycle only after release prerequisites are met, then re-audit |
| Latest system-to-QT coverage | Production PostgreSQL | Every report scope has QT evidence | Nine latest system scopes and 64 system keys; zero QT evidence; all scopes and keys missing QT | missing | Current production data cannot support the strict QT report snapshot |
| QT divergence and closures | Production PostgreSQL | Manual differences and zero closures are represented | No QT rows, so equal/different pairs and zero closures are all zero | not verifiable | Re-audit after legitimate QT state exists; zero here means absent evidence, not successful equivalence |
| Legacy override classification | Production PostgreSQL | Existing rows classified by candidate portfolio | Override table contains zero rows; all candidate and duplicate categories are zero | done | Record that migration 012 has no existing legacy rows to infer at this checkpoint |
| Production compatibility with reviewed code | Shared | Database supports the proposed Trade Ngin and AlgoLens contracts | QT evidence is absent and migration 012 schema is absent | missing | Do not treat this checkpoint as deployment approval |

## Sanitized aggregate evidence

| Measure | Count |
|---|---:|
| Total production position rows | 3,803 |
| System stream rows | 3,803 |
| QT stream rows | 0 |
| Duplicate complete position-key groups | 0 |
| Latest system book/strategy scopes | 9 |
| Latest system symbol keys | 64 |
| Latest scopes missing all QT evidence | 9 |
| Latest system keys missing matching QT evidence | 64 |
| Observed QT zero-closure rows (QT stream absent; behavior not verifiable) | 0 |
| Position override rows | 0 |
| Legacy overrides with zero / one / multiple candidate portfolios | 0 / 0 / 0 |
| Exact duplicate override groups | 0 |

## Daily Trading Report interpretation

The reviewed code preserves the existing email template, recipients, schedule, and non-position content. However, the current production database has no QT position evidence to supply the report. The strict report path must not be represented as production-ready until the authorized migration/deployment sequence has occurred and a repeat read-only audit confirms QT coverage. This audit did not send an email, run the live portfolio process, seed rows, or verify a deployed runtime.

## Required next sequence

1. Have the production owner approve a write/change window for migration 012. This audit does **not** authorize or perform that migration.
2. Apply migration 012 through the normal controlled deployment process, then verify the portfolio column, companion table, constraint, index, and append-only rules.
3. Deploy the compatible Trade Ngin and AlgoLens releases in the approved order and run the normal QT seed lifecycle. Do not manufacture audit rows or test writes in production.
4. Repeat this read-only audit. Require non-missing QT evidence for every report scope before enabling or representing strict QT-backed investor reporting as ready.
5. Replace the superuser connection with a dedicated read-only audit role or service profile for future verification.

## Verification limits

This audit establishes live schema and aggregate data facts only. It does not prove that reviewed source commits are deployed, does not authorize deployment, does not validate email delivery, and is not a security or compliance certification. The lack of QT and override rows is an observed absence of evidence—not proof that manual position editing works.
