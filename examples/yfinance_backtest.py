"""End-to-end backtest on yfinance data, with no database and no config files.

Run it from anywhere:

    pip install yfinance pandas
    python examples/yfinance_backtest.py

Pass --synthetic to run without network access; the synthetic path is what the
acceptance test uses, since it must pass with no Postgres reachable and no
./config tree present.
"""

from __future__ import annotations

import argparse
import datetime as dt
import math
import sys

import tradengin

from pandas_source import PandasDataSource, frame_from_yfinance, register_equities

SYMBOLS = ["AAPL", "MSFT"]


class MomentumStrategy(tradengin.BaseStrategy):
    """Go long whichever symbols closed above their own running average."""

    def __init__(self):
        super().__init__()
        self._history = {}

    def generate_positions_from_data(self, data):
        positions = []

        for bar in data:
            closes = self._history.setdefault(bar.symbol, [])
            closes.append(float(bar.close))
            if len(closes) > 50:
                del closes[0]

            # Warm up before trading.
            if len(closes) < 20:
                continue

            average = sum(closes) / len(closes)

            position = tradengin.Position()
            position.symbol = bar.symbol
            position.quantity = 100.0 if float(bar.close) > average else 0.0
            positions.append(position)

        return positions


def synthetic_frame(symbols, days=400):
    """Deterministic OHLCV data, so the example runs without network access."""
    import pandas as pd

    start = dt.datetime(2023, 1, 2)
    rows = []

    for index, symbol in enumerate(symbols):
        price = 100.0 * (index + 1)
        for day in range(days):
            # A slow sine wave plus drift gives trends to trade.
            price *= 1.0 + 0.02 * math.sin(day / 25.0) + 0.0004
            rows.append(
                {
                    "symbol": symbol,
                    "timestamp": start + dt.timedelta(days=day),
                    "open": price * 0.995,
                    "high": price * 1.01,
                    "low": price * 0.99,
                    "close": price,
                    "volume": 1_000_000.0,
                }
            )

    return pd.DataFrame(rows)


def build_config(frame, symbols):
    config = tradengin.BacktestRunConfig()
    config.symbols = symbols
    config.start_date = frame["timestamp"].min().to_pydatetime()
    config.end_date = frame["timestamp"].max().to_pydatetime()
    config.asset_class = tradengin.AssetClass.EQUITIES
    config.data_freq = tradengin.DataFrequency.DAILY
    config.data_source = PandasDataSource(frame)
    config.initial_capital = 1_000_000.0
    config.strategies = [tradengin.StrategySpec("MOMENTUM", 1.0)]

    # No database and an arbitrary working directory: keep both off.
    config.store_results = False
    config.export_csv = False

    return config


def run(frame, symbols):
    register_equities(symbols)

    runner = tradengin.BacktestRunner()
    runner.register_strategy("MOMENTUM", MomentumStrategy)
    runner.initialize_with_config("yfinance_demo", build_config(frame, symbols))

    return runner.run_backtest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--synthetic",
        action="store_true",
        help="use generated data instead of downloading from yfinance",
    )
    args = parser.parse_args()

    if args.synthetic:
        frame = synthetic_frame(SYMBOLS)
    else:
        frame = frame_from_yfinance(SYMBOLS, period="2y")

    print(f"Loaded {len(frame)} bars for {sorted(frame['symbol'].unique())}")

    results = run(frame, SYMBOLS)

    print(f"Total return: {results.total_return * 100:.2f}%")
    print(f"Sharpe ratio: {results.sharpe_ratio:.2f}")
    print(f"Max drawdown: {results.max_drawdown * 100:.2f}%")
    print(f"Total trades: {results.total_trades}")

    if results.total_trades == 0:
        print("No trades generated", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
