# The futures release

What this folder does: it takes production from the old futures setup to the released one. It loads micro gold (MGC) and
micro silver (SIL), fills the missing continuous rows, and re-seeds the CONSERVATIVE book's stored backtests and live
history on the merged engine. It runs once, at the release (AlgoGators/trade-ngin#270), by one operator, in one sitting.

**Already done on production (2026-10-30):** step 03 (migration 040) and step 05 (part A: 47,790 MGC and SIL contract
rows to 2026-10-26, and 2 metadata rows), and the rewording of those 2 rows' notes, which `load/load_micro_metals.sql`
now writes as stored. Do not run them again; both refuse.

## Prerequisites

- The integration PR is merged. `REF` below is its merge commit.
- `config/defaults.json` has the production login. It is read at run time and never printed.
- `config/databento.env` has the Databento key (the pull).
- zsh, Python 3, `psql` and `pg_dump` matching the server, `zstd`, cmake and the engine's build tools, on macOS (the scripts
  use `sed -i ''`).
- `RUN_DIR`, default `<repository>/futures_release_run`, not tracked: every log, archive and result goes there. It holds
  bought bars (the pull, the built rows, each run's ledger): never commit it.

## The order

It cannot be changed: each load step refuses out of order, and a continuous MGC or SIL row stored without its listing
dates makes every futures config without them refuse to run.

| # | Step | Who | Command (from the repository root; `S=scripts/futures_release`) |
|---|---|---|---|
| 1 | The Databento pull, 2026-10-27 to the day before the merge | operator | `START=2010-06-06 zsh $S/pull_micro_metals.sh <merge day>` (`PULL_REQUEST.md`) |
| 2 | Build the rows | operator | `zsh $S/06_build_backfill.sh production "$RUN_DIR/pull_<today>"` |
| 3 | Part A2: the contract bars after 2026-10-26 | operator | `BACKFILL=<folder> zsh $S/06_micros_part_A2.sh production` |
| 4 | AlgoGators/algogators#84 merges (daily per-contract pull of MGC and SIL) | data owner | the pull's D-5 to D re-fetch covers any day after A2 |
| 5 | The hole fill, with AlgoGators/algogators#87 merging alongside | operator, data owner | `zsh $S/04_fill_holes.sh production 476 [<all rows>]` |
| 6 | The QT read (below) | operator | `zsh $S/01_readonly_checks.sh production before_07` |
| 7 | Part B: MGC.v.0 and SIL.v.0, once, with every day | operator | `ROWS=<folder>/rows BACKFILL=<folder> zsh $S/07_micros_part_B.sh production` |
| 8 | The micros' 8 holes | operator | `zsh $S/08_fill_micros.sh production` (type 8) |
| 9 | AlgoGators/algogators#85 merges (daily MGC.v.0 and SIL.v.0) | data owner | after part B: it refuses if those rows are stored |
| 10 | The listing pairs in every production futures config, QT's included | QT desk's owner, operator | MGC before GC from 2010-10-04, ratio 10; SIL before SI from 2013-06-17, ratio 5 |
| 11 | The merged build on production; `per_contract_rolls` on | operator | |
| 12 | Check the data | operator | `zsh $S/01_readonly_checks.sh production after_data` (`EXPECTED.md`: must equal the fingerprints) |
| 13 | The backtests: 2y, 5y, 10y, 16y, all on one last bar | operator | `TARGET=production REF=<sha> zsh $S/09_backtests.sh prepare`, then `... run` |
| 14 | The live reset, archive first | operator | `TARGET=production CONFIRM=new_algo_data zsh $S/10_live_reset.sh reset` |
| 15 | The dated live history, 2025-10-05 to the run that settles the last complete bar | operator | `TARGET=production REF=<sha> zsh $S/11_live_chain.sh prepare`, `... run`, `... check`, `... export` |
| 16 | The package | operator | `TARGET=production REF=<sha> zsh $S/12_package.sh backtests`, `... 12_package.sh live <END>` |

Run one step at a time. Stop at the first mismatch. Never pass `--send-email`: no script does, and every run's log is
checked for an e-mail line. Run the read-only check after every step (the last column below), and go on only when it
reads as `EXPECTED.md` says.

## What each step changes and how to undo it

Every script says at its top what it changes, what it reads, how long it takes and how to undo it.

| Step | Changes | Must print | Undo | Check after |
|---|---|---|---|---|
| 06 A2 | contract rows of MGC and SIL after 2026-10-26 | `fingerprint check: PASS` | delete those rows (root MGC or SIL, date after 2026-10-26) | `01_readonly_checks.sh` |
| 04 hole fill | the missing `.v.0` rows of both continuous tables, insert only | `476 rows into each table`, `04 fingerprint check: PASS` | delete the (symbol, date) pairs of the run's ledger in `$RUN_DIR/fill/` | `EXPECTED.md`, 04 |
| 07 part B | MGC.v.0 and SIL.v.0 in both continuous tables, insert only | `fingerprint check: PASS` | delete those two symbols from both tables | the QT lines, `EXPECTED.md`, 07 |
| 08 micros fill | 8 rows per table | `8 rows into each table`, `second dry run: 0 holes left` | delete the (symbol, date) pairs of its ledger in `$RUN_DIR/fill_micros/` | the data fingerprint |
| 09 backtests | one new run per window, insert only | `Total Return` per window, the run ids in `runs.tsv` | delete the run ids' rows from each backtest table | 4 more stored runs |
| 10 reset | deletes CONSERVATIVE_PORTFOLIO's live rows, and no other book's, after the archive | `RESET DONE`, the archive's path | `10_live_reset.sh restore <archive>` | 0 live rows, `live_start_date` 2025-10-05 |
| 11 chain | one day per run, CONSERVATIVE_PORTFOLIO only | `CHAIN DONE`, then every identity PASS | `10_live_reset.sh restore <archive>` | `check`, other books UNCHANGED |

## The QT precondition

QT_CONSERVATIVE_PORTFOLIO writes a row to production every day. The live runner takes its universe from the stored `.v.0`
symbols. So once MGC.v.0 and SIL.v.0 are stored, a QT run without the micros' listing pairs refuses, or, on code without
the refusal, takes MGC and SIL in beside GC and SI.

**Before step 7, read the QT lines of `01_readonly_checks.sh`:** each QT book's last live day, what it holds, and how many
symbols it signals. The QT desk's owner must have the pairs ready for step 10 and the QT book's run planned around the
release. Never touch a QT book's rows: every statement in these scripts names CONSERVATIVE_PORTFOLIO.

## What is in the folder

| Path | What |
|---|---|
| `01_readonly_checks.sh`, `sql/`, `EXPECTED.md` | the read-only checks and the answer each must give |
| `03_apply_040.sh` | migration 040 (done) |
| `04_fill_holes.sh`, `fill/` | the hole fill, with its tested ledger (keys and a hash of each bar; the bought bars are not in the repository) |
| `05_preflight.sh`, `05_micros_part_A.sh` | part A (done) |
| `06_build_backfill.sh`, `06_micros_part_A2.sh`, `07_micros_part_B.sh`, `micros_part.zsh`, `load/` | the MGC and SIL load |
| `08_fill_micros.sh` | the micros' 8 holes (`fill/LEDGER_fill_micros.csv`) |
| `09_backtests.sh`, `10_live_reset.sh`, `11_live_chain.sh`, `12_package.sh`, `report/` | the seed and its package |
| `config_reference/` | the frozen run config every seed run is written from |
| `pull_micro_metals.sh`, `PULL_REQUEST.md` | the Databento pull |

## Tested, and not tested

The copies in this folder ran end to end on a copy of production: part A, the hole fill, part B and the micros fill reproduce
the data fingerprints in `EXPECTED.md`, and each refuses a second run. Both fills and the load are plain INSERTs, and
part A has run on production's TimescaleDB hypertables. Part A2 ran on test rows only: it needs the pull. The backtests equal the
stored reference runs on every cell, and the dated chain equals the backtest of the same year.
