"""Feed a trade-ngin backtest from a pandas DataFrame.

This module has no dependency on the in-house data service. Any provider that
can produce a tidy OHLCV frame -- yfinance, a CSV file, a parquet dataset --
can drive a backtest through PandasDataSource.
"""

from __future__ import annotations

import tradengin

REQUIRED_COLUMNS = ("symbol", "timestamp", "open", "high", "low", "close", "volume")


class PandasDataSource(tradengin.MarketDataSource):
    """A MarketDataSource backed by a tidy pandas DataFrame.

    The frame must have one row per symbol per bar, with the columns listed in
    REQUIRED_COLUMNS. ``timestamp`` must be datetime-like.
    """

    def __init__(self, frame):
        super().__init__()

        missing = [column for column in REQUIRED_COLUMNS if column not in frame.columns]
        if missing:
            raise ValueError(f"DataFrame is missing required columns: {missing}")

        self._frame = frame.sort_values(["timestamp", "symbol"]).reset_index(drop=True)

    def load_bars(self, symbols, start_date, end_date, asset_class, freq):
        """Return the bars for ``symbols`` between ``start_date`` and ``end_date``."""
        frame = self._frame
        selected = frame[
            frame["symbol"].isin(list(symbols))
            & (frame["timestamp"] >= start_date)
            & (frame["timestamp"] <= end_date)
        ]

        return [
            tradengin.Bar(
                row.timestamp.to_pydatetime(),
                float(row.open),
                float(row.high),
                float(row.low),
                float(row.close),
                float(row.volume),
                str(row.symbol),
            )
            for row in selected.itertuples(index=False)
        ]

    def get_symbols(self, asset_class):
        """Return every symbol present in the frame."""
        return sorted(self._frame["symbol"].unique().tolist())


def frame_from_yfinance(tickers, **download_kwargs):
    """Download ``tickers`` with yfinance and reshape into a tidy OHLCV frame.

    Kept out of PandasDataSource so the library itself never depends on
    yfinance. Requires ``pip install yfinance``.
    """
    import pandas as pd
    import yfinance as yf

    tickers = list(tickers)
    raw = yf.download(
        tickers,
        group_by="ticker",
        auto_adjust=True,
        progress=False,
        **download_kwargs,
    )

    frames = []
    for ticker in tickers:
        # A single ticker comes back without the outer column level.
        sub = raw[ticker] if isinstance(raw.columns, pd.MultiIndex) else raw
        sub = sub.reset_index().rename(
            columns={
                "Date": "timestamp",
                "Open": "open",
                "High": "high",
                "Low": "low",
                "Close": "close",
                "Volume": "volume",
            }
        )
        sub["symbol"] = ticker
        frames.append(sub[list(REQUIRED_COLUMNS)])

    combined = pd.concat(frames, ignore_index=True)
    return combined.dropna(subset=["open", "high", "low", "close"])


def register_equities(symbols, exchange="NASDAQ", currency="USD", tick_size=0.01):
    """Register ``symbols`` as equities so the engine knows their contract size.

    Equities use a multiplier of 1.0, which EquityInstrument reports by
    default. Clearing first matters: the registry is a process-wide singleton,
    so a second run would otherwise reuse the first run's instruments.
    """
    registry = tradengin.InstrumentRegistry.instance()
    registry.clear()
    registry.initialize_without_database()

    for symbol in symbols:
        spec = tradengin.EquitySpec()
        spec.exchange = exchange
        spec.currency = currency
        spec.tick_size = tick_size
        registry.register_instrument(tradengin.EquityInstrument(symbol, spec))

    return registry
