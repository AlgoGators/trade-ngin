# Conservative futures evaluation — issue #113

**Status:** Awaiting HD's UTC scratch snapshot and real runs. This is an empty evidence template; no strategy conclusion exists yet.

## Provenance

| Evidence | Value |
|---|---|
| Snapshot UTC and scratch database name | PENDING |
| Latest merged engine `main` full SHA at handoff | PENDING |
| Resolved config hashes and runner binary hash | PENDING |
| Window protocol, start/end, warm-up rule | PENDING |
| Data vintage, migrations, row counts, roll/duplicate checks | PENDING |
| Run IDs, portfolio IDs and exported CSV SHA-256 hashes | PENDING |
| Complete trial ledger including missing exports | PENDING |

## 1. Causality gate

| Short run / window | Long run / window | Prefix rows | First difference | Result |
|---|---|---:|---|---|
| PENDING | PENDING | PENDING | PENDING | PENDING |

**Stop rule:** If any prefix row differs or is missing, report the first row and do not interpret sections 2–4.

## 2. Fixed conservative run precision

| Metric | Point estimate | 95% block CI (20 days) | Blocks 10 / 40 | IID CI |
|---|---:|---:|---:|---:|
| Annualized Sharpe (252) | PENDING | PENDING | PENDING | PENDING |
| Compounded annual return | PENDING | PENDING | PENDING | PENDING |
| Max drawdown | PENDING | PENDING | PENDING | PENDING |

PSR against zero: PENDING. Drawdown distribution and years until Sharpe interval excludes zero: PENDING. Reconcile these recalculations with engine-stored metrics; explain annualization, warm-up and cost differences.

## 3. Scored calendar years

PENDING: one row per year with actual scored day count, fixed configuration, point estimates and the same block/IID intervals. Distinguish retrospective slices from years selected before they began.

## 4. Configuration search and offline selection

PENDING: all attempted variants and trial count, T×N common date index, exclusions, CSCV PBO with 252 splits where possible, DSR with raw full-trial count, and trailing-year selection applied to the following year versus the fixed variant. If trial history is incomplete, label PBO as a demonstration and omit definitive DSR.

## Limitations and HD review

PENDING: assumptions, sample window and roll splice, warm-up evidence, bootstrap dependence sensitivity, PSR/DSR assumptions, and what the evidence can and cannot support. Record HD's review before running equity mean reversion.
