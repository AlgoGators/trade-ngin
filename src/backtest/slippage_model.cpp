// src/backtest/slippage_models.cpp
#include <algorithm>
#include <cmath>
#include <numeric>
#include "trade_ngin/backtest/slippage_models.hpp"
#include "trade_ngin/core/logger.hpp"

namespace trade_ngin {
namespace backtest {

// VolumeSlippageModel Implementation
VolumeSlippageModel::VolumeSlippageModel(VolumeSlippageConfig config)
    : config_(std::move(config)) {}

double VolumeSlippageModel::calculate_slippage(double price, double quantity, Side side,
                                               const std::optional<Bar>& market_data) const {
    if (!market_data) {
        // If no market data, use simple linear impact
        double impact = std::abs(quantity) * config_.price_impact_coefficient;
        return side == Side::BUY ? price * (1.0 + impact) : price * (1.0 - impact);
    }

    const Bar& bar = *market_data;

    // Calculate volume ratio
    double avg_volume =
        average_volumes_.count(bar.symbol) ? average_volumes_.at(bar.symbol) : bar.volume;

    double volume_ratio = std::abs(quantity) / avg_volume;
    volume_ratio = std::clamp(volume_ratio, config_.min_volume_ratio, config_.max_volume_ratio);

    // Get volatility adjustment if available
    double vol_adjust = volatilities_.count(bar.symbol)
                            ? volatilities_.at(bar.symbol) * config_.volatility_multiplier
                            : 1.0;

    // Calculate base impact using square-root formula
    double base_impact = config_.price_impact_coefficient * std::sqrt(volume_ratio) * vol_adjust;

    // Add extra impact if above max volume ratio
    if (volume_ratio > config_.max_volume_ratio) {
        double excess_ratio = volume_ratio - config_.max_volume_ratio;
        base_impact *= (1.0 + excess_ratio);
    }

    // Apply impact based on side
    return side == Side::BUY ? price * (1.0 + base_impact) : price * (1.0 - base_impact);
}

void VolumeSlippageModel::update(const Bar& market_data) {
    constexpr size_t VOLUME_WINDOW = 20;  // Rolling window for volume average

    // Update average volume
    auto& avg_volume = average_volumes_[market_data.symbol];
    if (avg_volume == 0.0) {
        avg_volume = market_data.volume;
    } else {
        avg_volume = (avg_volume * (VOLUME_WINDOW - 1) + market_data.volume) / VOLUME_WINDOW;
    }

    // Update volatility estimate
    // Using simple high-low volatility measure
    auto& volatility = volatilities_[market_data.symbol];
    double current_vol = (market_data.high.as_double() - market_data.low.as_double()) /
                         market_data.close.as_double();
    if (volatility == 0.0) {
        volatility = current_vol;
    } else {
        volatility = 0.9 * volatility + 0.1 * current_vol;  // EWMA
    }
}

// SpreadSlippageModel Implementation
SpreadSlippageModel::SpreadSlippageModel(SpreadSlippageConfig config)
    : config_(std::move(config)) {}

double SpreadSlippageModel::calculate_slippage(double price, double quantity, Side side,
                                               const std::optional<Bar>& market_data) const {
    // Get base spread in basis points
    double spread_bps = config_.min_spread_bps;
    if (market_data && spread_estimates_.count(market_data->symbol)) {
        spread_bps = std::max(config_.min_spread_bps, spread_estimates_.at(market_data->symbol));
    }

    // Adjust spread based on trade size and urgency
    double adjusted_spread = spread_bps * config_.spread_multiplier;

    // Add market impact for larger sizes
    if (market_data) {
        double volume_ratio = std::abs(quantity) / market_data->volume;
        if (volume_ratio > 0.1) {  // Size is significant
            adjusted_spread *= (1.0 + config_.market_impact_multiplier * (volume_ratio - 0.1));
        }
    }

    // Convert basis points to decimal and apply to price
    double impact = adjusted_spread / 10000.0;
    return side == Side::BUY ? price * (1.0 + impact) : price * (1.0 - impact);
}

void SpreadSlippageModel::update(const Bar& market_data) {
    // Estimate spread from high-low range
    // This is a simplified approach; in practice you might use bid-ask data
    double estimated_spread = ((market_data.high.as_double() - market_data.low.as_double()) /
                               market_data.close.as_double()) *
                              10000.0;  // Convert to bps

    // Update spread estimate with exponential smoothing
    auto& current_spread = spread_estimates_[market_data.symbol];
    if (current_spread == 0.0) {
        current_spread = estimated_spread;
    } else {
        current_spread = 0.95 * current_spread + 0.05 * estimated_spread;
    }
}

// SlippageModelFactory Implementation
std::unique_ptr<SlippageModel> SlippageModelFactory::create_volume_model(
    const VolumeSlippageConfig& config) {
    return std::make_unique<VolumeSlippageModel>(config);
}

std::unique_ptr<SlippageModel> SlippageModelFactory::create_spread_model(
    const SpreadSlippageConfig& config) {
    return std::make_unique<SpreadSlippageModel>(config);
}

}  // namespace backtest
}  // namespace trade_ngin
