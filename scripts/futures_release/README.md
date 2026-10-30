# The futures release

What this folder does: it takes production from the old futures setup to the released one. It loads micro gold (MGC) and
micro silver (SIL), fills the missing continuous rows, and re-seeds the CONSERVATIVE book's stored backtests and live
history on the merged engine. It runs once, at the release (AlgoGators/trade-ngin#270), by one operator, in one sitting.

**Already done on production (2026-10-30):** step 03 (migration 040) and step 05 (part A: 47,790 MGC and SIL contract
rows to 2026-10-26, and 2 metadata rows), and the rewording of those 2 rows' notes, which `load/load_micro_metals.sql`
now writes as stored. Do not run them again: part A refuses a second run.

## Prerequisites

Run `zsh scripts/futures_release/00_prerequisites.sh` first. It checks everything in this list that a machine can check.

- AlgoGators/trade-ngin#282 is merged. `REF` below is its merge commit on `main`.
- `config/defaults.json` has the production login. It is read at run time and never printed.
- `config/databento.env` has the Databento key (the pull).
- macOS (the scripts use `sed -i ''` and `md5`), with zsh, `zstd`, `curl`, `git`, cmake and the engine's build tools.
- Python 3 with **pandas** and **numpy** (the live report imports both; tested with pandas 3.0.0 and numpy 2.4.1). Every
  other Python file uses the standard library only.
- `psql`, `pg_dump` and `pg_restore` matching the server, found on `PATH` or in `PGBIN`.
- `RUN_DIR`, default `<repository>/futures_release_run`, not tracked: every log, archive and result goes there. It holds
  bought bars (the pull, the built rows, each run's ledger): never commit it. Set it once with `export RUN_DIR=<folder>` if
  you want another place, and keep it for every step.
- `S=scripts/futures_release` in your shell, and every command run from the repository root.

## Before the seed: the fees

SIL's exchange fee is still to confirm (0.60, so 0.86 per contract, is a value from an older notice; MGC's 0.96 is confirmed).
Confirm it from CME's fee schedule or the charge on a SIL fill **before step 13**. The search reads the fee, so a different
fee makes a different whole-contract book from the first traded day. It also sits in the metadata row, so it changes the
metadata fingerprint. If the fee changes, the reference anchors must be remeasured first, and `EXPECTED.md`,
`sql/expected_fingerprint.txt` and the live reference in `11_live_chain.sh` rewritten from the new runs. Do not pass
`ACCEPT_METADATA_MD5` for a fee: it is for a display column only. The margins change no book row.

## The order

Keep this order. Only some wrong orders are refused, and the table below says which. A continuous MGC or SIL row stored
without its listing dates makes every futures config without them refuse to run, and on code without that refusal the run
takes MGC and SIL in beside GC and SI.

| # | Step | Who | Command (from the repository root) |
|---|---|---|---|
| 0 | The machine | operator | `zsh $S/00_prerequisites.sh` |
| 1 | The Databento pull, 2010-06-06 (the history part B needs) to the day before the merge | operator | `zsh $S/pull_micro_metals.sh <merge day>` (`PULL_REQUEST.md`); it prints `done: <pull folder>` |
| 2 | Build the rows | operator | `zsh $S/06_build_backfill.sh production "<pull folder>"`; it prints `BACKFILL=<folder>` and `ROWS=<folder>/rows` |
| 3 | Part A2: the contract bars after 2026-10-26 | operator | `BACKFILL="<folder>" zsh $S/06_micros_part_A2.sh production` |
| 4 | AlgoGators/algogators#84 merges (daily per-contract pull of MGC and SIL) | data owner | its first daily run must come after step 7 (below) |
| 5 | The hole fill, with AlgoGators/algogators#87 merging alongside | operator, data owner | `zsh $S/04_fill_holes.sh production 476` (below if it stops at exit 5) |
| 6 | **Stop the QT runs** (below), then read the QT lines | QT desk's owner, then operator | `zsh $S/01_readonly_checks.sh production before_07` |
| 7 | Part B: MGC.v.0 and SIL.v.0, once, with every day | operator | `ROWS="<folder>/rows" BACKFILL="<folder>" zsh $S/07_micros_part_B.sh production` |
| 8 | The micros' 8 holes | operator | `zsh $S/08_fill_micros.sh production` (type 8, or more if the list shows holes after 2026-10-23) |
| 9 | AlgoGators/algogators#85 merges (daily MGC.v.0 and SIL.v.0) | data owner | only after part B: part B refuses once #85 has stored those rows |
| 10 | The listing pairs in every production futures config, QT's included | QT desk's owner, operator | below |
| 11 | The merged build on production; `per_contract_rolls` on; **the QT runs restart** | deployment owner, QT desk's owner | below |
| 12 | Check the data | operator | `zsh $S/01_readonly_checks.sh production after_data` (`EXPECTED.md`: must equal the fingerprints) |
| 13 | The backtests: 2y, 5y, 10y, 16y, all on one last bar | operator | `TARGET=production REF=<sha> zsh $S/09_backtests.sh prepare`, then `... run` |
| 14 | The live reset, archive first | operator | `TARGET=production zsh $S/10_live_reset.sh reset` (type the database name) |
| 15 | The dated live history, 2025-10-05 to the run that settles the last complete bar | operator | `TARGET=production REF=<sha> zsh $S/11_live_chain.sh prepare`, `... run`, `... check`, `... export` |
| 16 | The package | operator | `TARGET=production REF=<sha> zsh $S/12_package.sh backtests`, then `... 12_package.sh live` |

Run one step at a time. Stop at the first mismatch. Never pass `--send-email`: no script does, and every run's log is
checked for an e-mail line. Run the read-only check after every step (the last column below), and go on only when it
reads as `EXPECTED.md` says.

A command's first word beats a `TARGET` left in the shell, so `zsh $S/04_fill_holes.sh scratch 476` stays on scratch. A scratch
target also stops when `PGHOST` and `PGPORT` are the server of `config/defaults.json`.

**What refuses a wrong order, and what does not:**

| Wrong order | What happens |
|---|---|
| 07 before 04 | 07 refuses, exit 8, nothing written |
| 07 with the QT runs still going | **nothing refuses.** No script can see a server's config (below) |
| 07 without `BACKFILL` after A2 | 07 refuses |
| 08 before 07 | 08 refuses, exit 7 |
| 13 or 15 before the data steps, or on other data | 09 and 11 refuse, exit 3 (`sql/expected_fingerprint.txt`) |
| 15 before 14 | 11 refuses: the book is not empty |
| Any data step a second time | it refuses, exit 1 or 4 |
| #84's first daily run before 07 | 07 refuses if the bars pass the backfill (below); nothing prevents the run itself |
| 10 or 11 skipped or wrong | **nothing refuses** here; the runners refuse at run time |

**Step 5, the holes after 2026-10-23.** Run it with `476` only. If a hole dated after 2026-10-23 exists, the fill stops at the
confirmation (exit 5, nothing written), and prints every hole's list and the count in all. Read the list, then run:

```
zsh $S/04_fill_holes.sh production 476 <count printed>
```

Such a hole has no tested ledger behind it; it passes the rule's checks only. 08 takes such holes the same way.

## Steps 3 to 7 and the daily pull

Steps 3 to 7 must finish before #84's first daily run. Ask the data owner when it runs, and keep clear of it. If a daily pull
lands during 04 or 07, its fingerprint check can read FAIL after the commit: the step's own rows are in; read the log, and
do not run the step again.

**If #84's first daily run lands before step 7**, part B refuses with `the continuous rows end before the stored contract
rows`, and nothing is written. The way on:

1. Find the last stored contract day: the `last bar` line of `01_readonly_checks.sh`, the `contract` date.
2. Pull again with that day plus one as the end: `zsh $S/pull_micro_metals.sh <that day + 1>`. A later end is a new folder.
   Never go past it: a vendor bar with no stored contract bar also refuses.
3. Build it (step 2) to get the new `BACKFILL` and `ROWS`.
4. Do not run A2 again: the daily pull stored those bars, and A2 refuses them.
5. Run step 7 with the new folders.

The same refusal fires if the pull's last day has a contract bar and no `.v.0` bar. Then delete that root's contract rows of that
day, which is A2's undo for that one day, and run step 7. The daily pull stores those bars again, and the holes are then 08's or
#87's.

## The QT runs

**Stop the QT runs before part B. Restart them only after their configs and the merged build are in.** The QT desk's owner
does both, and tells the operator; the operator does not run step 7 before that word.

Why: QT_CONSERVATIVE_PORTFOLIO writes a row to production every day, and the live runner takes its universe from the stored
`.v.0` symbols. No config is valid on both sides of part B (`src/data/listing_dates.cpp`): a config with the MGC and SIL pairs
refuses to run while MGC.v.0 and SIL.v.0 are not stored, a config without the pairs refuses once they are, and code without
that refusal takes MGC and SIL in beside GC and SI.

**This is a warning, not a check.** `01_readonly_checks.sh` shows each QT book's last live day, what it holds and how many
symbols it signals; it cannot show whether the runs are stopped. No script here reads a server's config or schedule, and 07
will run with the QT runs going. Never touch a QT book's rows: every statement in these scripts names CONSERVATIVE_PORTFOLIO.

## The way back from part B

```
zsh $S/07_undo_part_B.sh production
```

It keeps a CSV copy of the two series, then deletes MGC.v.0 and SIL.v.0 from both continuous tables, in one transaction, and
moves 08's marker aside. The contract rows and the hole fill stay. Then step 7 and step 8 can run again, with their old or
new folders. Use it after any part B you cannot stand behind. The QT runs stay stopped until the configs fit what is stored.

## Steps 10 and 11

**Step 10, the file and the keys.** On every production host or service that runs a futures book, in
`config/portfolios/<book>/portfolio.json`, for `base`, `conservative`, `qt_conservative`, `qt_conservative_model` and `qt_e2e`
(`equity_mr` has no futures):

- `listing_dates.contracts` holds these two entries, beside any others:
  `{"symbol": "MGC", "listed": "2010-10-04", "before": "GC", "ratio": 10}` and
  `{"symbol": "SIL", "listed": "2013-06-17", "before": "SI", "ratio": 5}`.
- `per_contract_rolls` is `true`.

`config_template/portfolios/<book>/portfolio.json` at `REF` holds both; copy the keys, not the file, because a host's file has
its own values. To read one back, on that host:

```
python3 -c "import json,sys; d=json.load(open(sys.argv[1])); print(d.get('per_contract_rolls'), [c['symbol'] for c in d.get('listing_dates',{}).get('contracts',[]) if c['symbol'] in ('MGC','SIL')])" config/portfolios/<book>/portfolio.json
```

It must print `True ['MGC', 'SIL']`. Steps 13 to 15 do not read these files: they write their own config from
`config_reference/`.

**Step 11, the build.** It is `REF`, the merge of #282 into `main`, not the head of `main-rolls`. The new image is built from it
and the daily jobs move to `new_algo_data` as AlgoGators/trade-ngin#275 says (its dry run first):

```
git checkout <REF> && docker build -t trade-ngin -f Dockerfile .
```

The QT runs restart on that build, with step 10's configs. Until #275 is done the CONSERVATIVE book has no daily run on
`new_algo_data`, so the seed's rows stay as they are, and the `first CONSERVATIVE cron day` line of `EXPECTED.md` waits for it.

## Steps 15 and 16: one END

`11_live_chain.sh prepare` fixes the chain's END and prints `END=<date>`: the last stored weekday bar plus one day. It is kept in
`$RUN_DIR/chain_end_production.txt`, and `run`, `check`, `export` and `12_package.sh live` all read it, so a daily bar landing in
the 3 hours of the chain moves nothing. An `END=` that differs refuses. After a stop, resume with the printed `FROM=<date>` line.
`12_package.sh backtests` takes the newest finished set, so it is safe across UTC midnight.

## What each step changes and how to undo it

Every script says at its top what it changes, what it reads, how long it takes and how to undo it.

| Step | Changes | Must print | Undo | Check after |
|---|---|---|---|---|
| 06 A2 | contract rows of MGC and SIL after 2026-10-26 | `fingerprint check: PASS` | delete those rows (root MGC or SIL, date after 2026-10-26) | `01_readonly_checks.sh` |
| 04 hole fill | the missing `.v.0` rows of both continuous tables, insert only | `476 rows into each table` (or the count given), `04 fingerprint check: PASS` | delete the (symbol, date) pairs of the run's ledger in `$RUN_DIR/fill/` | `EXPECTED.md`, 04 |
| 07 part B | MGC.v.0 and SIL.v.0 in both continuous tables, insert only | `fingerprint check: PASS` | `07_undo_part_B.sh` | the QT lines, `EXPECTED.md`, 07 |
| 08 micros fill | 8 rows per table, and any hole after 2026-10-23 | `8 rows into each table` (or the count typed), `second dry run: 0 holes left` | `07_undo_part_B.sh`, or delete the (symbol, date) pairs of its ledger in `$RUN_DIR/fill_micros/` | the data fingerprint |
| 09 backtests | one new run per window, insert only | the fingerprint lines `equal`, `Total Return` per window, the run ids in `runs.tsv` | delete the run ids' rows from each backtest table | 4 more stored runs |
| 10 reset | deletes CONSERVATIVE_PORTFOLIO's live rows, and no other book's, after the archive | `RESET DONE`, the archive's path | `10_live_reset.sh restore <archive>` | 0 live rows, `live_start_date` 2025-10-05 |
| 11 chain | one day per run, CONSERVATIVE_PORTFOLIO only | the fingerprint lines `equal`, `CHAIN DONE`, then every identity and the live reference PASS | `10_live_reset.sh restore <archive>` | `check`, other books UNCHANGED |

## What is in the folder

| Path | What |
|---|---|
| `00_prerequisites.sh` | the machine's check |
| `01_readonly_checks.sh`, `sql/`, `EXPECTED.md` | the read-only checks and the answer each must give; `sql/expected_fingerprint.txt` is what 09 and 11 compare |
| `03_apply_040.sh` | migration 040 (done) |
| `04_fill_holes.sh`, `fill/` | the hole fill, with its tested ledger (keys and a hash of each bar; the bought bars are not in the repository) |
| `05_preflight.sh`, `05_micros_part_A.sh` | part A (done) |
| `06_build_backfill.sh`, `06_micros_part_A2.sh`, `07_micros_part_B.sh`, `07_undo_part_B.sh`, `micros_part.zsh`, `load/` | the MGC and SIL load |
| `08_fill_micros.sh` | the micros' 8 holes (`fill/LEDGER_fill_micros.csv`) |
| `09_backtests.sh`, `10_live_reset.sh`, `11_live_chain.sh`, `12_package.sh`, `report/` | the seed and its package |
| `config_reference/` | the frozen run config every seed run is written from |
| `lib.zsh`, `scratch_guard.zsh` | what the scripts share |
| `pull_micro_metals.sh`, `PULL_REQUEST.md` | the Databento pull |

## Dates after 2026-10-23

**Every reference in this folder stops at 2026-10-23.** No date after it has a reference, so a wrong day after it cannot be told
from a right one by comparison.

| Item | What holds |
|---|---|
| Data to 2026-10-23 | fingerprinted. 09 and 11 refuse to run unless it equals `sql/expected_fingerprint.txt` |
| Holes after 2026-10-23 | no tested ledger; the rule's checks only, and the list for the operator to read |
| Bars after 2026-10-23 | no fingerprint; each load step's own counts only |
| The backtests (09) | they end at the last bar, so their windows start later than the reference runs: no number can be compared |
| The live chain to 2026-10-23 | `11 check` compares it with the rehearsal: 384 rows, book value 519,541.86 on 2026-10-23, an md5 of each day's value, P&L and costs |
| The live chain after 2026-10-23 | the nine identities and the other-books compare only. The rehearsal's chain stopped at 2026-10-24, so new contracts and any roll after it have run on nothing |

## Tested, and not tested

The copies in this folder ran on a copy of production: part A, the hole fill, part B and the micros fill reproduce the data
fingerprints in `EXPECTED.md`, and each refuses a second run. The backtests equal the stored reference runs on every cell, and
the dated chain equals the backtest of the same year.

Not tested:

- The fills have never run on a TimescaleDB hypertable; part A's insert has, and compression is off on both tables. The
  apply takes one transaction over about 857 chunks of each of the two hypertables, plus their indexes, about 5,000 locks by
  estimate. Have the database owner read `max_locks_per_transaction` and `max_connections` first; if too small, the dry run stops
  with nothing written.
- Part A2 has run on test rows only: it needs the pull.
- `07_undo_part_B.sh` and every refusal on a production target have run on copies only.
- Step 11's image, and the QT runs on it.
