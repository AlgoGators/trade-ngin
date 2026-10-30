# The MGC and SIL pull

The MGC and SIL bars are bought from Databento and are not kept in the repository. Part A loaded the contract bars to
2026-10-26 on production. The release needs two sets of rows:

| Rows | Used by | Covers |
|---|---|---|
| `BACKFILL` | 06 (part A2) and 07 (part B) | 2026-10-27 to the day before the merge |
| `ROWS` | 07 (part B) | the MGC.v.0 and SIL.v.0 history to 2026-10-26 |

**One pull gives both.** Run it at the release, after the integration PR merges:

```
START=2010-06-06 zsh scripts/futures_release/pull_micro_metals.sh <merge day>
```

The end date stays open until then: `<merge day>` is the day of the merge. Databento's end is exclusive, so the pull
holds bars through the day before. The key is read from `config/databento.env` and never printed. The files go to
`$RUN_DIR/pull_<today>/`.

| File | dataset | symbols | stype_in | schema | start | end |
|---|---|---|---|---|---|---|
| `MGC_contracts_ohlcv-1d.csv.zst` | GLBX.MDP3 | `MGC.FUT` | parent | ohlcv-1d | START | merge day |
| `SIL_contracts_ohlcv-1d.csv.zst` | GLBX.MDP3 | `SIL.FUT` | parent | ohlcv-1d | START | merge day |
| `MGC_v0_ohlcv-1d.csv.zst` | GLBX.MDP3 | `MGC.v.0` | continuous | ohlcv-1d | START | merge day |
| `SIL_v0_ohlcv-1d.csv.zst` | GLBX.MDP3 | `SIL.v.0` | continuous | ohlcv-1d | START | merge day |
| `MGC_definition.csv.zst`, `SIL_definition.csv.zst` | GLBX.MDP3 | `MGC.FUT`, `SIL.FUT` | parent | definition | 2026-10-27 | merge day |

Every request uses encoding csv, compression zstd, `map_symbols`, `pretty_px`, `pretty_ts`.

**Definitions are optional.** `build_rows.py` takes each contract's expiration from the vendor's definition where one is
given, otherwise from the full-size twin's same-month expiration (GC for MGC, SI for SIL). Checked to 2026-10-26: the rows
of all three tables are identical with or without the vendor definitions; only the source named in `expirations.csv`
changes. So the script pulls definitions from 2026-10-27 only, which covers any contract listed since; the full history
of definitions is the slow pull and is not needed.

**Then build the rows:**

```
zsh scripts/futures_release/06_build_backfill.sh production "$RUN_DIR/pull_<today>"
```

It decompresses the files under `$RUN_DIR/tmp/`, reads the GC and SI twin rows read only from the target, and runs
`load/build_rows.py`. It writes `$RUN_DIR/backfill/<pull>_production/` (the `BACKFILL` rows) and its `rows/` folder (the
`ROWS` rows), and prints both paths. The `rows/` folder must reproduce the rows loaded by part A: part B refuses if any
vendor bar names no stored contract bar of the same date and close.
