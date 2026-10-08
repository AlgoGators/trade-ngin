#include "trade_ngin/transaction_cost/transaction_cost_manager.hpp"

#include <algorithm>
#include <cmath>

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
            // Percentage-based cap. The equity configs are IBKR Pro FIXED, so this is 1.0%
            // of trade value (0.5% is the Tiered cap -- see the field's comment in
            // asset_cost_config.hpp before changing it).
            effective_max = asset_config.max_commission_pct * abs_qty * reference_price;
        } else {
            effective_max = asset_config.max_commission_per_order;
        }
        // E2-C2: the cap is a CEILING and must be applied last.
        //
        // This was `max(floor, min(cap, raw))`, which lets the floor override the cap. IBKR
        // publishes the schedule as "Minimum per order: USD 1.00 / Maximum per order: 1
        // percent of trade value" -- a maximum that a minimum can exceed is not a maximum.
        //
        // The old order also made max_commission_pct DEAD for any normal stock: the inner
        // min(cap, raw) picks the cap only when `pct * price < commission_per_unit`, i.e.
        // below $0.50 a share, so above that the expression collapsed to max(floor, raw) and
        // the configured cap could not change a single output value.
        //
        // The two only collide on very small orders, which is exactly what a fractional-share
        // book trades. Measured 2026-07-24..08-03: the cap should have bound on 5 of 15
        // executions and did not -- an $18.70 AMZN trade paid $1.00 (535 bps) where the 1%
        // cap says $0.187. 21% over-charge, concentrated on the smallest clips.
        //
        // The floor protects the broker on small-but-normal orders; the cap protects the
        // client on tiny ones. Both belong here, in this order.
        result.commissions_fees = std::min(
            effective_max, std::max(asset_config.min_commission_per_order, raw_commission));
    } else {
        // Global fee per contract (futures default)
        result.commissions_fees = abs_qty * config_.explicit_fee_per_contract;
    }

    // 1b. Regulatory fees (equity sell-side only)
    //
    // E2-F29: this is the ONLY place the SIGN of `quantity` is read; everything else uses
    // abs_qty. Every caller used to pass |qty| -- generate_execution() had already split the
    // trade into a positive filled_quantity plus a Side -- so the gate was unreachable and
    // these fees were dead code. The callers now pass the signed trade; do not "normalise"
    // the argument at a call site.
    //
    // Note this is latent rather than live: every production config sets
    // apply_regulatory_fees = false (IBKR Pro FIXED is all-inclusive, E2-C3). It arms the
    // day anyone switches an equity config to Tiered, where fees are passed through.
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

    // E2-C5: enforce max_total_implicit_bps.
    //
    // This field is set on EVERY asset config (75-200 bps depending on tier) and was read by
    // NOTHING -- the documented cap on total implicit cost did not exist. max_impact_bps
    // bounds the market-impact term alone; nothing bounded spread + impact together.
    //
    // It should not bind in normal conditions: the impact term is already capped below this
    // value and futures spreads are a tick or two, so the sum stays well under. That is
    // precisely why it went unnoticed. It is a tail guard, so when it DOES bind that is worth
    // seeing rather than absorbing silently -- hence the warning.
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

double TransactionCostManager::get_annual_volatility(const std::string& symbol) const {
    return spread_model_.get_annual_volatility(symbol);
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
            // E2-F14: register the untiered equity default rather than skipping.
            //
            // Skipping left the symbol ABSENT from configs_, and an absent symbol does NOT
            // fall back to the equity default -- AssetCostConfigRegistry::get_config() only
            // reaches get_equity_default_config() when the caller passes
            // AssetType::EQUITY, and NONE of the nine production call sites passes an
            // AssetType at all. So an unregistered equity resolved to the FUTURES default:
            // $1.50 per SHARE instead of $0.005, and point_value 100 instead of 1, i.e. 100x
            // the slippage. The old WARN here claimed "will use equity default if traded",
            // which was simply untrue.
            //
            // Worse, configs_ is one flat symbol->config map shared across asset classes, and
            // real tickers collide with futures roots: CL is Colgate-Palmolive AND Crude Oil,
            // ES is Eversource AND the E-mini S&P. An unregistered CL equity would have
            // resolved to the Crude Oil config -- point_value 1000.
            //
            // Registering the default keeps every configured equity inside the equity cost
            // model, so the futures fallthrough is unreachable for them regardless of what
            // any call site passes. Fail-safe by construction rather than by every caller
            // remembering an argument.
            WARN("register_equity_costs_from_bars: no bars for " + symbol +
                 " -- registering the untiered equity default so it can never resolve to a "
                 "futures cost config");
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
            // E2-F14: same reasoning as the no-bars branch above -- never leave a configured
            // equity unregistered, or it inherits futures pricing.
            WARN("register_equity_costs_from_bars: zero ADV for " + symbol +
                 " over " + std::to_string(n) + " bars -- registering the untiered equity "
                 "default rather than leaving it to resolve as a future");
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

void TransactionCostManager::clear_all_data() {
    spread_model_.clear_all();
    impact_model_.clear_all();
}

namespace {

// Audit §3.2 risk scoring: count high-risk flags and map to annual base
// borrow rate. Dollar-volume substitutes for market cap (audit fallback).
double base_rate_from_flags(int flag_count) {
    if (flag_count <= 0) return 0.0025;   //  25 bps -- ETB / liquid stocks
    if (flag_count == 1) return 0.0050;   //  50 bps -- one tilt away from clean
    if (flag_count == 2) return 0.0150;   // 150 bps -- borderline HTB
    return 0.0500;                        // 500 bps -- HTB+ proxy
}

}  // namespace

std::unordered_map<std::string, double>
TransactionCostManager::calculate_overnight_borrow_fees(
    const std::unordered_map<std::string, Position>& positions,
    const std::unordered_map<std::string, double>& current_prices,
    const InstrumentRegistry& registry) const {

    std::unordered_map<std::string, double> fees;

    for (const auto& [symbol, position] : positions) {
        const double qty = position.quantity.as_double();
        if (qty >= 0.0) {
            continue;  // Only shorts accrue borrow fees.
        }

        auto equity = registry.get_equity_instrument(symbol);
        if (!equity) {
            // Non-equity (e.g., short futures) doesn't pay equity borrow fees.
            continue;
        }

        // Resolve current price.
        double price = 0.0;
        auto price_it = current_prices.find(symbol);
        if (price_it != current_prices.end()) {
            price = price_it->second;
        } else {
            price = position.average_price.as_double();
            WARN("calculate_overnight_borrow_fees: no current price for " +
                 symbol + ", using avg_price " + std::to_string(price));
        }
        if (price <= 0.0) {
            WARN("calculate_overnight_borrow_fees: non-positive price for " +
                 symbol + ", skipping");
            continue;
        }

        const double short_position_value = std::abs(qty) * price;
        const auto& spec = equity->get_spec();
        double annual_rate;

        if (spec.borrow_rate_override >= 0.0) {
            // Broker-provided rate or test override; formula bypassed.
            annual_rate = spec.borrow_rate_override;
        } else {
            // Multi-factor scoring per audit §3.2.
            int flag_count = 0;

            // Dollar-volume signal: ADV × price. <$5M/day flags as high risk.
            const double adv = impact_model_.get_adv(symbol);
            const double dollar_volume = adv * price;
            if (dollar_volume > 0.0 && dollar_volume < 5'000'000.0) {
                ++flag_count;
            }

            // Price-level signal: penny / micro-cap risk.
            if (price < 5.0) {
                ++flag_count;
            }

            // HTB override: explicit is_easy_to_borrow=false forces high tier.
            if (!spec.is_easy_to_borrow) {
                ++flag_count;
            }

            const double base = base_rate_from_flags(flag_count);

            // Volatility multiplier: clamp(annual_vol / 0.25, 1.0, 3.0).
            const double annual_vol = spread_model_.get_annual_volatility(symbol);
            double vol_mult = annual_vol / 0.25;
            if (vol_mult < 1.0) vol_mult = 1.0;
            if (vol_mult > 3.0) vol_mult = 3.0;

            annual_rate = base * vol_mult;
        }

        // Daily fee charged per overnight position held.
        const double daily_fee = annual_rate * short_position_value / 365.0;
        fees[symbol] = daily_fee;
    }

    return fees;
}

}  // namespace transaction_cost
}  // namespace trade_ngin
