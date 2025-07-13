// src/strategy/regime_detector.cpp

#include "trade_ngin/strategy/regime_detector.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/state_manager.hpp"

namespace trade_ngin {

RegimeDetector::RegimeDetector(RegimeDetectorConfig config) : config_(std::move(config)) {
    // Register with state manager
    ComponentInfo info{ComponentType::STRATEGY,
                       ComponentState::INITIALIZED,
                       "REGIME_DETECTOR",
                       "",
                       std::chrono::system_clock::now(),
                       {{"lookback_period", static_cast<double>(config_.lookback_period)},
                        {"confidence_threshold", config_.confidence_threshold}}};

    auto register_result = StateManager::instance().register_component(info);
    if (register_result.is_error()) {
        throw std::runtime_error(register_result.error()->what());
    }
}

Result<void> RegimeDetector::initialize() {
    try {
        // Initialize ML models if enabled
        if (config_.use_machine_learning) {
            // TODO: Implement ML model loading
        }

        // Update state
        auto state_result =
            StateManager::instance().update_state("REGIME_DETECTOR", ComponentState::RUNNING);

        if (state_result.is_error()) {
            return state_result;
        }

        INFO("Regime Detector initialized successfully");
        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::NOT_INITIALIZED,
                                std::string("Failed to initialize regime detector: ") + e.what(),
                                "RegimeDetector");
    }
}

Result<void> RegimeDetector::update(const std::vector<Bar>& data) {
    try {
        std::lock_guard<std::mutex> lock(mutex_);

        // Group data by symbol and update histories
        for (const auto& bar : data) {
            price_history_[bar.symbol].push_back(static_cast<double>(bar.close));
            volume_history_[bar.symbol].push_back(bar.volume);

            // Maintain lookback window
            if (price_history_[bar.symbol].size() > static_cast<size_t>(config_.lookback_period)) {
                price_history_[bar.symbol].erase(price_history_[bar.symbol].begin());
                volume_history_[bar.symbol].erase(volume_history_[bar.symbol].begin());
            }

            // Skip regime detection if we don't have enough data
            if (price_history_[bar.symbol].size() < static_cast<size_t>(config_.lookback_period)) {
                continue;
            }

            // Calculate features
            RegimeFeatures features;
            const auto& prices = price_history_[bar.symbol];

            features.trend_strength = calculate_trend_strength(prices);
            features.mean_reversion_strength = calculate_mean_reversion(prices);
            features.volatility = calculate_volatility(prices);
            features.hurst_exponent = calculate_hurst(prices);

            // Detect regime change
            DetailedMarketRegime new_regime = detect_regime_change(features, bar.symbol);

            // Update regime information
            auto& current_result = current_regimes_[bar.symbol];
            bool regime_changed = new_regime != current_result.current_regime;

            if (regime_changed && validate_regime_change(bar.symbol, new_regime, features)) {
                // Store previous regime
                current_result.previous_regime = current_result.current_regime;
                current_result.current_regime = new_regime;
                current_result.last_change = bar.timestamp;
                current_result.regime_duration = 0;
                current_result.change_reason = "Feature-based detection";

                // Add to history
                regime_history_[bar.symbol].push_back(current_result);

                INFO("Regime change detected for " + bar.symbol + ": " +
                     std::to_string(static_cast<int>(current_result.previous_regime)) + " -> " +
                     std::to_string(static_cast<int>(new_regime)));
            } else {
                current_result.regime_duration++;
            }

            // Update features and probabilities
            current_result.features = features;
            current_result.regime_probability =
                calculate_change_probability(features, current_result.current_regime);
        }

        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::UNKNOWN_ERROR,
                                std::string("Error updating regime detector: ") + e.what(),
                                "RegimeDetector");
    }
}

Result<RegimeDetectionResult> RegimeDetector::get_regime(const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = current_regimes_.find(symbol);
    if (it == current_regimes_.end()) {
        return make_error<RegimeDetectionResult>(ErrorCode::INVALID_ARGUMENT,
                                                 "No regime information for symbol: " + symbol,
                                                 "RegimeDetector");
    }

    return Result<RegimeDetectionResult>(it->second);
}

Result<RegimeFeatures> RegimeDetector::get_features(const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = current_regimes_.find(symbol);
    if (it == current_regimes_.end()) {
        return make_error<RegimeFeatures>(ErrorCode::INVALID_ARGUMENT,
                                          "No feature information for symbol: " + symbol,
                                          "RegimeDetector");
    }

    return Result<RegimeFeatures>(it->second.features);
}

Result<bool> RegimeDetector::has_regime_changed(const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = current_regimes_.find(symbol);
    if (it == current_regimes_.end()) {
        return make_error<bool>(ErrorCode::INVALID_ARGUMENT,
                                "No regime information for symbol: " + symbol, "RegimeDetector");
    }

    return Result<bool>(it->second.regime_duration < config_.min_regime_duration);
}

Result<double> RegimeDetector::get_change_probability(const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = current_regimes_.find(symbol);
    if (it == current_regimes_.end()) {
        return make_error<double>(ErrorCode::INVALID_ARGUMENT,
                                  "No regime information for symbol: " + symbol, "RegimeDetector");
    }

    return Result<double>(it->second.regime_probability);
}

Result<std::vector<RegimeDetectionResult>> RegimeDetector::get_regime_history(
    const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = regime_history_.find(symbol);
    if (it == regime_history_.end()) {
        return make_error<std::vector<RegimeDetectionResult>>(
            ErrorCode::INVALID_ARGUMENT, "No regime history for symbol: " + symbol,
            "RegimeDetector");
    }

    return Result<std::vector<RegimeDetectionResult>>(it->second);
}

double RegimeDetector::calculate_trend_strength(const std::vector<double>& prices) const {
    if (prices.size() < 2)
        return 0.0;

    double positive_moves = 0.0;
    double total_moves = 0.0;

    for (size_t i = 1; i < prices.size(); ++i) {
        double move = std::abs(prices[i] - prices[i - 1]);
        total_moves += move;
        if (prices[i] > prices[i - 1]) {
            positive_moves += move;
        }
    }

    return total_moves > 0.0 ? (2.0 * positive_moves / total_moves - 1.0) : 0.0;
}

double RegimeDetector::calculate_mean_reversion(const std::vector<double>& prices) const {
    if (prices.size() < 2)
        return 0.0;

    // Calculate variance ratio with multiple lags
    std::vector<int> lags = {2, 5, 10};
    double var_ratio = calculate_variance_ratio(prices, lags);

    // Normalize to [0,1] range where 1 indicates strong mean reversion
    return std::max(0.0, 1.0 - var_ratio / config_.var_ratio_threshold);
}

double RegimeDetector::calculate_volatility(const std::vector<double>& prices) const {
    if (prices.size() < 2)
        return 0.0;

    // Calculate returns
    std::vector<double> returns;
    returns.reserve(prices.size() - 1);

    for (size_t i = 1; i < prices.size(); ++i) {
        returns.push_back(std::log(prices[i] / prices[i - 1]));
    }

    // Calculate standard deviation
    double mean = std::accumulate(returns.begin(), returns.end(), 0.0) / returns.size();
    double sq_sum = std::inner_product(returns.begin(), returns.end(), returns.begin(), 0.0);
    double variance = (sq_sum / returns.size()) - (mean * mean);

    return std::sqrt(variance * 252.0);  // Annualized
}

double RegimeDetector::calculate_hurst(const std::vector<double>& prices) const {
    if (prices.size() < 2)
        return 0.5;  // Random walk

    std::vector<int> lags = {2, 4, 8, 16, 32};
    std::vector<double> rs_values;

    for (int lag : lags) {
        if (static_cast<size_t>(lag) >= prices.size())
            break;

        std::vector<double> means;
        std::vector<double> stds;

        for (size_t i = 0; i < prices.size() - lag + 1; ++i) {
            std::vector<double> window(prices.begin() + i, prices.begin() + i + lag);

            double mean = std::accumulate(window.begin(), window.end(), 0.0) / window.size();

            double sq_sum = std::inner_product(window.begin(), window.end(), window.begin(), 0.0);
            double variance = (sq_sum / window.size()) - (mean * mean);

            means.push_back(mean);
            stds.push_back(std::sqrt(variance));
        }

        if (!means.empty() && !stds.empty()) {
            double rs = *std::max_element(stds.begin(), stds.end()) /
                        *std::min_element(stds.begin(), stds.end());
            rs_values.push_back(std::log(rs) / std::log(static_cast<double>(lag)));
        }
    }

    if (rs_values.empty())
        return 0.5;

    return std::accumulate(rs_values.begin(), rs_values.end(), 0.0) / rs_values.size();
}

double RegimeDetector::calculate_variance_ratio(const std::vector<double>& prices,
                                                const std::vector<int>& lags) const {
    if (prices.size() < 2)
        return 1.0;

    // Calculate returns
    std::vector<double> returns;
    returns.reserve(prices.size() - 1);

    for (size_t i = 1; i < prices.size(); ++i) {
        returns.push_back(std::log(prices[i] / prices[i - 1]));
    }

    double var_1 = 0.0;
    {
        double mean = std::accumulate(returns.begin(), returns.end(), 0.0) / returns.size();
        double sq_sum = std::inner_product(returns.begin(), returns.end(), returns.begin(), 0.0);
        var_1 = (sq_sum / returns.size()) - (mean * mean);
    }

    double avg_ratio = 0.0;
    int valid_lags = 0;

    for (int lag : lags) {
        if (static_cast<size_t>(lag) >= returns.size())
            continue;

        std::vector<double> lagged_returns;
        lagged_returns.reserve(returns.size() / lag);

        for (size_t i = 0; i < returns.size() - lag + 1; i += lag) {
            double sum = std::accumulate(returns.begin() + i, returns.begin() + i + lag, 0.0);
            lagged_returns.push_back(sum);
        }

        double mean = std::accumulate(lagged_returns.begin(), lagged_returns.end(), 0.0) /
                      lagged_returns.size();
        double sq_sum = std::inner_product(lagged_returns.begin(), lagged_returns.end(),
                                           lagged_returns.begin(), 0.0);
        double var_q = (sq_sum / lagged_returns.size()) - (mean * mean);

        if (var_1 > 0.0) {
            avg_ratio += (var_q / lag) / var_1;
            valid_lags++;
        }
    }

    return valid_lags > 0 ? avg_ratio / valid_lags : 1.0;
}

DetailedMarketRegime RegimeDetector::detect_regime_change(const RegimeFeatures& features,
                                                          const std::string& symbol) const {
    // Start with trend analysis
    if (std::abs(features.trend_strength) > 0.7) {
        if (features.trend_strength > 0) {
            return features.volatility > config_.volatility_threshold
                       ? DetailedMarketRegime::STRONG_UPTREND
                       : DetailedMarketRegime::WEAK_UPTREND;
        } else {
            return features.volatility > config_.volatility_threshold
                       ? DetailedMarketRegime::STRONG_DOWNTREND
                       : DetailedMarketRegime::WEAK_DOWNTREND;
        }
    }

    // Check mean reversion
    if (features.mean_reversion_strength > 0.7) {
        return features.volatility > config_.volatility_threshold
                   ? DetailedMarketRegime::HIGH_MEAN_REVERSION
                   : DetailedMarketRegime::LOW_MEAN_REVERSION;
    }

    // Check volatility regimes
    if (features.volatility > config_.volatility_threshold) {
        if (features.volatility_of_volatility > config_.volatility_threshold * 0.5) {
            return DetailedMarketRegime::VOLATILITY_EXPANSION;
        }
        return DetailedMarketRegime::HIGH_VOLATILITY;
    } else if (features.volatility_of_volatility < config_.volatility_threshold * 0.2) {
        return DetailedMarketRegime::VOLATILITY_CONTRACTION;
    }

    // Check correlation regimes
    if (features.correlation > config_.correlation_threshold) {
        return DetailedMarketRegime::HIGH_CORRELATION;
    } else if (features.correlation < config_.correlation_threshold * 0.5) {
        return DetailedMarketRegime::LOW_CORRELATION;
    }

    // Check liquidity regimes
    if (features.liquidity < 0.3) {
        return DetailedMarketRegime::LIQUIDITY_CRISIS;
    } else if (features.liquidity < 0.7) {
        return DetailedMarketRegime::LOW_LIQUIDITY;
    }

    // Check market stress
    if (features.market_stress > 0.8) {
        return DetailedMarketRegime::CRISIS;
    } else if (features.market_stress > 0.5) {
        return DetailedMarketRegime::STRESS;
    }

    return DetailedMarketRegime::NORMAL;
}

double RegimeDetector::calculate_change_probability(const RegimeFeatures& features,
                                                    DetailedMarketRegime current_regime) const {
    // Calculate base probability from feature divergence
    double base_prob = 0.0;
    int num_features = 0;

    // Trend divergence
    if (std::abs(features.trend_strength) > 0.7 &&
        (current_regime != DetailedMarketRegime::STRONG_UPTREND &&
         current_regime != DetailedMarketRegime::STRONG_DOWNTREND)) {
        base_prob += std::abs(features.trend_strength);
        num_features++;
    }

    // Mean reversion divergence
    if (features.mean_reversion_strength > 0.7 &&
        (current_regime != DetailedMarketRegime::HIGH_MEAN_REVERSION &&
         current_regime != DetailedMarketRegime::LOW_MEAN_REVERSION)) {
        base_prob += features.mean_reversion_strength;
        num_features++;
    }

    // Volatility divergence
    if (features.volatility > config_.volatility_threshold &&
        current_regime != DetailedMarketRegime::HIGH_VOLATILITY) {
        base_prob += features.volatility / config_.volatility_threshold;
        num_features++;
    }

    // Correlation divergence
    if (features.correlation > config_.correlation_threshold &&
        current_regime != DetailedMarketRegime::HIGH_CORRELATION) {
        base_prob += features.correlation / config_.correlation_threshold;
        num_features++;
    }

    // Calculate average probability
    double avg_prob = num_features > 0 ? base_prob / num_features : 0.0;

    // Apply confidence threshold
    return avg_prob > config_.confidence_threshold ? avg_prob : 0.0;
}

bool RegimeDetector::validate_regime_change(const std::string& symbol,
                                            DetailedMarketRegime new_regime,
                                            const RegimeFeatures& features) const {
    auto it = current_regimes_.find(symbol);
    if (it == current_regimes_.end())
        return true;  // First regime

    const auto& current = it->second;

    // Check minimum duration requirement
    if (current.regime_duration < config_.min_regime_duration) {
        return false;
    }

    // Calculate change probability
    double prob = calculate_change_probability(features, current.current_regime);
    if (prob < config_.confidence_threshold) {
        return false;
    }

    // Validate transitions
    switch (current.current_regime) {
        case DetailedMarketRegime::STRONG_UPTREND:
            // Can't directly switch to strong downtrend
            if (new_regime == DetailedMarketRegime::STRONG_DOWNTREND) {
                return false;
            }
            break;

        case DetailedMarketRegime::STRONG_DOWNTREND:
            // Can't directly switch to strong uptrend
            if (new_regime == DetailedMarketRegime::STRONG_UPTREND) {
                return false;
            }
            break;

        case DetailedMarketRegime::CRISIS:
            // Need high confidence to exit crisis regime
            if (prob < config_.confidence_threshold * 1.5) {
                return false;
            }
            break;

        case DetailedMarketRegime::LIQUIDITY_CRISIS:
            // Need extreme confidence to exit liquidity crisis
            if (prob < config_.confidence_threshold * 2.0) {
                return false;
            }
            break;

        default:
            break;
    }

    return true;
}

}  // namespace trade_ngin