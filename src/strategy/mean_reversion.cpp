// src/strategy/mean_reversion.cpp
#include "trade_ngin/strategy/mean_reversion.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace trade_ngin {

MeanReversionStrategy::MeanReversionStrategy(std::string id, StrategyConfig config,
                                             MeanReversionConfig mr_config,
                                             std::shared_ptr<PostgresDatabase> db,
                                             std::shared_ptr<InstrumentRegistry> registry)
    : BaseStrategy(std::move(id), std::move(config), std::move(db)),
      mr_config_(std::move(mr_config)),
      registry_(registry) {
    Logger::register_component("MeanReversion");

    metadata_.name = "Mean Reversion Strategy";
    metadata_.description = "Z-score based mean reversion strategy";
}

Result<void> MeanReversionStrategy::validate_config() const {
    auto result = BaseStrategy::validate_config();
    if (result.is_error())
        return result;

    if (mr_config_.lookback_period < 2) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Lookback period must be at least 2",
                                "MeanReversionStrategy");
    }

    if (mr_config_.entry_threshold <= 0.0) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Entry threshold must be positive",
                                "MeanReversionStrategy");
    }

    if (mr_config_.risk_target <= 0.0 || mr_config_.risk_target > 1.0) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Risk target must be between 0 and 1",
                                "MeanReversionStrategy");
    }

    return Result<void>();
}

Result<void> MeanReversionStrategy::initialize() {
    auto base_result = BaseStrategy::initialize();
    if (base_result.is_error()) {
        ERROR("Base strategy initialization failed: " + std::string(base_result.error()->what()));
        return base_result;
    }

    set_pnl_accounting_method(PnLAccountingMethod::MIXED);
    INFO("Mean reversion strategy initialized with MIXED PnL accounting");

    try {
        for (const auto& [symbol, _] : config_.trading_params) {
            MeanReversionInstrumentData data;
            data.price_history.reserve(
                static_cast<size_t>(std::max(mr_config_.lookback_period, mr_config_.vol_lookback) * 2));
            data.volatility_history.reserve(
                static_cast<size_t>(mr_config_.vol_lookback * 2));
            instrument_data_[symbol] = data;

            Position pos;
            pos.symbol = symbol;
            pos.quantity = 0.0;
            pos.average_price = 0.0;
            pos.last_update = std::chrono::system_clock::now();
            positions_[symbol] = pos;
        }

        INFO("Mean reversion strategy initialized for " + std::to_string(instrument_data_.size()) +
             " symbols");
        return Result<void>();

    } catch (const std::exception& e) {
        ERROR("Error in MeanReversionStrategy::initialize: " + std::string(e.what()));
        return make_error<void>(ErrorCode::NOT_INITIALIZED,
                                "Failed to initialize mean reversion strategy: " +
                                    std::string(e.what()),
                                "MeanReversionStrategy");
    }
}

Result<void> MeanReversionStrategy::on_data(const std::vector<Bar>& data) {
    if (state_ != StrategyState::RUNNING) {
        return make_error<void>(ErrorCode::STRATEGY_ERROR,
                                "Strategy is not in running state",
                                "MeanReversionStrategy");
    }

    try {
        std::unordered_map<std::string, double> signals;

        for (const auto& bar : data) {
            auto& inst_data = instrument_data_[bar.symbol];

            // Update price history
            inst_data.price_history.push_back(bar.close.as_double());
            inst_data.current_price = bar.close.as_double();
            inst_data.last_update = bar.timestamp;

            // FIX (MAJOR #1): Trim price/volatility history to prevent unbounded memory growth
            trim_history(inst_data);

            // Need enough data for calculations
            if (inst_data.price_history.size() < static_cast<size_t>(mr_config_.lookback_period)) {
                DEBUG("Insufficient data for " + bar.symbol + ": " +
                      std::to_string(inst_data.price_history.size()) + "/" +
                      std::to_string(mr_config_.lookback_period));
                continue;
            }

            // Calculate indicators
            inst_data.moving_average = calculate_sma(inst_data.price_history, mr_config_.lookback_period);
            inst_data.std_deviation = calculate_std_dev(inst_data.price_history, mr_config_.lookback_period, inst_data.moving_average);
            inst_data.z_score = calculate_z_score(bar.close.as_double(), inst_data.moving_average, inst_data.std_deviation);

            // Calculate volatility
            inst_data.current_volatility = calculate_volatility(inst_data.price_history, mr_config_.vol_lookback);
            inst_data.volatility_history.push_back(inst_data.current_volatility);

            // Generate signal
            double signal = generate_signal(bar.symbol, inst_data);
            signals[bar.symbol] = signal;

            // Calculate position size
            if (std::abs(signal) > 0.01) {
                double position_size = calculate_position_size(bar.symbol, bar.close.as_double(), inst_data.current_volatility);
                double new_target = signal * position_size;
                inst_data.target_position = new_target;
            } else {
                inst_data.target_position = 0.0;
            }

            // Update position quantity in base class
            // DO NOT set average_price here -- on_execution() manages cost basis
            Position pos = positions_[bar.symbol];
            pos.quantity = Quantity(inst_data.target_position);
            pos.last_update = bar.timestamp;
            positions_[bar.symbol] = pos;

            DEBUG("Symbol: " + bar.symbol +
                  " | Price: " + std::to_string(bar.close) +
                  " | MA: " + std::to_string(inst_data.moving_average) +
                  " | Z-Score: " + std::to_string(inst_data.z_score) +
                  " | Signal: " + std::to_string(signal) +
                  " | Position: " + std::to_string(inst_data.target_position));
        }

        // Update unrealized PnL for all positions based on current prices
        // (BaseStrategy::on_data does this but we override it, so compute here)
        for (const auto& bar : data) {
            auto pos_it = positions_.find(bar.symbol);
            if (pos_it != positions_.end()) {
                pos_it->second.unrealized_pnl =
                    (bar.close - pos_it->second.average_price) * pos_it->second.quantity;
            }
        }

        return Result<void>();

    } catch (const std::exception& e) {
        ERROR("Error in MeanReversionStrategy::on_data: " + std::string(e.what()));
        return make_error<void>(ErrorCode::UNKNOWN_ERROR,
                                "Failed to process market data: " + std::string(e.what()),
                                "MeanReversionStrategy");
    }
}

void MeanReversionStrategy::trim_history(MeanReversionInstrumentData& data) const {
    size_t max_price_size = static_cast<size_t>(
        std::max(mr_config_.lookback_period, mr_config_.vol_lookback) * 2);
    while (data.price_history.size() > max_price_size) {
        data.price_history.erase(data.price_history.begin());
    }

    size_t max_vol_size = static_cast<size_t>(mr_config_.vol_lookback * 2);
    while (data.volatility_history.size() > max_vol_size) {
        data.volatility_history.erase(data.volatility_history.begin());
    }
}

double MeanReversionStrategy::calculate_sma(const std::vector<double>& prices, int period) const {
    if (prices.empty() || period <= 0 || prices.size() < static_cast<size_t>(period)) {
        return 0.0;
    }

    double sum = 0.0;
    for (size_t i = prices.size() - period; i < prices.size(); ++i) {
        sum += prices[i];
    }
    return sum / period;
}

double MeanReversionStrategy::calculate_std_dev(const std::vector<double>& prices, int period, double mean) const {
    if (prices.empty() || period <= 0 || prices.size() < static_cast<size_t>(period)) {
        return 0.0;
    }

    // Population std dev (n) for technical indicators (z-score, Bollinger Bands)
    double sum_squared_diff = 0.0;
    for (size_t i = prices.size() - period; i < prices.size(); ++i) {
        double diff = prices[i] - mean;
        sum_squared_diff += diff * diff;
    }
    return std::sqrt(sum_squared_diff / period);
}

double MeanReversionStrategy::calculate_z_score(double price, double mean, double std_dev) const {
    if (std_dev < 1e-8) {
        return 0.0;
    }
    return (price - mean) / std_dev;
}

double MeanReversionStrategy::calculate_position_size(const std::string& symbol, double price, double volatility) const {
    if (price < 1e-8 || volatility < 1e-8) {
        return 0.0;
    }

    double capital = config_.capital_allocation;
    double target_value = capital * mr_config_.position_size;

    // Adjust for volatility - reduce position if volatility is high
    double vol_scalar = mr_config_.risk_target / std::max(volatility, 0.01);
    vol_scalar = std::min(vol_scalar, 2.0);   // Cap at 2x
    vol_scalar = std::max(vol_scalar, 0.25);  // Floor at 0.25x

    double position_value = target_value * vol_scalar;
    double num_shares = position_value / price;

    // Fractional shares: disabled for stocks below min price threshold
    // Industry standard: $1.00 (exchange listing maintenance, SEC Rule 612 boundary)
    if (mr_config_.allow_fractional_shares && price >= mr_config_.fractional_min_price) {
        num_shares = std::round(num_shares * 1000000.0) / 1000000.0;
    } else {
        num_shares = std::floor(num_shares);
    }

    // FIX (MAJOR #2): Enforce position limits
    auto limit_it = config_.position_limits.find(symbol);
    if (limit_it != config_.position_limits.end()) {
        num_shares = std::min(num_shares, limit_it->second);
    }

    return num_shares;
}

double MeanReversionStrategy::calculate_volatility(const std::vector<double>& prices, int lookback) const {
    if (prices.size() < 2 || lookback < 2) {
        return 0.01;  // Default 1% volatility
    }

    size_t start_idx = prices.size() > static_cast<size_t>(lookback)
                       ? prices.size() - lookback
                       : 0;

    std::vector<double> returns;
    for (size_t i = start_idx + 1; i < prices.size(); ++i) {
        if (prices[i-1] > 0) {
            double ret = std::log(prices[i] / prices[i-1]);
            returns.push_back(ret);
        }
    }

    if (returns.empty()) {
        return 0.01;
    }

    double mean = std::accumulate(returns.begin(), returns.end(), 0.0) / returns.size();
    double sum_squared_diff = 0.0;
    for (double ret : returns) {
        double diff = ret - mean;
        sum_squared_diff += diff * diff;
    }
    // Sample std dev (n-1) for risk/volatility estimation (position sizing)
    double std_dev = std::sqrt(sum_squared_diff / (returns.size() - 1));

    // Annualize (assuming daily data, 252 trading days)
    return std_dev * std::sqrt(252.0);
}

double MeanReversionStrategy::generate_signal(const std::string& symbol, const MeanReversionInstrumentData& data) const {
    double current_position = 0.0;
    auto pos_it = positions_.find(symbol);
    if (pos_it != positions_.end()) {
        current_position = pos_it->second.quantity.as_double();
    }

    // Entry signals
    if (std::abs(current_position) < 1e-6) {
        if (data.z_score > mr_config_.entry_threshold) {
            return -1.0;  // Price too high - go short
        } else if (data.z_score < -mr_config_.entry_threshold) {
            return 1.0;   // Price too low - go long
        }
        return 0.0;
    }

    // Exit signals — use pos.average_price (maintained by on_execution) for stop-loss
    double avg_price = static_cast<double>(pos_it->second.average_price);

    if (current_position > 0) {
        if (data.z_score > -mr_config_.exit_threshold) {
            return 0.0;  // Mean reversion complete - exit long
        }
        if (mr_config_.use_stop_loss && avg_price > 0) {
            double pnl_pct = (data.current_price - avg_price) / avg_price;
            if (pnl_pct < -mr_config_.stop_loss_pct) {
                return 0.0;  // Stop loss hit
            }
        }
        return 1.0;  // Hold long
    }

    if (current_position < 0) {
        if (data.z_score < mr_config_.exit_threshold) {
            return 0.0;  // Mean reversion complete - exit short
        }
        if (mr_config_.use_stop_loss && avg_price > 0) {
            double pnl_pct = (avg_price - data.current_price) / avg_price;
            if (pnl_pct < -mr_config_.stop_loss_pct) {
                return 0.0;  // Stop loss hit
            }
        }
        return -1.0;  // Hold short
    }

    return 0.0;
}

}  // namespace trade_ngin
