#include "trade_ngin/transaction_cost/spread_model.hpp"

#include <numeric>
#include <stdexcept>

namespace trade_ngin {
namespace transaction_cost {

SpreadModel::SpreadModel(const VolatilityConfig& vol_config)
    : vol_config_(vol_config) {}

double SpreadModel::calculate_spread_price_impact(
    const AssetCostConfig& config,
    double volatility_multiplier) const {

    // Apply volatility widening to baseline spread
    double spread_ticks = config.baseline_spread_ticks * volatility_multiplier;

    // Clamp to min/max bounds
    spread_ticks = std::clamp(spread_ticks, config.min_spread_ticks, config.max_spread_ticks);

    // Half-spread cost (one-way, we pay half the spread to cross)
    // spread_price_impact = 0.5 * spread_ticks * tick_size
    double spread_price_impact = 0.5 * spread_ticks * config.tick_size;

    return spread_price_impact;
}

double SpreadModel::calculate_volatility_multiplier(const std::vector<double>& log_returns) const {
    // Need at least 2 returns to calculate volatility
    if (log_returns.size() < 2) {
        return 1.0;  // Default multiplier if insufficient data
    }

    // Calculate current volatility (stdev of log returns)
    double mean = compute_mean(log_returns);
    double sigma = compute_stdev(log_returns, mean);

    // For the z-score, we need historical mean and stdev of volatility
    // In a simple implementation, we use the current volatility relative to
    // a baseline assumption. A more sophisticated approach would track
    // rolling volatility history.
    //
    // Simplified approach: assume baseline volatility of 1% daily (0.01)
    // and typical stdev of volatility of 0.5% (0.005)
    constexpr double baseline_sigma = 0.01;
    constexpr double sigma_of_sigma = 0.005;

    // Calculate z-score of current volatility
    double z_sigma = (sigma - baseline_sigma) / sigma_of_sigma;

    // Clip z-score to [-2, 2]
    z_sigma = std::clamp(z_sigma, -2.0, 2.0);

    // Calculate volatility multiplier
    double vol_mult = 1.0 + vol_config_.lambda * z_sigma;

    // Clamp to configured bounds
    vol_mult = std::clamp(vol_mult, vol_config_.min_multiplier, vol_config_.max_multiplier);

    return vol_mult;
}

void SpreadModel::update_log_returns(const std::string& symbol, double log_return) {
    auto& returns = symbol_log_returns_[symbol];

    returns.push_back(log_return);

    // Maintain rolling window size
    while (returns.size() > vol_config_.lookback_days) {
        returns.pop_front();
    }
}

double SpreadModel::get_volatility_multiplier(const std::string& symbol) const {
    auto it = symbol_log_returns_.find(symbol);
    if (it == symbol_log_returns_.end() || it->second.size() < 2) {
        return 1.0;  // Default if no data
    }

    // Convert deque to vector for calculation
    std::vector<double> returns(it->second.begin(), it->second.end());
    return calculate_volatility_multiplier(returns);
}

void SpreadModel::clear_symbol_data(const std::string& symbol) {
    symbol_log_returns_.erase(symbol);
}

void SpreadModel::clear_all() {
    symbol_log_returns_.clear();
}

double SpreadModel::compute_mean(const std::vector<double>& values) {
    if (values.empty()) {
        return 0.0;
    }
    double sum = std::accumulate(values.begin(), values.end(), 0.0);
    return sum / static_cast<double>(values.size());
}

double SpreadModel::compute_stdev(const std::vector<double>& values, double mean) {
    if (values.size() < 2) {
        return 0.0;
    }

    double sum_sq_diff = 0.0;
    for (double v : values) {
        double diff = v - mean;
        sum_sq_diff += diff * diff;
    }

    // Use sample standard deviation (N-1)
    double variance = sum_sq_diff / static_cast<double>(values.size() - 1);
    return std::sqrt(variance);
}

}  // namespace transaction_cost
}  // namespace trade_ngin
