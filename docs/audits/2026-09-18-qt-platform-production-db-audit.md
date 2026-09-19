# QT platform production database audit

> Related local build and disposable-PostgreSQL evidence is recorded in
> [`2026-09-19-qt-platform-verification.md`](2026-09-19-qt-platform-verification.md).
> That evidence does not replace the blocked production read-only audit.

**Verdict: blocked for live evidence.** Repository contracts were inspected, but an existing approved production connection could not be established from the scoped local configuration. Production schema compatibility, position integrity, and legacy override classifications remain unverified. This report does not satisfy the design's live-audit acceptance criterion or authorize deployment.

## Evidence scope and provenance

- Audit checkpoint: **2026-09-19T02:32:18Z (UTC)**. The filename follows the design's 2026-09-18 task date.
- Trade Ngin source inspected: `bc07b4e70a74e9c3bdfe8e1d3651e68163b4be0c`.
- AlgoLens source inspected: `5a51c466cff1d58047f869c6ab5c5b929e177822`.
- Scope: local QT reporting and override schema contracts, approved-client discovery, and the production checks specified in Task 8.
- Method: the auditing-project-truth evidence hierarchy. Local implementation, planned migrations, live schema, and live data are separate evidence classes.
- Both tracked worktrees were clean when their source evidence was inspected. Repository SHAs identify local source, not deployed binaries or applied database migrations.

## Safety posture and access finding

Existing PostgreSQL client binaries were found. No approved production connection configuration was established. Filename-only discovery in the two review worktrees and corresponding source checkouts found environment examples but no actual environment files. Standard local PostgreSQL service/password configuration files checked were absent. A boolean-only check found none of the relevant PostgreSQL or application connection environment settings. No environment values were emitted, and no secrets were requested or reconstructed. This is a scoped discovery result, not a claim that production access does not exist elsewhere.

The repository's schema checker accepts connection material, but its existence is not proof of approved production access. The exploratory schema script can print sample rows and does not implement this audit's posture gate; neither script was executed.

**Database connections attempted: 0. SQL statements executed: 0. Production changes: 0.** Database identity, session user, default read-only setting, and transaction read-only setting were not observed. No transaction was opened, so there was no transaction to roll back. No migration, repair, test write, stored procedure, temporary object, explicit lock, or session-setting change was attempted. Migration 012 was read as source text only and was not applied.

## Highest-impact gaps

1. Live schema compatibility is blocked. AlgoLens's local contract requires the new override portfolio column and legacy-scope table; their production presence is unknown. Do not treat passing local tests or the migration file as deployment evidence.
2. Live QT coverage, duplicate keys, divergence, and zero closures are blocked. No operational report correctness claim can be inferred from this audit.
3. Legacy rows with zero, one, or multiple candidate portfolios were not counted. Migration 012's local inference joins strategy and symbol across available positions without a date predicate. A future live audit must examine available date evidence before any separate migration/deployment decision; this report establishes no safe legacy mapping.

## Evidence matrix

| Item | Owner/repository | Claimed state | Evidence | Verified state | Gap/next action |
|---|---|---|---|---|---|
| Read-only transaction and database identity | Production PostgreSQL | Required before any audit query | No approved connection established; zero SQL executed | blocked | Use an existing approved client and prove the required posture first |
| Applied migration/version evidence | Production PostgreSQL | Must support the proposed application contract | No live catalog observations | blocked | Collect permitted live catalog evidence; source migration presence is insufficient |
| Full position key and stream contract in source | Trade Ngin | Six-part key separates books, strategies, dates, symbols, and streams | Migration 001 defines `(portfolio_id, strategy_id, strategy_name, date, symbol, portfolio_type)`; migration 003 permits `system`, `qt`, and `benchmark` | done | Verify the corresponding production constraints separately |
| Production position columns, types, defaults, nullability, keys, checks, and indexes | Production PostgreSQL | Match the reporting contract | Only migration source and application expectations available | blocked | Inspect allowed catalogs after proving read-only posture |
| Existing override append-only contract in source | Trade Ngin | Override rows have update/delete suppression rules | Migration 004 defines the table, indexes, and two rules | done | Rules in source do not prove live enforcement or protection from privileged bypass/TRUNCATE |
| Portfolio-scoped override migration in source | Trade Ngin | Preserve original legacy rows while requiring portfolio scope for new rows | Migration 012 adds a `TEXT` portfolio column, a `NOT VALID` non-null check, portfolio/strategy/time index, and append-only legacy mapping table | done | Planned migration only; production application and data suitability remain blocked |
| Companion schema requirements | AlgoLens | Reads/writes require portfolio-scoped override history | `algolens-api/algolens/infrastructure/db/schema_contract.py` declares the new column and legacy table | done | Compare against live schema before deployment |
| Local report snapshot handoff | Trade Ngin | Fresh QT snapshot supplies report position maps | `apps/strategies/live_portfolio_runner.cpp` calls `load_qt_report_position_snapshot`, blocks on error, and passes its strategy map to the current-positions exporter | partial | This source inspection is not a runtime or investor-report test |
| Live override and legacy-table columns, constraints, indexes, and append-only rules | Production PostgreSQL | Support scoped history without rewriting legacy evidence | No live catalog observations | blocked | Inspect `information_schema.columns`, `pg_indexes`, `pg_constraint`, and `pg_rules` |
| Stream population and duplicate full position keys | Production PostgreSQL | Supported streams and unique complete identities | No grouped data queries executed | blocked | Return aggregate stream categories and duplicate-group counts only |
| Latest system/QT coverage and missing QT evidence | Production PostgreSQL | Complete reporting evidence within each full scope | No grouped data queries executed | blocked | Count coverage categories using complete identity and scoped dates |
| QT zero closures and QT/system divergence | Production PostgreSQL | Zero rows count as closure evidence; quantities may differ | No grouped data queries executed | blocked | Return closure/divergence counts, without quantities or raw position identifiers |
| Legacy overrides: zero, exactly one, or multiple candidate portfolios | Production PostgreSQL | Only unambiguous scope may be considered later | No classification queries executed | blocked | Count candidate classes using strategy/symbol and available date evidence; do not insert mappings |
| Duplicate/orphaned/cross-book-ambiguous audit evidence | Production PostgreSQL | Audit history remains attributable | No data observations | blocked | Establish aggregate anomaly categories within the permitted scope |
| Production compatibility and visible reporting behavior | Shared | Proposed changes work in the deployed environment | Local source only; no live or deployment evidence | not verifiable | Complete the live audit and separate deployment/runtime verification |

The local `done` rows mean the specified source artifacts were verified to exist and contain the stated contract. They do not mean the migration is applied, tests were rerun in this audit, or production is ready.

## Sanitized count categories

All requested production counts are **not collected**, not zero: supported/other stream populations; duplicate complete-key groups; system/QT coverage groups; missing-QT groups; QT zero closures; equal/different/unmatched QT/system groups; legacy rows with zero/exactly-one/multiple candidates; and duplicate/orphaned/ambiguous override groups. No database names, account names, investor identifiers, user identifiers, symbols, reasons, quantities, or payload samples were retained.

## Permitted resumption sequence

1. Resolve an existing approved production client/configuration through the normal access owner. Do not infer a target from example files or guess access.
2. The first SQL must be `BEGIN READ ONLY;`, followed by the exact Task 8 posture query selecting database/user identity and the transaction/default read-only settings. Keep identity values in memory and emit only sanitized status. Stop immediately unless `transaction_read_only` is `on`.
3. Only after that gate, run catalog `SELECT`s against the four allowed catalog surfaces and grouped/count/sanitized position and legacy-classification `SELECT`s. Scope positions with the complete six-part key; compare streams within matching book, strategy, name, date, and symbol scopes. Never export raw identifiers or payloads.
4. End with `ROLLBACK;`, including after query errors when possible. Document only sanitized observations and timestamps. Do not apply migration 012 or repair any data.
5. Review the evidence and unresolved legacy/date ambiguities before a separately authorized deployment or migration decision.

## Verification and limits

This was documentation-only work. Source migration and contract files were inspected, not executed. No application, migration, or integration tests were run against production. The documentation is subject to a content review for secrets/raw data and `git diff --check` before commit. No compliance certification, absence-of-vulnerability claim, or production readiness claim is made.
