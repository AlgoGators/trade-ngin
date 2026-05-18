#include "trade_ngin/transaction_cost/transaction_cost_manager.hpp"

#include <algorithm>
#include <cmath>

#include "trade_ngin/core/logger.hpp"

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
    double reference_price,
    AssetType asset_type) const {

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

    return calculate_costs(symbol, quantity, reference_price, adv, vol_mult, asset_type);
}

TransactionCostResult TransactionCostManager::calculate_costs(
    const std::string& symbol,
    double quantity,
    double reference_price,
    double adv,
    double volatility_multiplier,
    AssetType asset_type) const {

    TransactionCostResult result;

    // Ensure quantity is absolute
    double abs_qty = std::abs(quantity);

    // Get asset configuration. asset_type is consulted only when the symbol
    // has no registered config -- it routes the fallback to the equity
    // default ($0.005/share, $1 min) instead of the futures default
    // ($1.50/share, point_value=100). Closes audit §1.1 dispatch dead-end.
    AssetCostConfig asset_config = asset_configs_.get_config(symbol, asset_type);

    // 1. Calculate explicit costs (commissions)
    if (asset_config.commission_per_unit >= 0.0) {
        // Per-asset commission (e.g., equities: $0.005/share with min/max per order)
        double raw_commission = abs_qty * asset_config.commission_per_unit;
        double effective_max;
        if (asset_config.max_commission_pct >= 0.0) {
            // Percentage-based cap: e.g., 0.5% of trade value (IBKR Tiered)
            // IBKR Fixed uses 1.0% -- configurable via max_commission_pct
            effective_max = asset_config.max_commission_pct * abs_qty * reference_price;
        } else {
            effective_max = asset_config.max_commission_per_order;
        }
        result.commissions_fees = std::max(asset_config.min_commission_per_order,
                                           std::min(effective_max, raw_commission));
    } else {
        // Global fee per contract (futures default)
        result.commissions_fees = abs_qty * config_.explicit_fee_per_contract;
    }

    // 1b. Regulatory fees (equity sell-side only)
    if (asset_config.apply_regulatory_fees && quantity < 0) {
        double trade_value = abs_qty * reference_price;
        // SEC Transaction Fee (sell-side only)
        double sec_fee = (trade_value / 1000000.0) * asset_config.sec_fee_per_million;
        // FINRA TAF (sell-side only, capped per trade)
        double taf = std::min(abs_qty * asset_config.finra_taf_per_share,
                              asset_config.finra_taf_cap_per_trade);
        result.commissions_fees += sec_fee + taf;
    }

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
        spread_model_.update_log_returns(symbol, log_return);
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

int TransactionCostManager::register_equity_costs_from_bars(
    const std::vector<std::string>& symbols,
    const std::unordered_map<std::string, std::vector<Bar>>& bars_by_symbol,
    int adv_lookback_days) {

    if (adv_lookback_days < 1) {
        adv_lookback_days = 1;
    }

    int registered = 0;
    for (const auto& symbol : symbols) {
        auto it = bars_by_symbol.find(symbol);
        if (it == bars_by_symbol.end() || it->second.empty()) {
            WARN("register_equity_costs_from_bars: no bars for " + symbol +
                 " -- skipping (will use equity default if traded)");
            continue;
        }

        const auto& bars = it->second;
        const size_t n = std::min(static_cast<size_t>(adv_lookback_days), bars.size());
        const size_t start_idx = bars.size() - n;

        double volume_sum = 0.0;
        for (size_t i = start_idx; i < bars.size(); ++i) {
            volume_sum += bars[i].volume;
        }
        const double adv = volume_sum / static_cast<double>(n);

        if (adv <= 0.0) {
            WARN("register_equity_costs_from_bars: zero ADV for " + symbol +
                 " over " + std::to_string(n) + " bars -- skipping");
            continue;
        }

        const double price = bars.back().close.as_double();
        AssetCostConfig config = AssetCostConfigRegistry::get_tiered_equity_config(price, adv);
        config.symbol = symbol;
        asset_configs_.register_config(config);
        ++registered;
    }

    INFO("Registered " + std::to_string(registered) + " equity cost configs (tiered by ADV, " +
         std::to_string(adv_lookback_days) + "-day lookback)");
    return registered;
}

void TransactionCostManager::clear_all_data() {
    spread_model_.clear_all();
    impact_model_.clear_all();
}

}  // namespace transaction_cost
}  // namespace trade_ngin
