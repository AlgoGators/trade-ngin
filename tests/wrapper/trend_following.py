from tradengin import BaseStrategy, Position
from dataclasses import dataclass, field
from datetime import datetime
from decimal import Decimal

import math


def _safe(x, default):
    if math.isnan(x) or math.isinf(x):
        return default
    return x


@dataclass
class InstrumentData:
    # Static instrument properties
    contract_size: float = 1.0
    weight: float = 1.0

    # Dynamic forecast data
    raw_forecasts: list[float] = field(default_factory=list)
    scaled_forecasts: list[float] = field(default_factory=list)
    current_forecast: float = 0.0

    # Position data
    raw_position: float = 0.0
    final_position: float = 0.0

    # Market data
    price_history: list[float] = field(default_factory=list)
    volatility_history: list[float] = field(default_factory=list)
    current_volatility: float = 0.01


class TrendFollowingStrategy(BaseStrategy):
    def __init__(
        self,
        weight: float = 1.0,
        risk_target: float = 0.2,
        fx_rate: float = 1.0,
        idm: float = 2.5,
        max_symbol_concentration: float = 0.15,
        use_position_buffering: bool = True,
        ema_windows=None,
        vol_lookback_short: int = 32,
        vol_lookback_long: int = 2520,
        fdm=None,
    ):
        super().__init__()  # Necessary to initialize the base class

        # Scalars
        self.weight = weight
        self.risk_target = risk_target
        self.fx_rate = fx_rate
        self.idm = idm
        self.max_symbol_concentration = max_symbol_concentration
        self.use_position_buffering = use_position_buffering

        # List of (int, int)
        self.ema_windows = (
            ema_windows
            if ema_windows is not None
            else [
                (2, 8),
                (4, 16),
                (8, 32),
                (16, 64),
                (32, 128),
                (64, 256),
            ]
        )

        # Volatility params
        self.vol_lookback_short = vol_lookback_short
        self.vol_lookback_long = vol_lookback_long

        # List of (int, float)
        self.fdm = (
            fdm
            if fdm is not None
            else [
                (1, 1.0),
                (2, 1.03),
                (3, 1.08),
                (4, 1.13),
                (5, 1.19),
                (6, 1.26),
            ]
        )

        # Instrument data
        self.instrument_data: dict[str, InstrumentData] = {}

    def initialize(self):
        for symbol in self.config.trading_params:
            # Initialize positions with zero quantity
            pos = Position()
            pos.symbol = symbol
            pos.quantity = Decimal(0)
            pos.average_price = Decimal(0)
            pos.last_update = datetime.now()

            # TODO self.positions[symbol] = pos - doesn't work for now since the internal C++ is const
            self.update_position(symbol, pos)

    def get_price_history(self):
        return {
            symbol: data.price_history for symbol, data in self.instrument_data.items()
        }

    def calculate_ewma(self, prices: list[float], window: int) -> list[float]:
        if not prices or window <= 0:
            return [0.0] * len(prices)

        lam = 2.0 / (window + 1)
        ewma = [0.0] * len(prices)
        ewma[0] = prices[0]

        for i in range(1, len(prices)):
            ewma[i] = lam * prices[i] + (1 - lam) * ewma[i - 1]

        return ewma

    def ewma_standard_deviation(self, prices: list[float], window: int) -> list[float]:
        if len(prices) < 2 or window <= 0:
            return [0.01] * len(prices)

        returns = []
        for i in range(1, len(prices)):
            if prices[i - 1] <= 0 or prices[i] <= 0:
                returns.append(0.0)
            else:
                r = math.log(prices[i] / prices[i - 1])
                returns.append(_safe(r, 0.0))

        lam = 2.0 / (window + 1)

        mean = [0.0] * len(returns)
        var = [0.0] * len(returns)
        std = [0.0] * len(returns)

        mean[0] = returns[0]
        var[0] = returns[0] ** 2 * 0.1

        for t in range(1, len(returns)):
            mean[t] = lam * returns[t] + (1 - lam) * mean[t - 1]
            dev = _safe(returns[t] - mean[t], 0.0)
            var[t] = lam * (dev**2) + (1 - lam) * var[t - 1]
            var[t] = max(var[t], 1e-6)

            std[t] = math.sqrt(var[t]) * 16.0
            if std[t] <= 0 or math.isnan(std[t]) or math.isinf(std[t]):
                std[t] = 0.005
            elif std[t] > 5.0:
                std[t] = 5.0

        std[0] = std[1]

        result = [std[0]] + std
        return result[: len(prices)]

    def blended_ewma_stddev(
        self,
        prices: list[float],
        window: int,
        weight_short: float = 0.5,
        weight_long: float = 0.5,
        max_history: int = 2520,
    ) -> list[float]:

        if len(prices) < 2:
            return [0.01] * len(prices)

        ewma_std = self.ewma_standard_deviation(prices, window)
        ewma_std = (ewma_std + [ewma_std[-1]])[: len(prices)]

        blended = []
        history = []
        MIN_STD = 0.005

        for t in range(len(prices)):
            val = max(MIN_STD, _safe(ewma_std[t], MIN_STD))
            val = min(val, 5.0)
            history.append(val)

            long_avg = sum(history[-max_history:]) / max(1, len(history[-max_history:]))
            long_avg = max(MIN_STD, _safe(long_avg, MIN_STD))

            x = weight_short * val + weight_long * long_avg
            if x <= 0 or math.isnan(x) or math.isinf(x):
                x = MIN_STD

            blended.append(x)

        return blended

    def get_raw_forecast(self, prices, short_w, long_w):
        if len(prices) < max(short_w, long_w):
            return [0.0] * len(prices)

        short_ema = self.calculate_ewma(prices, short_w)
        long_ema = self.calculate_ewma(prices, long_w)

        vol = self.blended_ewma_stddev(prices, self.vol_lookback_short)

        if len(prices) >= 252:
            vol_mult = self.calculate_vol_regime_multiplier(prices, vol)
        else:
            vol_mult = 1.0

        out = [0.0] * len(prices)

        for i in range(len(prices)):
            v = max(vol[i], 0.10)
            p = max(prices[i], 1.0)

            out[i] = (short_ema[i] - long_ema[i]) / (p * v / 16.0)
            out[i] *= vol_mult

        return out

    def get_scaled_forecast(self, raw):
        if not raw:
            return []

        abs_avg = sum(abs(x) for x in raw) / len(raw)
        abs_avg = max(abs_avg, 1.0)

        return [max(-20.0, min(20.0, 10.0 * x / abs_avg)) for x in raw]

    def get_raw_combined_forecast(self, prices):
        combined = [0.0] * len(prices)
        count = 0

        for short_w, long_w in self.ema_windows:
            rf = self.get_raw_forecast(prices, short_w, long_w)
            if len(rf) != len(prices):
                continue

            # vol = self.blended_ewma_stddev(prices, short_w)
            scaled = self.get_scaled_forecast(rf)

            combined = [c + s for c, s in zip(combined, scaled)]
            count += 1

        if count > 0:
            combined = [c / count for c in combined]
        else:
            combined = []

        return combined

    def get_scaled_combined_forecast(self, raw):
        if not raw:
            return []

        num_rules = len(self.ema_windows)
        fdm = dict(self.fdm).get(num_rules, 1.0)

        return [max(-20.0, min(20.0, x * fdm)) for x in raw]

    def calculate_position(self, symbol, forecast, price, vol):
        if abs(forecast) > 20 or math.isnan(forecast):
            return 0.0

        price = max(price, 1.0)
        vol = max(vol, 0.01)

        data = self.instrument_data.get(symbol)
        if not data:
            return 0.0

        contract_size = data.contract_size
        weight = data.weight

        capital = max(1000.0, self.config.capital_allocation)
        idm = max(0.1, self.idm)
        risk = max(0.01, min(0.5, self.risk_target))
        fx = max(0.1, self.fx_rate)

        vol = max(0.10, min(2.0, vol))

        denom = max(10.0 * contract_size * price * fx * vol, 1.0)

        pos = (forecast * capital * weight * idm * risk) / denom

        if math.isnan(pos) or math.isinf(pos):
            pos = 0.0

        limit = self.config.position_limits.get(symbol)
        pos = max(-limit, min(limit, pos))

        notional = abs(pos * contract_size * price)

        max_notional = capital * self.max_symbol_concentration

        if notional > max_notional:
            scale = max_notional / notional
            pos *= scale

        return pos

    def apply_position_buffer(self, symbol, raw_pos, price, vol):
        if not self.use_position_buffering:
            return raw_pos

        if any(math.isnan(x) or math.isinf(x) for x in [raw_pos, price, vol]):
            return 0.0

        current = (
            float(self.positions.get(symbol, 0.0).quantity)
            if self.positions.get(symbol)
            else 0.0
        )  # TODO Temporary fix but still need to figure out why positions aren't initializing properly
        # current = float(self.positions.get(symbol).quantity)

        weight = max(0.0, self.weight)
        contract_size = self.instrument_data.get(symbol, InstrumentData()).contract_size

        buffer = (
            0.1
            * self.config.capital_allocation
            * self.idm
            * self.risk_target
            * weight
            / (contract_size * price * self.fx_rate * vol)
        )

        lower = raw_pos - buffer
        upper = raw_pos + buffer

        if current < lower:
            new_pos = round(lower)
        elif current > upper:
            new_pos = round(upper)
        else:
            new_pos = round(current)

        limit = self.config.position_limits.get(symbol)
        return max(-limit, min(limit, new_pos))

    def calculate_vol_regime_multiplier(self, prices, vol):
        if len(prices) < 252:
            return 2.0 / 3.0

        current = vol[-1]
        lookback = min(len(vol), 2520)

        hist = vol[-lookback:]
        avg = sum(hist) / len(hist)

        rel = current / avg
        rel_hist = sorted(v / avg for v in hist[:-1])

        rank = sum(1 for x in rel_hist if x <= rel) / len(rel_hist)

        raw = 2.0 - 1.5 * rank

        alpha = 2.0 / 11.0
        ewma = raw

        n = min(10, len(rel_hist))

        for i in range(10):
            hist_rel = rel_hist[-n + i]
            hist_q = sum(1 for x in rel_hist if x <= hist_rel) / len(rel_hist)
            hist_mult = 2.0 - 1.5 * hist_q

            ewma = alpha * hist_mult + (1 - alpha) * ewma

        return ewma

    def generate_positions_from_data(self, data):
        if not data:
            return

        bars_by_symbol = {}
        for bar in data:
            if not bar.symbol or bar.close <= 0:
                continue
            bars_by_symbol.setdefault(bar.symbol, []).append(bar)

        max_window = max(w[1] for w in self.ema_windows)

        result: list[Position] = []

        for symbol, bars in bars_by_symbol.items():
            # Use dataclass default if not present
            inst = self.instrument_data.setdefault(symbol, InstrumentData())

            # Update price history
            for bar in bars:
                inst.price_history.append(float(bar.close))
                if len(inst.price_history) > 2520:
                    inst.price_history.pop(0)

            prices_full = inst.price_history
            if len(prices_full) < max_window:
                continue
            prices = prices_full[-1000:] if len(prices_full) > 1000 else prices_full

            # Volatility
            inst.volatility_history = self.blended_ewma_stddev(
                prices, self.vol_lookback_short
            )[-2520:]
            inst.current_volatility = inst.volatility_history[-1]

            # Forecasts
            inst.raw_forecasts = self.get_raw_combined_forecast(prices)
            inst.scaled_forecasts = self.get_scaled_combined_forecast(
                inst.raw_forecasts
            )
            inst.current_forecast = (
                inst.scaled_forecasts[-1] if inst.scaled_forecasts else 0.0
            )

            # Load instruments if not yet cached
            if inst.contract_size == 1.0:
                # Look up instrument and cache contract size and weight
                lookup_symbol = symbol

                if ".v." in symbol:
                    lookup_symbol = symbol[: symbol.find(".v.")]

                if self.registry and self.registry.has_instrument(lookup_symbol):
                    instrument = self.registry.get_instrument(lookup_symbol)

                    if instrument:
                        inst.contract_size = instrument.get_multiplier()
                    else:
                        print(
                            f"Warning: Instrument {lookup_symbol} not found in registry."
                        )

            # Position calculation
            raw_pos = self.calculate_position(
                symbol, inst.current_forecast, prices[-1], inst.current_volatility
            )
            final_pos = (
                self.apply_position_buffer(
                    symbol, raw_pos, prices[-1], inst.current_volatility
                )
                if self.use_position_buffering
                else raw_pos
            )
            inst.raw_position = raw_pos
            inst.final_position = final_pos

            # Signal callback
            self.on_signal(symbol, inst.current_forecast)

            # Commit position to C++
            prev = self.positions.get(symbol)
            prev_qty = float(prev.quantity) if prev else 0
            prev_avg = float(prev.avg_price) if prev else prices[-1]
            prev_pnl = float(prev.realized_pnl) if prev else 0.0
            # TODO not ideal to have to convert back and forth between Decimal and float - should we just use Decimal everywhere? Or is it better to use float internally and only convert to Decimal when creating the Position object to pass back to C++?

            pnl, avg_price = 0.0, prices[-1]

            current_price = prices[-1]

            same_direction = (
                prev_qty != 0
                and final_pos != 0
                and (
                    (prev_qty > 0 and final_pos > 0) or (prev_qty < 0 and final_pos < 0)
                )
            )

            position_reduced = abs(final_pos) < abs(prev_qty)
            position_increased = abs(final_pos) > abs(prev_qty)
            position_flipped = prev_qty * final_pos < 0
            position_closed = prev_qty != 0 and final_pos == 0

            if same_direction:

                # Reduced existing position
                if position_reduced:
                    closed_qty = abs(prev_qty) - abs(final_pos)

                    direction = 1.0 if prev_qty > 0 else -1.0

                    pnl = closed_qty * direction * (current_price - prev_avg)

                    avg_price = prev_avg

                # Added to existing position
                elif position_increased:
                    added_qty = abs(final_pos) - abs(prev_qty)

                    avg_price = (
                        abs(prev_qty) * prev_avg + added_qty * current_price
                    ) / abs(final_pos)

            # Fully flipped direction
            elif position_flipped:

                closed_qty = abs(prev_qty)

                direction = 1.0 if prev_qty > 0 else -1.0

                pnl = closed_qty * direction * (current_price - prev_avg)

                avg_price = current_price

            # Fully closed position
            elif position_closed:

                pnl = prev_qty * (current_price - prev_avg)

                avg_price = current_price

            # TODO we're forced to default initialize unless we pass in everything I think
            new_position = Position()
            new_position.symbol = symbol
            new_position.quantity = Decimal(final_pos).quantize(Decimal("0.00000000"))
            new_position.avg_price = Decimal(avg_price).quantize(Decimal("0.00000000"))
            new_position.realized_pnl = Decimal(prev_pnl + pnl).quantize(
                Decimal("0.00000000")
            )

            result.append(new_position)
            # print(
            #     {
            #         "symbol": symbol,
            #         "price": prices[-1],
            #         "forecast": inst.current_forecast,
            #         "vol": inst.current_volatility,
            #         "raw_pos": raw_pos,
            #         "final_pos": final_pos,
            #         "prev_qty": prev_qty,
            #         "prev_avg": prev_avg,
            #         "avg_price": avg_price,
            #         "pnl": pnl,
            #         "realized": prev_pnl + pnl,
            #     }
            # )

        return result
