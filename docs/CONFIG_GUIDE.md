# Configuration Guide

This guide explains how to configure the trade-ngin trading system using the modular configuration system.

---

## Table of Contents

1. [Overview](#overview)
2. [Directory Structure](#directory-structure)
3. [Configuration Files](#configuration-files)
4. [Adding a New Portfolio](#adding-a-new-portfolio)
5. [Configuration Reference](#configuration-reference)
6. [Keys that are present and not read](#keys-that-are-present-and-not-read)
7. [Examples](#examples)
8. [Migration from Legacy Config](#migration-from-legacy-config)

---

## Overview

The configuration system uses a hierarchical structure with:

- **Shared defaults** (`config/defaults.json`): Settings common to all portfolios
- **Per-portfolio configs**: Portfolio-specific settings organized by domain:
  - `portfolio.json`: Capital, strategies, allocations, and the book's own blocks
  - `risk.json`: The risk modules the book runs, and its risk reporter
  - `email.json`: Notification settings

This approach allows you to:
- Change shared settings in one place
- Customize individual portfolios without code changes
- Keep each portfolio's files in its own directory. A new portfolio also needs a runner that
  loads it: each shipped runner names its directory in code (`conservative`, `base`,
  `equity_mr`; for example `apps/strategies/live_portfolio_conservative.cpp:161`) and takes no
  portfolio argument

`ConfigLoader::load` (`src/core/config_loader.cpp:884`) reads `defaults.json`, merges
`portfolio.json` over it, and attaches `risk.json` and `email.json` under the keys `risk` and
`email`. All four files are required.

The copy to start from is `config_template/`, which writes every key of the three shipped books.
Write them all: the next section says which keys stop a run when they are missing and which fall
back to a value in the code without a message.

### What stops a run, and what falls back to the code

A run stops at start-up, with a message that names what is missing, when one of these is missing:

- any of the four files: `defaults.json`, and the book's `portfolio.json`, `risk.json` and
  `email.json` (`src/core/config_loader.cpp:887-931`);
- `portfolio_id`; `database.host`, `username`, `password` and `name`; a `strategies` block with at
  least one sleeve; `use_optimization` (`src/core/config_loader.cpp:620-639`, `:249-257`);
- in `risk.json`: `schema` (which must be 2), `modules`, `max_drawdown`, `risk_reporting` with
  every key of the reporter, every key of each module's type, and `max_leverage` on a book that
  does not run the overlay (`src/risk/risk_module_config.cpp:757-805`, `:372-390`, `:1017-1062`);
- on a futures book: `equity_slow_rule`, `sizing_mode`, `starting_capital`, the carver module's
  `R_max`, `R_jump_max` and `R_shock_max` (with `per_name_cap` and `trim_max`), and
  `optimization.cost_penalty_scalar`, `sign_close_band` and `b_sigma_floor`
  (`ConfigLoader::require_loop_keys`, `src/core/config_loader.cpp:728-805`);
- in each futures sleeve's `config`: `risk_target`, `idm` and `vol_lookback_short`
  (`read_required_sleeve_keys`, `include/trade_ngin/strategy/sleeve_config.hpp:19-51`);
- at least one sleeve with `enabled_live` set (live) or `enabled_backtest` set (backtest)
  (`apps/strategies/live_portfolio_conservative.cpp:253`, `:263-266`;
  `apps/backtest/bt_portfolio_conservative.cpp:303-314`).

`risk.json` is strict: a key that is not one of its own, at the top level, in the reporting block
or in a module, is a load error, so a misspelt key there is caught
(`src/risk/risk_module_config.cpp:766-775`, `:1023-1030`, `:377-382`). A retired key is refused
by name; [Retired keys](#retired-keys) lists them and the one entry that is ignored with a warning.

Every other key falls back to a value written in the code when it is absent, and a misspelt key
is an absent key: the file loads, no message is printed, and the in-code value is used. This
includes values that size a book or bound what it reads: `initial_capital` (500,000,
`include/trade_ngin/core/config_loader.hpp:310`), a sleeve's `default_allocation` (0.5), `type`
(`TrendFollowingStrategy`) and `ema_windows` (the pairs in the code: six, or four on the FAST sleeve)
(`apps/strategies/live_portfolio_conservative.cpp:254`, `:710`, `:739`;
`include/trade_ngin/strategy/trend_following.hpp:26-28`), `live.historical_days` (300, where the
template writes 730) and `live.data_staleness_tolerance_days` (4)
(`include/trade_ngin/core/config_loader.hpp:200`, `:204`), the position limits (`:118-119`), and
the whole of the `execution`, `optimization`, `backtest`, `live` and `strategy_defaults` blocks
if the block's own name is misspelt: each block is read only when a key of exactly that name is
present (`src/core/config_loader.cpp:166-177`, `:586-598`). On a futures book a misspelt
`optimization` block does stop the run, because three of its keys are required there. A sleeve
whose `enabled_live` key is absent or misspelt is skipped.

The three optional blocks of `portfolio.json` have their own rule, under
[A misspelt block name](#a-misspelt-block-name).

---

## Directory Structure

```
config/
├── defaults.json                    # Shared defaults (database, execution, optimization)
│
└── portfolios/
    ├── base/                        # BASE_PORTFOLIO
    │   ├── portfolio.json           # Capital, strategies, allocations
    │   ├── risk.json                # Risk modules and the risk reporter
    │   └── email.json               # Email recipients
    │
    ├── conservative/                # CONSERVATIVE_PORTFOLIO
    │   ├── portfolio.json
    │   ├── risk.json
    │   └── email.json
    │
    └── equity_mr/                   # EQUITY_MR_PORTFOLIO
        ├── portfolio.json
        ├── risk.json
        └── email.json
```

`config/` is not tracked. Create it with `cp -r config_template config` and replace the
placeholders.

---

## Configuration Files

### defaults.json

Contains settings shared across all portfolios. Edit this file to change:

| Section | Description |
|---------|-------------|
| `database` | Database connection settings |
| `execution` | The position limits. `commission_rate` and `slippage_bps` are present and reach no cost model (see [Keys that are present and not read](#keys-that-are-present-and-not-read)) |
| `optimization` | The whole-contract search's parameters |
| `backtest` | Backtest-specific settings (see [Backtest window](#backtest-window)) |
| `live` | Live trading settings (see [Live Parameters](#live-parameters)) |
| `strategy_defaults` | The allocation bounds of a sleeve. `fdm` repeats the multiplier table of the code and is not read (same section) |

There is no `risk_defaults` section: the key is a load error. Every risk value lives in the
book's own `risk.json`.

**Example** (the keys of `config_template/defaults.json`, without its notes; the connection
values are shown as placeholders):
```json
{
  "database": {
    "host": "YOUR_DB_HOST",
    "port": "YOUR_DB_PORT",
    "username": "YOUR_DB_USERNAME",
    "password": "YOUR_DB_PASSWORD",
    "name": "YOUR_DB_NAME",
    "num_connections": 5
  },
  "execution": {
    "commission_rate": 0.0005,
    "slippage_bps": 1,
    "position_limit_backtest": 1000,
    "position_limit_live": 500
  },
  "optimization": {
    "cost_penalty_scalar": 100,
    "sign_close_band": 2,
    "b_sigma_floor": 0.05,
    "max_iterations": 100,
    "convergence_threshold": 0.000001,
    "use_buffering": true
  },
  "backtest": {
    "lookback_years": 2,
    "store_trade_details": true
  },
  "live": {
    "historical_days": 730,
    "data_staleness_tolerance_days": 4,
    "execution_price_max_staleness_days": 5,
    "spinoff_child_policy": "liquidate_at_first_close"
  },
  "strategy_defaults": {
    "fdm": [[1, 1], [2, 1.03], [3, 1.08], [4, 1.13], [5, 1.19], [6, 1.26]],
    "max_strategy_allocation": 1,
    "min_strategy_allocation": 0.1
  }
}
```

`database.port` is a JSON string, not a number. `database.host`, `username`, `password` and
`name` are required (`src/core/config_loader.cpp:623-628`); `num_connections` is the size of the
connection pool, 5 when absent (`apps/strategies/live_portfolio_conservative.cpp:182-183`).
`strategy_defaults.max_strategy_allocation` and `min_strategy_allocation` are the bounds on one
sleeve's share of the book's capital: a sleeve whose share is outside them is refused when it is
added to the portfolio (`apps/strategies/live_portfolio_conservative.cpp:666-669`;
`src/portfolio/portfolio_manager.cpp:215-216`).

### portfolio.json

Defines portfolio identity, capital, the strategies (sleeves) and the book's own blocks.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `portfolio_id` | string | yes | Unique identifier for the portfolio |
| `initial_capital` | number | written in every shipped book; must be positive. A file without it loads at 500000 (the struct's own initial value), so write it | Starting capital in dollars |
| `strategies` | object | yes, non-empty | Strategy definitions (see below) |
| `use_optimization` | boolean | yes, no default | Whether the whole-contract search runs. `true` on the futures books; the equity runners refuse to start on `true`. |
| `equity_slow_rule` | object | on a futures book | `{"symbols": [...], "pairs": [[32, 128], [64, 256]]}`: on the book's first sleeve a negative combined forecast of these symbols stands only when the scaled forecast of every pair named is negative, and is 0 otherwise |
| `sizing_mode` | string | on a futures book | `"half_compounding"`, the one value accepted. It selects nothing: there is one mode (`src/core/config_loader.cpp:520-531`, `:757-762`) |
| `starting_capital` | number | on a futures book | Checked, not read. It must be positive and equal to `initial_capital` (`src/core/config_loader.cpp:532-552`, `:764-769`); the sizing takes `initial_capital` (`include/trade_ngin/live/live_sizing_read.hpp:250`). |
| `covariance_history_prices` | integer | no (756) | A whole number of at least 2 when present (`src/core/config_loader.cpp:262-274`). It sets how many closes per symbol the portfolio manager keeps for the generic optimiser's covariance (`src/portfolio/portfolio_manager.cpp:1034`). The futures search does not use that history: it takes 756 closes from a constant of the code (`src/strategy/trend_following.cpp:505`), and the equity books run with the optimiser off. Not a control of any shipped book |
| `covariance_stale_dates` | integer | no (5) | A whole number of at least 0 when present (`src/core/config_loader.cpp:279-292`). It sets the stale rule of the generic optimiser's covariance (`src/portfolio/portfolio_manager.cpp:1298`). The futures search uses the constant 5 (`src/optimization/one_pass.cpp:519`). Not a control of any shipped book |
| `sleeve_risk_modules` | object | no | Risk modules at sleeve scope (see [risk.json](#riskjson)) |
| `listing_dates` | object | no | See [Listing dates](#listing_dates) |
| `instrument_id_relabels` | array | no | See [Instrument id relabels](#instrument_id_relabels) |
| `trading_rule_removals` | object | no | See [Trading rule removals](#trading_rule_removals) |

A key that starts with an underscore (`_description`, `_sizing_note`) is a note and is never read
as a value, except the underscore keys a risk module requires (see
[Keys that are present and not read](#keys-that-are-present-and-not-read)).

"On a futures book" means the four futures runners (`bt_portfolio`, `bt_portfolio_conservative`,
`live_portfolio`, `live_portfolio_conservative`), which call `ConfigLoader::require_loop_keys`
(`src/core/config_loader.cpp:728`) right after the load and exit when a key is missing.

**Strategy Configuration:**
```json
{
  "strategies": {
    "TREND_FOLLOWING": {
      "enabled_backtest": true,
      "enabled_live": true,
      "default_allocation": 1.0,
      "type": "TrendFollowingStrategy",
      "config": {
        "risk_target": 0.2,
        "idm": 2.5,
        "ema_windows": [[2, 8], [4, 16], [8, 32], [16, 64], [32, 128], [64, 256]],
        "vol_lookback_short": 32,
        "vol_lookback_long": 252
      }
    }
  }
}
```

The keys of a futures sleeve, as the futures runners read them:

| Key | When absent | Meaning |
|---|---|---|
| `enabled_live`, `enabled_backtest` | the sleeve is skipped by the live runner, or by the backtest runner; a book left with no sleeve stops | whether the sleeve runs (`apps/strategies/live_portfolio_conservative.cpp:253`; `apps/backtest/bt_portfolio_conservative.cpp:303-304`) |
| `default_allocation` | 0.5 | the sleeve's share of the book's capital. The shares of the enabled sleeves are rescaled to sum to 1, with a warning line when they did not (`apps/strategies/live_portfolio_conservative.cpp:254`, `:272-285`) |
| `type` | `TrendFollowingStrategy` | the sleeve class (see [Strategy Types](#strategy-types); `apps/strategies/live_portfolio_conservative.cpp:710`) |
| `config.risk_target`, `config.idm`, `config.vol_lookback_short` | the run stops | each a positive number (`vol_lookback_short` a positive whole number); no in-code default (`read_required_sleeve_keys`, `include/trade_ngin/strategy/sleeve_config.hpp:19`) |
| `config.ema_windows` | the pairs in the code: six on a `TrendFollowingStrategy` sleeve (`include/trade_ngin/strategy/trend_following.hpp:26-28`), four on a `TrendFollowingFastStrategy` sleeve (`fast_trend_following_config`, `:57-62`) | the EMA pairs the sleeve runs (`apps/strategies/live_portfolio_conservative.cpp:739-745`, `:786-792`) |
| `config.vol_lookback_long` | 252 | present and not read: the estimator's long-run mean is a constant of the code, 2,520 values (`include/trade_ngin/strategy/trend_following.hpp:30`) |

The first sleeve's `risk_target` is the tau that the risk overlay's limits are ratios to.

The equity sleeve carries a `symbols` list and its own `config` keys (`lookback_period`,
`entry_threshold`, `exit_threshold`, `risk_target`, `position_size`, `vol_lookback`,
`use_stop_loss`, `stop_loss_pct`, `allow_fractional_shares`). Each `config` key falls back to
the value in the code when absent (`build_mean_reversion_config`,
`include/trade_ngin/strategy/equity_strategy_builder.hpp:32`).

**`weight` is not a sizing key.** No code reads a `weight` under a sleeve's `config`: a
contract's size comes from the risk target, the IDM, the instrument weights the sleeve computes
and the forecast. The key is retired, and a futures book whose sleeve still carries it does not
run (see [Retired keys](#retired-keys)).

#### `listing_dates`

Optional. Names contracts that trade only from a listing date, and the contract the book trades
before it. The shipped CONSERVATIVE book declares the four equity index micros:

```json
"listing_dates": {
  "switch_rule": "open_at_target",
  "contracts": [
    {"symbol": "MES", "listed": "2019-05-06", "before": "ES",  "ratio": 10},
    {"symbol": "MNQ", "listed": "2019-05-06", "before": "NQ",  "ratio": 10},
    {"symbol": "MYM", "listed": "2019-05-06", "before": "YM",  "ratio": 10},
    {"symbol": "M2K", "listed": "2019-05-06", "before": "RTY", "ratio": 10}
  ]
}
```

| Key | Meaning |
|---|---|
| `contracts[].symbol` | the root that lists on `listed` (`"MES"`, not `"MES.v.0"`) |
| `contracts[].listed` | the listing date, `YYYY-MM-DD`, UTC |
| `contracts[].before` | the root the book trades before that date |
| `contracts[].ratio` | listed contracts per `before` contract of the same exposure: a positive whole number, and it must equal the ratio of the two contracts' `"Contract Size"` in `metadata.contract_metadata` or the backtest stops |
| `switch_rule` | optional; how the book moves from `before` to `symbol` on the switch day. `"open_at_target"` when absent: a held `before` position is exited and the listed contract is entered at that day's scaled target, bounded by the per-name cap, rounded to whole contracts. `"close_reenter"`, `"convert"` and `"carry_to_target"` are also accepted. |

What it does: the vendor stores the earlier contract's bars under the listed contract's symbol
(`docs/DATA_SOURCES_OF_TRUTH.md`, section 4.6). With the block, a backtest whose window starts
before a listing date adds the `before` contract as a symbol of the run, on those same bars and
on its own metadata row (size, fee, margin). The `before` contract may hold a position only on
signal bars dated before the listing date, the listed contract only on or after it; the pair is
one instrument with one weight. The switch books two fills with ids starting `LC-` (the close) and
`LO-` (the listed contract's entry). A live run trades no `before` contract. With the block
present it refuses to run when a `before` contract is in its universe or its stored book on a signal
date on or after the listing date, and when the signal date is before the listing date of a listed
contract in its universe (`include/trade_ngin/live/live_listing_guard.hpp:19-25`).

Where it is read: parsed at `src/core/config_loader.cpp:404`; installed by the backtest runners
(`apps/backtest/bt_portfolio_conservative.cpp:176`) and the live runners
(`apps/strategies/live_portfolio_conservative.cpp:536`); the rule itself is
`include/trade_ngin/data/listing_dates.hpp:108`.

**Absent block:** no contract has a listing date. Every symbol trades at its own metadata row's
size over its whole stored history, and no predecessor is added to any run.

#### `instrument_id_relabels`

Optional. A list of vendor instrument-id changes that are **not** rolls: from `date` the vendor
labels the same contract of `symbol` with id `to` where it had used `from`.

```json
"instrument_id_relabels": [
  {"symbol": "MES", "date": "2026-02-22", "from": "42140878", "to": "42003800"},
  {"symbol": "MNQ", "date": "2026-02-22", "from": "42002475", "to": "42004946"},
  {"symbol": "MYM", "date": "2026-02-22", "from": "42005850", "to": "42001953"},
  {"symbol": "M2K", "date": "2026-02-22", "from": "42005017", "to": "42002147"}
]
```

All four keys are strings (the ids are text). What it does: every bar and id row of `symbol`
dated on or after `date` that carries `to` is read as carrying `from`, in the backtest and in
live. No consumer sees a contract change, so no change bar is held, no ROLL fills or roll costs
are booked, and that day's move is booked as on any other day. An entry that rewrites no bar, or
whose new id already appears before its date, is reported on a WARN line (`RELABEL_UNMATCHED`,
`RELABEL_LATE`).

Where it is read: parsed at `src/core/config_loader.cpp:370`; installed before any bar is loaded
(`apps/backtest/bt_portfolio_conservative.cpp:165`,
`apps/strategies/live_portfolio_conservative.cpp:528`); applied by `ListingDates::apply_relabels`
(`include/trade_ngin/data/listing_dates.hpp:177`).

**Absent block:** every instrument-id change is read as a contract change. On the four micros the
change of 2026-02-22 is then treated as a roll. `docs/FUTURES_ROLLS.md` describes what a roll
books.

#### `trading_rule_removals`

Optional. Maps a contract's base symbol to the EMA pairs that contract does **not** run. A rule
is removed from a contract when its yearly cost on that contract is too high for the rule's speed
(Robert Carver, Advanced Futures Trading Strategies, strategy nine, "Removing expensive trading
rules"). The shipped CONSERVATIVE book removes eighteen rules on twelve contracts:

```json
"trading_rule_removals": {
  "6L": [[2, 8], [4, 16]],
  "6M": [[2, 8]],
  "GF": [[2, 8]],
  "HE": [[2, 8]],
  "KE": [[2, 8], [4, 16]],
  "LE": [[2, 8]],
  "NG": [[2, 8]],
  "ZC": [[2, 8], [4, 16]],
  "ZL": [[2, 8]],
  "ZR": [[2, 8], [4, 16], [8, 32]],
  "ZS": [[2, 8]],
  "ZW": [[2, 8], [4, 16]]
}
```

A listed contract runs the pairs that are left at equal weight, with the multiplier for their
number from the table in the code (`TrendFollowingConfig::fdm`,
`include/trade_ngin/strategy/trend_following.hpp:32`; `strategy_defaults.fdm` repeats that table
and is not read). Every contract not listed runs all of the sleeve's pairs. The
rules the block must satisfy:

| Rule | Checked at |
|---|---|
| the key is a base symbol (`"ZR"`, not `"ZR.v.0"`); each value is a non-empty list of pairs of positive whole numbers | `src/core/config_loader.cpp:455` |
| the book has exactly one enabled sleeve and it is a `TrendFollowingStrategy` (a book of several sleeves carrying the block is refused, never read as if the block were absent) | `src/core/config_loader.cpp:512` |
| the symbol has a metadata row; the pairs removed are the contract's fastest and leave at least one; the multiplier table of the code has a row for the number left; a pair the equity slow rule reads on that symbol is not removed | `TrendFollowingStrategy::validate_config`, `src/strategy/trend_following.cpp:100` |
| the two contracts of a `listing_dates` pair lose the same rules | `src/strategy/trend_following.cpp:146` |

The list is handed from the loader to the sleeve in one place
(`hand_over_trading_rule_removals`, `include/trade_ngin/strategy/sleeve_config.hpp:58`), called by
every futures runner. `scripts/trading_rule_costs.py` shows how the list follows from the cost
inputs and, with `--check`, reports whether the run records it is given still give the same
list. The list in the file is the authority: the script reports and never writes it.
`docs/TREND_FOLLOWING_SYSTEM.md` gives the procedure and its source.

**Absent block:** nothing is removed. Every contract runs every pair of its sleeve.

#### A misspelt block name

The three optional blocks (`listing_dates`, `instrument_id_relabels`, `trading_rule_removals`)
are strict inside: an unknown key in `listing_dates`, in one of its `contracts` entries or in a
relabel entry is a load error (the check is at `src/core/config_loader.cpp:341`, applied at
`:384`, `:418` and `:430`). A misspelt BLOCK name is caught in one case only: a top-level key
that begins with the block's name less its last letter and is not the name itself
(`listing_date`, `listing_dates_v2`, `instrument_id_relabel`, `trading_rule_removal`) is a load
error (`src/core/config_loader.cpp:353-367`). Any other misspelling (`listing-dates`,
`listng_dates`, `Listing_dates`, `rule_removals`) is not detected and reads as an absent block,
which switches the block off without a message.

`config_template/portfolios/conservative/portfolio.json` carries all three blocks. The BASE and
equity templates carry none.

### risk.json

`risk.json` is **schema 2**: it names the risk modules a book runs, with every parameter written
literally. `docs/RISK_MODULES.md` describes what each module does; this section is the file
format as the loader reads it (`parse_risk_schema`, `src/risk/risk_module_config.cpp:738`; the
design comment is at `include/trade_ngin/risk/risk_module_config.hpp:3`).

**Top level.** Exactly these keys, plus notes starting with an underscore; any other key is an
error:

| Key | Required | Rule |
|---|---|---|
| `schema` | yes | the integer `2` |
| `modules` | yes | a non-empty array of module objects (the portfolio scope) |
| `risk_reporting` | yes | the reporter block, below |
| `max_drawdown` | yes | a number in (0, 1] (`src/risk/risk_module_config.cpp:782-787`). It is copied to every sleeve, and its one use is a warning line when a sleeve's cumulative P&L over its allocated capital is below minus the value: the sleeve keeps running and nothing is cut (`src/strategy/base_strategy.cpp:467-471`) |
| `max_leverage` | on a book whose carver module carries no overlay limits; **retired** (an error) on a book whose module does | a positive number |

**A module object** has `id` (a non-empty string, unique across the book), `type`, and exactly
the keys of its type. An unknown key and a missing key are both errors.

| `type` | Keys | What it is |
|---|---|---|
| `carver` | below | the risk overlay; portfolio scope only |
| `none` | `_reason`, `_ruled_by`, `_ruled_on` | "this book runs no risk layer", as a recorded decision |
| `constant_scale` | `scale` in (0, 1]; optional `every_lap` (boolean) | a fixed cut |
| `warn` | `condition`, `reason` | logs when the condition holds; changes nothing |
| `refuse` | `condition`, `reason` | refuses the rebalance when the condition holds |

A `condition` is `{"kind": ..., "threshold": ...}` with `kind` one of `always`, `never`,
`lap_at_least`, `nonzero_positions_above`, `max_abs_quantity_above`. `always` and `never` take no
threshold; a `never` condition needs a non-empty `_never_reason` on its module.

**The `carver` module on a futures book.** This is the form the four futures runners require:

```json
{
  "schema": 2,
  "modules": [
    {
      "id": "carver",
      "type": "carver",
      "max_gross_leverage": 8.0,
      "max_net_leverage": 6.0,
      "confidence_level": 0.99,
      "lookback_period": 252,
      "lookback_unit": "dates",
      "min_gate_dates": 21,
      "R_max": 2.25,
      "R_jump_max": 4.5,
      "R_shock_max": 4.0,
      "per_name_cap": 2,
      "trim_max": 5,
      "missing_symbol_policy": "ignore",
      "_missing_symbol_policy_reason": "why the gate reads the symbols present in the window"
    }
  ],
  "risk_reporting": {
    "type": "carver",
    "window": "all_bars",
    "max_gross_leverage": 8.0,
    "max_net_leverage": 6.0,
    "confidence_level": 0.99,
    "lookback_period": 252
  },
  "max_drawdown": 0.3
}
```

| Key | Rule | Meaning |
|---|---|---|
| `R_max`, `R_jump_max`, `R_shock_max` | all three or none; each a positive number | the overlay's three risk limits as ratios to the first sleeve's risk target |
| `max_gross_leverage`, `max_net_leverage` | `0 < net <= gross <= 10` | the gross and net leverage limits on the sizing capital |
| `per_name_cap` | required with the three limits; a positive number | the cap on one symbol's weight on the sizing capital |
| `trim_max` | required with the three limits; a whole number, 0 or more | the most contracts the trim removes in a day |
| `confidence_level` | in (0, 1); must equal `risk_reporting.confidence_level` (`src/risk/risk_module_config.cpp:512-516`, `:1084-1103`) | validated and not read by the overlay. The reporting block's value is the one the report's risk reading uses (`src/risk/risk_manager.cpp:805`) |
| `lookback_period` | a positive integer, at least `min_gate_dates` and at least 120; must equal `risk_reporting.lookback_period` (`src/risk/risk_module_config.cpp:519-523`, `:264-282`, `:1084-1103`) | validated and not read by the overlay, whose window is a constant of the code, 252 dates (`include/trade_ngin/risk/overlay.hpp:32`) |
| `lookback_unit` | must be `"dates"` (`"bars"` is an error that names the fix; `src/risk/risk_module_config.cpp:244-255`) | selects nothing |
| `min_gate_dates` | an integer of at least 3 (`src/risk/risk_module_config.cpp:256-263`) | validated and not read by the overlay, which reports itself blind below a constant of the code, 21 complete dates (`include/trade_ngin/risk/overlay.hpp:34`; `src/risk/overlay.cpp:84`) |
| `missing_symbol_policy` | `"ignore"`, with a non-empty `_missing_symbol_policy_reason`; `"warn"` and `"refuse"` are recognised and not implemented, so they are refused (`src/risk/risk_module_config.cpp:285-316`) | validated only: the one test of it (`can_refuse`, `src/risk/risk_module_config.cpp:594`) compares it with `"refuse"`, which cannot load, so it selects no behaviour |

The last five rows are required keys: a file without one does not load. None of them is a
control of the overlay; the same is said in `docs/RISK_MODULES.md`.

With the three `R_` limits present, the module reads the book in capital terms, and the loader
refuses the limits of the other form beside them: `var_limit`, `jump_risk_limit` and
`max_correlation` in the module or in `risk_reporting`, and `max_leverage` at the top level, are
each a load error. The
per-name cap and the trim cap reach the portfolio through `apply_loop_config`
(`include/trade_ngin/portfolio/loop_config.hpp:17`).

A `carver` module without the three `R_` limits is the older form: it requires `var_limit` and
`jump_risk_limit` (each in (0, 1]) and `max_correlation` (in (0, 1)), the same three in
`risk_reporting`, and `max_leverage` at the top level. The futures runners refuse a book in that
form (`require_loop_keys`); it is the shape of the reporter on the equity book.

**`risk_reporting`** is required on every book. It configures the measurement the live runners
store and report, which is separate from the gate so that a book can stop gating without losing
its measurement. `type` must be `"carver"` and `window` `"all_bars"`; one value each is accepted
and they select nothing (`src/risk/risk_module_config.cpp:1064-1066`). While a `carver` module
gates the book, every shared value in this block must equal the module's, or the load fails.
The report's risk reading uses the block's `max_gross_leverage`, `max_net_leverage` and
`confidence_level`; its `lookback_period` is required and compared with the module's, and the
reading does not use it.

**The `none` assignment.** A book that runs no risk layer says so; leaving `modules` empty or
absent is an error, because an omission and a decision must not share an encoding. A `none`
module needs all of:

| Key | Rule |
|---|---|
| `id`, `type: "none"` | as any module |
| `_reason` | non-empty: why the book runs ungated |
| `_ruled_by` | non-empty: who decided |
| `_ruled_on` | the date of the decision, `YYYY-MM-DD` |

It must be the only module in `modules`, and it is not valid at sleeve scope (omit the sleeve's
entry instead). The file still needs `risk_reporting` in the older form (all seven values),
`max_drawdown` and `max_leverage`: the book's risk is still reported. Each run of such a book logs one
`RISK_NONE ... ruled_by=... ruled_on=...` line (`src/portfolio/portfolio_manager.cpp:98`). The
shipped equity book is a `none` book (`config_template/portfolios/equity_mr/risk.json`). A futures
book cannot be: the futures runners require the overlay's limits.

A book that has modules but no `carver` among them (only a `warn`, say) is just as ungated, so
it must carry `_ruled_by` and `_ruled_on` at the top level of `risk.json`.

**Sleeve scope.** Modules for one sleeve go in `portfolio.json`'s top-level `sleeve_risk_modules`,
keyed by strategy id, each an array of module objects with the same rules. `carver` and `none`
are not valid there.

A `refuse` module, at portfolio scope or at sleeve scope, is not accepted on a book with more
than one enabled sleeve (`src/risk/risk_module_config.cpp:933-945`).

**Worked shapes.** `config_template/examples/risk_modules/` holds one complete file per module
type. They carry placeholder numbers and no runner loads them.

#### Retired keys

A key that schema 2 removed is a load error on every book, and a key the futures loop removed
stops a futures book, because an ignored key lets a file say one thing while the engine does
another. The last row is the one exception: it is ignored with a warning.

| Key | Where | Result |
|---|---|---|
| a `risk.json` with flat gating keys and no `schema` | `risk.json` | load error on every book, naming `scripts/migrate_risk_json.py` (`check_not_schema1`, `src/risk/risk_module_config.cpp:671`) |
| `use_risk_management` | anywhere in any file | load error on every book (`check_removed_keys`, `src/core/config_loader.cpp:93`) |
| `risk_defaults` | `defaults.json` | load error on every book |
| `strategy_defaults.use_optimization` | `defaults.json` | load error on every book; the key lives at the top level of `portfolio.json` |
| `corr_shock_threshold`, `jump_shock_threshold` | anywhere in `risk.json` | load error on every book |
| `enabled` | a module object | load error: a module the book does not run is a module the book does not list |
| `var_limit`, `jump_risk_limit`, `max_correlation`, `max_leverage` | `risk.json` of a book whose carver module carries the overlay's limits | load error |
| `tau`, `asymmetric_risk_buffer`, `buffer_size_factor` | `defaults.json` `optimization` | a futures book does not run (`require_loop_keys`) |
| `carver_buffer_floor`, `carver_buffer_position_factor` | `defaults.json` `strategy_defaults`, or a sleeve's `config` | a futures book does not run |
| `weight`, `max_symbol_concentration`, `use_position_buffering` | a sleeve's `config` | a futures book does not run |
| `reserve_capital_pct`, `reserve_capital` | top level of `portfolio.json` | not an error: nothing reads the key, the value is ignored, and the load logs one WARN line asking for the key to be deleted (`src/core/config_loader.cpp:156`) |

`scripts/migrate_risk_json.py <config dir>` prints the schema-2 form of a schema-1 directory as a
dry run; `--in-place` writes it and leaves `.schema1.bak` copies. It does not invent the
decision a `none` module records: `config_template/README.md` gives the hand steps.

### email.json

Configures email notifications.

| Field | Type | Description |
|-------|------|-------------|
| `smtp_host` | string | SMTP server hostname |
| `smtp_port` | number | SMTP port (typically 587 for TLS) |
| `username` | string | SMTP authentication username |
| `password` | string | SMTP authentication password |
| `from_email` | string | The sender address of the futures runners' reports (`apps/strategies/live_portfolio_conservative.cpp:4398`). The equity runner does not pass it to the sender (`apps/strategies/live_equity_mean_reversion.cpp:6013-6018`) |
| `use_tls` | boolean | Present and not read: it is copied into the sender's config and nothing consults it. The sender always asks for TLS (`src/core/email_sender.cpp:163`) |
| `to_emails` | array | The recipients of every report the runners mail, the scheduled run's included (`apps/strategies/live_portfolio_conservative.cpp:4400`) |
| `to_emails_production` | array | Present and not read: it is loaded into the config object and nothing reads it (`include/trade_ngin/core/config_loader.hpp:60-61`). A recipient written only here is mailed nothing |

A live run given a date mails only with `--send-email`; a run with no date always mails
(`apps/strategies/live_portfolio_conservative.cpp:129-131`).

`TRADE_NGIN_EMAIL_BODY_DIR` applies only to a futures run that does not send: with the variable
set to a directory, the run builds the report body exactly as for a send, writes it there as
`email_body_<portfolio>_<date>.html` and mails nothing. With `--send-email` the variable is
ignored with one warning line in the log, and the report is mailed as usual. A run with no date
sends, so it ignores the variable in the same way. The equity runner does not read the variable
(`plan_email_report`, `include/trade_ngin/live/email_body_file.hpp:34-46`; the warning,
`apps/strategies/live_portfolio_conservative.cpp:4382-4384`). The running instructions are in
`src/live/README.md` ("Environment").

---

## Adding a New Portfolio

1. **Create the portfolio directory:**
   ```bash
   mkdir -p config/portfolios/my_portfolio
   ```

2. **Create portfolio.json.** For a futures book, start from
   `config_template/portfolios/conservative/portfolio.json` and write every key below. The run
   stops without `portfolio_id`, `strategies`, `risk_target`, `idm`, `vol_lookback_short`,
   `equity_slow_rule`, `sizing_mode`, `starting_capital` and `use_optimization`. The others fall
   back to values in the code (`initial_capital` 500,000, `default_allocation` 0.5, `type`
   `TrendFollowingStrategy`, the six EMA pairs), a sleeve without `enabled_live` or
   `enabled_backtest` is skipped, and `vol_lookback_long` is not read.
   ```json
   {
     "portfolio_id": "MY_PORTFOLIO",
     "initial_capital": 500000.0,
     "strategies": {
       "TREND_FOLLOWING": {
         "enabled_backtest": true,
         "enabled_live": true,
         "default_allocation": 1.0,
         "type": "TrendFollowingStrategy",
         "config": {
           "risk_target": 0.2,
           "idm": 2.5,
           "ema_windows": [[2, 8], [4, 16], [8, 32], [16, 64], [32, 128], [64, 256]],
           "vol_lookback_short": 32,
           "vol_lookback_long": 252
         }
       }
     },
     "equity_slow_rule": {"symbols": ["M2K", "MES", "MNQ", "MYM"], "pairs": [[32, 128], [64, 256]]},
     "sizing_mode": "half_compounding",
     "starting_capital": 500000.0,
     "use_optimization": true
   }
   ```
   Add `listing_dates`, `instrument_id_relabels` and `trading_rule_removals` from the same
   template if the book is to carry them.

3. **Create risk.json.** Copy the `carver` form shown under [risk.json](#riskjson), or
   `config_template/portfolios/conservative/risk.json`. Every value is written; none is inherited.

4. **Create email.json:**
   ```json
   {
     "smtp_host": "YOUR_SMTP_HOST",
     "smtp_port": 587,
     "username": "YOUR_SMTP_USERNAME",
     "password": "YOUR_SMTP_APP_PASSWORD",
     "from_email": "YOUR_FROM_EMAIL",
     "use_tls": true,
     "to_emails": ["recipient@example.com"]
   }
   ```

5. **Use in application:**
   ```cpp
   auto config_result = ConfigLoader::load("./config", "my_portfolio");
   ```
   A futures runner then calls `ConfigLoader::require_loop_keys(config)` and stops on an error.
   No shipped binary takes a portfolio name: each runner names its directory in this call
   (`conservative`, `base`, `equity_mr`), so a new directory is run by nothing until a runner
   names it.

---

## Configuration Reference

### Execution Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `commission_rate` | 0.0005 | Present and not read by any cost model. The equity live runner prints it in its start banner as not used and copies it into a per-symbol map that nothing reads (`apps/strategies/live_equity_mean_reversion.cpp:266`, `:535`, `:595`); the futures runners and the backtests do not read it |
| `slippage_bps` | 1.0 | Present and not read by any cost model. The equity live runner reads it into a local variable and prints it in the same banner; nothing else (`apps/strategies/live_equity_mean_reversion.cpp:267`, `:536`) |
| `position_limit_backtest` | 1000.0 | Max position size in backtest (`apps/backtest/bt_portfolio_conservative.cpp:354`) |
| `position_limit_live` | 500.0 | Max position size in live trading (`apps/strategies/live_portfolio_conservative.cpp:695`) |

No cost comes from this block. A futures fill's fee is the contract's own `"Fee Per Contract"`
in `metadata.contract_metadata`, and every other cost term, on the futures books and on the
equity book, comes from the cost model (`docs/COST_MODEL.md`).

### Optimization Parameters

| Parameter | Template value | Description |
|-----------|---------|-------------|
| `cost_penalty_scalar` | 100 | The search's cost multiplier. Required on a futures book. |
| `sign_close_band` | 2 | The forecast level below which a holding whose forecast has turned against it waits before it is closed. Required on a futures book; a positive number. |
| `b_sigma_floor` | 0.05 | The floor of the no-trade buffer as a ratio to the risk target. Required on a futures book; a positive number. |
| `max_iterations` | 100 | The floor on the search's pass cap |
| `convergence_threshold` | 1e-6 | Read only by the generic optimiser's configuration. The futures search does not read it: the smallest improvement it accepts is the constant 1e-6 (`src/optimization/one_pass.cpp:532`) |
| `use_buffering` | true | Read only by the generic optimiser (`src/optimization/dynamic_optimizer.cpp:90`), which no shipped book runs: a futures book is rebalanced by the one pass, and an equity book has the optimiser off |

`tau`, `asymmetric_risk_buffer` and `buffer_size_factor` are retired here (see
[Retired keys](#retired-keys)). `docs/OPTIMIZER_AND_RISK_DESIGN.md` describes the search these
parameters drive.

### Risk Parameters

There are no shared risk defaults. Each book's `risk.json` carries its own values; the keys, their
ranges and the values of the shipped futures books are under [risk.json](#riskjson). The table
gives the values written for CONSERVATIVE and BASE
(`config_template/portfolios/conservative/risk.json`,
`config_template/portfolios/base/risk.json`). The first seven rows are the overlay's controls;
the last three are required keys that the overlay does not read:

| Parameter | Value | Description |
|-----------|---------|-------------|
| `R_max` | 2.25 | Limit on the book's risk, as a ratio to the risk target |
| `R_jump_max` | 4.5 | Limit on the book's jump risk, as a ratio to the risk target |
| `R_shock_max` | 4.0 | Limit on the book's correlation-shock risk, as a ratio to the risk target |
| `max_gross_leverage` | 8.0 | Gross leverage limit on the sizing capital |
| `max_net_leverage` | 6.0 | Net leverage limit on the sizing capital |
| `per_name_cap` | 2 | Cap on one symbol's weight on the sizing capital |
| `trim_max` | 5 | Most contracts the trim removes in a day |
| `confidence_level` | 0.99 | Validated and not read by the overlay; equal to `risk_reporting.confidence_level`, which the report's risk reading uses |
| `lookback_period` | 252 | Validated and not read by the overlay, whose window is the constant 252 dates of the code |
| `min_gate_dates` | 21 | Validated and not read by the overlay, which reports itself blind below the constant 21 complete dates of the code |

### Live Parameters

The `live` block of `defaults.json`. Every key falls back to the value in the code when absent
(`LiveSpecificConfig`, `include/trade_ngin/core/config_loader.hpp:199-217`).

| Parameter | Template value | In the code | Description |
|-----------|---------|---------|-------------|
| `historical_days` | 730 | 300 | How many calendar days of bars a live run loads before its run clock (`apps/strategies/live_portfolio_conservative.cpp:316`) |
| `data_staleness_tolerance_days` | 4 | 4 | The tolerance, in calendar days, of the feed checks of a live run: how far the stalest symbol may be behind, and how long a held symbol may go without a bar (`apps/strategies/live_portfolio_conservative.cpp:1012`, `:1222`; the equity runner at `apps/strategies/live_equity_mean_reversion.cpp:886`). `docs/LIVE_RUN_CYCLE.md` says which of these checks stop a run in which run mode |
| `execution_price_max_staleness_days` | 5 | 5 | Equity runner only: how old, in calendar days, a substituted close may be for a symbol about to trade; a symbol past the bound is not traded (`apps/strategies/live_equity_mean_reversion.cpp:3924`) |
| `spinoff_child_policy` | `"liquidate_at_first_close"` | the same | Equity runner only: what is done with the shares a spin-off hands the book. `"liquidate_at_first_close"` books the child and sells it at its first close; `"hold"` keeps it; any other text takes the default (`apps/strategies/live_equity_mean_reversion.cpp:2651-2652`) |

### Backtest window

A backtest's window is `[end - lookback_years, end]`:

- **`end`** is the wall-clock time at which the runner starts. The backtest runners take no date
  argument, so two runs on different days cover different days.
- **`start`** is the same local calendar date and time with the year reduced by
  `backtest.lookback_years` (`ConfigLoader::resolve_backtest_window`,
  `src/core/config_loader.cpp:807`; called at `apps/backtest/bt_portfolio_conservative.cpp:136`).
- **`backtest.frozen_end_date`** (`"YYYY-MM-DD"`) is a test-only key that pins `end` to 00:00
  local time on that date, so repeated runs cover identical days. A run that honours it logs a
  WARN line saying so. It is absent from `config_template/defaults.json`, and a production config
  does not carry it. A value that is not of the form `YYYY-MM-DD`, or whose month is outside 1
  to 12 or whose day is outside 1 to 31, stops the run. A day the month does not have
  (`2026-02-31`) is accepted and rolls into the next month
  (`src/core/config_loader.cpp:832-856`).
- **`backtest.store_trade_details`** (`true` in the template and in the code) is the switch of
  the backtest's database storage: with `false` the run stores neither its daily positions after
  the warm-up (`src/backtest/backtest_coordinator.cpp:407`) nor its results
  (`save_portfolio_results_to_db`, `:2027`).

The start, and a frozen end, are built in the host's local time
(`src/core/config_loader.cpp:814`, `:855`, `:863`); an end that is not frozen is the wall-clock
instant (`:816`).

**The warm-up is inside the window.** The first rows of the window feed the sleeves and generate
no fill. Their number is the longest EMA window of the book's trend sleeves (never less than the
sleeve's `vol_lookback_short`): 256 rows with the shipped configuration
(`BacktestCoordinator::calculate_warmup_days`, `src/backtest/backtest_coordinator.cpp:1788`;
`TrendFollowingStrategy::get_max_required_lookback`, `src/strategy/trend_following.cpp:1201`). A
futures book has about 312 bar dates a year, so the warm-up is about ten months.

So a window is **named by the years it trades**, not by `lookback_years`, with one exception, the
last row:

| `lookback_years` | Trades about | Called |
|---|---|---|
| 3 | the last two years and two months | the two-year window |
| 6 | the last five years and two months | the five-year window |
| 11 | the last ten years and two months | the ten-year window |
| 16 | the last fifteen years and two months | the sixteen-year window: the one name that is the lookback itself, the longest whole-year window the stored history (from 2010-06-07) fills |

`lookback_years` shorter than the longest EMA needs is not refused: the loader logs a WARN and
the run proceeds on a rule that never warms up (`ConfigLoader::validate_config`,
`src/core/config_loader.cpp:696`).

### Strategy Types

| Type | Description |
|------|-------------|
| `TrendFollowingStrategy` | Trend following over the configured EMA pairs |
| `TrendFollowingFastStrategy` | The same sleeve class with a faster configuration (BASE's second sleeve) |
| `MeanReversionStrategy` | The equity mean-reversion sleeve |

---

## Keys that are present and not read

These keys are in the config files and change nothing a run computes. Do not tune them. "Not
read" means that no position, fill, cost or reported figure of a shipped book depends on the
value: either no code reads it, or the code that reads it feeds a step no shipped book runs (the
two covariance keys, which the portfolio manager reads on every run), or it produces a log line
only (`max_drawdown`). Several
of these keys are still required or checked when the config is loaded, so "not read" does not
mean "may be deleted": the third column says where deleting or changing one stops the run.

| Key | What the code does with it today | Checked at load |
|---|---|---|
| `strategy_defaults.fdm` (`defaults.json`) | Loaded into the config object. The runners copy it into a trend sleeve only when the sleeve's own table is empty, and the sleeve's table is the six-row table in the code, which is never empty. The multipliers in force are the code's (`include/trade_ngin/strategy/trend_following.hpp:32-33`; `apps/strategies/live_portfolio_conservative.cpp:749-751`). | No. |
| `execution.commission_rate` (`defaults.json`) | Read by the equity live runner only, printed in its start banner as not used, and copied into a per-symbol map that nothing reads (`apps/strategies/live_equity_mean_reversion.cpp:266`, `:535`, `:595`). The futures runners and the backtests do not read it. Costs come from the cost model. | No. |
| `execution.slippage_bps` (`defaults.json`) | Read by the equity live runner into a local variable and printed in the same banner; nothing else (`apps/strategies/live_equity_mean_reversion.cpp:267`, `:536`). | No. |
| `strategies.<sleeve>.config.vol_lookback_long` (`portfolio.json`) | Copied into the trend sleeve's config and read by nothing that computes: the estimator's long-run mean is a constant of the code, 2,520 values (`include/trade_ngin/strategy/trend_following.hpp:30`; `apps/strategies/live_portfolio_conservative.cpp:746`; the only other touch is a sanity fix of the field itself, `src/strategy/trend_following.cpp:29-30`). | No. Absent means 252. |
| `starting_capital` (`portfolio.json`) | Checked and not read. The sizing takes `initial_capital` (`include/trade_ngin/live/live_sizing_read.hpp:250`). | Yes: required on a futures book, positive, and equal to `initial_capital` (`src/core/config_loader.cpp:532-552`, `:764-769`). |
| `sizing_mode` (`portfolio.json`) | One value is accepted, `half_compounding`. It is copied into the run's stored config and selects nothing (`include/trade_ngin/portfolio/loop_config.hpp:20`). | Yes: required on a futures book (`src/core/config_loader.cpp:520-531`, `:757-762`). |
| `covariance_history_prices` (`portfolio.json`) | Sets how many closes per symbol the portfolio manager keeps for the generic optimiser's covariance (`src/portfolio/portfolio_manager.cpp:1034`). The futures search does not use that history: it takes 756 closes from a constant (`src/strategy/trend_following.cpp:505`). The equity books run with the optimiser off. | Yes, when present: a whole number of at least 2 (`src/core/config_loader.cpp:262-274`). Absent means 756. |
| `covariance_stale_dates` (`portfolio.json`) | Sets the stale rule of the generic optimiser's covariance (`src/portfolio/portfolio_manager.cpp:1298`). The futures search uses the constant 5 (`src/optimization/one_pass.cpp:519`). | Yes, when present: a whole number of at least 0 (`src/core/config_loader.cpp:279-292`). Absent means 5. |
| `optimization.use_buffering` (`defaults.json`) | Read only by the generic optimiser (`src/optimization/dynamic_optimizer.cpp:90`), which no shipped book runs: a futures book is rebalanced by the one pass, an equity book has the optimiser off. | No. |
| `optimization.convergence_threshold` (`defaults.json`) | Read only by the generic optimiser (`src/optimization/dynamic_optimizer.cpp:205`, `:213`). The futures search's threshold is a constant of the code. | No. |
| `confidence_level` in the `carver` module (`risk.json`) | Validated and not read by the overlay. It must equal `risk_reporting.confidence_level`, which the report's risk reading does use (`src/risk/risk_manager.cpp:805`). | Yes: required, in (0, 1), equal to the reporting block's value (`src/risk/risk_module_config.cpp:512-516`, `:1084-1103`). |
| `lookback_period` in the `carver` module (`risk.json`) | Validated and not read by the overlay, whose window is a constant of the code, 252 dates (`include/trade_ngin/risk/overlay.hpp:32`). The carver module's own window code reads it (`src/risk/carver_risk_module.cpp:128`), and that code runs only in the generic risk step and the sleeve step, which no shipped book takes. | Yes: required, a positive whole number, at least 120, at least `min_gate_dates`, equal to the reporting block's value (`src/risk/risk_module_config.cpp:519-523`, `:264-282`, `:1084-1103`). |
| `lookback_unit` in the `carver` module (`risk.json`) | One value is accepted, `dates`. It selects nothing. | Yes: required (`src/risk/risk_module_config.cpp:244-255`). |
| `min_gate_dates` in the `carver` module (`risk.json`) | Validated and not read by the overlay, which reports itself blind below a constant of the code, 21 complete dates (`include/trade_ngin/risk/overlay.hpp:34`; `src/risk/overlay.cpp:84`). The carver module's own blind test reads it (`src/risk/carver_risk_module.cpp:370`), and that code runs only in the generic risk step and the sleeve step, which no shipped book takes. | Yes: required, a whole number of at least 3 (`src/risk/risk_module_config.cpp:256-263`). |
| `missing_symbol_policy` in the `carver` module (`risk.json`) | One value is accepted, `ignore`, with its written reason. Nothing acts on it after the load: the one test of it compares it with `refuse`, which the load does not admit (`src/risk/risk_module_config.cpp:594`). | Yes: required; `warn` and `refuse` are refused as not implemented (`src/risk/risk_module_config.cpp:285-316`). |
| `risk_reporting.lookback_period` (`risk.json`) | Validated and compared with the module's value. The report's risk reading does not use it. | Yes: required (`src/risk/risk_module_config.cpp:1050-1062`, `:1084-1103`). |
| `risk_reporting.type`, `risk_reporting.window` (`risk.json`) | One value each is accepted, `carver` and `all_bars`. They select nothing. | Yes: required (`src/risk/risk_module_config.cpp:1064-1066`). |
| `max_drawdown` (`risk.json`) | Copied to every sleeve. Its one use is a warning line when a sleeve's cumulative P&L over its allocated capital is below minus the value; the sleeve keeps running and nothing is cut (`src/strategy/base_strategy.cpp:467-471`; `apps/strategies/live_portfolio_conservative.cpp:688`). | Yes: required, in (0, 1] (`src/risk/risk_module_config.cpp:782-787`). |
| `to_emails_production` (`email.json`) | Loaded into the config object and read by nothing. Every report goes to `to_emails` (`include/trade_ngin/core/config_loader.hpp:60-61`; `apps/strategies/live_portfolio_conservative.cpp:4400`). | No. |
| `use_tls` (`email.json`) | Copied into the sender's config and read by nothing. The sender always asks for TLS (`src/core/email_sender.cpp:163`; `apps/strategies/live_portfolio_conservative.cpp:4399`). | No. |

Any other key that begins with an underscore is a note and is not read as a value. The
exceptions are the underscore keys `risk.json` requires where the rules under
[risk.json](#riskjson) say so (`_reason`, `_ruled_by`, `_ruled_on`,
`_missing_symbol_policy_reason`, `_never_reason`): a file without one of those does not load.

---

## Examples

The three shipped books in `config_template/portfolios/` are the worked examples; each loads as
it stands once the placeholders are replaced.

### Conservative Portfolio

One trend sleeve on 500,000, the three optional blocks present, the overlay as its risk module:

**portfolio.json** (the notes, the three blocks and the two covariance keys are omitted here; see the template):
```json
{
  "portfolio_id": "CONSERVATIVE_PORTFOLIO",
  "initial_capital": 500000.0,
  "strategies": {
    "TREND_FOLLOWING": {
      "enabled_backtest": true,
      "enabled_live": true,
      "default_allocation": 1.0,
      "type": "TrendFollowingStrategy",
      "config": {
        "risk_target": 0.20,
        "idm": 2.5,
        "ema_windows": [[2, 8], [4, 16], [8, 32], [16, 64], [32, 128], [64, 256]],
        "vol_lookback_short": 32,
        "vol_lookback_long": 252
      }
    }
  },
  "equity_slow_rule": {"symbols": ["M2K", "MES", "MNQ", "MYM"], "pairs": [[32, 128], [64, 256]]},
  "sizing_mode": "half_compounding",
  "starting_capital": 500000.0,
  "use_optimization": true
}
```

**risk.json:** the `carver` form shown under [risk.json](#riskjson), with `max_drawdown` 0.3.

### Base Portfolio

Two sleeves (`TREND_FOLLOWING` at `default_allocation` 0.7 with `risk_target` 0.2,
`TREND_FOLLOWING_FAST` at 0.3 with `risk_target` 0.25 and four EMA pairs), the same required
top-level keys, no optional block, and the same `carver` module with `max_drawdown` 0.4. It is a
placeholder test book.

### Equity Mean Reversion Portfolio

One `MeanReversionStrategy` sleeve with its symbol list, `use_optimization` false, and a `none`
risk module with its recorded decision. Its `risk_reporting` block carries all seven values and the file
carries `max_leverage`.

---

## Migration from Legacy Config

If you're using the old single-file configuration (`config.json`), follow these steps:

1. **Keep legacy file temporarily**: `ConfigLoader::load_legacy` still reads the database,
   email and strategy sections of the single file; no shipped runner calls it

2. **Create new config structure:**
   ```bash
   cp -r config_template config
   ```

3. **Split your config:**
   - Database settings → `config/defaults.json`
   - Email settings → `config/portfolios/base/email.json`
   - Strategy definitions → `config/portfolios/base/portfolio.json`
   - Risk settings → `config/portfolios/base/risk.json`, in schema 2

4. **Update application code:**
   ```cpp
   // Old way (legacy)
   auto credentials = std::make_shared<CredentialStore>("./config.json");

   // New way
   auto config_result = ConfigLoader::load("./config", "base");
   if (config_result.is_error()) {
       // Handle error
   }
   auto config = config_result.value();
   ```

5. **Test thoroughly** before removing legacy config files

---

## Troubleshooting

### Config file not found
- Verify the config directory path is correct
- Check file permissions
- Ensure all required files exist (defaults.json, portfolio.json, risk.json, email.json)

### JSON parse error
- Validate JSON syntax using a JSON linter
- Check for trailing commas (not allowed in JSON)
- Ensure all strings are double-quoted

### The run stops with "config for <portfolio>: ..." or "risk config for <portfolio>: ..."
- The message names the key and the rule it broke. Nothing was run and nothing was stored.
- "is required": write the key; there is no default to fall back on.
- "is retired" or "was removed": delete the key (see [Retired keys](#retired-keys)).
- "risk.json is schema 1": run `scripts/migrate_risk_json.py`.
- "is not ... (a misspelt block is an absent block)": fix the block's name.

### Values not applied
- Check that the field name matches exactly (case-sensitive). Outside `risk.json` and the inside
  of the three optional blocks, a misspelt key that is not a required one is not reported: it
  reads as an absent key and the value in the code is used (see
  [What stops a run, and what falls back to the code](#what-stops-a-run-and-what-falls-back-to-the-code))
- Check that the key is not one of the [keys that are present and not read](#keys-that-are-present-and-not-read)
- Verify the value type (number vs string)
- `portfolio.json` values override `defaults.json`; `risk.json` and `email.json` are read whole

---

## Best Practices

1. **Version control**: Keep `config_template/` in version control; `config/` holds credentials and is not tracked
2. **Sensitive data**: Never put a real password, host or port in a tracked file or a document
3. **Documentation**: Add `_note` keys beside a value to record why it is what it is
4. **Testing**: Test config changes in backtest before live trading
5. **Backup**: Keep backups before major config changes
