#include "trade_ngin/transaction_cost/transaction_cost_manager.hpp"

#include <cmath>

namespace trade_ngin {
namespace transaction_cost {

TransactionCostManager::TransactionCostManager(const Config& config)
    : config_(config),
      asset_configs_(),
      spread_model_(config.spread_config),
      impact_model_(config.impact_config) {}

TransactionCostResult TransactionCostManager::calculate_costs(
    const std::string& symbol,
    double quantity,
    double reference_price) const {

    // Get internally tracked ADV and volatility multiplier
    double adv = impact_model_.get_adv(symbol);
    double vol_mult = spread_model_.get_volatility_multiplier(symbol);

    // Use defaults if insufficient data
    if (adv <= 0.0) {
        // Use a conservative default ADV based on asset config
        // This prevents zero ADV from causing issues
        adv = 100000.0;  // Assume medium liquidity
    }

    if (vol_mult <= 0.0) {
        vol_mult = 1.0;  // Neutral volatility
    }

    return calculate_costs(symbol, quantity, reference_price, adv, vol_mult);
}

TransactionCostResult TransactionCostManager::calculate_costs(
    const std::string& symbol,
    double quantity,
    double reference_price,
    double adv,
    double volatility_multiplier) const {

    TransactionCostResult result;

    // Ensure quantity is absolute
    double abs_qty = std::abs(quantity);

    // Get asset configuration
    AssetCostConfig asset_config = asset_configs_.get_config(symbol);

    // 1. Calculate explicit costs
    // commissions_fees = |qty| * fee_per_contract
    result.commissions_fees = abs_qty * config_.explicit_fee_per_contract;

    // 2. Calculate spread cost (in price units per contract)
    result.spread_price_impact = spread_model_.calculate_spread_price_impact(
        asset_config, volatility_multiplier);

    // 3. Calculate market impact (in price units per contract)
    result.market_impact_price_impact = impact_model_.calculate_market_impact(
        abs_qty, reference_price, adv, asset_config);

    // 4. Combine implicit costs
    // implicit_price_impact = spread + market impact (per contract, price units)
    result.implicit_price_impact =
        result.spread_price_impact + result.market_impact_price_impact;

    // 5. Convert implicit to dollars
    // slippage_market_impact = implicit_price_impact * |qty| * point_value
    result.slippage_market_impact =
        result.implicit_price_impact * abs_qty * asset_config.point_value;

    // 6. Calculate total transaction costs
    // total = explicit + implicit (both in dollars)
    result.total_transaction_costs =
        result.commissions_fees + result.slippage_market_impact;

    return result;
}

void TransactionCostManager::update_market_data(
    const std::string& symbol,
    double volume,
    double close_price,
    double prev_close_price) {

    // Update volume for ADV calculation
    impact_model_.update_volume(symbol, volume);

    // Calculate log return and update for volatility
    if (prev_close_price > 0.0 && close_price > 0.0) {
        double log_return = std::log(close_price / prev_close_price);
        // Note: spread_model_ is mutable for this operation
        const_cast<SpreadModel&>(spread_model_).update_log_returns(symbol, log_return);
    }
}

double TransactionCostManager::get_adv(const std::string& symbol) const {
    return impact_model_.get_adv(symbol);
}

double TransactionCostManager::get_volatility_multiplier(const std::string& symbol) const {
    return spread_model_.get_volatility_multiplier(symbol);
}

AssetCostConfig TransactionCostManager::get_asset_config(const std::string& symbol) const {
    return asset_configs_.get_config(symbol);
}

void TransactionCostManager::register_asset_config(const AssetCostConfig& config) {
    asset_configs_.register_config(config);
}

void TransactionCostManager::clear_all_data() {
    const_cast<SpreadModel&>(spread_model_).clear_all();
    const_cast<ImpactModel&>(impact_model_).clear_all();
}

}  // namespace transaction_cost
}  // namespace trade_ngin
