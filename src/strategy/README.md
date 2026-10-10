# Strategy Module

## Overview

`src/strategy/` holds the strategy interface, the common base class and the strategies the runners
build: the trend-following sleeve (in two configurations, TREND and FAST) and the equity
mean-reversion strategy. A strategy receives bars, computes a forecast per symbol and publishes a
target position per symbol. It does not round, buffer or cap its target on a futures book: that is
the portfolio's rebalance (see [Portfolio Module](../portfolio/README.md)).

The trend-following method, stage by stage, with every setting in force, is in
[docs/TREND_FOLLOWING_SYSTEM.md](../../docs/TREND_FOLLOWING_SYSTEM.md). The equity strategy is in
[docs/EQUITY_STRATEGY_GUIDE.md](../../docs/EQUITY_STRATEGY_GUIDE.md).

---

## File layout

```
src/strategy/
├── base_strategy.cpp        # state, positions, persistence of signals; shared by every strategy
├── trend_following.cpp      # the trend sleeve: history, weights, the unrounded target
├── trend_estimator.cpp      # volatility, EWMAC forecasts, scaling and capping, as pure functions
├── mean_reversion.cpp       # the equity mean-reversion strategy
└── regime_detector.cpp      # a regime classifier; no runner or strategy calls it

include/trade_ngin/strategy/
├── strategy_interface.hpp       # StrategyInterface
├── base_strategy.hpp            # BaseStrategy
├── types.hpp                    # StrategyConfig, StrategyState, StrategyMetadata, StrategyMetrics
├── trend_following.hpp          # TrendFollowingConfig, fast_trend_following_config(), TrendFollowingStrategy
├── trend_estimator.hpp          # the estimators' design comment, Window, Estimate, estimate()
├── trend_estimator_record.hpp   # optional per-day record of the estimators
├── vol_annualisation.hpp        # bars a year counted from the bars' own dates
├── sleeve_config.hpp            # a futures sleeve's required keys; hands over the rule removals
├── short_window_log.hpp         # the one line a run prints for symbols with a short history
├── mean_reversion.hpp           # MeanReversionConfig, MeanReversionStrategy
├── equity_strategy_builder.hpp  # builds the equity strategies from portfolio.json
└── regime_detector.hpp          # RegimeDetector
```

---

## The interface

`StrategyInterface` (`include/trade_ngin/strategy/strategy_interface.hpp:13`) is what the
`PortfolioManager` drives. `BaseStrategy` (`include/trade_ngin/strategy/base_strategy.hpp:19`)
implements all of it; a new strategy derives from `BaseStrategy` and overrides what it needs.

| Group | Methods |
|---|---|
| Lifecycle | `initialize`, `start`, `stop`, `pause`, `resume` |
| Data | `on_data(const std::vector<Bar>&)`, `on_execution`, `on_signal(symbol, signal)` |
| State | `get_state`, `get_metrics`, `get_config`, `get_metadata`, `get_price_history` |
| Positions | `get_positions`, `get_target_positions`, `update_position`, `seed_positions` |
| Capital and history | `set_capital_allocation`, `seed_history`, `set_backtest_mode` |
| Risk limits | `update_risk_limits`, `check_risk_limits` |
| For the rebalance | `overlay_series`, `is_signalling` |

`StrategyConfig` (`include/trade_ngin/strategy/types.hpp:119`) carries `capital_allocation`,
`max_leverage`, `position_limits`, `trading_params`, `costs`, the asset classes and the data
frequencies. `BaseStrategy`'s constructor is
`BaseStrategy(std::string id, StrategyConfig config, std::shared_ptr<PostgresDatabase> db)`
(`base_strategy.hpp:27`), and `validate_config()` is the protected hook for parameter checks
(`base_strategy.hpp:222`).

---

## The trend sleeve in brief

`TrendFollowingStrategy` (`include/trade_ngin/strategy/trend_following.hpp:119`) is one class with
two configurations. `TrendFollowingConfig` (`trend_following.hpp:22`) is the TREND sleeve;
`fast_trend_following_config()` (`trend_following.hpp:57`) is the FAST sleeve (the four fast EMA
pairs, a 16-bar short volatility span, a 0.25 risk target). A futures sleeve's `risk_target`, `idm`
and `vol_lookback_short` are required in `portfolio.json`: a runner refuses a missing key and never
falls back to the struct's own values (`include/trade_ngin/strategy/sleeve_config.hpp:19`). The
sleeve's `vol_lookback_long` key is copied into the config and read by nothing that computes: the
long-run mean is a constant of the code, 2,520 values (`trend_following.hpp:30`).

What the sleeve computes at each signal bar:

- **Series.** Returns and the EMAs read the back-adjusted series; the price a forecast divides by
  and the price a position is sized on are the raw close. See
  [docs/FUTURES_ROLLS.md](../../docs/FUTURES_ROLLS.md).
- **Volatility.** A mean-centred EWMA standard deviation of returns, annualised by the square root
  of the bars a year counted from the bars' own dates (not the book's fixed 16, the square root of
  256, which only the forecast's volatility keeps), then blended:
  `sigma = 0.7 x sigma_short + 0.3 x sigma_long`, where `sigma_long` is the mean of `sigma_short`
  over the trailing 2,520 values. The design comment is at
  `include/trade_ngin/strategy/trend_estimator.hpp:12`; the annualisation rule is at
  `include/trade_ngin/strategy/vol_annualisation.hpp:16`.
- **Forecast.** For each EMA pair, `raw = (EMA_fast - EMA_slow) / (close x forecast_sigma / 16)`,
  multiplied by the attenuation and by the pair's fixed scalar and capped at plus or minus 20. The
  attenuation is `2 - 1.5 q`, `q` the smoothed share of the trailing 2,520 `sigma_short` values at
  or below the bar's own, and 1 until 252 values exist. The combined forecast is the
  equal-weight mean of the pairs the contract runs, times the forecast diversification multiplier
  for that number of pairs, capped at plus or minus 20. Pairs removed from a contract by cost come
  from `portfolio.json`'s `trading_rule_removals`; the equity slow rule comes from
  `equity_slow_rule` (`trend_following.hpp:34` to `:50`).
- **Instrument weights.** Sector-budgeted weights that sum to 1.0, with one symbol capped at half of
  its sector's allocation (`TrendFollowingStrategy::get_weights`,
  `src/strategy/trend_following.cpp:915`). The universe is every metadata symbol with a bar in
  `futures_data.ohlcv_1d` at any date, less ES (`:926` to `:937`). The weights are computed once per
  sleeve and are the same on every date of a run, so in a long backtest a contract carries its
  weight before its first bar.
- **Target.** The unrounded target

  ```
  N* = (forecast x capital x weight x IDM x risk_target) / (10 x multiplier x price x FX x sigma)
  ```

  (`TrendFollowingStrategy::calculate_position`, `src/strategy/trend_following.cpp:1046`), where
  `capital` is the sleeve's slice of the sizing capital. The target is published as it is: the
  strategy applies no buffer, no rounding and no position cap. The per-name cap, the buffer and the
  rounding belong to the portfolio's one pass; see
  [docs/OPTIMIZER_AND_RISK_DESIGN.md](../../docs/OPTIMIZER_AND_RISK_DESIGN.md).

The warm-up a backtest excludes is the sleeve's longest EMA window
(`TrendFollowingStrategy::get_max_required_lookback`, `src/strategy/trend_following.cpp:1201`).

---

## Adding a strategy

1. Add `include/trade_ngin/strategy/<name>.hpp` and `src/strategy/<name>.cpp`, with a class derived
   from `BaseStrategy`. Override `initialize()` and `on_data()`; override `validate_config()` for
   parameter checks. Return `Result<void>` and check every result.
2. Add the `.cpp` to `TRADE_NGIN_SOURCES` in the root `CMakeLists.txt`.
3. Add a branch for the strategy's `type` string where the runners build their sleeves, for example
   `apps/strategies/live_portfolio.cpp:722` and `apps/backtest/bt_portfolio.cpp:369` for a futures
   sleeve. The equity runners build theirs through
   `include/trade_ngin/strategy/equity_strategy_builder.hpp`.
4. Declare it under `strategies` in `config/portfolios/<name>/portfolio.json`, with
   `enabled_backtest`, `enabled_live`, `default_allocation`, `type` and its `config` object. See
   [docs/CONFIG_GUIDE.md](../../docs/CONFIG_GUIDE.md).
5. Add tests under `tests/strategy/`.

A book that holds a trend sleeve must name an overlay sleeve (the runners name the first sleeve);
any other book is rebalanced by the generic optimiser step. See
[Portfolio Module](../portfolio/README.md).

---

## Testing

Every test is built into one binary, `trade_ngin_tests`, and `ctest` lists each case by its suite
name, so filter on suite names:

```bash
cd build
ctest -R "TrendFollowing|TrendEstimator|BaseStrategy|MeanReversion" --output-on-failure
```

---

## References

- [docs/TREND_FOLLOWING_SYSTEM.md](../../docs/TREND_FOLLOWING_SYSTEM.md): the trend-following method end to end
- [docs/OPTIMIZER_AND_RISK_DESIGN.md](../../docs/OPTIMIZER_AND_RISK_DESIGN.md): from the target to the stored book
- [docs/FUTURES_ROLLS.md](../../docs/FUTURES_ROLLS.md): the adjusted series and rolls
- [docs/CONFIG_GUIDE.md](../../docs/CONFIG_GUIDE.md): the configuration keys
- [Backtest Module](../backtest/README.md)
- [Portfolio Module](../portfolio/README.md)
- [Transaction Cost Module](../transaction_cost/README.md)
- [Data Module](../data/README.md)
