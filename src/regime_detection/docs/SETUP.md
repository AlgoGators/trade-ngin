# Regime Detection — Environment & Setup

Everything a new developer needs to run the pipelines, the tests, and the regression
gate on this branch (`regime_detection_updated`). Architecture and concepts live in
`../README.md`; per-item history in `STATUS_ROLLUP.md`.

## 1. Database

Runners auto-load the Postgres connection from `config/defaults.json` at the repo root
(`database.{host,port,username,password,name}` — the repo gitignores `config/`; copy
`config_template/defaults.json` and fill in credentials). Target DB: `new_algo_data`.

Schemas consumed:
- **`macro_data`** — the macro pipeline's input. Six tables full-outer-joined on date by
  `macro_data_loader`: `inflation`, `growth`, `yield_curve`, `credit_spreads`,
  `liquidity`, (+ sentiment/labor per loader). ⚠ Stale since ~2026-04-08 as of
  2026-08-30 — fine for the fixed-window baseline gate (see §4), must be refreshed for
  any current-period regime run.
- **`futures_data.ohlcv_1d`** — the market pipeline's sleeve price/volume input via
  `market_data_loader` (continuous `.v.0` symbols).

## 2. Data ingest scripts

- `scripts/fetch_macro_data.py` — fetches the DFM macro series from FRED.
  Requires `pip install fredapi pandas` and the env var **`FRED_API_KEY`**
  (free key: https://fred.stlouisfed.org/docs/api/api_key.html). Exits with a clear
  error if the key is unset.
- `scripts/download_weekly_macro_data.py` — weekly BSTS inputs: 8 ETF proxies via
  yfinance + 12 FRED series, aligned to a weekly Friday grid with forward-fill
  (monthly/quarterly releases hold last known value until published — deliberately
  causal). Same `FRED_API_KEY` requirement for the FRED half.

## 3. Build & run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_SYSROOT=$(xcrun --show-sdk-path)   # macOS only; see note below
cmake --build build -j 8
```

macOS note: if the link fails on a stale `MacOSX26.sdk/.../libm.tbd` path, the
pkg-config-derived link files cached a dead SDK path — re-run configure with the
`CMAKE_OSX_SYSROOT` above and sed the generated `link.txt`/`build.make` files
(`fix_build/fix_sdk_and_build.sh` on the main branch shows the exact patch).

Five binaries in `apps/regime_detection/` (names unchanged since the reorg):
`macro_dfm_runner`, `macro_msdfm_runner`, `macro_regime_pipeline_runner`,
`bsts_regime_detector`, `market_regime_pipeline_runner`.

```bash
# market pipeline, explicit window (defaults: 2020-01-01 .. runner default end)
./build/bin/Release/market_regime_pipeline_runner "" 2020-01-01 2025-12-31
```

## 4. Tests and the bit-identity regression gate

```bash
cmake --build build -j 8 --target trade_ngin_tests
./build/bin/Release/trade_ngin_tests        # 430 tests on this branch
```

**The gate**: any change that should preserve regime calls must reproduce the market
timeline byte-for-byte.

```bash
TIMELINE_CSV=/tmp/timeline.csv ./build/bin/Release/market_regime_pipeline_runner "" 2020-01-01 2025-12-31
diff /tmp/timeline.csv src/regime_detection/baselines/market_timeline_K05plus.csv   # must be 0 lines
```

- The tracked baselines under `src/regime_detection/baselines/` are the 2026-05-05
  set (committed 2026-08-30; regenerated-fresh verification is scheduled as the first
  step of the current fix phase). `market_timeline_K05plus.csv` is the canonical gate;
  the `*_after_*` files are per-phase historical snapshots.
- Baselines were generated on a **UTC-normalised** host; on a non-UTC machine the
  local-timezone date parse (L-25, config-gated) can shift dates and fail the diff —
  expected until the L-25 default flips.
- Baseline-shifting changes (config-gated default flips, fingerprint geometry) must
  regenerate the baseline **once, deliberately, in a dedicated commit** — never as a
  side effect.

## 5. Known operational caveats

- GMM restarts are not deterministic across runs until the L-11 config-gated seed
  default flips (STATUS_ROLLUP).
- The macro pipeline needs `macro_data` refreshed (§1) before live/current-period use;
  the market pipeline needs `futures_data` current (feed status: see
  `docs/DATA_OWNER_ASKS_2026-08.md` in the main working tree).
