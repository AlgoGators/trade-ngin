#include "trade_ngin/transaction_cost/impact_model.hpp"

#include <algorithm>
#include <numeric>

namespace trade_ngin {
namespace transaction_cost {

ImpactModel::ImpactModel(const Config& config)
    : config_(config) {}

double ImpactModel::calculate_market_impact(
    double quantity,
    double reference_price,
    double adv,
    const AssetCostConfig& asset_config) const {

    // Ensure quantity is absolute
    quantity = std::abs(quantity);

    // Apply ADV floor to prevent division by very small numbers
    adv = std::max(adv, config_.min_adv);

    // Calculate participation rate
    double participation = quantity / adv;

    // Clamp participation to configured bounds
    participation = std::clamp(participation, config_.min_participation, config_.max_participation);

    // Get impact coefficient based on ADV bucket
    double k_bps = get_impact_k_bps(adv);

    // Square-root impact model: impact_bps = k * sqrt(participation)
    double impact_bps = k_bps * std::sqrt(participation);

    // Cap impact to prevent blowups
    impact_bps = std::min(impact_bps, asset_config.max_impact_bps);

    // Convert basis points to price impact
    // market_impact_price = (impact_bps / 10000) * ref_price
    double market_impact_price = (impact_bps / 10000.0) * reference_price;

    return market_impact_price;
}

double ImpactModel::get_impact_k_bps(double adv) const {
    // Liquidity buckets for impact coefficient selection
    // Higher ADV = more liquid = lower impact coefficient
    if (adv > 1000000.0) {
        return 10.0;  // Ultra liquid
    }
    if (adv > 200000.0) {
        return 20.0;  // Liquid
    }
    if (adv > 50000.0) {
        return 40.0;  // Medium
    }
    if (adv > 20000.0) {
        return 60.0;  // Thin
    }
    return 80.0;  // Very thin
}

void ImpactModel::update_volume(const std::string& symbol, double volume) {
    auto& volumes = symbol_volumes_[symbol];

    volumes.push_back(volume);

    // Maintain rolling window size
    while (volumes.size() > config_.adv_lookback_days) {
        volumes.pop_front();
    }
}

double ImpactModel::get_adv(const std::string& symbol) const {
    auto it = symbol_volumes_.find(symbol);
    if (it == symbol_volumes_.end() || it->second.empty()) {
        return 0.0;
    }

    const auto& volumes = it->second;
    double sum = std::accumulate(volumes.begin(), volumes.end(), 0.0);
    return sum / static_cast<double>(volumes.size());
}

bool ImpactModel::has_sufficient_data(const std::string& symbol, size_t min_days) const {
    auto it = symbol_volumes_.find(symbol);
    if (it == symbol_volumes_.end()) {
        return false;
    }
    return it->second.size() >= min_days;
}

void ImpactModel::clear_symbol_data(const std::string& symbol) {
    symbol_volumes_.erase(symbol);
}

void ImpactModel::clear_all() {
    symbol_volumes_.clear();
}

}  // namespace transaction_cost
}  // namespace trade_ngin
