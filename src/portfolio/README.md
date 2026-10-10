# Portfolio Module

## Overview

`PortfolioManager` (`include/trade_ngin/portfolio/portfolio_manager.hpp:192`) holds a book's sleeves
(strategies), feeds them bars, turns their targets into one whole-contract book, and records the
fills of each sleeve. The backtest coordinator and the live runners both drive it through
`process_market_data`.

The design of the futures rebalance is in
[docs/OPTIMIZER_AND_RISK_DESIGN.md](../../docs/OPTIMIZER_AND_RISK_DESIGN.md); the risk layer is in
[docs/RISK_MODULES.md](../../docs/RISK_MODULES.md).

---

## File layout

```
src/portfolio/
└── portfolio_manager.cpp     # the manager

include/trade_ngin/portfolio/
├── portfolio_manager.hpp     # PortfolioConfig, PortfolioManager
├── sizing_capital.hpp        # the sizing capital under half compounding (design comment at `:36`)
├── allocation_split.hpp      # how one symbol's contracts are split over the sleeves that asked for it
└── loop_config.hpp           # copies the one pass's constants from the loaded files into PortfolioConfig
```

---

## What a rebalance does

`PortfolioManager::process_market_data` (`src/portfolio/portfolio_manager.cpp:266`):

1. Feeds the bars to every sleeve; each sleeve publishes its target per symbol. A trend sleeve's
   target is unrounded and unbuffered.
2. Runs each sleeve's own risk modules once, if it has any (`apply_sleeve_risk`,
   `portfolio_manager.cpp:3637`). No shipped book configures sleeve modules.
3. Rebalances the book:
   - a book that names an overlay sleeve (every futures book) goes through the one pass,
     `rebalance_one_pass` (`portfolio_manager.cpp:2015`): the per-name cap, the risk overlay once,
     one search from the held book, the buffer and one rounding, the trim, then the fills;
   - any other book (the equity book) runs the generic optimiser step once
     (`optimize_positions`, `portfolio_manager.cpp:1693`) and then the portfolio risk step once.
4. Records each sleeve's fills and, in a backtest of a book with more than one sleeve, the netting
   adjustment of the bar's fills (`portfolio_manager.cpp:2860`; a live run nets in the runner; see
   [docs/COST_MODEL.md](../../docs/COST_MODEL.md)).

The overlay sleeve is the book's first sleeve; the runners set `PortfolioConfig::overlay_sleeve` and
`overlay_tau` from it (for example `apps/strategies/live_portfolio_conservative.cpp:762`). A book
that holds a trend sleeve and names no overlay sleeve is refused (`portfolio_manager.cpp:499`).

---

## Capital, allocations and aggregation

- **Allocations.** `add_strategy(strategy, initial_allocation, use_optimization)`
  (`portfolio_manager.cpp:198`) refuses an allocation outside the configured bounds and a total
  above 1.0. The runners normalise the configured `default_allocation` values to sum to 1.0, with a
  warning when they do not, before they add the sleeves (`apps/strategies/live_portfolio.cpp:268`).
- **Sizing capital.** A futures book is sized on the half-compounded sizing capital, not on the
  constant starting capital: the starting capital less the drawdown of the cumulative settled net
  P&L from its running peak, never above the starting capital (`half_compounded_capital`,
  `include/trade_ngin/portfolio/sizing_capital.hpp:58`). The backtest coordinator and the live futures runners hand it to the manager with
  `set_sizing_capital` (`portfolio_manager.cpp:3812`) before each rebalance. Without that call the
  manager sizes on `PortfolioConfig::total_capital` (the equity book).
- **Each sleeve's slice.** A sleeve sizes its contracts on `sizing capital x its allocation`
  (`portfolio_manager.cpp:3829`). Its contracts are therefore already contracts of the account's
  book.
- **Aggregation.** The account holds the plain sum of the sleeves' contracts per symbol. The
  allocation is not applied a second time. The rebalance works on that sum in account weight (one
  contract weighs notional per contract over the sizing capital), and the whole-contract answer for
  a symbol is split back over the sleeves by largest remainder so the stored integers sum to the
  book exactly (`distribute_optimizer_contracts`,
  `include/trade_ngin/portfolio/allocation_split.hpp:51`).
- **Reading the book.** For anything that must match what the account holds, sum
  `get_strategy_positions()` per symbol (targets) or `get_filled_strategy_positions()` (the filled
  ledger). `get_portfolio_positions()` applies the allocation again and is kept for legacy paths
  only (`portfolio_manager.hpp:279`).

---

## Configuration

`PortfolioConfig` (`portfolio_manager.hpp:39`) carries the total capital, the allocation bounds,
`use_optimization`, the overlay sleeve and its risk target, the one pass's constants
(`per_name_cap`, `sign_close_band`, `b_sigma_floor`, `trim_max`), the sizing mode, the risk modules
of the book and of each sleeve, the covariance history settings (`covariance_history_prices`,
`covariance_stale_dates`: they shape the history the generic optimiser step reads; the futures
search takes 756 closes and a stale rule of 5 dates from constants of the code), and the optimiser
and risk reporting configurations. The keys and the files they come from are in
[docs/CONFIG_GUIDE.md](../../docs/CONFIG_GUIDE.md).

---

## Testing

Every test is built into one binary, `trade_ngin_tests`, and `ctest` lists each case by its suite
name, so filter on suite names:

```bash
cd build
ctest -R "HalfCompounding|Allocation|OnePassBook" --output-on-failure
```

---

## References

- [docs/OPTIMIZER_AND_RISK_DESIGN.md](../../docs/OPTIMIZER_AND_RISK_DESIGN.md)
- [docs/RISK_MODULES.md](../../docs/RISK_MODULES.md)
- [docs/COST_MODEL.md](../../docs/COST_MODEL.md)
- [docs/CONFIG_GUIDE.md](../../docs/CONFIG_GUIDE.md)
- [Optimization Module](../optimization/README.md)
- [Strategy Module](../strategy/README.md)
- [Backtest Module](../backtest/README.md)
- [Live Trading Module](../live/README.md)
