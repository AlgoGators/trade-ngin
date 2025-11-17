#include "trade_ngin/strategy/regime_switching_fx_strategy.hpp"
#include "trade_ngin/core/logger.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <numeric>

namespace trade_ngin {

// Configuration Serialization

nlohmann::json RegimeSwitchingFXConfig::to_json() const {
    nlohmann::json j = StrategyConfig::to_json();

    j["volatility_window"] = volatility_window;
    j["momentum_lookback"] = momentum_lookback;
    j["ewmac_short_lookback"] = ewmac_short_lookback;
    j["ewmac_long_lookback"] = ewmac_long_lookback;
    j["zscore_lookback"] = zscore_lookback;
    j["regime_threshold"] = regime_threshold;
    j["num_long_positions"] = num_long_positions;
    j["num_short_positions"] = num_short_positions;
    j["use_volatility_scaling"] = use_volatility_scaling;
    j["momentum_rebalance_days"] = momentum_rebalance_days;
    j["mean_reversion_rebalance_days"] = mean_reversion_rebalance_days;
    j["stop_loss_pct"] = stop_loss_pct;
    j["symbols"] = symbols;

    return j;
}

void RegimeSwitchingFXConfig::from_json(const nlohmann::json& j) {
    StrategyConfig::from_json(j);

    if (j.contains("volatility_window")) volatility_window = j["volatility_window"];
    if (j.contains("momentum_lookback")) momentum_lookback = j["momentum_lookback"];
    if (j.contains("ewmac_short_lookback")) ewmac_short_lookback = j["ewmac_short_lookback"];
    if (j.contains("ewmac_long_lookback")) ewmac_long_lookback = j["ewmac_long_lookback"];
    if (j.contains("zscore_lookback")) zscore_lookback = j["zscore_lookback"];
    if (j.contains("regime_threshold")) regime_threshold = j["regime_threshold"];
    if (j.contains("num_long_positions")) num_long_positions = j["num_long_positions"];
    if (j.contains("num_short_positions")) num_short_positions = j["num_short_positions"];
    if (j.contains("use_volatility_scaling")) use_volatility_scaling = j["use_volatility_scaling"];
    if (j.contains("momentum_rebalance_days")) momentum_rebalance_days = j["momentum_rebalance_days"];
    if (j.contains("mean_reversion_rebalance_days")) mean_reversion_rebalance_days = j["mean_reversion_rebalance_days"];
    if (j.contains("stop_loss_pct")) stop_loss_pct = j["stop_loss_pct"];
    if (j.contains("symbols")) symbols = j["symbols"].get<std::vector<std::string>>();
}

// Constructor

RegimeSwitchingFXStrategy::RegimeSwitchingFXStrategy(
    std::string id,
    RegimeSwitchingFXConfig config,
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

    // Validate volatility window
    if (fx_config_.volatility_window < 30) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                               "Volatility window must be at least 30 days",
                               "RegimeSwitchingFX");
    }

    // Validate momentum lookback
    if (fx_config_.momentum_lookback <= 0) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                               "Momentum lookback must be positive",
                               "RegimeSwitchingFX");
    }

    // Validate EWMAC parameters
    if (fx_config_.ewmac_short_lookback <= 0 || fx_config_.ewmac_long_lookback <= 0) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                               "EWMAC lookback periods must be positive",
                               "RegimeSwitchingFX");
    }

    if (fx_config_.ewmac_short_lookback >= fx_config_.ewmac_long_lookback) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                               "EWMAC short lookback must be less than long lookback",
                               "RegimeSwitchingFX");
    }

    // Validate zscore lookback
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

    // Validate sufficient symbols for positions
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
            data.contract_size = 1.0;
            data.weight = 1.0 / static_cast<double>(fx_config_.symbols.size());

            // Reserve memory for history
            size_t reserve_size = fx_config_.volatility_window + fx_config_.zscore_lookback +
                                 std::max(fx_config_.momentum_lookback, fx_config_.ewmac_long_lookback) + 500;
            data.price_history.reserve(reserve_size);
            data.log_returns.reserve(reserve_size);
            data.rolling_volatilities.reserve(reserve_size);
            data.ewmac_values.reserve(reserve_size);

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

    DEBUG("on_data called with " + std::to_string(data.size()) + " bars");

    // DO NOT call base class on_data during backtests - it triggers database saves
    // that create an infinite loop with the portfolio manager
    // auto base_result = BaseStrategy::on_data(data);
    // if (base_result.is_error())
    //     return base_result;

    try {
        // Group data by symbol
        std::unordered_map<std::string, std::vector<Bar>> bars_by_symbol;
        for (const auto& bar : data) {
            // Validate bar data
            if (bar.symbol.empty() || bar.timestamp == Timestamp{} || bar.close <= 0.0) {
                return make_error<void>(ErrorCode::INVALID_DATA,
                                       "Invalid bar data",
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
                it->second.last_update = bar.timestamp;
            }
        }

        // Check if we have sufficient data for calculations
        if (!has_sufficient_data()) {
            DEBUG("Insufficient data for regime calculation (warm-up period)");
            return Result<void>();
        }

        // Update volatilities for all symbols
        DEBUG("Updating volatilities...");
        update_volatilities();

        // Update EWMAC values for all symbols
        DEBUG("Updating EWMAC values...");
        update_ewmac_values();

        // Update regime based on volatility dispersion
        DEBUG("Updating regime...");
        update_regime();

        // Check if we should rebalance
        bool rebalance = should_rebalance();

        if (rebalance) {
            std::string regime_str = (current_regime_ == RegimeSwitchingFXMarketRegime::MOMENTUM ? "MOMENTUM" :
                                      current_regime_ == RegimeSwitchingFXMarketRegime::MEAN_REVERSION ? "MEAN_REVERSION" : "UNDEFINED");
            DEBUG("Rebalancing positions in " + regime_str + " regime");

            // Generate signals based on current regime
            generate_signals(current_regime_);

            // Reset rebalance counter
            days_since_last_rebalance_ = 0;
        } else {
            // Increment days since last rebalance
            days_since_last_rebalance_++;
        }

        // Update positions for each symbol
        for (auto& [symbol, data] : instrument_data_) {
            double signal = data.current_signal;

            // Calculate position size
            double raw_position = calculate_position_size(symbol, signal);
            data.target_position = raw_position;

            // Apply volatility scaling if enabled
            double scaled_position = fx_config_.use_volatility_scaling ?
                                    apply_volatility_scaling(symbol, raw_position) : raw_position;

            // Apply risk controls
            double final_position = apply_risk_controls(symbol, scaled_position);
            data.scaled_position = final_position;

            // Store position internally only - DO NOT call update_position() here
            // The BacktestEngine/PortfolioManager will handle actual position updates
            // Calling update_position() from on_data() creates an infinite callback loop
            positions_[symbol].symbol = symbol;
            positions_[symbol].quantity = final_position;
            positions_[symbol].average_price = data.price_history.empty() ? 1.0 : data.price_history.back();
            positions_[symbol].last_update = data.last_update;

            // Don't call on_signal() from on_data() - causes infinite loop
            // The portfolio manager will pull signals when needed
        }

        return Result<void>();

    } catch (const std::exception& e) {
        ERROR("Error processing data in RegimeSwitchingFX: " + std::string(e.what()));
        return make_error<void>(ErrorCode::STRATEGY_ERROR,
                               std::string("Error processing data: ") + e.what(),
                               "RegimeSwitchingFX");
    }
}

// Statistical Utility Functions

double RegimeSwitchingFXStrategy::calculate_mean(const std::vector<double>& values) const {
    if (values.empty()) return 0.0;
    double sum = std::accumulate(values.begin(), values.end(), 0.0);
    return sum / values.size();
}

double RegimeSwitchingFXStrategy::calculate_variance(const std::vector<double>& values) const {
    if (values.size() < 2) return 0.0;
    double mean = calculate_mean(values);
    double sum_sq_diff = 0.0;
    for (double val : values) {
        sum_sq_diff += (val - mean) * (val - mean);
    }
    return sum_sq_diff / (values.size() - 1);  // Sample variance
}

double RegimeSwitchingFXStrategy::calculate_stdev(const std::vector<double>& values) const {
    double variance = calculate_variance(values);
    return std::sqrt(variance);
}

// Core Calculation Methods

std::vector<double> RegimeSwitchingFXStrategy::calculate_log_returns(
    const std::vector<double>& prices) const {

    std::vector<double> log_returns;
    log_returns.reserve(prices.size());

    for (size_t i = 1; i < prices.size(); i++) {
        if (prices[i] > 0.0 && prices[i-1] > 0.0) {
            double log_return = std::log(prices[i] / prices[i-1]);
            log_returns.push_back(log_return);
        } else {
            log_returns.push_back(0.0);
        }
    }

    return log_returns;
}

std::vector<double> RegimeSwitchingFXStrategy::calculate_rolling_volatility(
    const std::vector<double>& returns, int window) const {

    std::vector<double> volatilities;
    volatilities.reserve(returns.size());

    for (size_t t = window - 1; t < returns.size(); t++) {
        // Get window of returns
        std::vector<double> window_returns(returns.begin() + t - window + 1,
                                           returns.begin() + t + 1);

        // Calculate standard deviation
        double volatility = calculate_stdev(window_returns);
        volatilities.push_back(volatility);
    }

    return volatilities;
}

double RegimeSwitchingFXStrategy::calculate_volatility_dispersion(
    const std::vector<double>& volatilities) const {

    // Dispersion is the cross-sectional standard deviation of volatilities
    return calculate_stdev(volatilities);
}

double RegimeSwitchingFXStrategy::calculate_zscore(
    double value, const std::vector<double>& history) const {

    if (history.size() < 2) return 0.0;

    double mean = calculate_mean(history);
    double stdev = calculate_stdev(history);

    if (stdev > 0.0) {
        return (value - mean) / stdev;
    }
    return 0.0;
}

RegimeSwitchingFXMarketRegime RegimeSwitchingFXStrategy::determine_regime(
    double dispersion_zscore) const {

    // Low z-score (<0.5) indicates momentum regime
    if (dispersion_zscore < fx_config_.regime_threshold) {
        return RegimeSwitchingFXMarketRegime::MOMENTUM;
    }
    // High z-score (>0.5) indicates mean reversion regime
    else if (dispersion_zscore > fx_config_.regime_threshold) {
        return RegimeSwitchingFXMarketRegime::MEAN_REVERSION;
    }
    // Otherwise undefined
    return RegimeSwitchingFXMarketRegime::UNDEFINED;
}

double RegimeSwitchingFXStrategy::calculate_n_day_return(
    const std::vector<double>& prices, size_t current_idx, int lookback) const {

    if (current_idx < static_cast<size_t>(lookback) || prices.empty()) {
        return 0.0;
    }

    double current_price = prices[current_idx];
    double past_price = prices[current_idx - lookback];

    if (past_price > 0.0 && current_price > 0.0) {
        return std::log(current_price / past_price);
    }

    return 0.0;
}

std::vector<double> RegimeSwitchingFXStrategy::calculate_ewmac(
    const std::vector<double>& prices, int short_window, int long_window) const {

    std::vector<double> ewmac_values;
    ewmac_values.reserve(prices.size());

    if (prices.size() < static_cast<size_t>(long_window)) {
        return ewmac_values;
    }

    // Calculate EMAs
    double alpha_short = 2.0 / (short_window + 1.0);
    double alpha_long = 2.0 / (long_window + 1.0);

    // Initialize EMAs
    double ema_short = prices[0];
    double ema_long = prices[0];

    for (size_t i = 1; i < prices.size(); i++) {
        ema_short = alpha_short * prices[i] + (1.0 - alpha_short) * ema_short;
        ema_long = alpha_long * prices[i] + (1.0 - alpha_long) * ema_long;

        // EWMAC = short EMA - long EMA
        double ewmac = ema_short - ema_long;
        ewmac_values.push_back(ewmac);
    }

    return ewmac_values;
}

std::vector<std::pair<std::string, double>>
RegimeSwitchingFXStrategy::rank_by_performance() const {

    std::vector<std::pair<std::string, double>> rankings;

    for (const auto& [symbol, data] : instrument_data_) {
        if (data.price_history.size() >= static_cast<size_t>(fx_config_.momentum_lookback)) {
            size_t current_idx = data.price_history.size() - 1;
            double performance = calculate_n_day_return(
                data.price_history, current_idx, fx_config_.momentum_lookback);
            rankings.emplace_back(symbol, performance);
        }
    }

    // Sort by performance (descending - best performers first)
    std::sort(rankings.begin(), rankings.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    return rankings;
}

std::vector<std::pair<std::string, double>>
RegimeSwitchingFXStrategy::rank_by_ewmac() const {

    std::vector<std::pair<std::string, double>> rankings;

    for (const auto& [symbol, data] : instrument_data_) {
        if (!data.ewmac_values.empty()) {
            double ewmac = data.ewmac_values.back();
            rankings.emplace_back(symbol, ewmac);
        }
    }

    // Sort by EWMAC (ascending - most negative first)
    std::sort(rankings.begin(), rankings.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });

    return rankings;
}

void RegimeSwitchingFXStrategy::generate_signals(RegimeSwitchingFXMarketRegime regime) {
    // Reset all signals to 0
    for (auto& [symbol, data] : instrument_data_) {
        data.current_signal = 0.0;
    }

    if (regime == RegimeSwitchingFXMarketRegime::UNDEFINED) {
        // Keep current positions - don't change signals
        return;
    }

    if (regime == RegimeSwitchingFXMarketRegime::MOMENTUM) {
        // Momentum regime: Long top performers, short bottom performers
        auto rankings = rank_by_performance();

        if (rankings.size() >= static_cast<size_t>(
                fx_config_.num_long_positions + fx_config_.num_short_positions)) {

            // Long top performers
            for (int i = 0; i < fx_config_.num_long_positions; i++) {
                auto it = instrument_data_.find(rankings[i].first);
                if (it != instrument_data_.end()) {
                    it->second.current_signal = 1.0;
                }
            }

            // Short bottom performers
            for (int i = 0; i < fx_config_.num_short_positions; i++) {
                size_t idx = rankings.size() - 1 - i;
                auto it = instrument_data_.find(rankings[idx].first);
                if (it != instrument_data_.end()) {
                    it->second.current_signal = -1.0;
                }
            }

            DEBUG("Momentum: Long top " + std::to_string(fx_config_.num_long_positions) +
                  ", Short bottom " + std::to_string(fx_config_.num_short_positions));
        }
    }
    else if (regime == RegimeSwitchingFXMarketRegime::MEAN_REVERSION) {
        // Mean reversion regime: Use EWMAC contrarian logic
        // Most negative EWMAC (oversold) -> SHORT (expect reversion up)
        // Most positive EWMAC (overbought) -> LONG (expect reversion down)
        auto rankings = rank_by_ewmac();

        if (rankings.size() >= static_cast<size_t>(
                fx_config_.num_long_positions + fx_config_.num_short_positions)) {

            // Short the most negative EWMAC (oversold, expect bounce)
            for (int i = 0; i < fx_config_.num_short_positions; i++) {
                auto it = instrument_data_.find(rankings[i].first);
                if (it != instrument_data_.end()) {
                    it->second.current_signal = -1.0;
                }
            }

            // Long the most positive EWMAC (overbought, expect pullback)
            for (int i = 0; i < fx_config_.num_long_positions; i++) {
                size_t idx = rankings.size() - 1 - i;
                auto it = instrument_data_.find(rankings[idx].first);
                if (it != instrument_data_.end()) {
                    it->second.current_signal = 1.0;
                }
            }

            DEBUG("Mean Reversion: Short bottom " + std::to_string(fx_config_.num_short_positions) +
                  " EWMAC, Long top " + std::to_string(fx_config_.num_long_positions) + " EWMAC");
        }
    }
}

// Position Sizing and Risk Management

double RegimeSwitchingFXStrategy::calculate_position_size(
    const std::string& symbol, double signal) const {

    if (std::abs(signal) < 1e-6) {
        return 0.0;
    }

    auto it = instrument_data_.find(symbol);
    if (it == instrument_data_.end()) {
        return 0.0;
    }

    const auto& data = it->second;

    // Equal weight allocation across all positions
    int total_positions = fx_config_.num_long_positions + fx_config_.num_short_positions;
    if (total_positions == 0) {
        return 0.0;
    }

    double capital = config_.capital_allocation;
    double position_capital = capital / total_positions;

    // Get price
    double price = data.price_history.empty() ? 1.0 : data.price_history.back();
    if (price <= 0.0) price = 1.0;

    // Position sizing
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
    const std::string& symbol, double position) const {

    auto it = instrument_data_.find(symbol);
    if (it == instrument_data_.end()) {
        return position;
    }

    const auto& data = it->second;
    double volatility = data.current_volatility;

    if (volatility <= 0.0) {
        return position;
    }

    // Target volatility (daily)
    const double target_volatility = 0.01;  // 1% daily volatility target

    // Scale position inversely with volatility
    double volatility_scalar = target_volatility / volatility;
    volatility_scalar = std::clamp(volatility_scalar, 0.5, 2.0);

    return position * volatility_scalar;
}

double RegimeSwitchingFXStrategy::apply_risk_controls(
    const std::string& symbol, double position) const {

    // Apply position limits
    double position_limit = 1000.0;
    if (fx_config_.position_limits.count(symbol) > 0) {
        position_limit = fx_config_.position_limits.at(symbol);
    }
    position = std::clamp(position, -position_limit, position_limit);

    // Get current position
    double current_position = 0.0;
    auto pos_it = positions_.find(symbol);
    if (pos_it != positions_.end()) {
        current_position = static_cast<double>(pos_it->second.quantity);
    }

    // Check stop loss
    auto it = instrument_data_.find(symbol);
    if (it == instrument_data_.end() || it->second.price_history.empty()) {
        return std::round(position);
    }

    double current_price = it->second.price_history.back();

    if (std::abs(current_position) > 1e-6 && pos_it != positions_.end()) {
        double entry_price = static_cast<double>(pos_it->second.average_price);

        if (entry_price > 0.0) {
            double pnl_pct = (current_price - entry_price) / entry_price;

            // Check if stop loss triggered
            if ((current_position > 0 && pnl_pct < -fx_config_.stop_loss_pct) ||
                (current_position < 0 && pnl_pct > fx_config_.stop_loss_pct)) {
                WARN("Stop loss triggered for " + symbol);
                return 0.0;  // Close position
            }
        }
    }

    return std::round(position);
}

// Helper Methods

void RegimeSwitchingFXStrategy::update_price_history(
    const std::string& symbol, double price) {

    auto it = instrument_data_.find(symbol);
    if (it == instrument_data_.end()) {
        return;
    }

    it->second.price_history.push_back(price);

    // Limit history size
    const size_t MAX_HISTORY = fx_config_.volatility_window + fx_config_.zscore_lookback +
                               std::max(fx_config_.momentum_lookback, fx_config_.ewmac_long_lookback) + 1000;
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

    double current_price = data.price_history.back();
    double previous_price = data.price_history[data.price_history.size() - 2];

    if (previous_price > 0.0 && current_price > 0.0) {
        double log_return = std::log(current_price / previous_price);

        if (!std::isnan(log_return) && !std::isinf(log_return)) {
            data.log_returns.push_back(log_return);
        } else {
            data.log_returns.push_back(0.0);
        }
    } else {
        data.log_returns.push_back(0.0);
    }

    // Limit returns history size
    const size_t MAX_RETURNS = fx_config_.volatility_window + fx_config_.zscore_lookback + 1000;
    if (data.log_returns.size() > MAX_RETURNS) {
        data.log_returns.erase(data.log_returns.begin());
    }
}

void RegimeSwitchingFXStrategy::update_volatilities() {
    for (auto& [symbol, data] : instrument_data_) {
        if (data.log_returns.size() >= static_cast<size_t>(fx_config_.volatility_window)) {
            // Calculate rolling volatility
            data.rolling_volatilities = calculate_rolling_volatility(
                data.log_returns, fx_config_.volatility_window);

            if (!data.rolling_volatilities.empty()) {
                data.current_volatility = data.rolling_volatilities.back();
            }
        }
    }
}

void RegimeSwitchingFXStrategy::update_ewmac_values() {
    for (auto& [symbol, data] : instrument_data_) {
        if (data.price_history.size() >= static_cast<size_t>(fx_config_.ewmac_long_lookback)) {
            // Calculate EWMAC
            data.ewmac_values = calculate_ewmac(
                data.price_history,
                fx_config_.ewmac_short_lookback,
                fx_config_.ewmac_long_lookback);

            if (!data.ewmac_values.empty()) {
                data.current_ewmac = data.ewmac_values.back();
            }
        }
    }
}

void RegimeSwitchingFXStrategy::update_regime() {
    // Collect current volatilities across all symbols
    std::vector<double> current_volatilities;
    for (const auto& [symbol, data] : instrument_data_) {
        if (data.current_volatility > 0.0) {
            current_volatilities.push_back(data.current_volatility);
        }
    }

    if (current_volatilities.size() < 2) {
        return;  // Need at least 2 symbols for cross-sectional dispersion
    }

    // Calculate cross-sectional dispersion
    double dispersion = calculate_volatility_dispersion(current_volatilities);
    dispersion_history_.push_back(dispersion);

    // Limit dispersion history size
    if (dispersion_history_.size() > static_cast<size_t>(fx_config_.zscore_lookback + 500)) {
        dispersion_history_.erase(dispersion_history_.begin());
    }

    // Calculate z-score if we have enough history
    if (dispersion_history_.size() >= static_cast<size_t>(fx_config_.zscore_lookback)) {
        // Get last N dispersions for z-score calculation
        std::vector<double> window_dispersions(
            dispersion_history_.end() - fx_config_.zscore_lookback,
            dispersion_history_.end());

        dispersion_zscore_ = calculate_zscore(dispersion, window_dispersions);

        // Determine regime
        previous_regime_ = current_regime_;
        current_regime_ = determine_regime(dispersion_zscore_);

        DEBUG("Dispersion z-score: " + std::to_string(dispersion_zscore_) +
              ", Regime: " + (current_regime_ == RegimeSwitchingFXMarketRegime::MOMENTUM ? "MOMENTUM" :
                             current_regime_ == RegimeSwitchingFXMarketRegime::MEAN_REVERSION ? "MEAN_REVERSION" : "UNDEFINED"));
    }
}

bool RegimeSwitchingFXStrategy::has_sufficient_data() const {
    // Need enough data for volatility calculation and z-score
    size_t min_required = fx_config_.volatility_window + fx_config_.zscore_lookback + 1;

    for (const auto& [symbol, data] : instrument_data_) {
        if (data.price_history.size() < min_required) {
            return false;
        }
    }

    return true;
}

bool RegimeSwitchingFXStrategy::should_rebalance() const {
    // Rebalance on regime change
    if (current_regime_ != previous_regime_ &&
        previous_regime_ != RegimeSwitchingFXMarketRegime::UNDEFINED) {
        return true;
    }

    // Initial position entry
    if (previous_regime_ == RegimeSwitchingFXMarketRegime::UNDEFINED &&
        current_regime_ != RegimeSwitchingFXMarketRegime::UNDEFINED) {
        return true;
    }

    // Periodic rebalancing based on regime
    if (current_regime_ == RegimeSwitchingFXMarketRegime::MEAN_REVERSION &&
        days_since_last_rebalance_ >= fx_config_.mean_reversion_rebalance_days) {
        return true;
    }

    if (current_regime_ == RegimeSwitchingFXMarketRegime::MOMENTUM &&
        days_since_last_rebalance_ >= fx_config_.momentum_rebalance_days) {
        return true;
    }

    return false;
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