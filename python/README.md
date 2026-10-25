# tradengin

Python bindings for the AlgoGators **trade-ngin** backtest engine.

```python
import tradengin
```

- `BacktestRunner` runs a backtest, either from a portfolio config tree plus the engine's
  database, or fully argument-driven from a `BacktestRunConfig` with no database.
- `MarketDataSource` is the base class for your own data provider (a DataFrame, a CSV, a vendor).
  Subclass it and return `Bar` objects from `load_bars()`.
- Strategies subclass `tradengin.BaseStrategy` in Python.

The engine's own trend-following sleeve is available as `register_trend_following_strategy()`
(`fast=True` for the FAST sleeve), built exactly as the engine's runners build it. To run a book as
the engine runs it, point `BacktestRunConfig.config_base_path` at a config directory in the
engine's layout (`defaults.json` and `portfolios/<name>/portfolio.json`, `risk.json`, ...) and give
`initialize_with_config` the portfolio's name: the book's files then decide the capital, sleeves,
risk modules, overlay limits and the rest, and the run equals the engine's own on the same bars
(`tests/backtest/test_wrapper_parity.cpp` checks this to the last bit). The sleeve weights its
contracts by sector; give your source the same data with `get_contract_sectors()`. Bars that carry
a vendor `instrument_id` let the session classifier judge contract-id changes as the database run
does.

A run must say what its risk layer is: the engine has no default. Set `no_risk_reason`,
`no_risk_ruled_by` and `no_risk_ruled_on` on the `BacktestRunConfig` for a run with none.

See `examples/yfinance_backtest.py` in the source repository for a complete database-free run:
https://github.com/AlgoGators/trade-ngin

Licensed under the GNU General Public License v3.
