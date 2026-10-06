#include "trade_ngin/transaction_cost/transaction_cost_manager.hpp"

#include <cmath>
#include <algorithm>
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/instruments/equity.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"

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
    CostChargeObservation* observation) const {
    return calculate_costs(symbol, quantity, reference_price, AssetType::NONE, observation);
}

TransactionCostResult TransactionCostManager::calculate_costs(
    const std::string& symbol, double quantity, double reference_price,
    AssetType asset_type, CostChargeObservation* observation) const {
    if (observation) {
        *observation = {};
        observation->input_source = CostInputSource::internally_tracked;
        observation->quantity = quantity;
        observation->reference_price = reference_price;
    }

    // Get internally tracked ADV and volatility multiplier
    double adv = impact_model_.get_adv(symbol);
    double vol_mult = spread_model_.get_volatility_multiplier(
        symbol, observation ? &observation->volatility : nullptr);
    if (observation) {
        observation->retrieved_adv = adv;
        observation->retrieved_volatility_multiplier = vol_mult;
    }

    // Use defaults if insufficient data
    if (adv <= 0.0) {
        // Use a conservative default ADV based on asset config
        // This prevents zero ADV from causing issues
        adv = 100000.0;  // Assume medium liquidity
    }

    if (vol_mult <= 0.0) {
        vol_mult = 1.0;  // Neutral volatility
    }
    if (observation) {
        observation->effective_adv = adv;
        observation->effective_volatility_multiplier = vol_mult;
    }

    return calculate_charges(symbol, quantity, reference_price, adv, vol_mult, asset_type, observation);
}

TransactionCostResult TransactionCostManager::calculate_costs(
    const std::string& symbol,
    double quantity,
    double reference_price,
    double adv,
    double volatility_multiplier,
    CostChargeObservation* observation) const {
    return calculate_costs(symbol, quantity, reference_price, adv, volatility_multiplier,
                           AssetType::NONE, observation);
}

TransactionCostResult TransactionCostManager::calculate_costs(
    const std::string& symbol, double quantity, double reference_price,
    double adv, double volatility_multiplier, AssetType asset_type,
    CostChargeObservation* observation) const {
    if (observation) {
        *observation = {};
        observation->input_source = CostInputSource::explicit_values;
        observation->quantity = quantity;
        observation->reference_price = reference_price;
        observation->adv_argument = adv;
        observation->volatility_multiplier_argument = volatility_multiplier;
        observation->effective_adv = adv;
        observation->effective_volatility_multiplier = volatility_multiplier;
    }
    return calculate_charges(symbol, quantity, reference_price, adv, volatility_multiplier,
                             asset_type, observation);
}

TransactionCostResult TransactionCostManager::calculate_charges(
    const std::string& symbol,
    double quantity,
    double reference_price,
    double adv,
    double volatility_multiplier,
    AssetType asset_type,
    CostChargeObservation* observation) const {
    TransactionCostResult result;

    // Ensure quantity is absolute
    double abs_qty = std::abs(quantity);

    // Get asset configuration
    AssetCostConfig asset_config = asset_configs_.get_config(
        symbol, asset_type, observation ? &observation->asset_lookup : nullptr);

    // 1. Calculate explicit costs
    if (asset_config.asset_type == AssetType::EQUITY) {
        if (observation) observation->commission_per_unit = asset_config.commission_per_unit;
        if (asset_config.commission_per_unit >= 0.0) {
            const double raw_commission = abs_qty * asset_config.commission_per_unit;
            if (observation) observation->max_commission_pct = asset_config.max_commission_pct;
            double effective_max;
            if (asset_config.max_commission_pct >= 0.0) {
                effective_max = asset_config.max_commission_pct * abs_qty * reference_price;
            } else {
                if (observation) observation->max_commission_per_order = asset_config.max_commission_per_order;
                effective_max = asset_config.max_commission_per_order;
            }
            if (observation) observation->min_commission_per_order = asset_config.min_commission_per_order;
            // Main's existing Fixed schedule: the ceiling applies after the floor.
            result.commissions_fees = std::min(effective_max,
                std::max(asset_config.min_commission_per_order, raw_commission));
        } else {
            if (observation) observation->explicit_fee_per_contract = config_.explicit_fee_per_contract;
            result.commissions_fees = abs_qty * config_.explicit_fee_per_contract;
        }
        if (observation) observation->apply_regulatory_fees = asset_config.apply_regulatory_fees;
        if (asset_config.apply_regulatory_fees && quantity < 0) {
            if (observation) {
                observation->sec_fee_per_million = asset_config.sec_fee_per_million;
                observation->finra_taf_per_share = asset_config.finra_taf_per_share;
                observation->finra_taf_cap_per_trade = asset_config.finra_taf_cap_per_trade;
            }
            const double trade_value = abs_qty * reference_price;
            const double sec_fee = (trade_value / 1000000.0) * asset_config.sec_fee_per_million;
            const double taf = std::min(abs_qty * asset_config.finra_taf_per_share,
                                       asset_config.finra_taf_cap_per_trade);
            result.commissions_fees += sec_fee + taf;
        }
    } else {
        // Keep the existing futures calculation and observation unchanged.
        if (observation) observation->explicit_fee_per_contract = config_.explicit_fee_per_contract;
        result.commissions_fees = abs_qty * config_.explicit_fee_per_contract;
    }

    // 2. Calculate spread cost (in price units per contract)
    result.spread_price_impact = spread_model_.calculate_spread_price_impact(
        asset_config, volatility_multiplier, observation ? &observation->spread : nullptr);

    // 3. Calculate market impact (in price units per contract)
    result.market_impact_price_impact = impact_model_.calculate_market_impact(
        abs_qty, reference_price, adv, asset_config,
        observation ? &observation->impact : nullptr);

    // 4. Combine implicit costs
    // implicit_price_impact = spread + market impact (per contract, price units)
    result.implicit_price_impact =
        result.spread_price_impact + result.market_impact_price_impact;

    // Import main's equity cap without activating its previously unused futures cap.
    if (asset_config.asset_type == AssetType::EQUITY) {
        if (observation) observation->max_total_implicit_bps = asset_config.max_total_implicit_bps;
        if (asset_config.max_total_implicit_bps >= 0.0 && reference_price > 0.0) {
            const double cap = (asset_config.max_total_implicit_bps / 10000.0) * reference_price;
            if (result.implicit_price_impact > cap) {
                WARN("Implicit cost cap binding for " + symbol + ": " +
                     std::to_string((result.implicit_price_impact / reference_price) * 10000.0) +
                     " bps clamped to " + std::to_string(asset_config.max_total_implicit_bps) +
                     " bps (spread + market impact)");
                result.implicit_price_impact = cap;
            }
        }
    }

    // 5. Convert implicit to dollars
    // slippage_market_impact = implicit_price_impact * |qty| * point_value
    if (observation) observation->point_value = asset_config.point_value;
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
    double prev_close_price,
    MarketDataObservation* observation) {
    if (observation) *observation = {};

    // Update volume for ADV calculation
    impact_model_.update_volume(symbol, volume,
                                observation ? &observation->volume : nullptr);

    // Calculate log return and update for volatility
    if (prev_close_price > 0.0 && close_price > 0.0) {
        double log_return = std::log(close_price / prev_close_price);
        // Note: spread_model_ is mutable for this operation
        const_cast<SpreadModel&>(spread_model_).update_log_returns(
            symbol, log_return, observation ? &observation->log_returns : nullptr);
    }
}

double TransactionCostManager::get_adv(const std::string& symbol) const {
    return impact_model_.get_adv(symbol);
}

double TransactionCostManager::get_volatility_multiplier(const std::string& symbol,
                                                         VolatilityObservation* observation) const {
    return spread_model_.get_volatility_multiplier(symbol, observation);
}

double TransactionCostManager::get_annual_volatility(const std::string& symbol) const {
    return spread_model_.get_annual_volatility(symbol);
}

AssetCostConfig TransactionCostManager::get_asset_config(const std::string& symbol,
                                                         AssetLookupObservation* observation) const {
    return asset_configs_.get_config(symbol, observation);
}

AssetCostConfig TransactionCostManager::get_asset_config(const std::string& symbol,
                                                         AssetType asset_type,
                                                         AssetLookupObservation* observation) const {
    return asset_configs_.get_config(symbol, asset_type, observation);
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
            // Preserve main's default registration within the explicit equity domain.
            // QT market-data admission is a separate caller responsibility.
            WARN("register_equity_costs_from_bars: no bars for " + symbol +
                 " -- registering the untiered equity default in the "
                 "explicit equity domain");
            AssetCostConfig fallback = AssetCostConfigRegistry::get_equity_default_config();
            fallback.symbol = symbol;
            asset_configs_.register_config(fallback);
            ++registered;
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
            // No usable ADV: preserve the existing untiered equity default.
            WARN("register_equity_costs_from_bars: zero ADV for " + symbol +
                 " over " + std::to_string(n) + " bars -- registering the untiered equity "
                 "default in the explicit equity domain");
            AssetCostConfig fallback = AssetCostConfigRegistry::get_equity_default_config();
            fallback.symbol = symbol;
            asset_configs_.register_config(fallback);
            ++registered;
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

namespace {

double base_rate_from_flags(int flag_count) {
    if (flag_count <= 0) return 0.0025;
    if (flag_count == 1) return 0.0050;
    if (flag_count == 2) return 0.0150;
    return 0.0500;
}

}  // namespace

std::unordered_map<std::string, double>
TransactionCostManager::calculate_overnight_borrow_fees(
    const std::unordered_map<std::string, Position>& positions,
    const std::unordered_map<std::string, double>& current_prices,
    const InstrumentRegistry& registry) const {
    std::unordered_map<std::string, double> fees;
    for (const auto& [symbol, position] : positions) {
        const double quantity = position.quantity.as_double();
        if (quantity >= 0.0) continue;

        auto equity = registry.get_equity_instrument(symbol);
        if (!equity) continue;

        double price = position.average_price.as_double();
        if (const auto it = current_prices.find(symbol); it != current_prices.end()) {
            price = it->second;
        } else {
            WARN("calculate_overnight_borrow_fees: no current price for " + symbol +
                 ", using avg_price " + std::to_string(price));
        }
        if (price <= 0.0) {
            WARN("calculate_overnight_borrow_fees: non-positive price for " + symbol +
                 ", skipping");
            continue;
        }

        const auto& spec = equity->get_spec();
        double annual_rate = spec.borrow_rate_override;
        if (annual_rate < 0.0) {
            int flag_count = 0;
            const double dollar_volume = impact_model_.get_adv(symbol) * price;
            if (dollar_volume > 0.0 && dollar_volume < 5'000'000.0) ++flag_count;
            if (price < 5.0) ++flag_count;
            if (!spec.is_easy_to_borrow) ++flag_count;
            double volatility_multiplier = spread_model_.get_annual_volatility(symbol) / 0.25;
            volatility_multiplier = std::clamp(volatility_multiplier, 1.0, 3.0);
            annual_rate = base_rate_from_flags(flag_count) * volatility_multiplier;
        }
        fees[symbol] = annual_rate * std::abs(quantity) * price / 365.0;
    }
    return fees;
}

void TransactionCostManager::clear_all_data() {
    const_cast<SpreadModel&>(spread_model_).clear_all();
    const_cast<ImpactModel&>(impact_model_).clear_all();
}

}  // namespace transaction_cost
}  // namespace trade_ngin
