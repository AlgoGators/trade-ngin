# How the futures loop works

This document describes one rebalance of a futures book, from the forecast to the stored whole-contract
book, as the engine runs it. It is written for an engineer who has to read, debug or change the loop. It
covers the two futures books: CONSERVATIVE (one trend sleeve) and BASE (two trend sleeves, a placeholder
test book). The equity book does not go through this loop.

What is elsewhere:

| Topic | Document |
|---|---|
| The strategy end to end, with the source in Carver's book for every stage | [TREND_FOLLOWING_SYSTEM.md](TREND_FOLLOWING_SYSTEM.md) |
| The risk module interface, the refusal contract, the overlay as a module | [RISK_MODULES.md](RISK_MODULES.md) |
| Change bars, the adjusted series, roll legs | [FUTURES_ROLLS.md](FUTURES_ROLLS.md) |
| What a fill costs, fees per contract, netting between sleeves | [COST_MODEL.md](COST_MODEL.md) |
| The order of a live run, the tables, the column dictionary | [LIVE_RUN_CYCLE.md](LIVE_RUN_CYCLE.md) |
| Every config key | [CONFIG_GUIDE.md](CONFIG_GUIDE.md) |

"The book" in a citation is Robert Carver, Advanced Futures Trading Strategies.

Code citations are `path:line`. A file is given with its full path the first time and by
its file name after that. The two live futures runners (`apps/strategies/live_portfolio_conservative.cpp`
and `apps/strategies/live_portfolio.cpp`) carry the same loop code; the conservative one is cited.

---

## 1. The loop on one page

One rebalance per book per signal date. A live run given the date T signals on the bars up to T-1 and books
its fills at the T-1 close with the date T. A backtest cycle for day T signals on the previous bar group in
the same way.

| # | Stage | What it produces | Where |
|---|---|---|---|
| 1 | Sizing capital | E, the capital every capital-terms quantity divides by | `half_compounded_capital`, `include/trade_ngin/portfolio/sizing_capital.hpp:58`; set through `PortfolioManager::set_sizing_capital`, `src/portfolio/portfolio_manager.cpp:3812` |
| 2 | Forecast | F in [-20, 20] per symbol per sleeve | `trend_estimator::estimate`, `src/strategy/trend_estimator.cpp:132`; called from `TrendFollowingStrategy::on_data`, `src/strategy/trend_following.cpp:276` |
| 3 | Unrounded target | N*, fractional contracts, long or short | `TrendFollowingStrategy::calculate_position`, `src/strategy/trend_following.cpp:1046` |
| 4 | Classification | each symbol is free, held (fixed) or a close-out | `one_pass::rebalance`, `src/optimization/one_pass.cpp:379` (lines 393 to 414) |
| 5 | Cap and overlay | the capped target, five readings, one scalar m | `one_pass.cpp:416` to `471`; arithmetic in `src/risk/overlay.cpp` |
| 6 | Forecast-sign close, search | a whole-contract answer y from the held book | `one_pass.cpp:473` to `535`; `search`, `one_pass.cpp:170` |
| 7 | Buffer, rounding, clip | the book before the trim | `b_sigma` `one_pass.cpp:231`, `buffer` `:253`, `clip_to_cap` `:48` |
| 8 | Trim | the stored book | `trim`, `one_pass.cpp:305` |
| 9 | Split, fills, record | per-sleeve books, fills, `risk_detail`, log lines | `PortfolioManager::rebalance_one_pass`, `src/portfolio/portfolio_manager.cpp:2015` (split at 2748, fills at 2813) |

Stages 4 to 8 are one pure function of plain vectors, `one_pass::rebalance(const DayInputs&)`. It reads no
database, no clock and no state from the previous call. `rebalance_one_pass` builds its inputs, calls it
once, and stores what comes back. Nothing feeds back from a later stage into an earlier one: the scalar
is computed from the target alone, never from the optimiser's answer, and is applied once. The rounded book
is read again by the overlay only for the trim (section 7.5).

Read the design comments before the code. They state the rules this document summarises:

| Comment | File |
|---|---|
| The estimators: window rule, volatility, forecast | `include/trade_ngin/strategy/trend_estimator.hpp:12` |
| The annualisation count | `include/trade_ngin/strategy/vol_annualisation.hpp:16` |
| Half compounding as a closed form | `include/trade_ngin/portfolio/sizing_capital.hpp:36` |
| The live sizing reads and their failure outcomes | `include/trade_ngin/live/live_sizing_read.hpp:1` |
| The pass as pure functions; `DayInputs` and `DayResult` field by field | `include/trade_ngin/optimization/one_pass.hpp:15`, `:137` |
| The gate window, the readings, the multiplier | `include/trade_ngin/risk/overlay.hpp:12`, `:60`, `:80` |
| The log lines | `include/trade_ngin/optimization/one_pass_log.hpp:3` |
| The stored record and its nine keys | `include/trade_ngin/risk/risk_detail.hpp:16` |
| Listing dates, the switch rules, the relabel list | `include/trade_ngin/data/listing_dates.hpp:15`, `:61`, `:108` |
| The live session rule and the hold | `include/trade_ngin/live/session_book_gate.hpp:24`, `:251` |
| The split of a symbol over sleeves | `include/trade_ngin/portfolio/allocation_split.hpp:35` |

Where a header comment cites a section number, the rule it refers to is stated in this document or in one of
the documents it links.

A book is rebalanced by this loop when its `PortfolioConfig` names an overlay sleeve
(`one_pass_book`, `portfolio_manager.cpp:2000`). The runners name the book's first sleeve
(`apps/backtest/bt_portfolio_conservative.cpp:410`). A book that holds a trend sleeve and names none is
refused, never sent through the generic optimiser step (`portfolio_manager.cpp:499` to `518`). The class in
`dynamic_optimizer.hpp` serves books with no overlay sleeve and takes no part in a futures rebalance.

---

## 2. Notation and settings

| Symbol | Meaning | Value in force | Where it is set |
|---|---|---|---|
| S_0 | starting capital | 500,000 | `portfolio.json` `initial_capital`, which the sizing reads (`include/trade_ngin/live/live_sizing_read.hpp:250`). `starting_capital` is required on a futures book and checked equal to it (`src/core/config_loader.cpp:543`); nothing computes from it |
| E | sizing capital of the rebalance (section 4) | recomputed every run | not a config key, not a column |
| V | account value, S_0 plus cumulative settled net P&L | | `risk_detail.account_value` |
| tau | annual risk target | 0.20 (BASE's fast sleeve 0.25) | `portfolio.json` `strategies.<sleeve>.config.risk_target`, required (`include/trade_ngin/strategy/sleeve_config.hpp:19`) |
| IDM | instrument diversification multiplier | 2.5 | `strategies.<sleeve>.config.idm`, required |
| w_i | instrument weight (section 5) | from the metadata `Sector` column | `TrendFollowingStrategy::get_weights`, `trend_following.cpp:915` |
| M_i | contract multiplier | metadata `Contract Size` | instrument registry |
| P_i | the raw close of the signal bar | | |
| sigma_i | annualised volatility of the adjusted returns | | `trend_estimator.cpp:167` to `216` |
| F_i | combined forecast after the equity slow rule | [-20, 20] | section 3 |
| N*_i | unrounded target in contracts | | section 5 |
| u_i | weight of one contract on the sizing capital, M_i P_i / E | | `one_pass.cpp:383` |
| L | per-name cap on a position's weight, abs(n_i) u_i <= L | 2 | `risk.json` carver module `per_name_cap` |
| m | the overlay's scalar, in (0, 1] | | section 6 |
| h_i, n_i | held and stored position, whole contracts | | |

The other constants of the pass:

| Constant | Value | Key |
|---|---|---|
| Cost multiplier of the search | 100 | `defaults.json` `optimization.cost_penalty_scalar` |
| Deferral band | 2 | `defaults.json` `optimization.sign_close_band` |
| Floor of the no-trade band, as a ratio to tau | 0.05 | `defaults.json` `optimization.b_sigma_floor` |
| Floor of the search's pass cap | 100 | `defaults.json` `optimization.max_iterations` |
| R_max, R_jump_max, R_shock_max, as ratios to tau | 2.25, 4.5, 4.0 | `risk.json` carver module `R_max`, `R_jump_max`, `R_shock_max` |
| Gross and net leverage limits | 8.0, 6.0 | `risk.json` carver module `max_gross_leverage`, `max_net_leverage` |
| Most contracts the trim removes in a day | 5 | `risk.json` carver module `trim_max` |
| Sizing mode | `half_compounding` | `portfolio.json` `sizing_mode`. It is required, it is the one value accepted, and it selects nothing: the code has one sizing rule (`src/core/config_loader.cpp:520`) |

`ConfigLoader::require_loop_keys` (`src/core/config_loader.cpp:728`) refuses a futures book that lacks the
equity slow rule, the three risk ratios, `sizing_mode`, `starting_capital`, `cost_penalty_scalar`,
`sign_close_band` or `b_sigma_floor`; the risk schema requires `per_name_cap` and `trim_max` with the ratios
(`src/risk/risk_module_config.cpp:188`). `apply_loop_config`
(`include/trade_ngin/portfolio/loop_config.hpp:17`) then carries the band, the floor, the sizing mode, the
slow rule, the cap and the trim cap into `PortfolioConfig`; the cost multiplier and the pass-cap floor are
read by the loader into `opt_config`, and the limits stay on the carver module. One constant has an in-code
default: `optimization.max_iterations` is not required and reads 100 when absent
(`include/trade_ngin/optimization/dynamic_optimizer.hpp:37`).

---

## 3. The forecast

The method and its citations are in [TREND_FOLLOWING_SYSTEM.md](TREND_FOLLOWING_SYSTEM.md). This section
is the mechanics.

### 3.1 The window

Every estimator is a function of the last 3,200 consumed bars of the symbol ending at the signal bar
(`kWindowBars`, `trend_estimator.hpp:52`) and of nothing else. Every recursion starts at the window's first
bar. Nothing is carried from one call to the next, so a live run and a backtest that hold the same bars
compute the same numbers. A symbol with fewer bars uses what it has; the run counts such rows and prints one
`ESTIMATOR_SHORT_WINDOW` line.

Returns and EMAs read the back-adjusted series; every level (the price the forecast divides by, the sizing
price) reads the raw close. See [FUTURES_ROLLS.md](FUTURES_ROLLS.md).

### 3.2 Volatility

A mean-centred EWMA standard deviation of the adjusted returns, span 32 (the fast sleeve: 16).
`sigma_short` is that daily figure times the annualisation factor, clamped to [0.005, 5.0]. `sigma_long` is
the mean of `sigma_short` over the trailing 2,520 values. The sizing volatility is
`sigma = 0.7 sigma_short + 0.3 sigma_long`.

The annualisation factor is the square root of the bars a year, counted over the trailing 256 bars:
`(256 - 1) / (calendar days they span / 365.25)` (`trend_estimator.cpp:156` to `164`;
the rule is stated at `vol_annualisation.hpp:16`). A series
that prints a Sunday session has more bars a year than 256, so its factor is above 16. The forecast's own
volatility uses the same variance with the fixed factor 16.

### 3.3 The six speeds, scaling and capping

For each EWMAC pair (fast, slow), with both EMAs on the adjusted level:

```
raw     = (EMA_fast - EMA_slow) / (P_raw x forecast_sigma / 16)
scaled  = clamp(raw x attenuation x scalar, -20, +20)
```

| Pair | 2/8 | 4/16 | 8/32 | 16/64 | 32/128 | 64/256 |
|---|---|---|---|---|---|---|
| Scalar (`forecast_scalar`, `trend_estimator.cpp:46`) | 12.1 | 8.53 | 5.95 | 4.10 | 2.79 | 1.91 |

The scalars are fixed (the book, strategy nine, table 29). A pair with no scalar is refused when the sleeve
is initialised (`trend_following.cpp:67`). The trend sleeve runs all six pairs; BASE's fast sleeve runs the first
four.

The attenuation is `2 - 1.5 q`, where q is the share of the trailing 2,520 `sigma_short` values at or below
today's, smoothed by a 10-bar EWMA. It is 1 until 252 values exist (`trend_estimator.cpp:218` to `255`).

### 3.4 Weights and the forecast diversification multiplier

The combined forecast is the equal-weight mean of the scaled forecasts of the pairs the contract runs,
times the multiplier for that number of pairs, capped at +/-20 (`trend_estimator.cpp:296`).

| Pairs run | 6 | 5 | 4 | 3 | 2 | 1 |
|---|---|---|---|---|---|---|
| Weight of each | 1/6 | 1/5 | 1/4 | 1/3 | 1/2 | 1 |
| Multiplier | 1.26 | 1.19 | 1.13 | 1.08 | 1.03 | 1.00 |

These are the rows of the book's table 36 (strategy nine). The engine reads the row by the number of pairs
run. The book states each row for the slowest pairs of that number, which is what a contract with removed
rules runs. BASE's fast sleeve runs the four fastest pairs and reads the four-pair row all the same. The
table in force is
`TrendFollowingConfig::fdm` (`include/trade_ngin/strategy/trend_following.hpp:32`). `defaults.json`
`strategy_defaults.fdm` carries the same six rows and changes nothing: the runners copy it into a sleeve
only when the sleeve's own table is empty, and the table in the code is never empty
(`apps/strategies/live_portfolio_conservative.cpp:749`).

### 3.5 Rules removed from a contract by cost

`portfolio.json` `trading_rule_removals` names, per contract, the pairs that contract does not run. Eighteen
rules are removed on twelve contracts:

| Contracts | Pairs not run | Pairs left | Multiplier |
|---|---|---|---|
| 6M, GF, HE, LE, NG, ZL, ZS | 2/8 | 5 | 1.19 |
| 6L, KE, ZC, ZW | 2/8, 4/16 | 4 | 1.13 |
| ZR | 2/8, 4/16, 8/32 | 3 | 1.08 |

Every other contract runs all six.

How the forecast is rebuilt for a listed contract (`trend_following.cpp:463` to `491`): the sleeve looks the
symbol's root up in the list, `trend_estimator::pairs_after_removal` (`trend_estimator.cpp:94`) returns the
pairs left, the multiplier is read from the table for their number, and `estimate` is called with those
pairs and that multiplier. Each pair left is scaled, attenuated and capped exactly as on any contract. The
mean is over the pairs left, so each carries an equal weight of one over their number. A removed pair is
never computed: it has no value, not a value of zero.

What is refused when the sleeve is initialised (`validate_config`, `trend_following.cpp:96` to `164`):

- a name that is not a base symbol, or that the instrument registry does not hold;
- a contract listed with no pair;
- a set of removed pairs that is not the contract's fastest ones in order, that leaves none, or that names
  one twice (only the fastest can be removed, because table 36 is stated for exactly those sets);
- a number of pairs left that the multiplier table has no row for;
- the removal of a pair the equity slow rule reads, on a symbol the rule names;
- the two contracts of a listing-date pair (section 8.4) losing different rules.

The config loader accepts the block only on a book of exactly one enabled trend sleeve
(`src/core/config_loader.cpp:512`). BASE, with two sleeves, carries no list. The list crosses from the
loader to the sleeve in one function, `hand_over_trading_rule_removals`
(`include/trade_ngin/strategy/sleeve_config.hpp:58`), which all four futures runners call. One
`TRADING_RULE_REMOVALS` line per listed contract is logged when the sleeve initialises
(`trend_following.cpp:183`).

Where the list comes from. It is the book's procedure (strategy nine, "Removing expensive trading rules"): a
rule is removed from an instrument when the rule's yearly cost on it is over 0.15 Sharpe-ratio units, where

```
yearly cost     = (turnover of the rule + 2 x rolls a year) x cost per trade
cost per trade  = cost of one contract / (price x multiplier) / sigma
```

The turnovers are the book's (table 35: 98.5, 50.2, 25.4, 13.2, 7.6, 5.2 for the six pairs, the same for
every contract). The cost inputs are ours: the engine's own cost of one contract at the signal close (the
fee per contract plus spread and impact, see [COST_MODEL.md](COST_MODEL.md)), and its sizing
volatility, whose ratio is taken as the median over the signal days of the sixteen-year run the list is computed from, and the
rolls the engine confirmed on the contract's series in that run, over the series' span in years.
`scripts/trading_rule_costs.py` shows the arithmetic and, with `--check`, reports whether the costs of a
new run of that kind still give the committed list. The committed list is the authority; the script never writes it.

### 3.6 The equity slow rule

On the book's first sleeve only, for the symbols `portfolio.json` `equity_slow_rule.symbols` names (M2K,
MES, MNQ, MYM): a negative combined forecast stands only when the scaled forecasts of both slow pairs,
32/128 and 64/256, are negative. Otherwise it is set to 0. A forecast that is not negative is never touched
(`equity_slow_ruled`, `trend_estimator.cpp:66`; applied at `trend_following.cpp:542`).

The ruled forecast is the forecast from there on: it sizes the target, its sign drives the sign close and
the deferral band, and it is the value stored in `trading.signals.signal_value`.

A held equity-index short whose forecast the rule sets to 0 is not closed by rule. A zero forecast has no
sign, so the symbol is a free row with a target of 0 that may only step toward zero; the short leaves the
book as the search and the buffer take it.

On BASE the fast sleeve carries neither slow pair and is not ruled (`apps/backtest/bt_portfolio.cpp:401`).
A predecessor contract of a listing-date pair (ES for MES) is ruled through its listed contract's root
(`ListingDates::pair_root`, `include/trade_ngin/data/listing_dates.hpp:151`).

### 3.7 Worked forecasts

An example of the arithmetic, from a replay of dated live runs of the conservative book, one run a day in
date order, on its template configuration, which carries the listing-date, relabel and rule-removal blocks:
run date 2026-04-30, signal bar 2026-04-29. The per-pair figures are from the run's estimator record, a diagnostic file the
sleeve writes when `TRADE_NGIN_SERIES_DUMP_DIR` is set (`trend_estimator_record.hpp`). The combined forecast
in the last column equals the stored `trading.signals.signal_value` of that run date.

| Symbol | 2/8 | 4/16 | 8/32 | 16/64 | 32/128 | 64/256 | Pairs | Mean | Multiplier | Combined F |
|---|---|---|---|---|---|---|---|---|---|---|
| MES | 7.512 | 13.665 | 12.832 | 6.412 | 2.420 | 3.045 | 6 | 7.648 | 1.26 | 9.636 |
| MBT | -8.605 | 3.367 | 11.016 | 5.381 | -9.629 | -17.790 | 6 | -2.710 | 1.26 | -3.415 |
| ZW | not run | not run | 3.505 | 4.663 | 3.458 | -0.696 | 4 | 2.733 | 1.13 | 3.088 |
| ZR | not run | not run | not run | -6.124 | -9.420 | -18.844 | 3 | -11.462 | 1.08 | -12.379 |

Check for ZW: (3.505 + 4.663 + 3.458 - 0.696) / 4 = 2.733; 2.733 x 1.13 = 3.088. MES is a ruled symbol; its
forecast is positive, so the equity slow rule leaves it alone.

---

## 4. The sizing capital

### 4.1 The rule

The book is sized on Carver's half compounding (the book, Tactic three, "Half compounding"): the starting
capital less the current drawdown of the settled P&L, never above the starting capital. Losses come off the
sizing capital at once. A profit rebuilds it up to the start. Profit above the start is set aside and never
sized on.

With C the cumulative settled net P&L in date order (0 at the start) and P its running peak floored at 0:

```
E = S_0 - (P - C)          account V = S_0 + C
```

Row by row this is `E' = min(S_0, E + net)`. It is a function of the settled history alone
(`half_compounded_capital`, `sizing_capital.hpp:58`). A day's net is its P&L less the day's
costs after netting: each fill's own cost minus its `netting_adjustment` (see
[COST_MODEL.md](COST_MODEL.md)). On a one-sleeve book every adjustment is 0.

E is the capital of every capital-terms quantity: each sleeve's capital, the weight per contract u, the
overlay's weights and leverage, the per-name cap, and the cost term of the search.
`set_sizing_capital` hands it to every strategy (times its allocation) and every risk module in one call; a
refusal from any of them stops the rebalance.

No column persists E. Every run recomputes it:

| Engine | Settled history | Code |
|---|---|---|
| Backtest | the run's own equity curve up to its last row, which is the previous cycle's close. Warm-up rows are flat, so warm-up sizes on S_0 | `backtest_half_compounding`, `sizing_capital.hpp:78`; `src/backtest/backtest_coordinator.cpp:954` |
| Live | the book's stored `trading.live_results.daily_pnl` rows before Day T-1, then Day T-1's own net rebuilt before the rebalance | `read_live_sizing_equity`, `live_sizing_read.hpp:146`; `apps/strategies/live_portfolio_conservative.cpp:1477` |

The live run sizes before it finalises Day T-1, so it rebuilds Day T-1's net from the parts the
finalize will use: the T-1 settlement move of the stored T-1 book on the consumed bars
(`live_sizing_equity`, `sizing_capital.hpp:119`), less the costs stored on Day T-1's row. After the
finalize, a `SIZING_CAPITAL_CHECK` line compares the finalized `daily_pnl` with the rebuilt net.

### 4.2 Worked table: a loss, the recovery, a gain above the start, a loss again

An example of the arithmetic, from the same run as section 3.7 (a replay of dated live runs of the
conservative book on its template configuration, with the listing-date, relabel and rule-removal blocks),
run dates 2026-04-24 to 2026-05-02. `E` and `V` are the stored `risk_detail.sizing_capital` and
`risk_detail.account_value` of each run date's `trading.live_results` row. "Net added" is the stored
`daily_pnl` of the day or days that settled since the previous sized run. C is V minus 500,000. The first
row's peak is not stored; it is derived from the two stored keys as P = (S_0 - E) + C.

| Run date | Net added (day settled) | C | Peak P | E = 500,000 - (P - C) | V = 500,000 + C | What happened |
|---|---|---|---|---|---|---|
| 2026-04-24 | | -2,559.13 | 2,254.73 | 495,186.14 | 497,440.87 | in drawdown: the account once stood 2,254.73 above the start |
| 2026-04-25 | -201.48 (04-24) | -2,760.60 | 2,254.73 | 494,984.67 | 497,239.40 | a loss comes off at once |
| 2026-04-27 | 0.00 (04-25), +234.38 (04-26) | -2,526.23 | 2,254.73 | 495,219.04 | 497,473.77 | a gain rebuilds it |
| 2026-04-28 | +6,139.19 (04-27) | 3,612.96 | 3,612.96 | 500,000.00 | 503,612.96 | recovered: 495,219.04 + 6,139.19 = 501,358.23, capped at the start; a new peak |
| 2026-04-29 | +4,224.78 (04-28) | 7,837.74 | 7,837.74 | 500,000.00 | 507,837.74 | gain above the start: set aside, E does not move |
| 2026-04-30 | +3,168.22 (04-29) | 11,005.96 | 11,005.96 | 500,000.00 | 511,005.96 | the same |
| 2026-05-01 | -5,134.39 (04-30) | 5,871.57 | 11,005.96 | 494,865.61 | 505,871.57 | a loss from the peak comes off E, although the account is still above the start |
| 2026-05-02 | -572.06 (05-01) | 5,299.51 | 11,005.96 | 494,293.55 | 505,299.51 | a further loss |

The run of 2026-04-26 stores no `risk_detail`: its signal date, a Saturday, has no session bar, so no
rebalance is sized. The row of 2026-05-01 shows the point of the rule: the account is 5,871.57 above the
start and the book is sized on less than the start, because the drawdown is taken from the peak.

V minus E is the profit set aside (11,005.96 on 2026-05-01). It is derived from the two keys and is not
stored.

### 4.3 The failure-path rule

When the live sizing read finds Day T-1 on one of the finalize's failure paths, the run keeps the sizing
capital of the last settled day, adds nothing for the unsettled day, and logs `SIZING_CAPITAL_UNSETTLED`
(WARN) with the unsettled date, the reason and the capital used (`live_sizing_read.hpp:232` to `248`,
`:293`). The paths, tested with the finalize's own predicates:

| Reason logged | Condition |
|---|---|
| `no Day T-1 row` | the book has stored history and no `live_results` row for Day T-1 |
| `no T-1 closes` | the price manager's T-1 close map is empty and the book holds a position |
| `no T-2 closes` | the price manager's T-2 close map is empty |

The next run that finds the day settled recomputes E over the whole settled history in date order. Because
E is a closed form of that history, the late day lands in its own date's place and the result equals the
row-by-row recursion. Rows sized while a day was unsettled keep the capital they were sized on
(`risk_detail.sizing_capital`).

A stored day on which no symbol printed (a held Saturday) is never finalised; it counts at its stored
`daily_pnl` as soon as any later bar is loaded (`sizing_history_day_unsettled`, `live_sizing_read.hpp:133`).

A database error is a different case from "nothing stored" (`live_sizing_read.hpp:9` to `30`):

| Outcome | When | What the run does |
|---|---|---|
| `kHoldBook` | every sleeve's T-1 book loaded, but the Day T-1 row, the previous row or the P&L history could not be read | no rebalance and no order: every sleeve is held at its T-1 book, the day's `live_run_metadata.risk_refusal` is marked with scope `sizing`, exit code 3 |
| `kRefuseRun` | a sleeve's T-1 book could not be read | the run refuses to start, exit code 1, no row |

---

## 5. The unrounded target

```
N* = (F / 10) x (C_s x IDM x w x tau) / (M x P x FX x sigma)
```

`C_s` is the sleeve's capital, E times the sleeve's allocation. FX is 1 (every traded contract is in
dollars). A negative forecast gives a short target. The strategy publishes N* as it is: unbuffered,
unrounded and unclamped (`trend_following.cpp:642`). The buffer, the rounding and the per-name cap all
belong to the pass.

Guards in `calculate_position` (`trend_following.cpp:1046` to `1115`): a forecast that is not finite or is
beyond 20 in size sizes at 0; sigma is clamped to [0.01, 1.0]; tau to [0.01, 0.5]; the denominator is at
least 1. Also floored: the capital at 1,000, the IDM and FX at 0.1. A sigma or a price that is not positive
is replaced (the caller puts 0.2 for a sigma that is not a positive finite number, `trend_following.cpp:609`
to `614`, and the function itself 0.01; a bad price takes the last price of the symbol's history, or 1 when
there is none), and a result that is not finite is set to 0. None binds on this book.

### 5.1 Instrument weights and the IDM

`get_weights` (`trend_following.cpp:915`) builds the weights from the metadata `Sector` column. The
universe is every metadata symbol that has a row in `futures_data.ohlcv_1d` at any date, less ES
(`trend_following.cpp:926` to `937`, `:968`; the symbol list is read with no date filter,
`src/data/postgres_database.cpp:819` to `829`). If that symbol list comes back empty, every metadata symbol
is kept (`:968`). The weights are computed once per sleeve and cached (`:916`, `:1042`), so they are the
same on every date of a run. In a backtest that starts before a contract's first bar, the contract carries
its weight from the first day and the other weights are not rescaled for the dates it has no bar: MBT,
whose first bar is dated 2021-05-03, holds 0.071429 on every date of a window that starts in 2010.

The rule:

1. each sector gets an equal share, and each symbol an equal share of its sector;
2. a symbol is capped at half of its sector's share;
3. the weights are brought back to a sum of 1 by scaling only the symbols that were not capped.

With the 36 traded symbols in seven sectors:

| Sector | Symbols | Weight of each |
|---|---|---|
| Agriculture | GF, HE, KE, LE, ZC, ZL, ZM, ZR, ZS, ZW | 0.015476 |
| FX | 6A, 6B, 6C, 6E, 6J, 6L, 6M, 6N, 6S | 0.017196 |
| Energy | CL, HO, NG, RB | 0.038690 |
| Equities | M2K, MES, MNQ, MYM | 0.038690 |
| Interest Rates | UB, ZF, ZN, ZT | 0.038690 |
| Metals | GC, HG, PL, SI | 0.038690 |
| Crypto | MBT | 0.071429 |

Check: MBT is alone in its sector, so it is capped at half of 1/7, 0.071429. The other 35 are scaled by
(1 - 0.071429) / (6/7) = 1.083333: an agriculture symbol is (1/7) / 10 x 1.083333 = 0.015476. The sum is 1.

The two contracts of a listing-date pair read one weight: a predecessor takes its listed contract's
(`trend_following.cpp:574`).

The IDM is 2.5 on both books (the book, strategy four, table 16: the value for 30 or more instruments). It
is a required sleeve key with no default.

### 5.2 Worked targets

Same run as section 3.7 (signal bar 2026-04-29, E = 500,000, one sleeve at allocation 1, IDM 2.5,
tau 0.20). `sigma`, weight, multiplier and price are from the estimator record.

| Symbol | F | w | E x IDM x w x tau | M | P | sigma | M x P x sigma | N* |
|---|---|---|---|---|---|---|---|---|
| MES | 9.636 | 0.038690 | 9,672.62 | 5 | 7,192.50 | 0.20078 | 7,220.41 | 1.291 |
| MBT | -3.415 | 0.071429 | 17,857.14 | 0.1 | 76,035 | 0.43330 | 3,294.60 | -1.851 |
| ZW | 3.088 | 0.015476 | 3,869.05 | 50 | 653.00 | 0.29830 | 9,739.59 | 0.123 |
| ZR | -12.379 | 0.015476 | 3,869.05 | 2,000 | 10.92 | 0.17968 | 3,924.21 | -1.221 |

Check for MES: 0.9636 x 9,672.62 / 7,220.41 = 1.291. For ZW, sigma is 0.7 x 0.31336 + 0.3 x 0.26317 =
0.29830.

---

## 6. The overlay in brief

The module, its config and its refusal contract are in [RISK_MODULES.md](RISK_MODULES.md). What the loop
needs:

**What it reads.** The capped target book: every free symbol at `sign(N*) x min(abs(N*), L / u)`
(`cap_target`, `one_pass.cpp:35`), plus every held row at its held quantity. Capping first means the
overlay never reads a position the book cannot hold. The scalar is never computed from the optimiser's
answer.

**In capital terms.** Each position is a signed weight on the sizing capital, `x_i = N_i M_i P_i / E`. The
five readings (`overlay.hpp:60`):

| Reading | Formula | Limit |
|---|---|---|
| R, portfolio risk | sqrt(x' Sigma x), Sigma the annualised covariance of adjusted returns over the gate window | R_max = 2.25 tau = 0.45 |
| R_jump | sqrt(x' Sigma_jump x): the window's correlations on each symbol's jump volatility, the 99th percentile of its trailing 2,520 `sigma_short` values each taken back to a daily figure by its own bar's factor (`trend_estimator.cpp:302` to `308`), annualised by the gate window's factor (`one_pass.cpp:434` to `437`) | R_jump_max = 4.5 tau = 0.90 |
| R_shock | sum of abs(x_i) s_i, with s_i the symbol's own volatility in the gate window, sqrt(Sigma_ii) (every correlation taken as 1). It is not the sizing volatility of section 3.2 | R_shock_max = 4.0 tau = 0.80 |
| L_g, gross leverage | sum of abs(x_i) | 8.0 |
| L_n, net leverage | sum of x_i, signed; the limit applies to its size | 6.0 |

The gate window is the last 252 dates on which any participant has a return, ending at the signal date.
Below 21 complete dates the three covariance readings are blind (their multipliers are 1, `OVERLAY_BLIND`
is warned) and the two leverage readings still apply. The full window rules are in the comment at
`overlay.hpp:12`.

**Where the limits are.** The five limits are fixed values of the carver module in `risk.json`. The pass
multiplies the three risk limits by the first sleeve's tau and takes the two leverage limits as they are
(`portfolio_manager.cpp:2223` to `2226`), so a change of tau moves the three risk limits with it and leaves
the leverage limits and the per-name cap where the file puts them. The book sets the limits of its own
overlay as the 99th percentile of each reading's own history, rounded (Tactic four, "An exogenous risk
overlay"); section 13 gives its values beside ours.

**One scalar, applied once.** `m` is the smallest of `min(1, limit / reading)` over the five terms
(`overlay::multiplier`, `src/risk/overlay.cpp:180`). The scaled target of every free symbol is
`m x capped target` (`one_pass.cpp:470`). Held rows are counted in every reading and are not scaled. The
binding term is the first term at m, or `none`.

**When it cannot answer.** If an input or a reading is not a finite number, the pass is refused
(`one_pass.cpp:456` to `467`): every sleeve keeps its held book, there is no search, no trim and no fill,
and a REFUSE decision is recorded. A blind window is not a refusal.

---

## 7. The one pass

### 7.1 The three kinds of row

Each symbol in the pass is classified once (`one_pass.cpp:393` to `414`):

| Kind | Condition | Treatment |
|---|---|---|
| Free | some sleeve signals it and it is not in the hold set | capped, scaled, searched, buffered, rounded, clipped, a trim candidate |
| Held (fixed) | it is not free and not a close-out, and it has a non-zero held position, or it is a signalling hold-set symbol at zero | fixed at its held quantity; counted in every reading; never scaled, traded or trimmed |
| Close-out | it has a bar, is not held, no sleeve signals it any more, some sleeve signalled it before, and it holds a position | closed to flat with one fill |

The hold set is section 8. The deferral band (7.2) adds to it.

### 7.2 The deferral band and the forecast-sign close

Before the search, a free symbol whose held position is on the other side of its target's sign is closed
to flat with one fill at the signal close (`one_pass.cpp:475` to `482`). The sign is the sign of the uncapped N*,
summed over the sleeves that publish. A target of exactly zero has no sign and closes nothing. The search,
its cost term and the buffer all start from the closed book.

The deferral band postpones that close. A symbol the first sleeve signals, whose held position is opposite
to that sleeve's forecast while `abs(F) < 2`, is held that day (`one_pass.cpp:399`): it joins the hold set,
is fixed at its held quantity and gets no fill. The test is strict: at `abs(F) = 2` the symbol is free and
the close applies. A zero forecast is never in the band. On BASE only the first sleeve's forecast is read;
the fast sleeve's never enters the band.

A change bar wins over both: a symbol on a change bar is held (section 8).

### 7.3 The search from the held book

The optimiser's covariance is its own (`optimiser_covariance`, `one_pass.cpp:61`): the sample covariance of
the free symbols' adjusted returns over their last 756 consumed closes, on the dates they share, annualised
by the count of those dates. A symbol whose last close trails the newest by more than five dates, or that
leaves fewer than 20 shared returns, gets a 0.01 variance on its own and is named in a
`COVARIANCE_STALE_PARTICIPANT` or `COVARIANCE_FLOOR_DROP` warning.

The search minimises, over whole-contract books n:

```
TE(n) = sqrt((n u - x)' Sigma (n u - x)) + 100 x sum_i abs(n_i - h_i) x c_i / E
```

`x` is the scaled target in weights, `h` the held book after the sign closes, and `c_i` the cost of trading
one contract at the signal close as the cost model prices it (`portfolio_manager.cpp:2132`). A symbol the
cost model has no usable volume for has no cost; it stays in the pass as a held row and is logged
`BOOK_UNPRICED`, never traded on a default.

How it runs (`search`, `one_pass.cpp:170`):

- It starts from the held book, not from zero.
- Each pass tries one contract up and one contract down in every free symbol, in symbol order.
- It keeps the single step with the lowest TE (ties go to the earlier candidate) and takes it only if it
  lowers TE by more than 1e-6.
- **The sign guard** (`admissible`, `one_pass.cpp:156`): a step is allowed when the result is on the
  target's side or at zero, or when it moves toward zero without crossing it. A symbol whose target is
  exactly zero may only step toward zero.
- **The cap guard**: a step away from zero that lands above the per-name cap is refused. A step toward zero
  never is.
- The pass cap is the larger of 100 and twice the contracts between the held book and the target, plus one
  (`pass_cap`, `one_pass.cpp:162`). Reaching it logs `OPTIMISER_PASS_CAP`.

The cost term is why the starting point matters: a position already held costs nothing to keep, so the
search moves a row only when the tracking error it removes is worth more than 100 times the trade's cost.

### 7.4 The granularity buffer, the rounding and the clip

The no-trade band is sized by the book's own contracts (`b_sigma`, `one_pass.cpp:231`):

```
B = max(0.05 x tau, largest u_i x sqrt(Sigma_ii) over the free symbols with a non-zero answer or holding)
```

The second term is the annual risk of one contract of the lumpiest symbol in the book, as a share of the
sizing capital. The first is the floor, 0.01 at tau 0.20.

Then (`buffer`, `one_pass.cpp:253`), with y the search's answer:

```
TE_h = sqrt(d' Sigma d),  d_i = (h_i - y_i) u_i        no cost term
if TE_h <= B: no trade, the book is h
else:         a = (TE_h - B) / TE_h,  book = round(h + a x (y - h))
```

The book trades toward the answer only as far as the edge of the band. The rounding is to the nearest whole
contract, a half away from zero (`round_half_away`, `one_pass.cpp:227`). It is the only rounding in the
loop. When the rounding gives the held book back, the OPTIMISER line says so.

After the rounding, every free row beyond the per-name cap is clipped toward zero to
`floor(L / u)` contracts (`clip_to_cap`, `one_pass.cpp:48`). A held row beyond the cap is not touched; it
is marked instead (7.5).

So the per-name cap acts in three places: on the target the overlay reads, as a guard in the search, and
as a clip on the rounded book.

### 7.5 The trim

The rounded, clipped book is read once more by the overlay (`one_pass.cpp:555` to `587`). If a reading is
over its limit, `trim` removes one contract at a time (`one_pass.cpp:305`):

- the first breached term in the order R, R_jump, R_shock, L_g, L_n is served first;
- the contract removed is the one, among the free non-zero rows, whose removal lowers that reading most,
  toward zero on a short as on a long;
- the book is re-read after each removal; at most five contracts in a day; a removal that lowers nothing
  stops it.

What is still over a limit afterwards is stored as it is and marked: `over_limit_after_rounding_terms` and
the excess in units of the largest stored weight per contract, with one `RISK_TRIM` warning. When held rows
alone keep a reading over, or a held row is beyond the cap (term `CAP`), the mark is
`over_limit_by_hold_terms` with the held rows named, and a `RISK_OVER_LIMIT_BY_HOLD` warning. Held rows are
never cut. There is never a second scalar.

---

## 8. Holds and the listing-date switch

### 8.1 The hold set

A held symbol is fixed at its held quantity for the rebalance: no fill, counted in every reading, never a
trim candidate. With a held quantity of zero it is not opened. It re-enters the search on its next usable
bar. The hold applies on sized days only; in warm-up the book follows the search
(`portfolio_manager.cpp:2260`).

| Hold | Rule | Where it is decided |
|---|---|---|
| Session classifier | a symbol whose T-1 verdict is not `SESSION` (`JUNK`, a closure, a feed hole) is held. The key is the verdict, never whether a price exists: a junk print has a price | `SessionVerdict`, `include/trade_ngin/data/session_classifier.hpp:39`; live: `live_portfolio_conservative.cpp:1790`; backtest: `backtest_coordinator.cpp:797` to `834` |
| Change bar | a symbol whose last consumed bar is pending (a change bar, either bar of a flip, a bar with no instrument id inside a pending roll) is held until its next consumed bar, on every rebalance | live: `CHANGE_BAR_HOLD`, `live_portfolio_conservative.cpp:1796` to `1800`; backtest: `backtest_coordinator.cpp:856`; see [FUTURES_ROLLS.md](FUTURES_ROLLS.md) |
| Deferral band | section 7.2 | `one_pass.cpp:399` |
| Cannot be weighed or priced | a held symbol with no series today is valued at its last usable close; a symbol with no cost is fixed | `BOOK_UNPRICED`, `portfolio_manager.cpp:2144` to `2210` |

The live runner hands its set to the pass through `PortfolioManager::set_hold_set`
(`live_portfolio_conservative.cpp:1817`). The backtest hands the set of session symbols, and every symbol
outside it is held.

### 8.2 Never-signalled symbols

A symbol publishes no target until its sleeve holds as many bars as its longest EMA (256 for the trend
sleeve, 64 for the fast one; `is_signalling`, `trend_following.cpp:849`). Such a symbol is not a free row.
At zero it is not a participant of the pass. With a held position it is a held row. It is not a close-out, because a
close-out needs an earlier signal (`one_pass.cpp:408`). A live process keeps no memory of earlier signals,
so there a held position stands for one (`portfolio_manager.cpp:2281`).

### 8.3 Close-outs

A symbol that signalled before and no longer does, or whose sleeve is stopped, is closed with one fill to
flat at the signal close on a session bar. It does not go through the buffer. On a non-session bar it is a
hold.

### 8.4 Listing dates and the switch

`portfolio.json` `listing_dates` declares contracts that trade only from a date, and the contract the book
trades before it. In force: MES, MNQ, MYM and M2K from 2019-05-06, with ES, NQ, YM and RTY before, at a
ratio of 10. The vendor stores the E-mini's history under the micro's symbol, so a backtest whose window
starts before the date runs the predecessor as a symbol of its own on those same bars
(`ListingDates`, `listing_dates.hpp:108`). The predecessor may hold a position only on signal bars dated
before the listing date, the listed contract only on or after it (`tradeable`, `listing_dates.hpp:137`;
read at `trend_following.cpp:401`). Each is costed on its own contract's size and fee. The pair counts as
one instrument for its weight, its removed rules and the equity slow rule.

The switch is made inside the pass of a backtest, once per pair, on the first sized rebalance whose signal bar is dated on
or after the listing date with both contracts free to trade and the listed contract signalling
(`portfolio_manager.cpp:2310` to `2566`). Under the rule in force, `open_at_target`
(`plan_listing_switch`, `src/data/listing_dates.cpp:251`):

1. the held predecessor is exited in full;
2. the listed contract is entered at that day's target after the overlay's scalar: the capped target times
   m, rounded to the nearest whole contract, and clipped to the cap. These are the pass's own `cap_target`,
   `round_half_away` and `clip_to_cap`, called, not copied;
3. the pass then runs from that held book.

The scalar is read from the pass itself, run once on the book with the due predecessors exited
(`portfolio_manager.cpp:2408`), so on a day the overlay cuts, the entry lands on the cut target.

The two moves are fills of type STRATEGY at the signal close, told apart by their ids: `LC-<sleeve>-<n>`
for the predecessor's exit and `LO-<sleeve>-<n>` for the listed contract's entry
(`make_listing_switch_fills`, `listing_dates.cpp:279`). On a book of several sleeves the switch is the
book's: the net holding is exited, the summed target entered, and the whole number is split and netted by
the pass's own rules (section 9).

Exceptions, all in the code cited above:

- a predecessor held inside the deferral band is carried instead: q predecessor contracts become exactly
  ratio x q listed contracts, and the pass holds them;
- if either contract is in the hold set that day, the switch waits, the predecessor stays held, and the
  listed contract is not opened beside it;
- if the pass is refused, the switch is undone and made on a later pass;
- a pair whose predecessor never traded in the run has no switch.

A live run never trades a predecessor. With contracts declared it refuses, before anything of the day is
stored, a predecessor in its universe or its stored book on or after the listing date, and a signal date
before the listing date of a listed contract in its universe (`live_listing_refusals`, `include/trade_ngin/live/live_listing_guard.hpp:30`).

Worked switch, an example of the arithmetic: a sixteen-year backtest of the conservative book (2010-10-07
to 2026-10-07) on its template configuration, which carries the listing-date, relabel and rule-removal
blocks, the cycle of 2019-05-07 on the signal bar of 2019-05-06. The overlay's scalar is 1 that day.
Targets are from the run's `LISTING_LEG` log lines; fills are that run's `backtest.executions` rows.

| Pair | Predecessor held | Listed contract's scaled target | Rounded entry | `LC-` fill | `LO-` fill | Fill price |
|---|---|---|---|---|---|---|
| ES to MES | 0 | 4.484 | 4 | none | BUY 4 MES | 2,918.00 |
| NQ to MNQ | 1 | 4.788 | 5 | SELL 1 NQ | BUY 5 MNQ | 7,762.75 |
| YM to MYM | 1 | 1.899 | 2 | SELL 1 YM | BUY 2 MYM | 26,281.00 |
| RTY to M2K | 0 | 3.526 | 4 | none | BUY 4 M2K | 1,609.60 |

The commission on each fill is the contract's own fee: 2.247 on the one NQ, 5 x 0.614 = 3.07 on the five
MNQ.

### 8.5 The relabel list

`portfolio.json` `instrument_id_relabels` declares vendor instrument-id changes that are not rolls (four
entries dated 2026-02-22, one per equity index micro). From the date, a bar carrying the new id is read as
carrying the old one, at the loader (`InstrumentIdRelabel`, `listing_dates.hpp:48`). No consumer sees a
change: no change bar, no hold, no roll legs.

---

## 9. Sleeves on BASE

BASE is a placeholder test book with two sleeves: the trend sleeve at allocation 0.7 and tau 0.20, and the
fast sleeve at 0.3 and tau 0.25. How the loop treats several sleeves:

- **Sized on capital times allocation.** Each sleeve's N* is sized on E times its allocation
  (`portfolio_manager.cpp:3829`), so its contracts are already contracts of the account's book.
- **Aggregation.** The book's target for a symbol is the sum of the sleeves' N*, with no allocation applied
  a second time (`portfolio_manager.cpp:2274` to `2278`). The held book is the sum of the sleeves' held
  quantities.
- **One pass on the book.** The overlay, the search, the buffer and the trim run once on the summed book.
  The book-level tau, the three risk ratios, the deferral band and the equity slow rule are the first
  sleeve's.
- **The split.** Each moved row is split back in proportion to the sleeves' unrounded contributions by
  largest remainder, so the sleeves' whole numbers sum to the book's exactly
  (`distribute_optimizer_contracts`, `allocation_split.hpp:51`). When the sleeves' targets have opposite
  signs, only the optimiser's deviation from the net target is split, so each sleeve keeps its own side.
- **Netting.** Each sleeve's fill is priced as if it traded alone; the account sends one order per symbol,
  and each row's `netting_adjustment` is its share of what the account did not pay. Every cost total (a
  day's costs, a run's costs, the costs the sizing capital reads) is the fills' own costs minus their
  adjustments. In the backtest the
  sign closes are netted among themselves and the other fills among themselves
  (`portfolio_manager.cpp:2860`). Roll legs are never netted. See [COST_MODEL.md](COST_MODEL.md).
- **A stopped sleeve** signals nothing, so its held rows close on the next session bar. A stopped first
  sleeve leaves nothing to weigh the book on and refuses the pass.

---

## 10. What is stored and what is logged

### 10.1 Stored each day

| Where | What |
|---|---|
| `trading.live_results.risk_scale` | the delivered scale: the stored book's gross weight over the gross weight of the capped target the overlay read, held rows counted in both, at the raw signal closes. It can exceed 1. A day with no sized rebalance, and a flat target, store 1. It is not the request (`delivered_scale_for_book`, `portfolio_manager.cpp:3047`; written at `live_portfolio_conservative.cpp:4052`) |
| `trading.live_results.risk_detail` and `backtest.equity_curve.risk_detail` (jsonb, migration 020) | the loop's record of the rebalance, nine flat keys. Written on the row of a sized rebalance the overlay answered; NULL on a refused rebalance, a day with no session, a warm-up row and every equity row (`risk_detail_json`, `risk_detail.hpp:64`) |
| `trading.signals.signal_value` | the ruled forecast F of each symbol |
| `trading.positions`, `trading.executions` | each sleeve's stored book and fills. A live forecast-sign close is its own fill with the id suffix `_SC` (`session_book_gate.hpp:421`), stored ahead of the symbol's other fill of the day |
| `backtest.executions` | the same fills with ids `EX-<sleeve>-<n>`, and the switch fills `LC-` and `LO-` |

The nine keys of `risk_detail` (`kRiskDetailKeys`, `risk_detail.hpp:48`):

| Key | Type | Meaning |
|---|---|---|
| `risk_requested` | number | m, 1.0 with no cut |
| `binding_term` | text | `R`, `R_jump`, `R_shock`, `L_g`, `L_n` or `none` |
| `over_limit_after_rounding_terms` | text or null | the terms still over their limits after the trim, separated by `;` |
| `over_limit_after_rounding_excess` | number or null | the largest of those excesses, in units of the largest non-zero stored weight per contract |
| `over_limit_by_hold_terms` | text or null | the terms held rows keep over; `CAP` for a held row beyond the cap |
| `over_limit_by_hold_symbols` | text or null | those held rows, sorted, separated by spaces |
| `overlay_blind` | boolean | the gate window had fewer than 21 complete dates |
| `sizing_capital` | number | E, the capital the book was sized on |
| `account_value` | number | V at the same instant: S_0 plus the cumulative settled net P&L that E was built from |

The five readings, their limits and their multipliers are not stored. They are on the OVERLAY
line. The sizing capital has no column of its own.

### 10.2 The design keys in `portfolio_config`

Every run records the constants it ran with. `PortfolioConfig::to_json`
(`include/trade_ngin/portfolio/portfolio_manager.hpp:111`) is written to
`backtest.run_metadata.portfolio_config` and to `trading.live_run_metadata.portfolio_config`:

| Key | Value in force |
|---|---|
| `total_capital` | 500,000, the starting capital (never the day's sizing capital) |
| `opt_config.cost_penalty_scalar` | 100 |
| `opt_config.sign_close_band` | 2 |
| `opt_config.b_sigma_floor` | 0.05 |
| `opt_config.max_iterations` | 100 |
| `risk_config.R_max`, `R_jump_max`, `R_shock_max` | 2.25, 4.5, 4.0 (ratios to tau) |
| `risk_config.max_gross_leverage`, `max_net_leverage` | 8.0, 6.0 |
| `risk_config.per_name_cap` | 2 |
| `risk_config.trim_max` | 5 |
| `sizing_mode` | `half_compounding` |
| `equity_slow_rule` | the symbols and the pairs |
| `oracle_hash_list` | a fixed string that the code writes on the record of every book that names an overlay sleeve (`portfolio_manager.hpp:153`). No code reads it and no config key sets it |

The backtest adds `strategy_names`, `strategy_allocations` and, when the block is present,
`trading_rule_removals` (`bt_portfolio_conservative.cpp:566`). Each sleeve's tau and IDM are recorded with
the sleeve's own config, not here.

### 10.3 Logged

One of each per rebalance, in this order. A refused rebalance prints the OVERLAY line only.

| Line | Carries | Built by |
|---|---|---|
| `SIZING_CAPITAL` | live: capital, account, peak, the last settled date, the count of settled rows, the count of earlier unsettled days, then the parts Day T-1 was rebuilt from (the row before, the settlement, the costs, positions priced and unpriced). Backtest: capital, account, peak, `settled_through` | `sizing_capital_log_line`, `live_sizing_read.hpp:278`; `backtest_coordinator.cpp:962` |
| `OVERLAY` | m, the binding term, the five readings (the three risk readings print `blind` on a blind window), the five limits, the five multipliers, capital, tau, the window (mode, dates, first, last, complete and dropped dates), the counts of participants, free and held rows, and the symbols left out of a reading | `overlay_line`, `one_pass_log.hpp:48` |
| `OPTIMISER` | the search's objective, its passes, TE_h, the band B and the symbol that sets it (`floor` when the floor governs), a, whether it traded, whether the rounding returned the held book, and the lists of sign closes, cap clips, close-outs and stale rows | `optimiser_line`, `one_pass_log.hpp:94` |
| `BOOK` | gross weights of the raw, capped and scaled targets and of the held and stored books, the stored net, the count of held rows, the deferral-band holds, the symbols the equity slow rule zeroed, and every participant as `symbol:N*:scaled:held:stored` | `book_line`, `one_pass_log.hpp:117` |

Beside them: `T4_RISK_WINDOW`, `COVARIANCE_DATE_ALIGNED`, `COVARIANCE_MAX_RHO` and
`OPTIMIZER_NOT_SIGNALLING` (info), and the warnings `SIZING_CAPITAL_UNSETTLED`, `SIZING_CAPITAL_HISTORY`,
`OVERLAY_BLIND`, `OPTIMISER_PASS_CAP`, `RISK_TRIM`, `RISK_OVER_LIMIT_BY_HOLD`, `BOOK_UNPRICED`,
`COVARIANCE_STALE_PARTICIPANT` and `COVARIANCE_FLOOR_DROP`. Listing switches log `LISTING_LEG` and
`LISTING_SWITCH`.

With `TRADE_NGIN_SERIES_DUMP_DIR` set, a run also writes one row a day and one row a symbol with every
value of the pass at seventeen digits (`one_pass_record.hpp:16`). A production run never sets it.

---

## 11. One rebalance, end to end

An example of the arithmetic, from the same run as section 3.7 (a replay of dated live runs of the
conservative book, one run a day in date order, on its template configuration, which carries the
listing-date, relabel and rule-removal blocks): run date 2026-04-30, signal bar 2026-04-29. Stored rows are
`trading.live_results`, `trading.positions`, `trading.executions` and `trading.signals` of that run. Figures that no stored row carries are from the
run's log lines and its one-pass record, and are marked so.

### Step 1: sizing capital

| Quantity | Value | Source |
|---|---|---|
| Account value V | 511,005.96 | `risk_detail.account_value` |
| Peak of cumulative settled P&L | 11,005.96 | `SIZING_CAPITAL` line |
| Sizing capital E | 500,000.00 | `risk_detail.sizing_capital` |
| Day T-1 net rebuilt | 3,202.06 settlement - 33.84 costs = 3,168.22 | `SIZING_CAPITAL` line; equals the stored `daily_pnl` of 2026-04-29 |

The account is at its peak, so the drawdown is 0 and E is the start: 500,000 - (11,005.96 - 11,005.96).

### Step 2: holds

All 36 symbols have a `SESSION` verdict for 2026-04-29. One symbol is in the hold set: HG, whose last
consumed bar is a pending contract change (`CHANGE_BAR_HOLD HG.v.0 ... kind=pending_change`). It holds 0, so
it stays at 0 and is not opened, although its forecast is +3.69. No symbol is in the deferral band.

### Step 3: the book, row by row

F is the stored signal. N* and u are from the one-pass record (N* is checked for four symbols in section
5.2; u = M x P / 500,000). This table carries every row through the later steps. The overlay's scalar is 1 (step 4), and no target is
beyond the cap, so the scaled target equals N*. The column `h + a (y - h)` uses a = 0.6145 from step 6, and h after the sign close.

| Symbol | F | N* | u | Held | Row | Scaled target | Search answer y | h + a (y - h) | Stored | Fill |
|---|---|---|---|---|---|---|---|---|---|---|
| 6A | 7.00 | 0.422 | 0.1424 | 0 | free | 0.422 | 0 | 0.000 | 0 | |
| 6B | 2.05 | 0.147 | 0.1686 | 0 | free | 0.147 | 0 | 0.000 | 0 | |
| 6C | 4.67 | 0.554 | 0.1466 | 0 | free | 0.554 | 0 | 0.000 | 0 | |
| 6E | -2.91 | -0.133 | 0.2927 | 0 | free | -0.133 | 0 | 0.000 | 0 | |
| 6J | -20.00 | -1.773 | 0.1566 | -1 | free | -1.773 | -2 | -1.615 | -2 | -1 |
| 6L | 20.00 | 4.010 | 0.0401 | 2 | free | 4.010 | 2 | 2.000 | 2 | |
| 6M | 8.78 | 1.245 | 0.0568 | 1 | free | 1.245 | 1 | 1.000 | 1 | |
| 6N | -0.94 | -0.066 | 0.1170 | 0 | free | -0.066 | 0 | 0.000 | 0 | |
| 6S | -5.40 | -0.207 | 0.3177 | 0 | free | -0.207 | 0 | 0.000 | 0 | |
| CL | 10.77 | 0.130 | 0.2159 | 0 | free | 0.130 | 0 | 0.000 | 0 | |
| GC | -0.38 | -0.003 | 0.9148 | 0 | free | -0.003 | 0 | 0.000 | 0 | |
| GF | 16.52 | 0.227 | 0.3724 | 0 | free | 0.227 | 0 | 0.000 | 0 | |
| HE | -6.44 | -0.309 | 0.0830 | 0 | free | -0.309 | 0 | 0.000 | 0 | |
| HG | 3.69 | 0.090 | 0.2972 | 0 | held | held at 0 | | | 0 | |
| HO | 4.60 | 0.039 | 0.3458 | 0 | free | 0.039 | 0 | 0.000 | 0 | |
| KE | 6.95 | 0.264 | 0.0704 | 1 | free | 0.264 | 1 | 1.000 | 1 | |
| LE | 17.58 | 0.485 | 0.2041 | 1 | free | 0.485 | 1 | 1.000 | 1 | |
| M2K | 8.21 | 2.492 | 0.0275 | 2 | free | 2.492 | 2 | 2.000 | 2 | |
| MBT | -3.41 | -1.851 | 0.0152 | 1 | free, sign closed | -1.851 | -2 | -1.229 | -1 | -1 (sign close), -1 |
| MES | 9.64 | 1.291 | 0.0719 | 1 | free | 1.291 | 2 | 1.615 | 2 | +1 |
| MNQ | 10.50 | 0.683 | 0.1102 | 1 | free | 0.683 | 1 | 1.000 | 1 | |
| MYM | 7.14 | 1.562 | 0.0489 | 1 | free | 1.562 | 1 | 1.000 | 1 | |
| NG | -9.34 | -0.781 | 0.0528 | 0 | free | -0.781 | -1 | -0.615 | -1 | -1 |
| PL | -0.62 | -0.014 | 0.1900 | 0 | free | -0.014 | 0 | 0.000 | 0 | |
| RB | 8.71 | 0.091 | 0.3024 | 0 | free | 0.091 | 0 | 0.000 | 0 | |
| SI | -2.12 | -0.010 | 0.7171 | 0 | free | -0.010 | 0 | 0.000 | 0 | |
| UB | -13.76 | -1.052 | 0.2297 | 0 | free | -1.052 | -2 | -1.229 | -1 | -1 |
| ZC | -0.65 | -0.060 | 0.0478 | 0 | free | -0.060 | 0 | 0.000 | 0 | |
| ZF | -13.27 | -3.546 | 0.2153 | -3 | free | -3.546 | -3 | -3.000 | -3 | |
| ZL | 16.13 | 0.540 | 0.0891 | 1 | free | 0.540 | 1 | 1.000 | 1 | |
| ZM | 10.20 | 0.527 | 0.0648 | 1 | free | 0.527 | 1 | 1.000 | 1 | |
| ZN | -10.18 | -1.681 | 0.2206 | 0 | free | -1.681 | 0 | 0.000 | 0 | |
| ZR | -12.38 | -1.221 | 0.0437 | 0 | free | -1.221 | 0 | 0.000 | 0 | |
| ZS | 5.28 | 0.223 | 0.1198 | 1 | free | 0.223 | 0 | 0.385 | 0 | -1 |
| ZT | -11.42 | -2.807 | 0.4139 | -1 | free | -2.807 | -3 | -2.229 | -2 | -1 |
| ZW | 3.09 | 0.123 | 0.0653 | 1 | free | 0.123 | 0 | 0.385 | 0 | -1 |

The "Held" column equals the stored `trading.positions` of 2026-04-29 and the "Stored" column those of
2026-04-30.

### Step 4: the overlay

Readings of the capped target, from the `OVERLAY` line. L_g and L_n are recomputed from the table above
(the sum of abs(N* x u) and of N* x u over the 35 free rows; HG adds 0) and agree to three decimals (the
table's columns are rounded).

| Term | Reading | Limit | min(1, limit / reading) |
|---|---|---|---|
| R | 0.1059 | 0.45 | 1 |
| R_jump | 0.3007 | 0.90 | 1 |
| R_shock | 0.3199 | 0.80 | 1 |
| L_g | 4.1905 | 8.0 | 1 |
| L_n | -1.9934 (size 1.9934) | 6.0 | 1 |

m = 1, binding term `none`. The window is complete: 252 dates from 2025-07-10 to 2026-04-29, of which 185
are complete; 36 participants, 35 free and 1 held.

### Step 5: the forecast-sign close

MBT holds +1 with a forecast of -3.41. That is beyond the band of 2, so it is not deferred: it is closed to
flat with one fill before the search. The search starts from MBT at 0.

### Step 6: the search and the buffer

From the `OPTIMISER` line: 11 passes, no pass cap; the search's answer is the column y.

| Quantity | Value |
|---|---|
| TE_h, the held book against the answer | 0.09137 |
| B, the band | 0.03522, set by NG (one NG contract: u 0.0528 times an annual volatility of about 0.668) |
| Floor of the band, 0.05 x 0.20 | 0.01 |
| a = (TE_h - B) / TE_h | (0.09137 - 0.03522) / 0.09137 = 0.6145 |

TE_h is above the band, so the book trades 0.6145 of the way from the held book to the answer and rounds.
Three rows show the effect:

- MES: 1 + 0.6145 x (2 - 1) = 1.615, stored 2.
- UB: 0 + 0.6145 x (-2 - 0) = -1.229, stored -1. The answer was -2; the band delivers one.
- ZS: 1 + 0.6145 x (0 - 1) = 0.385, stored 0.

The search answers for the book, not row by row: ZN has a target of -1.681 and stays at 0, while ZT and UB
each go beyond their own targets.

### Step 7: clip and trim

No row is beyond the cap (the largest stored weight is ZT, 2 x 0.4139 = 0.83, against 2). The re-read of
the stored book is inside every limit, so the trim removes nothing: stored L_g 3.0076, stored L_n -1.1612.

### Step 8: what is stored

| Stored | Value | Check |
|---|---|---|
| `risk_scale` | 0.7177 | stored gross 3.0076 / capped target gross 4.1905 = 0.7177 |
| `gross_notional` | 1,503,789.19 | 3.007578375 x 500,000 |
| `net_notional` | -580,612.19 | -1.161224375 x 500,000 |
| `active_positions` | 16 | the non-zero rows of the Stored column |
| `daily_transaction_costs` | 75.14 | the sum of the nine fills below |
| `risk_detail` | `risk_requested` 1.0, `binding_term` `none`, `overlay_blind` false, `sizing_capital` 500000.0, `account_value` 511005.961068, the four over-limit keys null | |

The nine fills, all at the 2026-04-29 close and dated 2026-04-30 (`trading.executions`):

| `exec_id` | Side | Quantity | Price | Cost |
|---|---|---|---|---|
| `EXEC_6J.v.0_20260430` | SELL | 1 | 0.006264 | 4.87 |
| `EXEC_MBT.v.0_20260430_SC` | SELL | 1 | 76,035 | 2.46 |
| `EXEC_MBT.v.0_20260430` | SELL | 1 | 76,035 | 2.46 |
| `EXEC_MES.v.0_20260430` | BUY | 1 | 7,192.50 | 1.34 |
| `EXEC_NG.v.0_20260430` | SELL | 1 | 2.638 | 15.51 |
| `EXEC_UB.v.0_20260430` | SELL | 1 | 114.84375 | 10.00 |
| `EXEC_ZS.v.0_20260430` | SELL | 1 | 1,197.75 | 15.20 |
| `EXEC_ZT.v.0_20260430` | SELL | 1 | 103.46875 | 3.65 |
| `EXEC_ZW.v.0_20260430` | SELL | 1 | 653.00 | 19.65 |

MBT has two fills: the sign close to flat, then the move to -1. Every `netting_adjustment` is 0: the book
has one sleeve.

---

## 12. Account size

The design is built for a 500,000 book. No other account size is proposed.

A book of this size is path-dependent because it holds whole contracts. One contract of many of the 36
instruments is a large share of the book's risk, so a target can be well below one contract (section 11
has such rows) and the book holds the contracts that fit. The no-trade band of section 7.4 is sized by the
risk of one contract of the lumpiest symbol in the book, and the search of section 7.3 starts from the held
book. So when a position opens, changes or closes depends on what the book already holds, on the sizing
capital of the day and on the prices of the day, and two runs that start on different dates can hold
different books on the same day.

---

## 13. Where the loop departs from Carver

Each row carries one of three labels, the same as in
[TREND_FOLLOWING_SYSTEM.md](TREND_FOLLOWING_SYSTEM.md). CARVER AS WRITTEN: the engine does what the book
describes, with the book's own numbers. CARVER, ADAPTED: the book's method with a stated difference. OURS:
the book has no such rule. "Not adopted" marks a rule of the book the engine does not use. Departures that concern rolls and the adjusted
series are in [FUTURES_ROLLS.md](FUTURES_ROLLS.md), and those of the cost model in
[COST_MODEL.md](COST_MODEL.md).

| Topic | The book | The engine | Why | Kind |
|---|---|---|---|---|
| Forecast scalars | fixed per rule (strategy nine, table 29) | the same six values | | CARVER AS WRITTEN |
| Forecast weights and multiplier | equal weights and a multiplier by the set of rules (strategy nine, table 36) | the same table, read by the number of pairs (BASE's fast sleeve reads the four-pair row for its four fastest pairs) | | CARVER AS WRITTEN |
| Removing rules by cost | a rule is dropped from an instrument when its cost is over 0.15 Sharpe-ratio units (strategy nine, "Removing expensive trading rules"; turnovers of table 35) | the same procedure, limit and turnover table, with our cost inputs: the engine's cost of one contract, its sizing volatility, its confirmed rolls | the costs must be the ones this engine charges | CARVER, ADAPTED |
| Asymmetric rules | advises against asymmetric adjustments to a forecast (strategy twelve) | the equity slow rule: an equity index short needs both slow pairs negative | a short against an equity index is taken only when the slow trend agrees: a short on the fast speeds alone against a slow uptrend is a bet against the drift the slow speeds measure | OURS |
| Volatility estimate | EWMA standard deviation of span 32, centred on its EWMA mean, blended 0.7 with 0.3 of a ten-year average (strategy three, "Forecasting future volatility") | the same estimator and blend, with a seeded and floored variance and a long run of the trailing 2,520 values, on the adjusted change over the raw previous close | a series that starts or sits flat must not give a zero | CARVER, ADAPTED |
| Annualisation | 16, the square root of 256 business days | the square root of the bars a year counted over the trailing 256 bars | a series with a Sunday session row has more than 256 bars a year | CARVER, ADAPTED |
| Volatility attenuation | the quantile of volatility relative to its long-run average, a multiplier of 2 - 1.5 Q, a ten-day smooth (strategy thirteen) | the same multiplier and smooth, with Q the quantile of `sigma_short` in its own trailing 2,520 values | one estimator serves the quantile | CARVER, ADAPTED |
| Instrument weights | a top-down method: equal shares by asset class, then by group, then by instrument (strategy four, "An algorithm for allocating instrument weights") | equal by sector in one layer, a symbol capped at half its sector | the metadata carries one sector field, and a one-contract sector must not take a whole share | CARVER, ADAPTED |
| IDM | by the number of instruments (strategy four, table 16) | 2.5, the table's value for 30 or more | | CARVER AS WRITTEN |
| Sizing capital | the book reports its backtests on fixed capital and offers two forms for a live account, full compounding and half compounding (Tactic three, "Half compounding") | half compounding in live and in the backtest alike, recomputed each run from the settled P&L | one sizing rule in both engines, so a backtest sizes as the live book will | CARVER, ADAPTED |
| Minimum position, strategy buffer | buffering under dynamic optimisation belongs to the optimiser (strategy twenty-five, "Dealing with costs: buffering") | the strategy publishes N* unbuffered and unrounded | | CARVER AS WRITTEN |
| Overlay: what is scaled | all positions are multiplied by the scalar | free rows are scaled; held rows are counted and not scaled; the trim bounds the stored book | a held row cannot trade that day | CARVER, ADAPTED |
| Overlay: net leverage reading | none: the book's leverage reading is gross | a fifth reading, net leverage, with its own limit of 6.0, beside gross | the book is long and short, so its net exposure is limited as well as its gross | OURS |
| Overlay: limits | the 99th percentile of each measure's own history on the book's portfolio, rounded: 1.5, 3.75 and 3.25 times the target, and 20 for leverage (Tactic four, "An exogenous risk overlay") | fixed values in `risk.json`: 2.25, 4.5 and 4.0 times the target, and 8.0 for gross leverage | the book sets each limit from the history of its own portfolio and notes that another set of instruments calls for other limits | CARVER, ADAPTED |
| Overlay: jump reading | a jump risk multiplier from the 99th percentile of each instrument's volatility | on each symbol's jump volatility (the 99th percentile of its trailing 2,520 `sigma_short` values, each taken back to a daily figure, annualised by the gate window's factor), with the window's correlations | uses the engine's own estimator | CARVER, ADAPTED |
| Overlay: thin windows | | below 21 complete dates the covariance readings are blind and leverage still applies; a participant with no return in the window is out of the three covariance readings and stays in leverage; one with fewer than 120 returns is left out of R and R_jump and stays in R_shock (on its own window volatility, once it has two returns) and in leverage | a new listing must not blind the book | OURS |
| Covariance | EWMA volatility with correlations from six months of weekly returns (strategy twenty-five) | the sample covariance of daily adjusted returns: 252 dates for the overlay, 756 closes for the optimiser | one estimator and one annualisation rule for every reading | CARVER, ADAPTED |
| Per-name cap | a position limit from a leverage ratio, with two as the worked example (Tactic four, "Setting position limits") | a cap of 2 on a position's weight on the sizing capital, applied to the target, in the search and on the rounded book | the overlay must never read a position the book cannot hold | CARVER, ADAPTED |
| Position limit at the maximum forecast, open-interest limit | a per-instrument limit from the position at the maximum forecast, and one as a share of open interest | not applied | section 14 | Not adopted |
| Starting point of the search | the greedy algorithm starts from a zero weight in every instrument (strategy twenty-five, "The greedy algorithm") | from the held book, one contract up or down per step, with the sign guard | the cost term then prices the trades the book would really make, and a holding is kept unless moving it lowers the objective | CARVER, ADAPTED |
| Cost multiplier | 50, with the remark that anything from 10 to 100 behaves much the same (strategy twenty-five, "Dealing with costs: a cost penalty") | 100, the top of that range | on a book this small the cost weight governs how often it trades; the value is inside the book's stated range | CARVER, ADAPTED |
| Forecast changes sign | the whole position is always closed | closed once the opposite forecast reaches 2; held while it is weaker | a forecast that has only just crossed zero does not close a position | OURS |
| Tracking-error buffer | 5 percent of the risk target, 1 percent at a 20 percent target | the larger of that figure and the risk of one contract of the lumpiest symbol in the book | on a 500,000 book one contract of many instruments is larger than a 1 percent band | CARVER, ADAPTED |
| After the rounding | | the trim: up to five contracts removed when the rounded book is over a limit | whole contracts can land a book above a limit the target was inside | OURS |
| Session logic | none | the classifier's holds, applied inside the pass | a bar that is not a usable print must not move the book | OURS |
| A held symbol's target | | set to its holding, so no correlated symbol stands in for it | avoids hedging a held row with its neighbours | OURS |
| Several sleeves in one book | one strategy per account | BASE's two sleeves: summed targets, one pass, a split by largest remainder | BASE is a placeholder test book | OURS |
| Netting between sleeves | | opposite sleeve fills in one symbol are credited the cost the account did not pay ([COST_MODEL.md](COST_MODEL.md)) | the account sends one order per symbol | OURS |
| Listing dates and the switch | | a contract trades only from its listing date, the predecessor before it; on the switch the predecessor is exited and the listed contract entered at the day's scaled target | a backtest must not trade a contract before it existed | OURS |
| The relabel list | | declared vendor id changes that are not rolls are read as no change | a relabel must not be booked as a roll | OURS |
| Minimum volatility | an instrument whose volatility is below a minimum is left out of the universe (Tactic four, "Instruments that are too safe to trade") | no screen; the per-name cap bounds a low-volatility contract's position every day | the cap is the daily limit, and no contract is removed from the 36 | Not adopted |
| A stopped sleeve | | its held rows are closed with one fill on the next session bar | a sleeve that no longer signals must not leave positions behind | OURS |
| Order of the overlay and the optimiser | the multiplier is applied to the unrounded positions, then the buffering (Tactic four, "An exogenous risk overlay") | the same order, once: the overlay reads the capped target, the scalar is applied once and nothing feeds back into it; the trim follows the rounding | | CARVER AS WRITTEN |
| Fast sleeve's volatility span | one span for every rule | BASE's fast sleeve uses 16 | a sleeve setting on the placeholder book | OURS |

---

## 14. Tried and not adopted

Each of these is an option the design does not use, with the reason.

| Option | What it is | Why it is not adopted |
|---|---|---|
| Carver's per-instrument position limit | a limit from the position at the maximum forecast, and a limit as a share of open interest (Tactic four, "Setting position limits") | the per-name cap already bounds every name, so the maximum-forecast limit would be a second, overlapping per-name bound on a chosen nominal weight. The open-interest limit needs data the engine does not have |
| Switching other contracts to micro or mini sizes | trading the micro or mini of the contracts that have one, beyond the four equity index micros the book already trades | the book then holds several contracts where it holds none or one, so every one-lot adjustment becomes a trade |
| Sector caps | at most K names of a sector may hold a position | the cap changes the risk the book runs instead of how it trades |
| Search hysteresis | the search pays an extra term, a multiple of a contract's own risk, to trade it | the term decides which groups of instruments the book can hold at all |
| Blanket speed changes | removing the fastest rules from every contract | the six speeds are the design. The book's procedure removes a rule only where its cost on that contract is over the limit; a blanket cut also removes rules from contracts that can afford them. The per-contract removal of section 3.5 is what is in force |
| The roll back-refusal | refusing a vendor switch back into the contract the series has just left | it cannot tell a whipsaw from the vendor's early look at the next contract, and in the second case it holds the symbol on the wrong contract. See [FUTURES_ROLLS.md](FUTURES_ROLLS.md) |
