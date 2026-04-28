# Unit Testing — Phase 3 Wrap-up and Deferred Refactor Plans

**[PHASE3_REPORT.md](PHASE3_REPORT.md)** is the wrap-up doc for the `feature/test-coverage-improvements` branch. It covers:
- Headline coverage numbers (57.2% repo-wide, 74.2% in-scope)
- How to reproduce the numbers (build + lcov commands per module)
- Per-file table for all 14 modules with reasons for any sub-80% file
- 8 production FIXMEs found during testing
- Forward plan with priorities A (production fixes), B (DB-DI refactor), C (push remaining sub-80% files), D (chart/email refactors)
- Recommended sequence for maximizing coverage-gain-per-hour

The three files below are the source-level refactor plans referenced from PHASE3_REPORT priority sections D and B.

Three production files were deferred from the unit-test coverage push because they require source-level refactoring (or have a tight external-system coupling) before they can be meaningfully unit-tested. Each file in this directory is a detailed plan for one of them.

| File | Source LOC | Current cov | Target cov after refactor | Effort |
|---|---|---|---|---|
| [chart_generator_refactor.md](chart_generator_refactor.md) | 1230 | 0.1% | 75% line / 50% branch | 3–4h |
| [email_sender_refactor.md](email_sender_refactor.md) | 2226 | 0.1% | 85% line / 55% branch | 2–3h |
| [postgres_database_refactor.md](postgres_database_refactor.md) | 1828 (incl extensions) | 1.8% | 70% line / 35% branch | 1–2 days |

**Total deferred LOC:** 5,284 (~23% of project-only line total of 22,871).

Without these refactors, the project-only line coverage ceiling is ~73%. With all three landed, the ceiling rises to ~78–82%, putting the long-term 80% target within reach.

Each plan documents:
- Why the file can't be unit-tested today
- Specific source changes needed (with line counts)
- Interface / API additions
- What gets unit-tested after the refactor
- Test-file plan and effort estimate
- Anything that still stays out of scope (real IO loops, integration concerns)
