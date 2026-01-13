// src/strategy/mean_reversion.cpp
#include "trade_ngin/strategy/mean_reversion.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
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

    // Initialize metadata
    metadata_.name = "Mean Reversion Strategy";
    metadata_.description = "Z-score based mean reversion strategy";
}

Result<void> MeanReversionStrategy::validate_config() const {
    auto result = BaseStrategy::validate_config();
    if (result.is_error())
        return result;

    // Validate mean reversion specific config
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
    // Call base class initialization first
    auto base_result = BaseStrategy::initialize();
    if (base_result.is_error()) {
        ERROR("Base strategy initialization failed: " + std::string(base_result.error()->what()));
        return base_result;
    }

    // Set PnL accounting method for equities
    set_pnl_accounting_method(PnLAccountingMethod::REALIZED_ONLY);
    INFO("Mean reversion strategy initialized with REALIZED_ONLY PnL accounting");

    try {
        // Initialize data structures for each symbol
        for (const auto& [symbol, _] : config_.trading_params) {
            MeanReversionInstrumentData data;
            data.price_history.reserve(mr_config_.lookback_period * 3);
            data.volatility_history.reserve(mr_config_.vol_lookback * 3);
            instrument_data_[symbol] = data;

            // Initialize positions with zero quantity
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
                inst_data.target_position = signal * position_size;

                // Track entry price for stop loss
                if (inst_data.target_position != 0.0 && positions_[bar.symbol].quantity.as_double() == 0.0) {
                    inst_data.entry_price = bar.close.as_double();
                }
            } else {
                inst_data.target_position = 0.0;
            }

            // Update position in base class
            Position pos = positions_[bar.symbol];
            pos.quantity = Quantity(inst_data.target_position);
            pos.average_price = bar.close;
            pos.last_update = bar.timestamp;
            positions_[bar.symbol] = pos;

            DEBUG("Symbol: " + bar.symbol +
                  " | Price: " + std::to_string(bar.close) +
                  " | MA: " + std::to_string(inst_data.moving_average) +
                  " | Z-Score: " + std::to_string(inst_data.z_score) +
                  " | Signal: " + std::to_string(signal) +
                  " | Position: " + std::to_string(inst_data.target_position));
        }

        // Save signals
        if (!signals.empty()) {
            auto save_result = save_signals(signals);
            if (save_result.is_error()) {
                WARN("Failed to save signals: " + std::string(save_result.error()->what()));
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

    // Simple position sizing based on risk target
    // Target position value as fraction of capital
    double capital = config_.capital_allocation;
    double target_value = capital * mr_config_.position_size;

    // Adjust for volatility - reduce position if volatility is high
    double vol_scalar = mr_config_.risk_target / std::max(volatility, 0.01);
    vol_scalar = std::min(vol_scalar, 2.0);  // Cap at 2x
    vol_scalar = std::max(vol_scalar, 0.25);  // Floor at 0.25x

    double position_value = target_value * vol_scalar;
    double num_shares = position_value / price;

    return std::floor(num_shares);  // Round down to whole shares for equities
}

double MeanReversionStrategy::calculate_volatility(const std::vector<double>& prices, int lookback) const {
    if (prices.size() < 2 || lookback < 2) {
        return 0.01;  // Default 1% volatility
    }

    size_t start_idx = prices.size() > static_cast<size_t>(lookback)
                       ? prices.size() - lookback
                       : 0;

    // Calculate returns
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

    // Calculate standard deviation of returns
    double mean = std::accumulate(returns.begin(), returns.end(), 0.0) / returns.size();
    double sum_squared_diff = 0.0;
    for (double ret : returns) {
        double diff = ret - mean;
        sum_squared_diff += diff * diff;
    }
    double std_dev = std::sqrt(sum_squared_diff / returns.size());

    // Annualize (assuming daily data, 252 trading days)
    return std_dev * std::sqrt(252.0);
}

double MeanReversionStrategy::generate_signal(const std::string& symbol, const MeanReversionInstrumentData& data) const {
    // Get current position
    double current_position = 0.0;
    auto pos_it = positions_.find(symbol);
    if (pos_it != positions_.end()) {
        current_position = pos_it->second.quantity.as_double();
    }

    // Entry signals
    if (std::abs(current_position) < 1e-6) {  // Currently flat
        if (data.z_score > mr_config_.entry_threshold) {
            // Price too high - go short
            return -1.0;
        } else if (data.z_score < -mr_config_.entry_threshold) {
            // Price too low - go long
            return 1.0;
        }
        return 0.0;
    }

    // Exit signals
    if (current_position > 0) {  // Currently long
        // Exit if z-score moves back above exit threshold (mean reversion complete)
        if (data.z_score > -mr_config_.exit_threshold) {
            return 0.0;  // Exit long
        }

        // Stop loss check
        if (mr_config_.use_stop_loss && data.entry_price > 0) {
            double pnl_pct = (data.current_price - data.entry_price) / data.entry_price;
            if (pnl_pct < -mr_config_.stop_loss_pct) {
                return 0.0;  // Stop loss hit
            }
        }

        return 1.0;  // Hold long
    }

    if (current_position < 0) {  // Currently short
        // Exit if z-score moves back below -exit threshold (mean reversion complete)
        if (data.z_score < mr_config_.exit_threshold) {
            return 0.0;  // Exit short
        }

        // Stop loss check
        if (mr_config_.use_stop_loss && data.entry_price > 0) {
            double pnl_pct = (data.entry_price - data.current_price) / data.entry_price;
            if (pnl_pct < -mr_config_.stop_loss_pct) {
                return 0.0;  // Stop loss hit
            }
        }

        return -1.0;  // Hold short
    }

    return 0.0;
}

}  // namespace trade_ngin
