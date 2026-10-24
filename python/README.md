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

A run must say what its risk layer is: the engine has no default. Set `no_risk_reason`,
`no_risk_ruled_by` and `no_risk_ruled_on` on the `BacktestRunConfig` for a run with none.

See `examples/yfinance_backtest.py` in the source repository for a complete database-free run:
https://github.com/AlgoGators/trade-ngin

Licensed under the GNU General Public License v3.
