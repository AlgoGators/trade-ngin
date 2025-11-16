#include "trade_ngin/strategy/regime_switching_fx_strategy.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <numeric>

namespace trade_ngin {

// Configuration Serialization

nlohmann::json RegimeSwitchingFXConfig::to_json() const {
    nlohmann::json j = StrategyConfig::to_json();

    j["volatility_window"] = volatility_window;
    j["performance_lookback"] = performance_lookback;
    j["low_dispersion_threshold"] = low_dispersion_threshold;
    j["high_dispersion_threshold"] = high_dispersion_threshold;
    j["zscore_lookback"] = zscore_lookback;
    j["num_long_positions"] = num_long_positions;
    j["num_short_positions"] = num_short_positions;
    j["use_volatility_scaling"] = use_volatility_scaling;
    j["stop_loss_pct"] = stop_loss_pct;
    j["symbols"] = symbols;

    return j;
}

void RegimeSwitchingFXConfig::from_json(const nlohmann::json& j) {
    StrategyConfig::from_json(j);

    if (j.contains("volatility_window")) volatility_window = j["volatility_window"];
    if (j.contains("performance_lookback")) performance_lookback = j["performance_lookback"];
    if (j.contains("low_dispersion_threshold")) low_dispersion_threshold = j["low_dispersion_threshold"];
    if (j.contains("high_dispersion_threshold")) high_dispersion_threshold = j["high_dispersion_threshold"];
    if (j.contains("zscore_lookback")) zscore_lookback = j["zscore_lookback"];
    if (j.contains("num_long_positions")) num_long_positions = j["num_long_positions"];
    if (j.contains("num_short_positions")) num_short_positions = j["num_short_positions"];
    if (j.contains("use_volatility_scaling")) use_volatility_scaling = j["use_volatility_scaling"];
    if (j.contains("stop_loss_pct")) stop_loss_pct = j["stop_loss_pct"];
    if (j.contains("symbols")) symbols = j["symbols"].get<std::vector<std::string>>();
}

// Constructor

RegimeSwitchingFXStrategy::RegimeSwitchingFXStrategy(
    std::string& id,
    RegimeSwitchingFXConfig& config,
    std::shared_ptr<PostgresDatabase> db)
    : BaseStrategy(std::move(id), config, std::move(db)),
      fx_config_(std::move(config)) {

    // Register logging component
    Logger::register_component("RegimeSwitchingFX");

    // Set metadata
    metadata_.name = "Regime Switching FX Strategy";
    metadata_.description = "Adaptive strategy switching between momentum and mean reversion based on volatility dispersion";

    INFO("RegimeSwitchingFX strategy constructed with " +
         std::to_string(fx_config_.symbols.size()) + " symbols");
}

// Configuration Validation

Result<void> RegimeSwitchingFXStrategy::validate_config() const {
    // Call base validation first
    auto result = BaseStrategy::validate_config();
    if (result.is_error())
        return result;

    // Validate volatility window (must be at least 30 days for rolling volatility)
    if (fx_config_.volatility_window < 30) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                               "Volatility window must be at least 30 days",
                               "RegimeSwitchingFX");
    }

    // Validate performance lookback
    if (fx_config_.performance_lookback <= 0) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                               "Performance lookback must be positive",
                               "RegimeSwitchingFX");
    }

    // Validate zscore lookback (must be at least 60 days)
    if (fx_config_.zscore_lookback < 60) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                               "Z-score lookback must be at least 60 days",
                               "RegimeSwitchingFX");
    }

    // Validate position counts
    if (fx_config_.num_long_positions <= 0 || fx_config_.num_short_positions <= 0) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                               "Number of positions must be positive",
                               "RegimeSwitchingFX");
    }

    // Validate stop loss
    if (fx_config_.stop_loss_pct <= 0.0 || fx_config_.stop_loss_pct > 1.0) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                               "Stop loss must be between 0 and 1",
                               "RegimeSwitchingFX");
    }

    // Validate symbols
    if (fx_config_.symbols.empty()) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                               "Must specify at least one symbol",
                               "RegimeSwitchingFX");
    }

    // Validate that we can actually select the requested number of positions
    if (fx_config_.num_long_positions + fx_config_.num_short_positions >
        static_cast<int>(fx_config_.symbols.size())) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                               "Total positions requested exceeds number of symbols",
                               "RegimeSwitchingFX");
    }

    return Result<void>();
}

// Initialization

Result<void> RegimeSwitchingFXStrategy::initialize() {
    // Call base class initialization
    auto base_result = BaseStrategy::initialize();
    if (base_result.is_error()) {
        ERROR("Base strategy initialization failed: " +
              std::string(base_result.error()->what()));
        return base_result;
    }

    INFO("Initializing RegimeSwitchingFX strategy...");

    try {
        // Initialize data structures for each symbol
        for (const auto& symbol : fx_config_.symbols) {
            // Initialize instrument data
            RegimeSwitchingFXData data;
            data.symbol = symbol;
            data.contract_size = 1.0;  // Will be updated from registry if available
            data.weight = 1.0 / static_cast<double>(fx_config_.symbols.size());

            // Reserve memory for price history
            // Need: volatility_window + zscore_lookback + buffer
            size_t reserve_size = fx_config_.volatility_window + fx_config_.zscore_lookback + 500;
            data.price_history.reserve(reserve_size);
            data.returns.reserve(reserve_size);

            instrument_data_[symbol] = std::move(data);

            // Initialize position in base class
            Position pos;
            pos.symbol = symbol;
            pos.quantity = 0.0;
            pos.average_price = 1.0;
            pos.last_update = std::chrono::system_clock::now();
            positions_[symbol] = pos;

            INFO("Initialized data structures for " + symbol);
        }

        // Reserve memory for dispersion history
        dispersion_history_.reserve(fx_config_.zscore_lookback + 500);

        INFO("RegimeSwitchingFX strategy initialized successfully with " +
             std::to_string(fx_config_.symbols.size()) + " symbols");

        return Result<void>();

    } catch (const std::exception& e) {
        ERROR("Error in RegimeSwitchingFX::initialize: " + std::string(e.what()));
        return make_error<void>(
            ErrorCode::STRATEGY_ERROR,
            std::string("Failed to initialize regime switching strategy: ") + e.what(),
            "RegimeSwitchingFX");
    }
}

// Main Data Processing

Result<void> RegimeSwitchingFXStrategy::on_data(const std::vector<Bar>& data) {
    // Validate data
    if (data.empty()) {
        return Result<void>();
    }

    // Call base class data processing
    auto base_result = BaseStrategy::on_data(data);
    if (base_result.is_error())
        return base_result;

    try {
        // Group data by symbol
        std::unordered_map<std::string, std::vector<Bar>> bars_by_symbol;
        for (const auto& bar : data) {
            // Validate bar data
            if (bar.symbol.empty()) {
                return make_error<void>(ErrorCode::INVALID_DATA,
                                       "Bar has empty symbol",
                                       "RegimeSwitchingFX");
            }

            if (bar.timestamp == Timestamp{}) {
                return make_error<void>(ErrorCode::INVALID_DATA,
                                       "Bar has invalid timestamp",
                                       "RegimeSwitchingFX");
            }

            if (bar.close <= 0.0) {
                return make_error<void>(ErrorCode::INVALID_DATA,
                                       "Invalid bar data for symbol " + bar.symbol,
                                       "RegimeSwitchingFX");
            }

            bars_by_symbol[bar.symbol].push_back(bar);
        }

        // Update price history and returns for each symbol
        for (const auto& [symbol, symbol_bars] : bars_by_symbol) {
            auto it = instrument_data_.find(symbol);
            if (it == instrument_data_.end()) {
                WARN("Received data for unknown symbol: " + symbol);
                continue;
            }

            for (const auto& bar : symbol_bars) {
                double price = static_cast<double>(bar.close);
                update_price_history(symbol, price);
                update_returns(symbol);
            }
        }

        // Check if we have sufficient data
        if (!has_sufficient_data()) {
            // Add detailed logging to understand warm-up period
            size_t min_required_prices = fx_config_.volatility_window + 1;
            for (const auto& [symbol, data] : instrument_data_) {
                if (data.price_history.size() < min_required_prices) {
                    DEBUG("Symbol " + symbol + " has " +
                          std::to_string(data.price_history.size()) + " prices, needs " +
                          std::to_string(min_required_prices) + " (warm-up period)");
                }
            }
            return Result<void>();  // Exit early - don't process until we have enough data
        }

        // STEP 1: Calculate rolling volatilities for all symbols
        std::vector<double> current_volatilities;
        current_volatilities.reserve(instrument_data_.size());

        for (auto& [symbol, data] : instrument_data_) {
            if (data.returns.size() >= static_cast<size_t>(fx_config_.volatility_window)) {
                // Get recent returns for volatility calculation
                // Window: [t-W+1, t-W+2, ..., t]
                std::vector<double> window_returns(
                    data.returns.end() - fx_config_.volatility_window,
                    data.returns.end()
                );

                data.realized_volatility = calculate_realized_volatility(window_returns);
                current_volatilities.push_back(data.realized_volatility);

                DEBUG("Symbol " + symbol + " rolling volatility: " +
                      std::to_string(data.realized_volatility));
            } else {
                // This shouldn't happen due to has_sufficient_data() check, but be safe
                WARN("Symbol " + symbol + " has insufficient returns for volatility calculation: " +
                     std::to_string(data.returns.size()) + "/" +
                     std::to_string(fx_config_.volatility_window));
                return Result<void>();  // Exit if any symbol lacks data
            }
        }

        // Safety check: Ensure we have volatilities for all symbols
        if (current_volatilities.size() != instrument_data_.size()) {
            WARN("Not all symbols have volatility calculated. Skipping this iteration.");
            return Result<void>();
        }

        // STEP 2: Calculate cross-sectional volatility dispersion (D_t)
        // This is the standard deviation across symbols at time t
        if (current_volatilities.size() < 2) {
            WARN("Need at least 2 symbols to calculate cross-sectional dispersion");
            return Result<void>();
        }

        double dispersion = calculate_volatility_dispersion(current_volatilities);
        dispersion_history_.push_back(dispersion);

        // Limit dispersion history size
        const size_t MAX_DISPERSION_HISTORY = fx_config_.zscore_lookback * 3;
        if (dispersion_history_.size() > MAX_DISPERSION_HISTORY) {
            dispersion_history_.erase(dispersion_history_.begin());
        }

        DEBUG("Cross-sectional volatility dispersion: " + std::to_string(dispersion));

        // STEP 3: Calculate z-score of current dispersion
        // Need at least zscore_lookback points to calculate z-score
        if (dispersion_history_.size() >= static_cast<size_t>(fx_config_.zscore_lookback)) {
            std::vector<double> recent_dispersion(
                dispersion_history_.end() - fx_config_.zscore_lookback,
                dispersion_history_.end()
            );
            dispersion_zscore_ = calculate_zscore(dispersion, recent_dispersion);

            DEBUG("Dispersion z-score: " + std::to_string(dispersion_zscore_));
        } else {
            dispersion_zscore_ = 0.0;
            DEBUG("Not enough dispersion history for z-score calculation: " +
                  std::to_string(dispersion_history_.size()) + "/" +
                  std::to_string(fx_config_.zscore_lookback));
            return Result<void>();  // Wait for more data
        }

        // STEP 4: Determine current regime based on z-score
        RegimeSwitchingFXMarketRegime previous_regime = current_regime_;
        current_regime_ = determine_regime(dispersion_zscore_);

        if (previous_regime != current_regime_) {
            INFO("Regime change detected: " +
                 std::to_string(static_cast<int>(previous_regime)) + " -> " +
                 std::to_string(static_cast<int>(current_regime_)) +
                 " (dispersion z-score: " + std::to_string(dispersion_zscore_) + ")");
        }

        // STEP 5: Calculate recent performance for ranking
        for (auto& [symbol, data] : instrument_data_) {
            if (data.price_history.size() > static_cast<size_t>(fx_config_.performance_lookback)) {
                data.recent_return = calculate_recent_return(
                    data.price_history,
                    fx_config_.performance_lookback
                );

                DEBUG("Symbol " + symbol + " recent return: " +
                      std::to_string(data.recent_return));
            }
        }

        // STEP 6: Generate signals based on current regime
        generate_signals(current_regime_);

        // STEP 7: Calculate and apply positions
        for (auto& [symbol, data] : instrument_data_) {
            // Calculate raw position
            double raw_position = calculate_position_size(symbol, data.current_signal);
            data.target_position = raw_position;

            // Apply volatility scaling if enabled
            double scaled_position = raw_position;
            if (fx_config_.use_volatility_scaling) {
                scaled_position = apply_volatility_scaling(symbol, raw_position);
            }

            // Apply risk controls
            double final_position = apply_risk_controls(symbol, scaled_position);
            data.scaled_position = final_position;

            // Update position in base class
            Position pos;
            pos.symbol = symbol;
            pos.quantity = final_position;
            pos.last_update = bars_by_symbol[symbol].back().timestamp;

            // Get current price for average price
            pos.average_price = static_cast<double>(bars_by_symbol[symbol].back().close);

            auto pos_result = update_position(symbol, pos);
            if (pos_result.is_error()) {
                WARN("Failed to update position for " + symbol + ": " +
                     pos_result.error()->what());
            }

            // Save signal
            auto signal_result = on_signal(symbol, data.current_signal);
            if (signal_result.is_error()) {
                WARN("Failed to save signal for " + symbol + ": " +
                     signal_result.error()->what());
            }
        }

        INFO("Data processed successfully. Regime: " +
              std::to_string(static_cast<int>(current_regime_)) +
              ", Dispersion: " + std::to_string(dispersion) +
              ", Z-score: " + std::to_string(dispersion_zscore_));

        return Result<void>();

    } catch (const std::exception& e) {
        ERROR("Error processing data in RegimeSwitchingFX: " + std::string(e.what()));
        return make_error<void>(
            ErrorCode::STRATEGY_ERROR,
            std::string("Error processing data: ") + e.what(),
            "RegimeSwitchingFX");
    }
}

// Volatility Calculations

double RegimeSwitchingFXStrategy::calculate_realized_volatility(
    const std::vector<double>& returns) const {

    if (returns.size() < 2) {
        return 0.01;  // Default volatility
    }

    // Calculate sample variance: sum((x_i - x_bar)^2) / (n - 1)
    double mean = std::accumulate(returns.begin(), returns.end(), 0.0) / returns.size();

    double sum_sq_diff = 0.0;
    for (double ret : returns) {
        double diff = ret - mean;
        sum_sq_diff += diff * diff;
    }
    double variance = sum_sq_diff / (returns.size() - 1);  // Sample variance (n-1)

    // Calculate standard deviation
    double daily_vol = std::sqrt(variance);

    // They use daily volatility directly for dispersion calculation
    // We'll return daily volatility to match their implementation

    // Ensure volatility is reasonable
    return std::clamp(daily_vol, 0.0001, 0.1);  // Daily volatility bounds
}

double RegimeSwitchingFXStrategy::calculate_volatility_dispersion(
    const std::vector<double>& volatilities) const {

    if (volatilities.size() < 2) {
        return 0.0;
    }

    // Cross-sectional dispersion = standard deviation across symbols at time t
    // Calculate mean
    double mean = std::accumulate(volatilities.begin(), volatilities.end(), 0.0) /
                  volatilities.size();

    // Calculate sample variance
    double sum_sq_diff = 0.0;
    for (double vol : volatilities) {
        double diff = vol - mean;
        sum_sq_diff += diff * diff;
    }
    double variance = sum_sq_diff / (volatilities.size() - 1);  // Sample variance

    // Return standard deviation (dispersion)
    return std::sqrt(variance);
}

double RegimeSwitchingFXStrategy::calculate_zscore(
    double value,
    const std::vector<double>& history) const {

    if (history.empty()) {
        return 0.0;
    }

    // Z-score = (D_t - mu_D) / sigma_D
    // where mu_D and sigma_D are calculated from the rolling window

    // Calculate mean (mu_D)
    double mean = std::accumulate(history.begin(), history.end(), 0.0) / history.size();

    // Calculate sample standard deviation (sigma_D)
    double sum_sq_diff = 0.0;
    for (double val : history) {
        double diff = val - mean;
        sum_sq_diff += diff * diff;
    }
    double variance = sum_sq_diff / (history.size() - 1);  // Sample variance
    double std_dev = std::sqrt(variance);

    if (std_dev <= 0.0) {
        return 0.0;
    }

    return (value - mean) / std_dev;
}

// Regime Detection

RegimeSwitchingFXMarketRegime RegimeSwitchingFXStrategy::determine_regime(double dispersion_zscore) const {
    // Logic:
    // if z_score < 0: MOMENTUM
    // elif z_score > 0: MEAN_REVERTING
    // else: UNDEFINED

    // We extend this with configurable thresholds for robustness
    if (dispersion_zscore < fx_config_.low_dispersion_threshold) {
        // Low dispersion z-score -> MOMENTUM regime
        return RegimeSwitchingFXMarketRegime::MOMENTUM;
    } else if (dispersion_zscore > fx_config_.high_dispersion_threshold) {
        // High dispersion z-score -> MEAN REVERSION regime
        return RegimeSwitchingFXMarketRegime::MEAN_REVERSION;
    } else {
        // Undefined regime - neutral zone
        return RegimeSwitchingFXMarketRegime::UNDEFINED;
    }
}

// Performance Calculations

double RegimeSwitchingFXStrategy::calculate_recent_return(
    const std::vector<double>& prices,
    int lookback) const {

    if (prices.size() < static_cast<size_t>(lookback + 1)) {
        return 0.0;
    }

    // N-day log return: ln(P_t / P_{t-N})
    double current_price = prices.back();
    double past_price = prices[prices.size() - lookback - 1];

    if (past_price <= 0.0 || current_price <= 0.0) {
        return 0.0;
    }

    return std::log(current_price / past_price);
}

std::vector<std::pair<std::string, double>>
RegimeSwitchingFXStrategy::rank_by_performance() const {

    std::vector<std::pair<std::string, double>> rankings;
    rankings.reserve(instrument_data_.size());

    for (const auto& [symbol, data] : instrument_data_) {
        rankings.emplace_back(symbol, data.recent_return);
    }

    // Sort by recent return (descending)
    std::sort(rankings.begin(), rankings.end(),
             [](const auto& a, const auto& b) { return a.second > b.second; });

    return rankings;
}


// Signal Generation

void RegimeSwitchingFXStrategy::generate_signals(RegimeSwitchingFXMarketRegime regime) {
    if (regime == RegimeSwitchingFXMarketRegime::UNDEFINED) {
        // In undefined regime, set all signals to zero (flat)
        for (auto& [symbol, data] : instrument_data_) {
            data.current_signal = 0.0;
        }
        INFO("Undefined regime - setting all signals to zero");
        return;
    }

    // Rank instruments by recent performance (descending order)
    auto rankings = rank_by_performance();

    // Clear all signals first
    for (auto& [symbol, data] : instrument_data_) {
        data.current_signal = 0.0;
    }

    // Logic for position selection:
    if (regime == RegimeSwitchingFXMarketRegime::MOMENTUM) {
        // MOMENTUM: Buy top 2 performers, short bottom 2 performers
        INFO("Generating MOMENTUM signals");

        // Long the top N performers
        for (int i = 0; i < fx_config_.num_long_positions &&
                        i < static_cast<int>(rankings.size()); ++i) {
            const auto& [symbol, performance] = rankings[i];
            instrument_data_[symbol].current_signal = 1.0;
            DEBUG("MOMENTUM Long: " + symbol + " (return: " +
                  std::to_string(performance) + ")");
        }

        // Short the bottom N performers
        for (int i = 0; i < fx_config_.num_short_positions &&
                        i < static_cast<int>(rankings.size()); ++i) {
            int idx = rankings.size() - 1 - i;
            const auto& [symbol, performance] = rankings[idx];
            instrument_data_[symbol].current_signal = -1.0;
            DEBUG("MOMENTUM Short: " + symbol + " (return: " +
                  std::to_string(performance) + ")");
        }

    } else if (regime == RegimeSwitchingFXMarketRegime::MEAN_REVERSION) {
        // MEAN REVERSION: Short top 2 performers, buy bottom 2 performers
        INFO("Generating MEAN REVERSION signals");

        // Short the top N performers (expecting mean reversion down)
        for (int i = 0; i < fx_config_.num_short_positions &&
                        i < static_cast<int>(rankings.size()); ++i) {
            const auto& [symbol, performance] = rankings[i];
            instrument_data_[symbol].current_signal = -1.0;
            DEBUG("MEAN_REV Short: " + symbol + " (return: " +
                  std::to_string(performance) + ")");
        }

        // Long the bottom N performers (expecting mean reversion up)
        for (int i = 0; i < fx_config_.num_long_positions &&
                        i < static_cast<int>(rankings.size()); ++i) {
            int idx = rankings.size() - 1 - i;
            const auto& [symbol, performance] = rankings[idx];
            instrument_data_[symbol].current_signal = 1.0;
            DEBUG("MEAN_REV Long: " + symbol + " (return: " +
                  std::to_string(performance) + ")");
        }
    }
}

// Position Sizing (Equal Weight Allocation)

double RegimeSwitchingFXStrategy::calculate_position_size(
    const std::string& symbol,
    double signal) const {

    if (std::abs(signal) < 1e-6) {
        return 0.0;
    }

    auto it = instrument_data_.find(symbol);
    if (it == instrument_data_.end()) {
        WARN("No instrument data for " + symbol);
        return 0.0;
    }

    const auto& data = it->second;

    // Equal weight allocation across all positions
    // Total positions = num_long + num_short
    int total_positions = fx_config_.num_long_positions + fx_config_.num_short_positions;
    if (total_positions == 0) {
        return 0.0;
    }

    double capital = config_.capital_allocation;
    double position_capital = capital / total_positions;

    // Get price
    double price = data.price_history.empty() ? 1.0 : data.price_history.back();
    if (price <= 0.0) price = 1.0;

    // Position sizing: (signal * position_capital) / (price * contract_size)
    double contract_size = data.contract_size;
    if (contract_size <= 0.0) contract_size = 1.0;

    double position = (signal * position_capital) / (price * contract_size);

    // Apply position limits
    double position_limit = 1000.0;
    if (config_.position_limits.count(symbol) > 0) {
        position_limit = config_.position_limits.at(symbol);
    }

    return std::clamp(position, -position_limit, position_limit);
}

double RegimeSwitchingFXStrategy::apply_volatility_scaling(
    const std::string& symbol,
    double position) const {

    auto it = instrument_data_.find(symbol);
    if (it == instrument_data_.end()) {
        return position;
    }

    const auto& data = it->second;
    double volatility = data.realized_volatility;

    if (volatility <= 0.0) {
        return position;
    }

    // Target volatility (daily)
    const double target_volatility = 0.01;  // 1% daily volatility target

    // Scale position inversely with volatility
    double volatility_scalar = target_volatility / volatility;
    volatility_scalar = std::clamp(volatility_scalar, 0.5, 2.0);  // Limit scaling

    double scaled_position = position * volatility_scalar;

    DEBUG("Volatility scaling for " + symbol + ": vol=" +
          std::to_string(volatility) + ", scalar=" +
          std::to_string(volatility_scalar) + ", position: " +
          std::to_string(position) + " -> " + std::to_string(scaled_position));

    return scaled_position;
}

// Risk Controls

double RegimeSwitchingFXStrategy::apply_risk_controls(
    const std::string& symbol,
    double position) const {

    // Get current position
    double current_position = 0.0;
    auto pos_it = positions_.find(symbol);
    if (pos_it != positions_.end()) {
        current_position = static_cast<double>(pos_it->second.quantity);
    }

    // Get current price and entry price
    auto it = instrument_data_.find(symbol);
    if (it == instrument_data_.end() || it->second.price_history.empty()) {
        return std::round(position);
    }

    double current_price = it->second.price_history.back();

    // Check stop loss if we have a position
    if (std::abs(current_position) > 1e-6 && pos_it != positions_.end()) {
        double entry_price = static_cast<double>(pos_it->second.average_price);

        if (entry_price > 0.0) {
            double pnl_pct = (current_price - entry_price) / entry_price;

            // Check if we need to stop out
            bool stop_triggered = false;
            if (current_position > 0 && pnl_pct < -fx_config_.stop_loss_pct) {
                // Long position hit stop loss
                stop_triggered = true;
                WARN("Stop loss triggered for LONG " + symbol +
                     ": PnL=" + std::to_string(pnl_pct * 100) + "%");
            } else if (current_position < 0 && pnl_pct > fx_config_.stop_loss_pct) {
                // Short position hit stop loss
                stop_triggered = true;
                WARN("Stop loss triggered for SHORT " + symbol +
                     ": PnL=" + std::to_string(pnl_pct * 100) + "%");
            }

            if (stop_triggered) {
                // Close position
                return 0.0;
            }
        }
    }

    // Round to whole contracts
    return std::round(position);
}

// Helper Methods

void RegimeSwitchingFXStrategy::update_price_history(
    const std::string& symbol,
    double price) {

    auto it = instrument_data_.find(symbol);
    if (it == instrument_data_.end()) {
        return;
    }

    it->second.price_history.push_back(price);

    // Limit history size
    const size_t MAX_HISTORY = fx_config_.volatility_window + fx_config_.zscore_lookback + 1000;
    if (it->second.price_history.size() > MAX_HISTORY) {
        it->second.price_history.erase(it->second.price_history.begin());
    }
}

void RegimeSwitchingFXStrategy::update_returns(const std::string& symbol) {
    auto it = instrument_data_.find(symbol);
    if (it == instrument_data_.end()) {
        return;
    }

    auto& data = it->second;

    if (data.price_history.size() < 2) {
        return;
    }

    // Calculate log return: r_i,t = ln(P_i,t / P_i,t-1)
    double current_price = data.price_history.back();
    double previous_price = data.price_history[data.price_history.size() - 2];

    if (previous_price <= 0.0 || current_price <= 0.0) {
        data.returns.push_back(0.0);
    } else {
        double log_return = std::log(current_price / previous_price);

        // Check for valid return
        if (std::isnan(log_return) || std::isinf(log_return)) {
            data.returns.push_back(0.0);
        } else {
            data.returns.push_back(log_return);
        }
    }

    // Limit returns history size
    const size_t MAX_RETURNS = fx_config_.volatility_window + fx_config_.zscore_lookback + 1000;
    if (data.returns.size() > MAX_RETURNS) {
        data.returns.erase(data.returns.begin());
    }
}

bool RegimeSwitchingFXStrategy::has_sufficient_data() const {

    size_t min_required_prices = fx_config_.volatility_window + 1;

    for (const auto& [symbol, data] : instrument_data_) {
        if (data.price_history.size() < min_required_prices) {
            return false;
        }
    }

    return true;
}

// Public Accessor Methods

double RegimeSwitchingFXStrategy::get_signal(const std::string& symbol) const {
    auto it = instrument_data_.find(symbol);
    if (it != instrument_data_.end()) {
        return it->second.current_signal;
    }
    return 0.0;
}

double RegimeSwitchingFXStrategy::get_position(const std::string& symbol) const {
    auto it = instrument_data_.find(symbol);
    if (it != instrument_data_.end()) {
        return it->second.scaled_position;
    }
    return 0.0;
}

const RegimeSwitchingFXData* RegimeSwitchingFXStrategy::get_instrument_data(
    const std::string& symbol) const {
    auto it = instrument_data_.find(symbol);
    if (it != instrument_data_.end()) {
        return &it->second;
    }
    return nullptr;
}

}