# The read-only checks and the answers each must give

`zsh scripts/futures_release/01_readonly_checks.sh production <label>` prints every line below (read only:
`default_transaction_read_only`, SELECTs only). The output is kept in `$RUN_DIR/checks/`.

## Before the release (read on production 2026-10-30, after 03 and 05)

| Check | Must read |
|---|---|
| server | new_algo_data, tz UTC, TimescaleDB 2.17.2 or later |
| v0 symbols | 36, no MGC.v.0 or SIL.v.0 |
| c0 symbols | 0 |
| full-size equity v0 (ES NQ YM RTY) | 0: a micro beside its full-size `.v.0` with no pair refuses every runner |
| MGC/SIL stored | 47,790 contract rows (MGC 29,172, SIL 18,618, to 2026-10-26), 0 ohlcv_1d, 0 raw, 2 metadata; more contract rows once #84 or A2 have run |
| symbols behind the newest weekday bar | 0 |
| data_ready last | complete |
| CONSERVATIVE live_results | 375 rows, 2025-10-05 .. 2026-10-14 |
| CONSERVATIVE live_start_date | LIVE_TREND_FOLLOWING 2025-10-05 |
| held GC/SI/MGC/SIL on each book's last day | none: a held predecessor would leave the universe at the release |
| permanent fill_ relations | 0: the fill's temp tables share the name |
| 040 | 8 of 8 columns, 2 of 2 CHECKs |
| schema fingerprint | 572 lines, md5 a3b22ef69309af27d1674a15228f0ed4 |

Data to the cut 2026-10-23, read 2026-10-30 (continuous tables before the fill):

| Table | Rows | md5 |
|---|---:|---|
| futures_data.ohlcv_1d | 166,611 | 67ef60fa863cfa17e9b5c05ae722bf42 |
| futures_data.ohlcv_1d_raw | 166,611 | 5fbef9b7a82fe7828e2d9cf5728e2829 |

## The QT lines (before step 7, and after it)

`QT <book> last <date>`: each QT book's last live day, what it holds on its last positions day by portfolio_type, and how many
symbols it signalled on its last day, with any micro among them. Read 2026-10-30: no QT book holds or signals MGC or SIL,
and each signals on 36 symbols. After the pairs are in, QT's own runs read as the QT desk's owner expects.

## After the data steps: the data to the cut must equal the tuned data

These are the fingerprints of the data every reference run read:

| Line | Must read after 08 |
|---|---|
| futures_data.ohlcv_1d | 176,199 rows to 2026-10-23, md5 dc462978180f96821ecf01a8a6a82953 |
| futures_data.ohlcv_1d_raw | 176,199 rows, md5 17bd9bc870c2cf03df935c1eb42224d1 |
| contract_ohlcv_1d_less_loaded_at | 1,280,034 rows, md5 6104c856dcb47ec71e30a63d43b3dd66 |
| metadata.contract_metadata | 42 rows, md5 37211534a7c7f9d5e5428406f674783e |
| micros ohlcv_1d / ohlcv_1d_raw | 9,112 rows each, md5 3a6e41d348e7aa2c076a54161b0268c2 / 5874a752339469177e5c1e9c9039b143 |
| v0 symbols | 38 (the 36 and MGC.v.0, SIL.v.0) |

The two lines with loaded_at (the contract table whole and the micros' contract rows) differ by the insert clock only.
If the metadata line differs, read what changed: a display column may have been edited since. Any other difference
stops the seed before the backtests: production would not be the data the reference runs read.

Per step (each script checks its own change and prints PASS or FAIL):

| Step | After it |
|---|---|
| 06 part A2 | the contract table grown by the backfill's MGC and SIL rows only |
| 04 hole fill | both continuous tables +476 rows to the cut (and the listed holes after it, with `<all rows>`); the contract table less loaded_at, metadata and micros unchanged |
| 07 part B | both continuous tables grown by the micros' rows only; contract table and metadata unchanged |
| 08 micros fill | both continuous tables +8 rows (4 MGC.v.0, 4 SIL.v.0), equal to `fill/LEDGER_fill_micros.csv` |

## After the backtests (09)

`CONSERVATIVE backtests stored` grows by 4; `runs.tsv` names the four run ids; every other book's backtest rows unchanged.

## After the live reset (10) and the chain (11)

| Check | Must read |
|---|---|
| CONSERVATIVE live_results after 10 | 0 rows; live_start_date 2025-10-05 |
| CONSERVATIVE live_results after 11 | one row per calendar date 2025-10-05 .. END, every identity PASS (`11_live_chain.sh check`) |
| QT books | every row dated before the chain's cutoff unchanged (11's check compares count and md5 per table and book) |
| no CONSERVATIVE row outside the scripts | the book's rows equal the chain's own: dates 2025-10-05 .. END, each run in `checks/progress.txt` |

## Not SQL

| Check | Must read |
|---|---|
| the cron's time zone | UTC |
| the production image's library versions (`ldd --version`, `ldd` of the runner) | recorded |
| the server's free disk | room for the archive of the trading schema and four backtests |
| the first CONSERVATIVE cron day on production | its stored run_metadata settings equal `config_reference/` |
