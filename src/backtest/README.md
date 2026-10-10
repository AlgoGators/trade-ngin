# Backtest Module

## Overview

`src/backtest/` replays stored daily bars through the same `PortfolioManager` the live runners use,
books the fills and their costs, builds the equity curve, computes the run's metrics and stores the
results. `BacktestCoordinator` (`include/trade_ngin/backtest/backtest_coordinator.hpp`) owns the
run; the four runners that run a book (`bt_portfolio`,
`bt_portfolio_conservative`, `bt_equity_mr`, `bt_equity_validation`) call `BacktestCoordinator::run_portfolio`
(`src/backtest/backtest_coordinator.cpp:184`).

How a rebalance works is described in
[docs/OPTIMIZER_AND_RISK_DESIGN.md](../../docs/OPTIMIZER_AND_RISK_DESIGN.md); what a fill costs in
[docs/COST_MODEL.md](../../docs/COST_MODEL.md); rolls in
[docs/FUTURES_ROLLS.md](../../docs/FUTURES_ROLLS.md); the stored tables in
[docs/LIVE_RUN_CYCLE.md](../../docs/LIVE_RUN_CYCLE.md).

---

## File layout

```
src/backtest/
├── backtest_coordinator.cpp          # the run: load, cycle by cycle, metrics, storage
├── backtest_data_loader.cpp          # bars from the database
├── backtest_execution_manager.cpp    # fills and their costs
├── backtest_pnl_manager.cpp          # daily P&L
├── backtest_price_manager.cpp        # price history per symbol
├── backtest_metrics_calculator.cpp   # the metrics below
├── backtest_csv_exporter.cpp         # CSV output under apps/backtest/results
├── equity_cost_warmup.cpp            # equity cost model warm-up
├── slippage_model.cpp                # the slippage model classes
└── transaction_cost_analysis.cpp     # cost analysis helpers

include/trade_ngin/backtest/
├── backtest_coordinator.hpp, backtest_data_loader.hpp, backtest_execution_manager.hpp,
│   backtest_pnl_manager.hpp, backtest_price_manager.hpp, backtest_metrics_calculator.hpp,
│   backtest_csv_exporter.hpp, backtest_types.hpp
├── junk_signal_feed.hpp              # which bars a cycle feeds and which it withholds
├── consumed_series_record.hpp        # optional record of the consumed series
├── equity_cost_retier.hpp, equity_cost_warmup.hpp
└── slippage_models.hpp, transaction_cost_analysis.hpp
```

The runners are in `apps/backtest/`:

| Binary | Source | Book |
|---|---|---|
| `bt_portfolio` | `bt_portfolio.cpp` | BASE (config `base`) |
| `bt_portfolio_conservative` | `bt_portfolio_conservative.cpp` | CONSERVATIVE (config `conservative`) |
| `bt_equity_mr` | `bt_equity_mean_reversion.cpp` | the equity book (config `equity_mr`) |
| `bt_equity_validation` | `bt_equity_validation.cpp` | equity validation runs |
| `bt_transaction_cost_report` | `bt_transaction_cost_report.cpp` | a cost report |

Each loads `./config` through `ConfigLoader::load`, so it is run from the repository root. Run one
runner at a time: the runners share tables and process-wide singletons.

```bash
./build/bin/Release/bt_portfolio_conservative
```

---

## The window and the warm-up

- **Window.** The window is `backtest.lookback_years` (in `config/defaults.json`) ending at the end
  date: `start = end - lookback_years`. The end date is the wall-clock time of the run, so the
  window slides with the clock, unless `backtest.frozen_end_date` is set, a test-only key that pins
  the end date and logs a warning (`ConfigLoader::resolve_backtest_window`,
  `src/core/config_loader.cpp:807`; the key's comment is at
  `include/trade_ngin/core/config_loader.hpp:149`). An end that is not frozen is the wall-clock
  instant; a frozen end is local midnight of the key's date on the host; the start is
  `lookback_years` earlier on the host's local calendar (`src/core/config_loader.cpp:807-866`).
- **Warm-up.** The first rows of the window are warm-up: the sleeves are fed and no fill is
  generated. The length is the longest EMA window of the book's trend sleeves, 256 rows with the
  shipped configuration (`BacktestCoordinator::calculate_warmup_days`,
  `src/backtest/backtest_coordinator.cpp:1788`). A window of `lookback_years = 3` therefore trades
  about two years. A book with no trend sleeve has no warm-up.
- **Rows.** The equity curve has one row for the start and one per bar date. A futures book has a
  bar date for every session the data carries, Sunday sessions included, which is about 312 rows a
  year.

---

## Metrics calculation

`BacktestMetricsCalculator::calculate_all_metrics`
(`src/backtest/backtest_metrics_calculator.cpp:670`) computes everything below. It first drops the
first warm-up rows of the equity curve (`filter_warmup_period`, `:777`): the curve it keeps starts at
the last flat warm-up row, so every return it measures, the monthly returns apart, is a post-warm-up
return. A curve with no more rows than the warm-up is not filtered (`:780-782`). Daily returns are
the row-to-row changes of that curve (`calculate_returns_from_equity`, `:36`).

The annualisation constant is `K = 252`, written as a literal in each formula, for every book.

| Metric | Formula as computed | Code |
|---|---|---|
| Total return | `last / first - 1` of the post-warm-up curve | `:18` |
| Volatility | population standard deviation of daily returns (divide by `n`) `x sqrt(252)` | `:111`, `:765` |
| Sharpe ratio | `(mean daily return x 252 - risk-free rate) / volatility`, risk-free rate 0; 0 when volatility is 0 | `:58` |
| Downside volatility | `sqrt(sum of squared returns below 0 / count of returns below 0) x sqrt(252)` | `:140` |
| Sortino ratio | `mean daily return x 252 / downside volatility`; 999 when there is no return below 0 and the mean is not negative | `:81` |
| Max drawdown | the largest `(peak - equity) / peak`, the peak being the running maximum | `:174`, `:194` |
| Calmar ratio | `mean daily return x 252 / max drawdown`; 999 when the drawdown is 0 and the return is not negative | `:102`, `:715` |
| VaR 95 | minus the return at index `floor(n x 0.05)` of the sorted daily returns | `:208` |
| CVaR 95 | minus the mean of the `floor(n x 0.05)` worst daily returns (at least one) | `:224` |
| Beta, correlation | lag-1 autocorrelation of the book's own daily returns: `beta` is the slope of today's return on yesterday's, `correlation` the lag-1 correlation. Neither is measured against a market benchmark | `:636` |
| Monthly returns | the sum of daily returns per calendar month (the month of the row's timestamp in the host's local time, `:612-614`), over the whole curve (warm-up rows are flat) | `:599` |

The annualised return used by the Sharpe, Sortino and Calmar ratios is arithmetic, `mean x 252`. It
is not a compounded figure.

Because `K` is 252 and a futures curve has about 312 rows a year, a futures book's annualised
figures from this calculator are on the 252 convention and not on the curve's own row frequency.

### Trade statistics

`calculate_trade_statistics` (`:414`) walks the executions in order and tracks one net position per
symbol, summed over every sleeve.

- **A trade** is an execution that reduces or closes the tracked position of its symbol. An
  execution that opens or adds is not a trade. `total_trades` is the count of those closing
  executions (`:514`), so one position closed in three steps counts as three trades, and two sleeves'
  fills on one symbol net together.
- **Its P&L** is `closed quantity x (fill price - average entry price)`, signed by the side of the
  position, less the fill's cost after netting (`transaction_cost::net_cost`, `:428`). The price
  move is not multiplied by the contract multiplier (`:476-477`), so it is in price points while the
  cost taken off it is in currency.
- **Win rate** is `winning trades / total trades`, a winning trade being one with P&L above 0
  (`:517`). It is not a count of positive days.
- **Profit factor** is total profit over total loss; 999 when there is profit and no loss.
- **Average holding period** is the mean number of days from the position's open (or its last
  closing fill) to each closing fill.
- **Rolls.** A `ROLL` leg moves no tracked position and scores no trade; the open trade's entry
  price is carried across the roll by the gap between the two legs, and the leg's cost goes to the
  roll total (`:448`). A `BORROW` row is a cost and not a trade (`:457`).

The run's cost totals are computed separately, after netting, by
`transaction_cost::run_cost_totals` (`src/backtest/backtest_coordinator.cpp:460`).

---

## Data loading

`BacktestDataLoader` reads daily bars for the run's symbols and window from the database. A futures
run also loads history before the window to seed the trend estimators
(`trend_estimator::kHistoryCalendarDays`, `include/trade_ngin/strategy/trend_estimator.hpp:59`), so
the estimators do not start cold at the window's first row; the warm-up above still applies. Which
bars a cycle consumes and which it withholds is stated in
`include/trade_ngin/backtest/junk_signal_feed.hpp`. Where each table comes from and how it must be
read is in [docs/DATA_SOURCES_OF_TRUTH.md](../../docs/DATA_SOURCES_OF_TRUTH.md).

---

## What is stored

A run is stored in the `backtest` schema: `results`, `equity_curve`, `executions` and `run_metadata`
at the end of the run (`BacktestResultsManager`, `src/storage/backtest_results_manager.cpp`, called
from `BacktestCoordinator::save_portfolio_results_to_db`), and `final_positions` during the run, once
per bar date after the warm-up (`BacktestCoordinator::save_daily_positions`,
`src/backtest/backtest_coordinator.cpp:407-416`). `backtest.signals` exists and no run writes it: the
manager's `add_signals` has no caller. What one row of each table is, its key and who writes it are in
[docs/LIVE_RUN_CYCLE.md](../../docs/LIVE_RUN_CYCLE.md).

---

## Testing

Every test is built into one binary, `trade_ngin_tests`, and `ctest` lists each case by its suite
name, so filter on suite names:

```bash
cd build
ctest -R "^Backtest" --output-on-failure
```

---

## References

- [docs/OPTIMIZER_AND_RISK_DESIGN.md](../../docs/OPTIMIZER_AND_RISK_DESIGN.md)
- [docs/COST_MODEL.md](../../docs/COST_MODEL.md)
- [docs/FUTURES_ROLLS.md](../../docs/FUTURES_ROLLS.md)
- [docs/LIVE_RUN_CYCLE.md](../../docs/LIVE_RUN_CYCLE.md)
- [docs/CONFIG_GUIDE.md](../../docs/CONFIG_GUIDE.md)
- [Strategy Module](../strategy/README.md)
- [Portfolio Module](../portfolio/README.md)
- [Optimization Module](../optimization/README.md)
- [Transaction Cost Module](../transaction_cost/README.md)
- [Data Module](../data/README.md)
