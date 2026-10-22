# Risk modules

This document describes the risk layer of the engine as it is built: the module interface, the
configuration that assigns modules to a book, what happens when a module cannot answer, the one
module the futures books run (the Carver overlay), and what each day leaves in the database and
the log.

Every `path:line` is in this repository. Symbols used throughout: `E` is the sizing capital of the
day, `tau` the risk target of the book's first sleeve, `u_i` the weight of ONE contract of symbol i
on the sizing capital (multiplier x raw signal close / `E`), `x_i` a position's weight (contracts x
`u_i`, signed), `N*` the unrounded target and `N*c` the capped target.

---

## 1. What the risk layer is, and what it is not

The risk layer answers one question per rebalance: may this book be traded as targeted, and if
not, by how much is the target reduced, or is the day refused. It is a set of **modules**, each a
small object with one `evaluate` call, assigned to a book (portfolio scope) or to one strategy of
it (sleeve scope) by the book's `risk.json`.

It is not the optimiser. The optimiser turns a target in fractional contracts into a book of whole
contracts at the lowest tracking error and trading cost; it has its own covariance, its own cap
handling and its own no-trade band, and it never decides how much risk the book may carry.
[OPTIMIZER_AND_RISK_DESIGN.md](OPTIMIZER_AND_RISK_DESIGN.md) describes it and the full order of a
futures rebalance. This document covers the parts of that rebalance that belong to risk: the
overlay's readings and its one scalar before the search, and the trim and the marks after the
rounding.

It is also not the reporter. The live runners build a separate `RiskManager` after the trade to
fill the reporting columns of `trading.live_results` (`portfolio_var`, `max_correlation`,
`jump_risk`) from the `risk_reporting` block of `risk.json`
(`apps/strategies/live_portfolio_conservative.cpp:2984-2993`). That object measures the stored book
and never moves a contract.

Three books ship in `config_template/portfolios/`:

| Book | Portfolio modules | Sleeve modules |
|---|---|---|
| `conservative` (futures, one trend sleeve) | one `carver` module carrying the overlay's limits | none |
| `base` (futures, two trend sleeves) | one `carver` module carrying the overlay's limits | none |
| `equity_mr` (equities) | one `none` assignment (section 10) | none |

## 2. Short map

The design comments in these headers are the primary description; this document points at them.

| File | What it holds |
|---|---|
| `include/trade_ngin/risk/risk_module.hpp` | the interface: actions, terms, scopes, phases, the decision, the context, the per-rebalance record |
| `include/trade_ngin/risk/basic_risk_modules.hpp` | the three plug-in modules: a constant cut, a warning, a refusal on a condition |
| `include/trade_ngin/risk/carver_risk_module.hpp` | the `carver` module; on a futures book it carries the overlay's limits (`set_overlay_limits`, line 104) |
| `include/trade_ngin/risk/overlay.hpp`, `src/risk/overlay.cpp` | the overlay's arithmetic as pure functions: the gate window, the five readings, the multiplier |
| `include/trade_ngin/optimization/one_pass.hpp`, `src/optimization/one_pass.cpp` | one rebalance end to end, including the overlay's scalar, the trim and the marks |
| `include/trade_ngin/optimization/one_pass_log.hpp` | the text of the OVERLAY, RISK_TRIM and RISK_OVER_LIMIT_BY_HOLD lines |
| `include/trade_ngin/risk/risk_detail.hpp` | the day's record and the nine keys of `risk_detail` |
| `include/trade_ngin/risk/risk_module_config.hpp`, `src/risk/risk_module_config.cpp` | schema 2 of `risk.json` and every load rule |
| `include/trade_ngin/portfolio/loop_config.hpp` | hands the per-name cap and the trim cap from `risk.json` to the portfolio |
| `include/trade_ngin/risk/risk_scale_report.hpp` | the RISK_SCALE_REPORT and RISK_DELIVERED lines |
| `include/trade_ngin/live/risk_module_failure.hpp` | the live runners' exit code, email flag and banner on a held day |
| `include/trade_ngin/live/run_metadata_marks.hpp` | the `risk_refusal` mark on `trading.live_run_metadata` |
| `src/portfolio/portfolio_manager.cpp` | where modules are built, validated, evaluated and recorded |

## 3. The module interface

A module implements `RiskModule` (`include/trade_ngin/risk/risk_module.hpp:103-139`). The
portfolio manager is its only caller. A module never sees the strategies: it is handed a book
(symbol to position) and a `RiskContext` (lines 60-83) and returns a `RiskDecision` (lines 48-57).

### 3.1 Actions

A decision carries exactly one action (`RiskAction`, `risk_module.hpp:19-29`).

| Action | What the portfolio manager does with the scope's book |
|---|---|
| `NONE` | nothing; the book passes untouched |
| `SCALE` | multiplies the book by `scale`, applied only when `0 <= scale < 1` |
| `WARN` | nothing to the book; logs `reason` and records the breach |
| `REFUSE` | does not trade the scope this rebalance: it is pinned to its previous positions |
| `REPLACE` | takes the module's own `book` as the scope's targets. At portfolio scope on a book of several strategies the book cannot be split, and every strategy is pinned to its previous positions instead (`src/portfolio/portfolio_manager.cpp:609-626`). None of the five module types of section 3.4 returns it: it is in the interface for a module written in code |

`REFUSE` is a value, never an error `Result`. An error `Result` (or an exception) from `evaluate`
means the module could not answer, which section 6 treats as a refusal of its own kind.

When a scope has several modules their decisions are combined by
`PortfolioManager::combine_risk_decisions` (`src/portfolio/portfolio_manager.cpp:3207`): the
precedence is REFUSE, then REPLACE, then SCALE, then WARN, then NONE; among several SCALE requests
the smallest is applied, never their product; a losing request is recorded and not applied.

### 3.2 Terms

Each module declares the families of risk it measures (`RiskTerm`, `risk_module.hpp:32-38`). Only
the validator reads them.

| Term | Meaning | Rule |
|---|---|---|
| `COMPOSITION` | a reading that does not change when the whole book is scaled | at most one such module along any sleeve to portfolio chain |
| `MAGNITUDE` | a reading that scales with the book (leverage, a constant cut) | none |
| `PATH` | a reading that depends on the P&L path | none |
| `CUSTOM` | warn and refuse conditions, anything else | none |

### 3.3 Scopes

| Scope | The book the module reads | Capital in the context | When it runs |
|---|---|---|---|
| `PORTFOLIO` | the whole book | the sizing capital | once per rebalance |
| `SLEEVE` | one strategy's own targets | the sizing capital x that strategy's allocation | once per rebalance, before the optimiser (`apply_sleeve_risk`, `portfolio_manager.cpp:3637`) |

A sleeve SCALE multiplies that sleeve's targets alone. On a futures book the factor is carried
into the sleeve's contribution to the summed target the one pass reads
(`portfolio_manager.cpp:2274-2276`).

### 3.4 Module types

The `type` string of a module in `risk.json` is the string the module's `type()` returns.

| Type | Class | Can return | Terms | Scope allowed |
|---|---|---|---|---|
| `carver` | `CarverRiskModule` (`carver_risk_module.hpp:23`) | SCALE, WARN | COMPOSITION, MAGNITUDE | portfolio only |
| `constant_scale` | `ConstantScaleRiskModule` (`basic_risk_modules.hpp:32`) | SCALE | MAGNITUDE | portfolio or sleeve |
| `warn` | `WarnRiskModule` (`basic_risk_modules.hpp:55`) | WARN | CUSTOM | portfolio or sleeve |
| `refuse` | `RefuseOnConditionRiskModule` (`basic_risk_modules.hpp:75`) | REFUSE | CUSTOM | portfolio or sleeve |
| `none` | builds no object (`make_risk_module`, `risk_module_config.cpp:695-698`) | nothing | none | portfolio only, and alone |

`warn` and `refuse` fire on a `RiskCondition` (`basic_risk_modules.hpp:16-25`): `always`, `never`,
`lap_at_least`, `nonzero_positions_above`, `max_abs_quantity_above`.

`carver` is refused at sleeve scope because the module is built with the portfolio's capital and
its leverage limits would be read against the whole book's money
(`risk_module_config.cpp:160-165`, and again in `PortfolioManager::validate_risk_modules`,
`portfolio_manager.cpp:2973-2979`).

### 3.5 The record of a rebalance

Every decision of a rebalance is kept as a `RiskDecisionRecord` (`risk_module.hpp:143-159`): the
phase and the lap, the scope and its id, the module id, the decision as the module returned it, the
action the portfolio manager applied because of it, the factor it multiplied by, whether the book
was empty, and the error text when the module failed. `PortfolioManager::last_risk_decisions()` returns the records of the last call and
`risk_decisions_json()` the same as JSON with each module's `describe()`. The live runners read
these records to decide the exit code and the metadata mark (section 6).

### 3.6 Which path a book takes

`PortfolioManager::process_market_data` runs the sleeve modules first, then one of two paths
(`portfolio_manager.cpp:487-523`).

| Book | Test | Path |
|---|---|---|
| names an overlay sleeve with a positive `tau` (both futures books; the runners set them from the first sleeve, `apps/backtest/bt_portfolio_conservative.cpp:410-411`) | `one_pass_book()`, `portfolio_manager.cpp:2000` | `rebalance_one_pass` (`portfolio_manager.cpp:2015`): the overlay, one search from the held book, one rounding, the trim, the fills. No module's `evaluate` is called at portfolio scope: the `carver` module is the carrier of the limits and the name on the decision record |
| any other book (the equity book) | otherwise | the optimiser step if enabled, then each portfolio module's `evaluate` once, then rounding (`portfolio_manager.cpp:524-658`) |

A book that holds a trend sleeve and names no overlay sleeve is an error, not a fallback to the
second path (`portfolio_manager.cpp:500-518`).

## 4. Configuration: schema 2 of `risk.json`

Schema 2 has no defaults layer. Every gating value is written literally in the book's own
`risk.json`, a book that runs no risk layer says so with a `none` module, and every rule below is a
load error. The design comment is `include/trade_ngin/risk/risk_module_config.hpp:3-14`.
[CONFIG_GUIDE.md](CONFIG_GUIDE.md) has the file layout of a config directory.

### 4.1 The file

The `conservative` book's file (`config_template/portfolios/conservative/risk.json`, comment keys
omitted). The `base` book's file (`config_template/portfolios/base/risk.json`) is the same except
`max_drawdown`, which is 0.4:

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
      "_missing_symbol_policy_reason": "..."
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

Sleeve modules do not live in `risk.json`. They are the `sleeve_risk_modules` object of
`portfolio.json`, keyed by strategy id, each value an array of module objects in the same form
(`risk_module_config.cpp:822-853`). No shipped book has one.

A key that starts with an underscore is a note. It is kept and never read as a value, except the
attribution keys the rules below require.

### 4.2 Keys of a `carver` module that carries the overlay's limits

A `carver` module that names `R_max`, `R_jump_max` and `R_shock_max` is the overlay. A futures
runner refuses to start without one (`ConfigLoader::require_loop_keys`,
`src/core/config_loader.cpp:737-756`).

| Key | Required | Meaning | Read by |
|---|---|---|---|
| `id`, `type` | yes | the module's name and `"carver"` | the loader, every record |
| `R_max`, `R_jump_max`, `R_shock_max` | yes, all three together | the three risk limits as ratios to `tau`; each a positive number | the overlay |
| `max_gross_leverage`, `max_net_leverage` | yes | the gross and net leverage limits, as they are; `0 < net <= gross <= 10` | the overlay; must equal the `risk_reporting` pair, which is what the reporter reads |
| `per_name_cap` | yes | the cap `L` on one symbol's weight on the sizing capital; positive | the cap on the target, the search, the clip, the CAP mark |
| `trim_max` | yes | the most contracts the trim removes in one rebalance; a whole number, 0 or more | the trim |
| `confidence_level` | yes | in (0, 1) | nothing in the overlay; checked at load (table below) |
| `lookback_period` | yes | a positive whole number of dates, at least 120 and at least `min_gate_dates` | nothing in the overlay; checked at load (table below) |
| `lookback_unit` | yes | must be `"dates"`; `"bars"` is refused with the command that upgrades the file | nothing; checked at load (table below) |
| `min_gate_dates` | yes | a whole number, at least 3 | nothing in the overlay; checked at load (table below) |
| `missing_symbol_policy` | yes | only `"ignore"` is accepted, and it needs a non-empty `_missing_symbol_policy_reason` | nothing; checked at load (table below) |

The overlay's own window sizes are constants of the code, not these keys: 252 window dates, 120
complete dates for the complete-date covariance, 21 for BLIND, 120 returns for a participant to be
in the covariance (`include/trade_ngin/risk/overlay.hpp:32-35`). The shipped files carry the same
252 and 21, and the loader holds `lookback_period` to the 120 floor, but changing those keys does
not change the overlay's window.

**Keys of `risk.json` that are present and not read.** These keys are in the config files and
change nothing a run computes. Do not tune them. Each is still required or checked when the file
is loaded, so none may be deleted.

| Key | What the code does with it today | Checked at load |
|---|---|---|
| `confidence_level` in the `carver` module (`risk.json`) | Validated and not read by the overlay. It must equal `risk_reporting.confidence_level`, which the report's risk reading does use (`src/risk/risk_manager.cpp:805`). | Yes: required, in (0, 1), equal to the reporting block's value (`src/risk/risk_module_config.cpp:512-516`, `:1084-1103`). |
| `lookback_period` in the `carver` module (`risk.json`) | Validated and not read by the overlay, whose window is a constant of the code, 252 dates (`include/trade_ngin/risk/overlay.hpp:32`). The carver module's own window code reads it (`src/risk/carver_risk_module.cpp:128`), and that code runs only in the generic risk step and the sleeve step, which no shipped book takes. | Yes: required, a positive whole number, at least 120, at least `min_gate_dates`, equal to the reporting block's value (`src/risk/risk_module_config.cpp:519-523`, `:264-282`, `:1084-1103`). |
| `lookback_unit` in the `carver` module (`risk.json`) | One value is accepted, `dates`. It selects nothing. | Yes: required (`src/risk/risk_module_config.cpp:244-255`). |
| `min_gate_dates` in the `carver` module (`risk.json`) | Validated and not read by the overlay, which reports itself blind below a constant of the code, 21 complete dates (`include/trade_ngin/risk/overlay.hpp:34`; `src/risk/overlay.cpp:84`). The carver module's own blind test reads it (`src/risk/carver_risk_module.cpp:370`), and that code runs only in the generic risk step and the sleeve step, which no shipped book takes. | Yes: required, a whole number of at least 3 (`src/risk/risk_module_config.cpp:256-263`). |
| `missing_symbol_policy` in the `carver` module (`risk.json`) | One value is accepted, `ignore`, with its written reason. Nothing acts on it after the load: the one test of it compares it with `refuse`, which the load does not admit (`src/risk/risk_module_config.cpp:594`). | Yes: required; `warn` and `refuse` are refused as not implemented (`src/risk/risk_module_config.cpp:285-316`). |
| `risk_reporting.lookback_period` (`risk.json`) | Validated and compared with the module's value. The report's risk reading does not use it. | Yes: required (`src/risk/risk_module_config.cpp:1050-1062`, `:1084-1103`). |
| `risk_reporting.type`, `risk_reporting.window` (`risk.json`) | One value each is accepted, `carver` and `all_bars`. They select nothing. | Yes: required (`src/risk/risk_module_config.cpp:1064-1066`). |
| `max_drawdown` (`risk.json`) | Copied to every sleeve. Its one use is a warning line when a sleeve's cumulative P&L over its allocated capital is below minus the value; the sleeve keeps running and nothing is cut (`src/strategy/base_strategy.cpp:467-471`; `apps/strategies/live_portfolio_conservative.cpp:688`). | Yes: required, in (0, 1] (`src/risk/risk_module_config.cpp:782-787`). |

[CONFIG_GUIDE.md](CONFIG_GUIDE.md) lists the keys of the other config files that are present and
not read.

`per_name_cap` and `trim_max` reach the portfolio through `apply_loop_config`
(`include/trade_ngin/portfolio/loop_config.hpp:17-29`). The gross leverage limit is also handed to
each sleeve as its own `max_leverage` (`loop_gross_leverage_limit`, `loop_config.hpp:36-42`).

### 4.3 Retired keys

On a book whose `carver` module carries the overlay's limits these keys are refused, never
ignored. The fix in every case is to remove the key.

| Key | Where it is refused | Rule |
|---|---|---|
| `var_limit`, `jump_risk_limit`, `max_correlation` | on the `carver` module | `risk_module_config.cpp:179-184` |
| `var_limit`, `jump_risk_limit`, `max_correlation` | in `risk_reporting` | `risk_module_config.cpp:1046-1057` |
| `max_leverage` | at the top level of `risk.json` | `risk_module_config.cpp:1038-1041` |

Keys removed on every book, whatever its modules:

| Key | Rule |
|---|---|
| `enabled` on a module (a module the book does not run is a module the book does not list) | `risk_module_config.cpp:104-108` |
| `use_risk_management` anywhere, `risk_defaults`, `strategy_defaults.use_optimization` | `check_removed_keys`, `src/core/config_loader.cpp:93-121` |
| `corr_shock_threshold`, `jump_shock_threshold` under `risk` | `src/core/config_loader.cpp:122-133` |

A book that does NOT carry the overlay's limits (the equity book) keeps the older shape: its
`risk_reporting` block requires `var_limit`, `jump_risk_limit` and `max_correlation`, and
`max_leverage` is required at the top level (`risk_module_config.cpp:1042-1045`, `1059-1062`).

### 4.4 The load rules

`parse_risk_schema` (`risk_module_config.cpp:738`) stops at the first broken rule. The order is in
the comment at `risk_module_config.cpp:6-12`.

| Rule | Line |
|---|---|
| A file with no `schema` key and any flat gating key is schema 1: refused, with the migration command in the message | `check_not_schema1`, 671-682 |
| `schema` is required and must be 2; `modules` is required | 757-764 |
| The only top-level keys are `schema`, `modules`, `risk_reporting`, `max_drawdown`, `max_leverage` | 767-775 |
| `max_drawdown` is required, in (0, 1] | 782-787 |
| `modules` is a non-empty array: an omission and a decision must not share an encoding | 802-806 |
| Every module has a known `type`; a key that is not one of its type's keys is refused; every required key of its type must be present (the only optional keys are `every_lap` on a `constant_scale` module and the three overlay limits on a `carver` module) | `ModuleParser::parse`, 97; `require_keys`, 382-387 |
| `none` needs non-empty `_reason`, `_ruled_by` and `_ruled_on` (YYYY-MM-DD), is valid at portfolio scope only, and must be the only module of its scope | 130-152, 815-819 |
| `carver` is valid at portfolio scope only | 160-165 |
| `R_max`, `R_jump_max`, `R_shock_max` come together or not at all | 169-176 |
| `constant_scale` needs `scale` in (0, 1]; `warn` and `refuse` need a `condition` and a non-empty `reason` | 319-349 |
| A `warn` or `refuse` whose condition is `never` needs a non-empty `_never_reason` | 986-999 |
| A sleeve key must name a strategy of the book; a sleeve's array must not be empty | 827-842 |
| One module id names one module across the whole book | 857-879 |
| At most one COMPOSITION-term module along any sleeve to portfolio chain (so two `carver` modules at portfolio scope are refused) | 883-916 |
| On a book with more than one enabled sleeve, a module that can REFUSE (a `refuse` module) is refused at either scope | 920-947 |
| A book with no `carver` module anywhere, other than the lone `none`, needs `_ruled_by` and `_ruled_on` at the top of `risk.json` | 955-981 |
| `risk_reporting` is required, with `type` `"carver"` and `window` `"all_bars"`, and no key outside its own list | 1017-1031, 1064-1068 |
| While a `carver` module gates the book, every `risk_reporting` value equals the module's | 1084-1105 |

The portfolio manager repeats the structural rules on what it is handed, so a config built in code
is held to them too: an empty module list throws (`portfolio_manager.cpp:50-54`), a module that
cannot be built throws rather than leaving the book ungated (`portfolio_manager.cpp:65-79`), and
`validate_risk_modules` (`portfolio_manager.cpp:2913`) refuses a null module, a duplicate id, more
than one REPLACE-capable module in a scope, a second COMPOSITION module, a `carver` on a sleeve and
a sleeve key that names no registered strategy.

### 4.5 Migrating an older file

`scripts/migrate_risk_json.py` rewrites a schema-1 config directory to schema 2 and upgrades a
schema-2 file that still says `"lookback_unit": "bars"`:

```
python3 scripts/migrate_risk_json.py <config dir>             # dry run: read the diff
python3 scripts/migrate_risk_json.py <config dir> --in-place  # write it
```

It keeps a `.schema1.bak` (or `.bars.bak`) copy of each file it changes and refuses, with the
reason, where a file does not say enough to migrate safely: it never writes a value nobody wrote.
It does not write a futures book's overlay keys. After it has run, a futures book still needs, by
hand, `R_max`, `R_jump_max`, `R_shock_max`, `per_name_cap` and `trim_max` on its `carver` module,
and the retired keys of section 4.3 removed; `config_template/` shows the result. The script's
header lists the other futures keys outside `risk.json` that the runners require.

## 5. The Carver overlay

The overlay is the book-level reduction described by Robert Carver in Advanced Futures Trading
Strategies, Part Six, "Tactic four: Risk management", section "An exogenous risk overlay": it
takes the positions the strategy wants as its input, measures them in several ways, turns each
measure into a multiplier no greater than one, and multiplies the positions by the smallest. Four
of the engine's five readings are the book's four (estimated portfolio risk, jump risk, correlation
shock, leverage). The net leverage reading, the per-name cap on the target, the treatment of held
rows, and the trim after rounding are this engine's own; the reasons for each are in
[OPTIMIZER_AND_RISK_DESIGN.md](OPTIMIZER_AND_RISK_DESIGN.md).

On a futures book the overlay is ONE module: the book's single `carver` module. Its arithmetic is
`overlay::gate_window`, `overlay::readings` and `overlay::multiplier`
(`src/risk/overlay.cpp:23`, `143`, `180`), called from `one_pass::rebalance`
(`src/optimization/one_pass.cpp:379`).

### 5.1 The book the overlay reads

The overlay reads the **capped target book in capital terms**, never the optimiser's output.

Each symbol of the rebalance is classified first (`one_pass.cpp:390-414`):

| Class | Rule | Quantity the overlay reads |
|---|---|---|
| free | some sleeve signals it and it is not in the hold set | its capped target `N*c = sign(N*) x min(abs(N*), L / u)` (`cap_target`, `one_pass.cpp:35`) |
| fixed (a held row) | not free and not a close-out, and either a non-zero held position or a signalled hold-set symbol at zero | its HELD quantity |
| close-out | has a bar today, is not in the hold set, no sleeve signals it now, it was signalled earlier in the run (on a live run a held position stands for that), and the held quantity is not zero | zero |

The hold set is the engine's own (a symbol whose bar is not a session bar, a withheld bar, a
change bar, a symbol the cost model cannot price) plus the deferral band (a held position on the
other side of the first sleeve's forecast while that forecast is inside the band).
[OPTIMIZER_AND_RISK_DESIGN.md](OPTIMIZER_AND_RISK_DESIGN.md) and
[FUTURES_ROLLS.md](FUTURES_ROLLS.md) describe when each applies.

The free, fixed and close-out symbols are the **participants**. A position's weight is
`x_i = quantity x u_i`, with `u_i = multiplier x raw signal close / E`
(`one_pass.cpp:383`, `441-448`). The price is the real contract close, not the back-adjusted one,
and `E` is the day's sizing capital, not the starting capital.

### 5.2 The five readings

| Reading | Formula | Limit | Where the limit is configured | Code |
|---|---|---|---|---|
| `R`, portfolio risk | `sqrt(x' Sigma x)`, `Sigma` the annualised covariance of the gate window | `R_max x tau` | `R_max` on the `carver` module | `overlay.cpp:159-172` |
| `R_jump`, jump risk | `sqrt(x' Sigma_jump x)`, `Sigma_jump` built from the window's correlations and each participant's own jump sigma | `R_jump_max x tau` | `R_jump_max` | `overlay.cpp:164-173` |
| `R_shock`, correlation shock | `sum of abs(x_i) x sigma_i`, `sigma_i` the participant's annualised window sigma | `R_shock_max x tau` | `R_shock_max` | `overlay.cpp:174-176` |
| `L_g`, gross leverage | `sum of abs(x_i)` | `max_gross_leverage` | `max_gross_leverage` | `overlay.cpp:146-149` |
| `L_n`, net leverage | `sum of x_i`, signed; the limit applies to its absolute value | `max_net_leverage` | `max_net_leverage` | `overlay.cpp:146-149`, `182-190` |

`tau` is the risk target of the book's first sleeve (`risk_target` in that sleeve's `config` in
`portfolio.json`, 0.20 for the first sleeve of both futures books in `config_template`). With the
shipped ratios 2.25, 4.5 and 4.0 the limits in reading units are 0.45, 0.90 and 0.80, beside 8.0
and 6.0 for the two leverage readings. The limits are turned from ratios into reading units at
`portfolio_manager.cpp:2223-2227`. Carver's book sets each of its own limits at the 99th
percentile of that reading's history on its own portfolio, so that the overlay acts on unusual
days and leaves ordinary days alone. The limits here are the values written in `risk.json`;
nothing in the code derives them. The three risk limits are held as ratios to `tau` so that they move with
the risk target.

The jump sigma of a participant is the 99th percentile of its trailing short-run volatility
estimates (up to 2,520 values), each taken back to a daily standard deviation
(`src/strategy/trend_estimator.cpp:302-308`), then annualised on the gate window's own factor
(`one_pass.cpp:433-438`). `R_jump` therefore answers: what would the book's risk be if every
position's volatility went to the top of its own range while correlations stayed as they are.
`R_shock` answers the opposite question: volatilities as they are, every correlation at plus or
minus one, whichever is worse.

Each reading gives a multiplier `min(1, limit / reading)`; a reading that is not computed or not
positive asks for nothing (`overlay::multiplier`, `overlay.cpp:180-206`). The names in the log and
in `risk_detail` are `R`, `R_jump`, `R_shock`, `L_g`, `L_n`.

### 5.3 The gate window

The window is built over the participants' back-adjusted percentage returns, the rows ending at
the signal date (`overlay::gate_window`, `overlay.cpp:23-141`; the rules are the comment at
`overlay.hpp:12-31`).

| Step | Rule |
|---|---|
| The window | the last 252 dates on which ANY participant has a return |
| A participant with no return in the window | out of `R`, `R_jump` and `R_shock`; still in both leverage readings |
| A young participant: fewer than 120 returns in the window | out of the complete-date intersection and of `R` and `R_jump`; in `R_shock` on its own window sigma once it has two returns; in both leverage readings |
| Complete dates | the window dates on which every remaining participant has a return |
| Fewer than 21 complete dates | the window is BLIND (section 5.4) |
| 21 to 119 complete dates | the covariance is the sample covariance of every window date with the missing returns set to zero (mode `zerofill`) |
| 120 or more complete dates | the covariance is the sample covariance (n minus 1) of the complete dates (mode `complete`) |
| Annualisation | bars a year = (complete dates minus 1) / (the calendar days they span / 365.25), one factor for the whole window |

The young-participant rule is what keeps a newly listed contract from shortening everybody's
window: a contract with twenty returns would otherwise cut the complete dates to twenty and blind
the whole book.

### 5.4 BLIND

A window with fewer than 21 complete dates cannot give a covariance worth reading. The overlay
does not guess and does not refuse:

- the three covariance readings are not computed, and their multipliers are 1;
- the two leverage readings are computed and can still cut;
- the day is stored with `overlay_blind` true and the log carries one `OVERLAY_BLIND` warning
  (`portfolio_manager.cpp:2639-2646`); the OVERLAY line prints `blind` in place of the three
  readings.

BLIND is a state of the data, not a failure. It is the only case in which the overlay answers
without all five readings.

### 5.5 One scalar, applied once

`m` is the smallest of the five multipliers, and the binding term is the first term at `m` in the
order `R`, `R_jump`, `R_shock`, `L_g`, `L_n` (`none` when `m` is 1). It is applied ONCE, to the
capped target of every FREE row (`one_pass.cpp:468-471`):

```
scaled target = m x N*c        on a free row
```

Nothing is scaled a second time. The optimiser then searches from the held book toward the scaled
target, the result is buffered and rounded, and the trim (section 5.7) is the only step after it
that can remove a contract for risk.

### 5.6 Held rows are not scaled

A fixed row is counted in every reading at its held quantity and is never multiplied by `m`: the
engine holds it for a reason that a risk cut does not override (no tradable bar today, a roll in
progress, a deferred close). Two consequences follow.

- Every reading scales linearly with the free rows only. With no held row in the book, a cut of
  `m` brings the binding reading exactly to its limit. With held rows, the reading of the scaled
  target can stay above the limit, because the held part did not shrink.
- The remedy is the trim and then a mark, never a second scalar.

### 5.7 The trim, and the two marks

After the rounding and the clip to the cap, the overlay re-reads the whole-contract book. This is
the only reading of a rounded book. If a reading is above its limit the trim runs
(`one_pass::trim`, `one_pass.cpp:305-346`):

1. take the first breached term in the order `R`, `R_jump`, `R_shock`, `L_g`, `L_n`;
2. among the FREE rows with a non-zero quantity, find the one contract whose removal lowers that
   reading the most (toward zero on a short as on a long; a tie goes to the first symbol);
3. remove it and re-read;
4. stop when no reading is above its limit, when `trim_max` contracts have been removed, or when
   no single removal lowers the reading.

A held row is never trimmed. What remains is stored as it is.

| Outcome | `risk_detail` | Log |
|---|---|---|
| The trim brings every reading inside its limit | no mark | no warning |
| The stored book is still above a limit after the trim | `over_limit_after_rounding_terms` (the terms, in the order above, separated by `;`) and `over_limit_after_rounding_excess` | one `RISK_TRIM` warning |
| The held rows ALONE are above a limit, and the stored book is above the same limit | `over_limit_by_hold_terms` gains the term; `over_limit_by_hold_symbols` names the held rows that keep it over | one `RISK_OVER_LIMIT_BY_HOLD` warning |
| A held row's own weight is beyond the per-name cap | `over_limit_by_hold_terms` gains `CAP`; the row is named in `over_limit_by_hold_symbols` | the same warning |

Details, all at `one_pass.cpp:578-619`:

- `over_limit_after_rounding_excess` is the largest excess (reading minus limit) divided by the
  largest `u_i` among the non-zero stored rows. It reads as "how many contracts of the heaviest
  held name the breach is worth".
- For `R` and `R_jump` the held rows named are those with a positive contribution
  `x_i x (Sigma x)_i` to the held reading, so a hedging row is not named. For `R_shock` every
  non-zero held row in the shock reading is named; for `L_g` every non-zero held row; for `L_n` the
  held rows on the net's side (`overlay::contributors`, `overlay.cpp:208-246`).
- `CAP` does not depend on any reading. The cap bounds the target (section 5.1), the search and the
  rounded free rows (`clip_to_cap`, `one_pass.cpp:48`); a held row is never clipped, so a held
  position whose price has run can sit beyond `L` and the mark says so.
- Neither warning is printed in warm-up.

### 5.8 When the overlay cannot answer

The overlay refuses the book, as section 6 describes, when it cannot produce `m`:

| Cause | Code |
|---|---|
| The overlay sleeve is not running, so nothing can be weighed | `portfolio_manager.cpp:2107-2113` |
| A held symbol cannot be weighed today and has no usable last close or multiplier | `portfolio_manager.cpp:2193-2198` |
| A sleeve of the book was refused or replaced by its own module | `portfolio_manager.cpp:2593-2594` |
| The sizing capital, a weight per contract, a held quantity or a target is not a finite positive number as required | `one_pass.cpp:456-460` |
| A reading of the capped target is not finite, or `m` is outside 0 to 1 | `one_pass.cpp:463-467` |
| The re-read of the rounded book is not finite | `one_pass.cpp:568-571` |
| Any exception inside the pass | `portfolio_manager.cpp:2596-2600` |

Too few dates is never a cause: that is BLIND.

## 6. Fail-closed behaviour and the REFUSE contract

The rule is the same at every level: a risk step that cannot answer does not let the book through
untouched. It holds the scope at the book it already has.

### 6.1 What a refusal does

A refusal, whether a module returned REFUSE or a module could not answer, has one meaning: **the
scope is not traded this rebalance**.

| | A refused futures day, live |
|---|---|
| The book | every sleeve is stored at the book the run started with (the previous day's stored positions, seeded before the rebalance); no search, no trim |
| Orders | none; no execution is written for the held scope |
| The day's rows | positions and `trading.live_results` are still written, so the chain of days is unbroken; `risk_scale` is 1 and `risk_detail` is NULL |
| `trading.live_run_metadata` | the day's row is written a second time with `risk_refusal` (the action, the module, the scope id, the phase and the lap, the reason, and the error when a failure caused the refusal; `portfolio_risk_refusal`, `run_metadata_marks.hpp:39-56`) and `risk_decisions` (the full record) added to its `portfolio_config` JSON; every other key is unchanged (`mark_risk_refusal`, `include/trade_ngin/live/run_metadata_marks.hpp:65-71`; the call is `apps/strategies/live_portfolio_conservative.cpp:1830-1848`) |
| Exit code | 3 (`kRiskModuleFailureExitCode`, `include/trade_ngin/live/risk_module_failure.hpp:35`; returned at `live_portfolio_conservative.cpp:4777`) |
| Log | `OVERLAY refused ... reason="..."` (the ordinary OVERLAY line when it is the re-read of the rounded book that failed), an ERROR `Risk overlay refused portfolio ...`, and an ERROR `RISK_MODULE_FAILURE ...` |
| Email | the subject is prefixed `[RISK MODULE FAILED - BOOK HELD] ` and the body opens with a banner (`risk_module_failure.hpp:92-152`) |

The row is marked, not deleted, because it records a run that did happen. Without the mark a held
day would look like an ordinary completed run: its positions and results are there. The watchdog
reads the mark (`scripts/check_live_trading.py:162`).
[LIVE_RUN_CYCLE.md](LIVE_RUN_CYCLE.md) and [performance_upkeep.md](performance_upkeep.md) cover the
exit codes and the watchdog from the operator's side.

A futures live run exits 0 when it reaches the end of the run with the book not refused, 3 when it reaches the
end with the book held because the risk step refused it or the sizing read failed, and 1 when it stops early.
Exit 0 covers a completed day, a carried day, and a run in which a store failed, was logged and was passed over;
the evidence of a completed day is the date's `trading.live_results` row, not the exit code. The wrapper also
returns 0, without starting the binary, when it finds its lock held (nothing stored). A refusal by the risk step never ends at 0: the
`live_run_metadata` row is marked `risk_refusal` and the run ends at 3, whether a module could not answer or a
sleeve module decided to refuse, or at 1 when the refused book has no stored previous positions to be held at.
Exit 1 stores nothing when the run refuses before its `live_run_metadata` row is written, and can leave that
row, the signals or a half-written day when it stops later. The equity runner and the backtest runners return
only 0 or 1.

The reason is in the one pass. It records every refusal of the book with its reason as the record's error
(`portfolio_manager.cpp:2689-2695`, `2878-2881`), including the refusal that a sleeve module's own REFUSE
causes (`2593-2594`), and the runner returns 3 for a portfolio record that carries an error
(`risk_module_failure.hpp:41-55`, `87-89`). The runner would return 0, with the row marked, for a portfolio
REFUSE recorded without an error; only the generic risk step records one (section 6.2), and neither futures
runner builds a book that takes that step: every sleeve they build is a trend sleeve and the first is named as
the overlay sleeve (`apps/strategies/live_portfolio_conservative.cpp:722-810`).
[LIVE_RUN_CYCLE.md](LIVE_RUN_CYCLE.md) has the table of every exit code and what each can leave stored.

In a backtest a refused rebalance stores the held book for that cycle, writes no fill, and leaves
the cycle's `backtest.equity_curve.risk_detail` NULL
(`src/backtest/backtest_coordinator.cpp:1568-1576`).

### 6.2 The cases

| Case | Scope held | Recorded as | Live exit |
|---|---|---|---|
| The overlay cannot answer (section 5.8) | the whole book | a portfolio REFUSE by the `carver` module, with the reason as its error (`portfolio_manager.cpp:2689-2698`, `2878-2881`) | 3 |
| A sleeve module returns REFUSE or REPLACE, or cannot answer, on a futures book | the WHOLE book: one search on the summed book cannot hold one sleeve apart, so the sleeve's refusal refuses the book (`portfolio_manager.cpp:2568-2594`) | the sleeve's own record, then a portfolio REFUSE with the reason "a sleeve of the book was refused by its own risk module" | 3 |
| A portfolio module cannot answer (an error or an exception from `evaluate`), on a book without an overlay sleeve | the whole book, whatever the module's capabilities (`refuse_on_failed_gatekeeper`, `portfolio_manager.cpp:3299-3338`) | a portfolio REFUSE on the failed module's row, with the error | not reached on a live run (see below) |
| The portfolio risk step itself fails (a module's `on_bars` throws, an exception after `evaluate`) | the whole book (`portfolio_manager.cpp:554-593`) | a portfolio REFUSE under the module id `(risk step)` (`kRiskStepModuleId`, `risk_module.hpp:164`) | not reached on a live run (see below) |
| A sleeve module cannot answer, on a book without an overlay sleeve | that sleeve alone; the other sleeves trade | a sleeve REFUSE with the error; the metadata row is marked with scope `sleeve` (`sleeve_risk_module_failure`, `risk_module_failure.hpp:62-82`) | not reached on a live run (see below) |
| A module returns REFUSE as its decision, on a book without an overlay sleeve | its scope | a REFUSE with the module's `reason` and no error; a portfolio REFUSE marks the metadata row | not reached on a live run (see below) |
| A refusal of a scope whose previous book was never seeded | nothing can be held: pinning would ship a flat book | `process_market_data` returns an error (the book: `portfolio_manager.cpp:2882-2888`; a sleeve: `3694-3708`, `3775-3785`, with no portfolio record) | 1. The day's `live_run_metadata` row is left marked `risk_refusal` when the refused scope is the book (`live_portfolio_conservative.cpp:1830-1841`) and when it is a sleeve whose module or risk step could not answer (scope `sleeve`, `1869-1898`): the runner writes the mark before it reads the error (`1900-1903`). A sleeve module that decided to refuse a sleeve with no seeded book leaves the row unmarked |

The exit code follows the record: 3 whenever a record carries an error and its scope was refused
(`portfolio_risk_module_failure` and `sleeve_risk_module_failure`,
`risk_module_failure.hpp:41-82`). The metadata mark follows the applied action: any portfolio
record whose applied action is REFUSE (`portfolio_risk_refusal`, `run_metadata_marks.hpp:39-56`).

The exit code, the metadata mark and the email flag are the two futures live runners'
(`apps/strategies/live_portfolio.cpp`, `apps/strategies/live_portfolio_conservative.cpp`). Both
build trend sleeves only, so every book they run names an overlay sleeve: on a live run only the
first two rows and the last occur. Rows 3 to 6 are the portfolio manager's contract for a book
without an overlay sleeve, as its record would be read by those runners' rule (3 where the record
carries an error, 0 where it does not). The equity live runner, the one live runner of such a
book, does not read the decision record: it sets no exit code 3 and writes no mark
(`apps/strategies/live_equity_mean_reversion.cpp:4473-4481` reads it for the RISK_SCALE_REPORT
and RISK_DELIVERED lines only).

A day on which the sizing read fails with every sleeve's book loaded (the previous day's `live_results` row,
the latest row before it or the stored P&L history cannot be read, `live_sizing_read.hpp:158-189`) is stored
the same way: the book held, the row marked
`risk_refusal` with scope `sizing`, exit 3 (`LiveSizingOutcome::kHoldBook`,
`include/trade_ngin/live/live_sizing_read.hpp:73`, `204`, `307-309`). A sleeve book the sizing read
cannot load stops the run with exit 1 before anything is stored (`live_sizing_read.hpp:74`, `195`;
`live_portfolio_conservative.cpp:1498-1502`). That is the sizing read's rule, not a module's;
[LIVE_RUN_CYCLE.md](LIVE_RUN_CYCLE.md) describes it.

### 6.3 A second portfolio module on a futures book

A book that names an overlay sleeve runs the overlay and nothing else at portfolio scope. The
check is at the start of every rebalance (`portfolio_manager.cpp:2033-2055`): the portfolio module
list must be exactly one `carver` module carrying the three risk limits. A second portfolio module
of any type, or no such `carver` module, makes `process_market_data` return an
`INVALID_ARGUMENT` error. This is a configuration error, not a refusal: no book is held, no
decision is recorded, and a live run exits 1 (`live_portfolio_conservative.cpp:1900-1903`). The
day's `live_run_metadata` row is written before the rebalance, so that row, unmarked, is what the
run leaves stored.

The loader catches one form of it earlier: two `carver` modules at portfolio scope are refused at
load by the COMPOSITION rule (section 4.4). A `carver` plus a `warn`, a `refuse` or a
`constant_scale` at portfolio scope loads and is then stopped by the check above on the first
rebalance.

## 7. What is stored each day

Migration `020_risk_detail.sql` adds `risk_detail` (jsonb, NULL) to `trading.live_results` and to
`backtest.equity_curve`, and sets the comment of `trading.live_results.risk_scale` to the
delivered scale (`migrations/020_risk_detail.sql:35-42`).

### 7.1 `trading.live_results.risk_scale`: the delivered scale

On a futures row `risk_scale` is what the day's rebalance DELIVERED, not what the overlay asked
for:

```
risk_scale = gross weight of the stored book / gross weight of the capped target book
```

Both sides are valued at the same raw signal closes and count the held rows, and the stored book is
the one the runner stores (`PortfolioManager::delivered_scale_for_book`,
`portfolio_manager.cpp:3047-3059`; `live_portfolio_conservative.cpp:4046-4056`).

- It can exceed 1: a book held through a falling target is larger than the target.
- It is not `m` and is not expected to equal it: the search from the held book and the no-trade
  band decide how far the stored book moves toward the scaled target.
- A day with no sized rebalance (no session, a sizing hold, a refused overlay) stores 1, and so
  does a flat capped target.
- The request `m` is never stored in this column. It is the `risk_detail` key `risk_requested`.

`backtest.equity_curve` has no `risk_scale` column; the backtest logs the same figure on its
RISK_DELIVERED line (section 8).

On an equity row `risk_scale` is the reporter's recommended scale of the stored book
(`apps/strategies/live_equity_mean_reversion.cpp:5711-5720`). Nothing cuts that book (section 10).

### 7.2 `risk_detail`

One flat JSON object with nine keys, the same on both tables (`kRiskDetailKeys` and
`risk_detail_json`, `include/trade_ngin/risk/risk_detail.hpp:48-82`).

| Key | Type | Meaning |
|---|---|---|
| `risk_requested` | number | `m`, 1.0 with no cut |
| `binding_term` | text | `R`, `R_jump`, `R_shock`, `L_g`, `L_n` or `none` |
| `over_limit_after_rounding_terms` | text or null | the terms still above their limits after the trim, separated by `;` |
| `over_limit_after_rounding_excess` | number or null | the largest of those excesses in units of the largest non-zero stored `u_i` |
| `over_limit_by_hold_terms` | text or null | the terms the held rows keep over, and `CAP` |
| `over_limit_by_hold_symbols` | text or null | the held rows named, separated by spaces |
| `overlay_blind` | boolean | the gate window had fewer than 21 complete dates |
| `sizing_capital` | number | `E`, the capital the day's book was sized on |
| `account_value` | number | the account value at the instant the sizing capital was read: the starting capital plus the cumulative settled net P&L it was built from |

It is written on the row of every sized rebalance the overlay answered (`OnePassDay::stores_detail`,
`risk_detail.hpp:44`): the live write is `live_portfolio_conservative.cpp:4220-4225`, the backtest
write `backtest_coordinator.cpp:1568-1576`. The cell is NULL on an equity row, on a warm-up row and
the backtest's seed row, on a refused rebalance, and on a cycle that ran no rebalance.

The five readings, their limits and the five multipliers are not stored. They are on the OVERLAY
line.

## 8. The log lines

OVERLAY, RISK_TRIM and RISK_OVER_LIMIT_BY_HOLD are written by the portfolio manager under the
`RiskManager` component tag (`portfolio_manager.cpp:2636-2667`); the other two are written by the
runner after the rebalance.

| Line | Level | When | Source |
|---|---|---|---|
| `OVERLAY` | INFO | once per rebalance of a futures book | `overlay_line`, `include/trade_ngin/optimization/one_pass_log.hpp:48-88` |
| `RISK_TRIM` | WARN | a sized rebalance whose stored book is still above a limit after the trim | `risk_trim_line`, `one_pass_log.hpp:145-160` |
| `RISK_OVER_LIMIT_BY_HOLD` | WARN | a sized rebalance with a by-hold term or `CAP` | `over_limit_by_hold_line`, `one_pass_log.hpp:164-171` |
| `RISK_SCALE_REPORT` | INFO | once per run (live), once per post-warm-up cycle of a futures backtest (`src/backtest/backtest_coordinator.cpp:1142-1155`) | `format_risk_scale_report`, `include/trade_ngin/risk/risk_scale_report.hpp:77-88` |
| `RISK_DELIVERED` | INFO | directly after RISK_SCALE_REPORT | `format_risk_delivered`, `risk_scale_report.hpp:191-207` |

**OVERLAY** is the full account of the reading. Its fields, in order: `m`, `binding`; the five
readings; `limits` (the five limits in reading units); `multipliers` (the five); `capital` (`E`) and
`tau`; `window` (`complete`, `zerofill` or `blind`), `dates`, `first`, `last`, `complete_dates`,
`dropped_dates`, `f5_engaged` (1 when the complete-date covariance is in use); `participants`,
`free`, `held`; `no_return=[...]` and `short_history=[...]`, the participants left out of readings
by section 5.3. A refused rebalance prints `OVERLAY refused capital=... tau=... participants=...
reason="..."` instead. A BLIND window adds the `OVERLAY_BLIND` warning.

**RISK_TRIM** reads `RISK_TRIM over_limit_after_rounding terms=[term:excess;...] excess_units=...
trimmed=[symbol:contracts ...] trim_capped=0|1`. `trim_capped=1` means the trim removed `trim_max`
contracts and the book was still over.

**RISK_OVER_LIMIT_BY_HOLD** reads `RISK_OVER_LIMIT_BY_HOLD terms=[...] symbols=[...]`.

**RISK_SCALE_REPORT** reads `RISK_SCALE_REPORT reporter=... applied_cumulative=... laps=...
cutting_laps=... binding_module=... [date=YYYY-MM-DD]`. On a futures book `applied_cumulative` is
`m` at eight decimal places (the factor on the decision record) or 1 with no cut, `laps` is 1 (0 on a day refused on the re-read of the rounded book, and on a cycle that ran no rebalance: a backtest cycle whose signal feed is empty reports an empty record, `src/backtest/backtest_coordinator.cpp:1139-1147`),
`cutting_laps` is 1 on a cut day and 0 otherwise, and `binding_module` is the `carver` module's id
on a cut day and `none` otherwise (`summarize_applied_risk`, `risk_scale_report.hpp:53-71`).
`reporter` is the reporting `RiskManager`'s recommended scale on a live run
(`live_portfolio_conservative.cpp:3024-3026`) and `na` in a backtest. It is not the stored
`risk_scale` of a futures row.

**RISK_DELIVERED** reads `RISK_DELIVERED requested=... delivered=... final_gross=...
capped_target_gross=... unpriced=... [date=YYYY-MM-DD]`. `requested` is the same number as
`applied_cumulative`; `final_gross` and `capped_target_gross` are gross notionals of the stored book
and of the capped target book with its held rows; `delivered` is their ratio.

The same rebalance also writes the optimiser's own lines (`OPTIMISER`, `BOOK`, the covariance
lines), which [OPTIMIZER_AND_RISK_DESIGN.md](OPTIMIZER_AND_RISK_DESIGN.md) covers.

## 9. A worked day

**An illustration with invented inputs.** This section shows the overlay's arithmetic on a book of
three contracts. Every input is invented and chosen round so that each step can be checked by
hand: no number here is a stored row or the output of a run. The settings are the shipped ones
(`tau` 0.20, the five limits of section 5.2, a per-name cap of 2), and every step is the one the
code takes, at the lines section 5 cites.

**The capital.** The book starts with 500,000. Its settled net P&L of the three days before the
rebalance is +15,000, -5,000 and +30,000.

| Day | Net P&L | Cumulative `C` | Peak of `C` | Account value 500,000 + `C` | Sizing capital 500,000 - (peak - `C`) |
|---|---|---|---|---|---|
| 1 | +15,000 | 15,000 | 15,000 | 515,000 | 500,000 |
| 2 | -5,000 | 10,000 | 15,000 | 510,000 | 495,000 |
| 3 | +30,000 | 40,000 | 40,000 | 540,000 | 500,000 |

The rebalance is sized on `E` = 500,000 with an account value of 540,000
(`half_compounded_capital`, `include/trade_ngin/portfolio/sizing_capital.hpp:58-71`).

**The book read.** Three participants: A and B are free, C is held (three contracts long, held by
the deferral band: its forecast has turned to -1.2, inside the band of 2, so the close is
deferred). The weight of one contract is `u = multiplier x close / E` (section 5.1).

| Contract | Class | Multiplier | Close | `u` | Quantity the overlay reads | Weight `x = quantity x u` |
|---|---|---|---|---|---|---|
| A | free | 50 | 2,000.00 | 50 x 2,000 / 500,000 = 0.20 | capped target `N*c` = +5.000000 | +1.00 |
| B | free | 1,000 | 125.00 | 1,000 x 125 / 500,000 = 0.25 | capped target `N*c` = -5.000000 | -1.25 |
| C | held | 2,000 | 100.00 | 2,000 x 100 / 500,000 = 0.40 | held quantity +3 | +1.20 |

No target is bound by the per-name cap: the largest free weight is 1.25, below 2. The gate window
is taken as complete (section 5.3), with these annualised figures:

| Contract | Window sigma | Jump sigma |
|---|---|---|
| A | 0.35 | 0.80 |
| B | 0.28 | 0.64 |
| C | 0.10 | 0.50 |

The correlation of A and B is 0.5; C is uncorrelated with both.

**Step 1: the five readings of the capped target book, and their multipliers.**

- `R`: `x' Sigma x` = 1.00^2 x 0.35^2 + 1.25^2 x 0.28^2 + 1.20^2 x 0.10^2
  + 2 x 0.5 x (1.00 x 0.35) x (-1.25 x 0.28) = 0.1225 + 0.1225 + 0.0144 - 0.1225 = 0.1369, so
  `R` = 0.37.
- `R_jump`: the same sum on the jump sigmas, 1.00^2 x 0.80^2 + 1.25^2 x 0.64^2 + 1.20^2 x 0.50^2
  + 2 x 0.5 x (1.00 x 0.80) x (-1.25 x 0.64) = 0.64 + 0.64 + 0.36 - 0.64 = 1.00, so `R_jump` = 1.00.
- `R_shock`: 1.00 x 0.35 + 1.25 x 0.28 + 1.20 x 0.10 = 0.35 + 0.35 + 0.12 = 0.82.
- `L_g`: 1.00 + 1.25 + 1.20 = 3.45. `L_n`: 1.00 - 1.25 + 1.20 = 0.95.

| Term | Reading | Ratio in `risk.json` | Limit | `limit / reading` | Multiplier `min(1, limit / reading)` |
|---|---|---|---|---|---|
| `R` | 0.370000 | 2.25 | 2.25 x 0.20 = 0.45 | 1.216216 | 1 |
| `R_jump` | 1.000000 | 4.5 | 4.5 x 0.20 = 0.90 | 0.900000 | **0.900000** |
| `R_shock` | 0.820000 | 4.0 | 4.0 x 0.20 = 0.80 | 0.975610 | 0.975610 |
| `L_g` | 3.450000 | (8.0, as it is) | 8.0 | 2.318841 | 1 |
| `L_n` | 0.950000 | (6.0, as it is) | 6.0 | 6.315789 | 1 |

Two terms ask for a cut. `m` is the smallest multiplier, 0.900000, and the binding term is
`R_jump`. `R_shock` gets no cut of its own.

**Step 2: the scalar on the free rows.**

| Contract | Class | `u` | Target `N*c` | Scaled target `m x N*c` | Held |
|---|---|---|---|---|---|
| A | free | 0.20 | +5.000000 | +4.500000 | +4 |
| B | free | 0.25 | -5.000000 | -4.500000 | -4 |
| C | held | 0.40 | (not scaled) | (not scaled) | +3 |

C is read at its held quantity, weight 3 x 0.40 = 1.20, in all five readings above, and is not
multiplied. That is why the scaled target is not brought to the limit: its weights are +0.90,
-1.125 and +1.20, its `R_jump` is sqrt(0.5184 + 0.5184 + 0.36 - 0.5184) = sqrt(0.8784) = 0.937230,
still above 0.90, because the held part did not shrink (section 5.6). Its `R_shock` is
0.315 + 0.315 + 0.12 = 0.75, inside 0.80.

**Step 3: the stored book.** The search, the buffer and the rounding
([OPTIMIZER_AND_RISK_DESIGN.md](OPTIMIZER_AND_RISK_DESIGN.md)) are not worked here: take as given
that they leave the free rows at the held book, A at +4 and B at -4, with C at +3. The weights of
that book are +0.80, -1.00 and +1.20. The overlay's re-read of it:

| Term | Capped target book | Stored book | Limit | Over? |
|---|---|---|---|---|
| `R` | 0.370000 | sqrt(0.0784 + 0.0784 + 0.0144 - 0.0784) = sqrt(0.0928) = 0.304631 | 0.45 | no |
| `R_jump` | 1.000000 | sqrt(0.4096 + 0.4096 + 0.36 - 0.4096) = sqrt(0.7696) = 0.877268 | 0.90 | no |
| `R_shock` | 0.820000 | 0.28 + 0.28 + 0.12 = 0.680000 | 0.80 | no |
| `L_g` | 3.450000 | 0.80 + 1.00 + 1.20 = 3.000000 | 8.0 | no |
| `L_n` | 0.950000 | 0.80 - 1.00 + 1.20 = 1.000000 | 6.0 | no |

No reading is over, so the trim removes nothing and no mark is set.

**Step 4: what such a day stores.**

| Where | Value |
|---|---|
| `risk_detail.risk_requested` | 0.9 |
| `risk_detail.binding_term` | `R_jump` |
| `risk_detail.over_limit_after_rounding_terms`, `..._excess` | null, null |
| `risk_detail.over_limit_by_hold_terms`, `..._symbols` | null, null |
| `risk_detail.overlay_blind` | false |
| `risk_detail.sizing_capital` | 500000.0 |
| `risk_detail.account_value` | 540000.0 |
| delivered scale (the RISK_DELIVERED ratio; `risk_scale` on a live row) | 3.00 / 3.45 = 0.869565 |

The day shows why the request and the delivery are two numbers. The overlay asked for 0.900 of
each free target. The delivered scale is the gross weight of the stored book over the gross weight
of the capped target book, held rows included (`one_pass.cpp:631-636`): 0.870 here, because the
free rows are stored at 4 contracts where the capped target was 5 and the held row did not move.
The overlay's request did not set that ratio. The sizing capital is 500,000 while the account
value is 540,000: profits above the starting capital are set aside from sizing
([OPTIMIZER_AND_RISK_DESIGN.md](OPTIMIZER_AND_RISK_DESIGN.md)).

**A day with a trim or a mark.** No worked row is shown for one. Section 5.7 gives the rule each mark
follows, and the marks are stored in `over_limit_after_rounding_terms` and `over_limit_by_hold_terms` of
`risk_detail`.

## 10. The equity book's `none` assignment

`config_template/portfolios/equity_mr/risk.json` assigns the equity book one module:

```json
{ "id": "no_portfolio_risk", "type": "none",
  "_reason": "...", "_ruled_by": "...", "_ruled_on": "..." }
```

`none` is a decision, not an omission, which is why the loader requires its `_reason`, `_ruled_by`
and `_ruled_on` keys and requires it to be the only module of the book (section 4.4). The reason
is the file's `_reason`: the readings are a futures construction; on this long-only equity book
the leverage and jump terms do not bind, and the one limit that remains is a value calibrated for
equities, not a term of the method.

What it means at run time:

- `make_risk_module` builds nothing for it, so the portfolio manager holds no module and no risk
  step runs (`portfolio_manager.cpp:55-56`, `89-103`, `594-596`). The log says so once per run:
  `Risk management is disabled in the configuration`, then a `RISK_NONE` line with the module id,
  `ruled_by` and `ruled_on`.
- Nothing can scale or refuse the book. The decision record of every rebalance is empty, so the
  RISK_SCALE_REPORT line reads `laps=0 cutting_laps=0 binding_module=none`.
- The book is still MEASURED. `risk_reporting` is required on every book and is unchanged here, so
  the reporting `RiskManager` still fills `portfolio_var`, `max_correlation`, `jump_risk` and
  `risk_scale` on the equity rows of `trading.live_results`. On this book `risk_scale` is that
  reporter's reading and not a cut.
- `risk_detail` is NULL on every equity row.
