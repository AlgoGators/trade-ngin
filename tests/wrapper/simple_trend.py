# TODO update to newer API
from tradengin import BaseStrategy, Position
from datetime import datetime
from decimal import Decimal


class SimpleTrendStrategy(BaseStrategy):
    def __init__(
        self,
        short_window: int = 10,
        long_window: int = 50,
        weight: float = 1.0,
    ):
        super().__init__()

        self.short_window = short_window
        self.long_window = long_window
        self.weight = weight

        # per-symbol price history
        self.prices: dict[str, list[float]] = {}

    def initialize(self):
        # Initialize positions and price history
        self.price_history = {}
        self.volatility_history = {}

        for symbol in self.config.trading_params:
            self.price_history[symbol] = []
            self.volatility_history[symbol] = []

            # Initialize positions with zero quantity
            pos = Position()
            pos.symbol = symbol
            pos.quantity = Decimal(0)
            pos.average_price = Decimal(0)
            pos.last_update = datetime.now()

            # TODO self.positions[symbol] = pos - doesn't work for now since the internal C++ is const
            self.update_position(symbol, pos)

    def on_data(self, data):
        if not data:
            return

        # group bars by symbol
        for bar in data:
            if not bar.symbol or bar.close <= 0:
                continue

            sym = bar.symbol
            px = float(bar.close)

            if sym not in self.prices:
                self.prices[sym] = []

            hist = self.prices[sym]
            hist.append(px)

            # keep bounded memory
            if len(hist) > 2000:
                hist.pop(0)

            if len(hist) < self.long_window:
                continue

            short = hist[-self.short_window :]
            long = hist[-self.long_window :]

            short_ma = sum(short) / len(short)
            long_ma = sum(long) / len(long)

            if long_ma <= 0:
                continue

            # simple trend signal
            forecast = (short_ma - long_ma) / long_ma

            # linear position mapping
            raw_pos = self.weight * forecast

            # optional: enforce position limits if present in config
            limit = self.config.position_limits.get(sym, None)
            if limit is not None:
                raw_pos = max(-limit, min(limit, raw_pos))

            # commit position
            pos = Position()
            pos.symbol = sym
            pos.quantity = Decimal(raw_pos).quantize(Decimal("0.00000000"))
            pos.avg_price = Decimal(px).quantize(Decimal("0.00000000"))

            self.update_position(sym, pos)
