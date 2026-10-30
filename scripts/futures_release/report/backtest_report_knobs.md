## Knobs: every number the book runs on

This section follows the engine from the bars it reads to the statistics it stores, and lists every number that
decides a result at each stage: the values in the config files (marked **config**, with the key) and the values
compiled into the code (marked **code**, with the constant and its file). Each was checked against the engine the
runs were made on. A **code** value changes only with a code change; a **config** value changes
with the config. The full design and the reasoning for each choice is `docs/TREND_FOLLOWING_SYSTEM.md` (section 2
lists where we depart from Carver and why); costs `docs/COST_MODEL.md`; the overlay `docs/RISK_MODULES.md` and
`docs/OPTIMIZER_AND_RISK_DESIGN.md`. Rolls: `docs/FUTURES_ROLLS.md` describes the vendor-series path (the switch
off); with `per_contract_rolls` on, as here, the roll rule is the design comment of
`include/trade_ngin/data/contract_roll_rule.hpp` and the bar source that of `include/trade_ngin/data/per_contract_source.hpp`.

### 0. The book and the windows

| Number | What it does | Set in |
|---|---|---|
| 500,000 | starting capital of every run | config `initial_capital`, `starting_capital` |
| 1 sleeve, 100 percent | `TREND_FOLLOWING`, the whole capital; long and short; no other sleeve | config `strategies` |
| carry off | the config has no `carry` block, so the forecast is the trend rules alone (absent means off) | config `carry` (absent); code `CarryConfig`, `config_loader.hpp` |
| 3, 6, 11, 16 years | prices loaded for the 2y, 5y, 10y, 16y windows (`backtest.lookback_years`), ending when the run starts. The vendor's grid begins 2010-06-07; with per-contract rolls each contract's series starts on its first grid date with a contract row | config, set per run by the script (`09_backtests.sh`); code `series_start`, `contract_rolls.hpp` |
| 256 bar dates | warm-up at the start of every window (the slowest moving average): no trading, dropped from every statistic. So a window trades about one year less than it loads: about 2.2, 5.2, 10.2 and 15.2 years | code `BacktestCoordinator::calculate_warmup_days`, `TrendFollowingStrategy::get_max_required_lookback` |
| about 311 a year | Sunday-to-Friday dates on which at least 9 symbols print (3,111 such dates over 2016 to 2025); MBT's Saturday bars are not rows | config note `statistics._sessions_per_year_note` |

The four runs of a set are made on the same UTC day on the same stored prices (the script stops if either changes),
so they end on the same last bar. Each run is labelled by its first traded day and its last day.

**Reading the results: each run is one path.** The book is 500,000 traded in whole contracts, the search starts each
day from the book already held, and the sizing capital follows the account's drawdowns. So two runs that start a few
days apart hold different books for years, and their results can differ widely on the same prices. Measured on the
2026-10-14 engine and setup: the 10y window ending 2026-10-07, on the same prices, returned 129.1% when it started on
2015-10-07 and 61.0% when it started on 2015-10-14. That measure was not repeated on this engine. Read a result
across the four windows, and do not treat a difference between two settings as real unless it is larger than what
moving the start date does.

### 1. Universe

| Number | What it does | Set in |
|---|---|---|
| 38 symbols, 36 instruments, 7 sectors | the 38 `.v.0` symbols stored (the 36 of before plus MGC and SIL). While MGC and SIL are stored, GC and SI are left out of the run's universe: each pair is one instrument, the micro | the data; code `ListingDates::run_universe`, `listing_dates.cpp` |
| 2019-05-06, ratio 10 | MES, MNQ, MYM, M2K trade from their listing date; before it the book trades ES, NQ, YM, RTY (one E-mini = 10 micros). Only the 10y and 16y windows reach back that far | config `listing_dates` |
| 2010-10-04, ratio 10 | MGC (10 oz) trades from its listing date; before it the book trades GC (100 oz). Every window of a run made after 2026-10-04 loads from after this date, so no window trades GC | config `listing_dates` |
| 2013-06-17, ratio 5 | SIL (1,000 oz) trades from its listing date; before it the book trades SI (5,000 oz). Only the 16y window reaches back that far | config `listing_dates` |
| open at target | on a listing date a held predecessor is closed and the listed contract opened at the day's capped target, rounded to whole contracts (fill ids `LC-`, `LO-`) | config `listing_dates.switch_rule` |
| ratio check | a pair whose ratio is not the two contracts' size ratio (before multiplier = ratio x listed multiplier) refuses the run | code `ListingDates::ratio_error`, `listing_dates.hpp` |
| refusals | a run refuses a stored micro beside its full-size root with no listing pair (MGC/GC, SIL/SI, MES/ES, MNQ/NQ, MYM/YM, M2K/RTY), and a gold or silver pair with `per_contract_rolls` off | code `ListingDates::unpaired_micro_refusals`, `ListingDates::refusals`, `listing_dates.hpp` |
| 2026-02-22 | the vendor relabelled the four equity micros' contract ids without a roll. The list is declared but NOT read with per-contract rolls on: the roll rule books that change as one roll between the two contracts of one expiry (trigger alias) | config `instrument_id_relabels`; code `relabels_read`, `listing_dates.cpp` |
| 1,000 contracts | engine position guard in a backtest; never binds at this size | config `execution.position_limit_backtest` |

| Sector | Contracts | First price (where after 2010-06) |
|---|---|---|
| Agriculture | GF, HE, KE, LE, ZC, ZL, ZM, ZR, ZS, ZW | KE 2013-12-16 |
| Crypto | MBT | 2021-05-03 |
| Energy | CL, HO, NG, RB | |
| Equities | M2K, MES, MNQ, MYM (ES, NQ, YM, RTY before 2019-05-06) | M2K 2017-07-09 |
| FX | 6A, 6B, 6C, 6E, 6J, 6L, 6M, 6N, 6S | |
| Interest Rates | UB, ZF, ZN, ZT | |
| Metals | HG, MGC, PL, SIL (GC before 2010-10-04, SI before 2013-06-17) | |

The first-price dates are the vendor series' as recorded for the 2026-10-14 set (data, not confirmed in code); with
per-contract rolls a micro's history before its change is its full-size root's contracts (section 8).

### 2. Bars: which prints are used

A bar that looks corrupt is withheld; a contract with no usable bar keeps its position (a hold) and gets no fill that
day. "Normal" volume is the median of the contract's last 20 accepted weekday bars. With per-contract rolls the bars
judged are the held contract's own (section 8).

| Number | What it does | Set in |
|---|---|---|
| 0.01 and 1,000 lots | a bar is junk when its volume is under 1 percent of normal AND under 1,000 lots | code `junk_fraction`, `junk_ceiling_lots`, `session_classifier.hpp` |
| 50 lots and 0.25 | a bar is junk when its volume is under 50 lots AND under 25 percent of normal | code `floor_lots`, `floor_fraction`, `session_classifier.hpp` |
| 20 bars | the window "normal" volume is the median of | code `norm_window_bars`, `session_classifier.hpp` |
| from the change | the normal volume counts only bars on or after the contract's latest change of price source, so a micro is judged on its own bars from its change, as a new contract is | code, `session_classifier.hpp` (the rule's design comment) |
| 8 weeks | how far back the classifier looks for a contract's usual sessions | code `expected_lookback_weeks`, `session_classifier.hpp` |
| 0.10 | a new contract id on volume under 10 percent of normal is held for a day | code `kIdChangeHoldFraction`, `session_classifier.hpp` |
| high = low | a locked or stub bar is not used | code, `session_classifier.hpp` |

### 3. Volatility

| Number | What it does | Set in |
|---|---|---|
| 32 | span of the short EWMA volatility | config `vol_lookback_short` |
| 0.7 | weight of the short estimate in the blend; 0.3 on the long-run average | code `kShortWeight`, `trend_estimator.cpp` |
| 2,520 values | length of the long-run average (about ten years of bars) | code `kLongRunValues`, `trend_estimator.hpp` |
| 252 | `vol_lookback_long` in the config file; NOT read (the long run is the 2,520 above) | config, unused |
| 256 bars | the trailing count that annualises the volatility (bars a year as the series shows them) | code `kAnnualisationBars`, `trend_estimator.hpp` |
| 16 | fixed annualisation factor of the forecast's volatility | code `kFixedAnnualisation`, `trend_estimator.hpp` |
| 3,200 bars | estimator window | code `kWindowBars`, `trend_estimator.hpp` |
| 0.1, 1e-6 | variance seed share and variance floor | code `kVarianceSeedShare`, `kVarianceFloor`, `trend_estimator.cpp` |
| 0.005 and 5.0 | floor and cap on the estimated volatility | code `kSigmaFloor`, `kSigmaCap`, `trend_estimator.cpp` |
| 0.01 to 1.0 | the sizing volatility is kept inside this range | code, `trend_following.cpp` |

### 4. Forecasts

| Number | What it does | Set in |
|---|---|---|
| (2,8) (4,16) (8,32) (16,64) (32,128) (64,256) | the six EWMAC trend speeds | config `ema_windows` |
| 12.1, 8.53, 5.95, 4.10, 2.79, 1.91 | forecast scalars for fast spans 2, 4, 8, 16, 32, 64 (Carver's values) | code `forecast_scalar`, `trend_estimator.cpp` |
| 20 | cap on every forecast, plus or minus | code `kForecastCap`, `trend_estimator.hpp` |
| 252, 2,520, 10 | attenuation: values needed, history it is ranked in, smoothing span | code `kAttenuationMinValues`, `kLongRunValues` (`trend_estimator.hpp`), `kAttenuationSmoothSpan` (`trend_estimator.cpp`) |
| equal | forecast weights across the speeds a contract runs | code |
| 1.00, 1.03, 1.08, 1.13, 1.19, 1.26 | forecast diversification multiplier for 1 to 6 speeds | code `TrendFollowingConfig::fdm`, `trend_following.hpp` (config `strategy_defaults.fdm` carries the same table) |
| 18 rules on 12 contracts | speeds removed because they cost too much on that contract: no 2/8 on 6M, GF, HE, LE, NG, ZL, ZS; no 2/8 or 4/16 on 6L, KE, ZC, ZW; no 2/8, 4/16 or 8/32 on ZR. No entry for the metals or the equity micros, so they run all six | config `trading_rule_removals` |
| 0.15 Sharpe a year | the cost limit that list follows from (Carver's rule); turnovers 98.5, 50.2, 25.4, 13.2, 7.6, 5.2 a year for the six speeds | `scripts/trading_rule_costs.py` (the list is fixed in config; the script only checks it) |
| M2K, MES, MNQ, MYM; (32,128) and (64,256) | equity slow rule: an equity index short stands only when both slow speeds are negative, otherwise the forecast is 0 | config `equity_slow_rule` |

### 5. Sizing

| Number | What it does | Set in |
|---|---|---|
| 0.20 | risk target tau: 20 percent annual volatility of the sizing capital | config `risk_target` |
| 2.5 | instrument diversification multiplier | config `idm` |
| 1/7 a sector, 0.50 cap | instrument weights: equal by sector, equal within it, no contract above half its sector's share (MBT 0.0714; the rest rescaled to sum to 1). A listing pair is one instrument, weighted under the listed contract | code `MAX_SYMBOL_TO_SECTOR_RATIO`, `trend_following.cpp`; `ListingDates::pair_root`, `listing_dates.hpp` |
| half compounding | sizing capital = 500,000 less the account's fall from its high-water mark, never above 500,000: a loss cuts size at once, profit above the start is set aside | config `sizing_mode` |
| formula | target contracts = forecast / 10 x weight x IDM x tau x sizing capital / (contract's annual dollar volatility), unrounded | code |

### 6. Risk: the per-name cap and the overlay

The target book is read once a day for risk; if any reading is over its limit, every target the search is free to
move is multiplied once by the same factor below 1 (a symbol on hold stays at its held quantity).

| Number | What it does | Set in |
|---|---|---|
| 2 | per-name cap: no contract's notional above 2 x sizing capital | config `risk.json` `per_name_cap` |
| 1.75 x tau = 0.35 | limit on portfolio risk sqrt(x' Sigma x) | config `R_max` |
| 4.75 x tau = 0.95 | limit on jump risk (each contract's 99th percentile volatility, on the covariance's correlations) | config `R_jump_max` |
| 99, over 2,520 values | the jump volatility's percentile and history | code `kJumpPercentile` (`trend_estimator.cpp`), `kLongRunValues` (`trend_estimator.hpp`) |
| 3.0 x tau = 0.60 | limit on correlation-shock risk (every correlation at 1) | config `R_shock_max` |
| 7.25 / 5.50 | gross / net leverage limits, notional over sizing capital | config `max_gross_leverage`, `max_net_leverage` (the same two in `risk_reporting`, the live runners' reporting snapshot) |
| carver | the overlay's covariance: each contract's own annualised standard deviation as its sleeve sizes it, times the exponentially weighted correlation of the two contracts' weekly returns (Saturday to Friday sums of daily returns, over every row of the history, not only the 252 dates). The jump reading uses the same correlations and the shock reading the same standard deviations. A pair with fewer than two common weeks has correlation 0 | config `risk.json` `overlay_covariance`; code `carver_covariance`, `carver_matrix`, `overlay.hpp`, `overlay.cpp` |
| 26 weeks | span of those weekly correlations | code `kCarverCorrelationWeeks`, `overlay.hpp` |
| 252 dates | the overlay's window: which contracts are in the readings, the blind rule and the annualisation | code `kWindowDates`, `overlay.hpp` (config `lookback_period` 252 is validated only) |
| 21 dates | below this many complete dates the three risk readings are not computed (blind day) | code `kBlindBelow`, `overlay.hpp` (config `min_gate_dates` 21 is validated only) |
| 120, 120 | complete-date floor for the window's covariance; a contract needs 120 returns to enter R and R_jump | code `kCompleteDatesFloor`, `kParticipantReturnsFloor`, `overlay.hpp` |
| ignore | a contract with no return in the window is left out of the risk readings | config `missing_symbol_policy` |
| 0.99 | VaR level of the reported risk only | config `confidence_level` |
| 0.3 | a WARN when the sleeve's drawdown passes 30 percent; never changes a position | config `max_drawdown`; code `base_strategy.cpp` |

### 7. From targets to trades

| Number | What it does | Set in |
|---|---|---|
| 100 | weight of trading cost in the whole-contract search (the top of Carver's range) | config `optimization.cost_penalty_scalar` |
| sample, 756 closes | the search's covariance: the equal-weight sample covariance of each contract's last 756 closes (adjusted returns over the raw previous close), on the dates they share, annualised on those dates' own frequency | config `optimization.optimiser_covariance` (absent: sample); code `kOptimiserBars`, `trend_following.cpp`; `optimiser_covariance`, `one_pass.cpp` |
| 5 dates, 20 returns, 0.01 | a contract more than 5 dates stale leaves the covariance; 20 common returns needed; a missing variance is filled at 0.01 | code, `one_pass.cpp` (the stale rule), `kMinReturns`, `kMissingVariance` (config `covariance_stale_dates` 5 and `covariance_history_prices` 756 shape only the generic optimiser's history) |
| held book | the search starts from the book held (after the sign closes) and moves one contract at a time | code `one_pass::search`, `one_pass.cpp` |
| 1e-6 | the smallest improvement a search step must make | code, `one_pass.cpp` (config `convergence_threshold` is not read by this search) |
| 100 | floor on the search's step count (the cap is the larger of 100 and twice the contracts to trade, plus 1) | config `optimization.max_iterations`; code `pass_cap`, `one_pass.cpp` |
| 0.05 x tau | no-trade buffer: the larger of 5 percent of tau (1 percent) and the risk of one contract of the largest contract held or searched | config `optimization.b_sigma_floor`; the larger-of rule is code, `b_sigma`, `one_pass.cpp` |
| fraction | outside the buffer, every contract moves the same fraction of the way from the held book to the search's answer, a = (tracking error of the held book - buffer) / that error, and is then rounded | code `buffer`, `one_pass.cpp` |
| 2 | a position whose forecast changed sign closes only once the opposite forecast reaches 2 | config `optimization.sign_close_band`; the closing itself is code |
| half away from zero | rounding to whole contracts | code `round_half_away`, `one_pass.cpp` |
| 5 contracts | at most 5 contracts trimmed a day when the rounded book is over a limit | config `risk.json` `trim_max` |
| signal close | fills are at the close of the bar the decision was made on; cost is charged separately against that day's P&L | code |

### 8. Rolls

The futures bars are the engine's own per-contract series: on each date of the vendor's grid, the bar of the contract
the roll rule holds, at its own prices. The vendor's `.v.0` splice is no longer the price series.

| Number | What it does | Set in |
|---|---|---|
| on | the bars come from the per-contract roll rule instead of the vendor's `.v.0` series; needs `futures_data.contract_ohlcv_1d` with the expiration filled | config `per_contract_rolls`; code `configure_per_contract_source`, `per_contract_source.hpp`, `bt_portfolio_conservative.cpp` |
| D* | the held contract's deadline: its first notice day, or its last trading day where that comes first or where it has no notice day. The last trading day is read from the contract's expiration; the first notice day comes from the exchange rule | code `contract_dates`, `contract_roll_rule.cpp`; `ExchangeRules`, `roll_calendar.hpp` |
| 5 trading days | the roll window opens on the 5th trading day before D* and ends on the last actionable bar (the last trading-day bar whose next calendar day is still before D*) | code `contract_dates`, `contract_roll_rule.cpp` |
| first of three | the roll happens on the first of: the vendor's confirmed forward switch; the first bar in the window where the next contract's volume is above the held one's; the last actionable bar | code, `contract_roll_rule.hpp` (the rule's design comment) |
| the cycle | the contract rolled into is the next one in the root's cycle that prints that day: for example GC and MGC G, J, M, Q, Z; SI and SIL H, K, N, U, Z; the rates, FX and equity indexes H, M, U, Z | code `roll_cycle_table`, `roll_cycle.hpp` |
| never back | a vendor switch into an earlier contract, or a one-bar stray, is ignored and the held contract's own bar is read | code, `contract_roll_rule.hpp` |
| exchange calendar | trading days are the exchange's own, per product group (CME Group), not the NYSE's; a vendor switch on a Sunday or a holiday session rolls on the next trading-day bar where both contracts print | code `FuturesCalendar`, `roll_calendar.hpp`; `include/trade_ngin/core/futures_holidays.json` |
| held | on a vendor switch bar the next bar has not yet confirmed, the bar handed out is the held contract's own | config `per_contract_switch_bar` |
| hold one bar on a vendor switch | a roll by crossover or deadline is known on its bar: no hold, the legs are booked on that bar. A vendor switch is held one consumed bar; the next bar confirms it and the legs are booked at the switch bar's two closes | code, `roll_series.hpp`, `per_contract_source.hpp` (design comments) |
| 2 legs at full cost, one bar | the roll is two outright fills (`execution_type = ROLL`, ids `RL-`): the closing leg at the old contract's close on the roll bar, the opening leg at the new contract's close on the same bar; each costed as any fill of that size | code `make_roll_legs`, `roll_series.hpp`; `backtest_coordinator.cpp` |
| booked | the held contract's own move over the roll bar is booked P&L, and the roll bar's return is the old contract's own over the bar (not 0) for the trend rules, the volatility and both covariances | code, `roll_series.hpp` (design comment); migration 040 `roll_bar_move` |
| shown, never P&L | the roll gap: the new contract's close minus the old contract's on the roll bar, times quantity and multiplier | code `attach_roll_split`, `roll_series.hpp`; migration 040 `roll_gap_points`, `roll_gap_value` |
| the micros' change | before 2026-02-22 the series of MES, MNQ, MYM, M2K is the ES, NQ, YM, RTY contracts; before 2010-10-04 MGC's is the GC contracts and before 2013-06-17 SIL's the SI contracts. The change is one roll (trigger alias) between the two contracts of the same expiry | code `micro_alias`, `contract_roll_rule.cpp`; `MicroAlias`, `contract_roll_rule.hpp` |
| carry, fallback | a held contract with no bar on a grid date carries its last close (or the vendor's bar, with a WARN, when the vendor names it); an old contract with no bar on the roll bar rolls on the next bar where both print, never after the last actionable bar, else by the fallback with a WARN (the new contract's move with the gap taken on the previous grid date, else 0) | code, `contract_roll_rule.hpp` |

**What this does to the results.** Both roll legs are charged at full cost, as two outright trades, so roll costs
are counted on the conservative side (a roll traded as one calendar-spread order would pay less). The day the
contract changes is now booked: the old contract's move up to its close on the roll bar is P&L, and both legs are
priced on that same bar. The difference between the two contracts' prices on the roll bar (the gap) is not P&L: the
same exposure is simply held in a contract that costs more or less. Each roll's split is stored on its closing leg
(`backtest.executions.roll_bar_move`, `roll_gap_points`, `roll_gap_value`, migration 040) and summed per run
(`backtest.results.roll_bar_moves`, `roll_gap_values`); the roll costs are `roll_costs` as before. Only a fallback
roll (no close of the old contract up to the last actionable bar) books an approximation, with a WARN.

### 9. Costs

| Number | What it does | Set in |
|---|---|---|
| 0.61 to 3.01 per contract per side | broker fee, Interactive Brokers' schedule: MYM 0.610; MES, MNQ, M2K 0.614; ZT, ZF 1.510; ZN 1.660; UB 1.810; MBT 2.011; CL, HO, RB 2.360; NG 2.460; FX 2.461; metals 2.510; GF, HE, LE 2.961; grains 3.010 (E-minis before 2019: ES, NQ, RTY 2.247, YM 2.240) | data `metadata.contract_metadata."Fee Per Contract"` (migration 026) |
| MGC 0.96, SIL 0.86 | the gold and silver micros' fee: commission 0.25 + exchange 0.70 (MGC) or 0.60 (SIL) + regulatory 0.01. SIL's 0.60 exchange fee and both micros' margins are derived, not confirmed (margins are not read by sizing or trades) | data `metadata.contract_metadata."Fee Per Contract"` (the micro-metals load) |
| 1.50 | fallback fee where a row has none; no row of the book uses it | code `transaction_cost_manager.hpp` |
| metadata required | a future with no usable metadata row (no positive contract size or tick size) logs an ERROR and its spread and impact are not priced; it is never priced on a guessed constant | code, `transaction_cost_manager.cpp` |
| 0.5 / 0.25 | share of the quoted spread paid: 0.25 on rates and FX, 0.5 on the rest | code `asset_cost_config.cpp`, `asset_cost_config.hpp` |
| 1 or 2 ticks | baseline spread: 2 for 6L, 6M, GF, HE, KE, LE, NG, ZC, ZL, ZR, ZS, ZW; 1 for the rest, MGC and SIL included (GC's and SI's entries; the bought data has no quotes) | code `asset_cost_config.cpp` |
| 0.15, 0.8 to 1.5, 20 returns, 0.01, 0.005 | the spread widens with the contract's recent volatility: sensitivity, bounds, window, baseline daily volatility, its scale | code `spread_model.hpp`, `spread_model.cpp` |
| 10, 20, 40, 60, 80 bps | market impact coefficient by the bar's volume (over 1m, 200k, 50k, 20k lots, else) times sqrt(share of volume); a micro's impact reads its own volume | code `impact_model.cpp` |
| 100 lots, 0.10 | impact volume floor and participation cap | code `impact_model.hpp` |
| 30 to 100 bps | impact cap by contract (MGC 50, SIL 60, as GC and SI) | code `asset_cost_config.cpp` |
| 200 bps | default cap on spread plus impact | code `asset_cost_config.hpp` |
| not used | `execution.commission_rate` 0.0005 and `execution.slippage_bps` 1 are read by no futures path | config, unused |

### 10. Statistics

| Number | What it does | Set in |
|---|---|---|
| 311.0574 | K, sessions a year, the annualisation of every statistic | config `statistics.sessions_per_year.futures` |
| Sunday to Friday, 9 contracts | the statistics grid: returns are taken on dates with at least 9 stored contracts printing, so thin Saturday and holiday rows are not days (a predecessor a listing date adds is not counted) | code `kStatisticsGridMinSymbols`, `live_historical_metrics.hpp`; `statistics_grid`, `backtest_coordinator.hpp` |
| 256 rows | warm-up rows dropped first | code |
| 0 | cash rate in the Sharpe ratio | code |
| n - 1 | sample standard deviation | code |

| Column | Definition |
|---|---|
| `total_return` | last equity / first equity after the warm-up, minus 1 |
| `volatility` | sample standard deviation of the daily grid returns x sqrt(K) |
| `sharpe_ratio` | mean daily return x K / volatility |
| `sortino_ratio` | mean daily return x K / downside volatility |
| `max_drawdown` | the largest fall of the account from its running peak, as a fraction of the peak |
| `calmar_ratio` | (1 + R)^(K / n) - 1 on the grid, over `max_drawdown` |
| `var_95`, `cvar_95` | the 5th percentile daily loss, and the mean of the losses beyond it |
| `worst_day` (date, symbol) | the worst daily return, its date and the contract that lost most that day |
| `volatility_sizing`, `max_drawdown_sizing`, `worst_day_sizing` | the same on the sizing capital (the day's net P&L over the capital the book was sized on) |
| `monthly_skew`, `monthly_tail_ratio` | skew of complete-month returns, and the ratio of the monthly distribution's tails |
| `calendar_year_returns`, `losing_years` | the account's return each calendar year (a part year is marked), and the count of whole years that lost |
| `transaction_costs`, `roll_costs`, `total_roll_fills`, `total_strategy_fills` | every fill's cost; the ROLL legs' cost and count; the STRATEGY fills' count |
| `roll_bar_moves`, `roll_gap_values` | the run's sum of the roll-bar moves booked (P&L, already inside the returns) and of the roll gaps (shown, never P&L), one value per roll |
| `total_trades`, `win_rate`, `profit_factor`, `avg_win`, `avg_loss`, `max_win`, `max_loss`, `avg_holding_period` | per round trip (a fill that reduces or closes a position), in dollars with the contract multiplier, after the fill's cost |
| `beta`, `correlation` | each day's return against the day before (a lag-one autocorrelation, not a market beta) |

Three figures the reports add from the stored rows (not stored columns):

| Figure | Definition |
|---|---|
| Costs per year, % of starting capital | every fill's cost over the window, divided by the years traded and by 500,000 |
| Costs in Sharpe units a year | that percentage divided by the annual volatility of the account. This is how Carver judges cost: what the costs take out of the Sharpe ratio. The Sharpe before costs is about the Sharpe plus this figure. His rule of thumb: one trading rule on one contract should cost no more than about 0.15 a year (the limit behind the rules removed in section 4), and the whole system well under a third of its Sharpe before costs |
| Turnover, round trips a year | strategy contracts traded a year divided by twice the average number of contracts held: how many times a year the average position is opened and closed. Roll legs are left out (they are counted under rolls) |

### The config files as they ran

`portfolio.json`:

```json
{{PORTFOLIO_JSON}}
```

`risk.json`:

```json
{{RISK_JSON}}
```

`defaults.json` (the database block left out; `backtest.lookback_years` is set per window by the script, 3, 6, 11 and 16, and shows here as the first run's):

```json
{{DEFAULTS_JSON}}
```
