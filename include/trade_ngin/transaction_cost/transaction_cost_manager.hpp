#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/transaction_cost/asset_cost_config.hpp"
#include "trade_ngin/transaction_cost/impact_model.hpp"
#include "trade_ngin/transaction_cost/spread_model.hpp"

namespace trade_ngin {
namespace transaction_cost {

/**
 * @brief Result of transaction cost calculation
 *
 * Contains detailed breakdown of all cost components.
 */
struct TransactionCostResult {
    // Explicit costs
    double commissions_fees = 0.0;  // |qty| * fee_per_contract (dollars)

    // Implicit cost components (in price units per contract)
    double spread_price_impact = 0.0;         // Half-spread cost in price units
    double market_impact_price_impact = 0.0;  // Market impact in price units

    // Combined implicit (price units and dollars)
    double implicit_price_impact = 0.0;   // spread + market impact (price units)
    double slippage_market_impact = 0.0;  // implicit * |qty| * point_value (dollars)

    // Total cost (dollars)
    double total_transaction_costs = 0.0;  // commissions_fees + slippage_market_impact
};

/**
 * @brief Central orchestrator for transaction cost calculation
 *
 * Combines:
 * - Explicit fees (fixed per contract)
 * - Spread costs (tick-based with volatility regime)
 * - Market impact (square-root model with ADV buckets)
 *
 * Usage:
 *   1. Create manager (typically once per backtest)
 *   2. Call update_market_data() daily to track ADV and volatility
 *   3. Call calculate_costs() for each execution
 */
class TransactionCostManager {
public:
    /**
     * @brief Configuration for the manager
     */
    struct Config {
        // Explicit fee per contract per side
        // Includes: brokerage + exchange + clearing + regulatory
        double explicit_fee_per_contract;

        // Spread model configuration
        SpreadModel::VolatilityConfig spread_config;

        // Impact model configuration
        ImpactModel::Config impact_config;

        Config() : explicit_fee_per_contract(1.50), spread_config(), impact_config() {}
    };

    explicit TransactionCostManager(const Config& config = Config());

    /**
     * @brief Calculate all transaction costs for an execution
     *
     * @param symbol Instrument symbol
     * @param quantity Trade quantity (signed; absolute value used)
     * @param reference_price Fill price (previous day close)
     * @return Detailed cost breakdown
     *
     * The manager uses internally tracked ADV and volatility.
     * If insufficient data, defaults are used (vol_mult=1.0, ADV from config).
     */
    TransactionCostResult calculate_costs(
        const std::string& symbol,
        double quantity,
        double reference_price,
        AssetType asset_type = AssetType::NONE) const;

    /**
     * @brief Calculate costs with explicit ADV and volatility multiplier
     *
     * Use this overload when you want to provide ADV/volatility externally.
     *
     * @param symbol Instrument symbol
     * @param quantity Trade quantity (signed; absolute value used)
     * @param reference_price Fill price
     * @param adv Average daily volume
     * @param volatility_multiplier Volatility regime multiplier (0.8-1.5)
     * @param asset_type Optional asset class for dispatch fallback when the
     *        symbol has no registered cost config. EQUITY callers should pass
     *        AssetType::EQUITY so unregistered symbols get equity defaults
     *        ($0.005/share, $1 min) instead of the futures fallback
     *        ($1.50/share, point_value=100).
     * @return Detailed cost breakdown
     */
    TransactionCostResult calculate_costs(
        const std::string& symbol,
        double quantity,
        double reference_price,
        double adv,
        double volatility_multiplier,
        AssetType asset_type = AssetType::NONE) const;

    /**
     * @brief Update market data for a symbol (call daily)
     *
     * Updates rolling ADV and volatility tracking.
     *
     * @param symbol Instrument symbol
     * @param volume Today's trading volume
     * @param close_price Today's close price
     * @param prev_close_price Previous day's close price (for log return)
     */
    void update_market_data(
        const std::string& symbol,
        double volume,
        double close_price,
        double prev_close_price);

    /**
     * @brief Get current ADV for a symbol
     */
    double get_adv(const std::string& symbol) const;

    /**
     * @brief Get current volatility multiplier for a symbol
     */
    double get_volatility_multiplier(const std::string& symbol) const;

    /**
     * @brief Get asset configuration for a symbol
     */
    AssetCostConfig get_asset_config(const std::string& symbol) const;

    /**
     * @brief Register custom asset configuration
     */
    void register_asset_config(const AssetCostConfig& config);

    /**
     * @brief Register tier-appropriate equity cost configs from recent bars.
     *
     * For each symbol in `symbols`, computes 20-day (configurable) ADV from
     * the most recent bars in `bars_by_symbol` and registers the matching
     * tier returned by AssetCostConfigRegistry::get_tiered_equity_config.
     *
     * Symbols with no bars, fewer than 1 bar, or zero average volume are
     * skipped with a WARN log; the registered config is taken from the
     * tiered equity classifier in asset_cost_config.cpp (mega / large /
     * mid / small / penny). Closes audit §1.1: today, unconfigured
     * equities fall through to the futures default ($1.50/share).
     *
     * @param symbols Equity symbols to register
     * @param bars_by_symbol Recent bars per symbol (most recent at back())
     * @param adv_lookback_days Number of trailing bars to average for ADV
     * @return Count of symbols successfully registered
     */
    int register_equity_costs_from_bars(
        const std::vector<std::string>& symbols,
        const std::unordered_map<std::string, std::vector<Bar>>& bars_by_symbol,
        int adv_lookback_days = 20);

    /**
     * @brief Clear all market data (for new backtest run)
     */
    void clear_all_data();

    /**
     * @brief Get the explicit fee per contract
     */
    double get_explicit_fee_per_contract() const { return config_.explicit_fee_per_contract; }

private:
    Config config_;
    AssetCostConfigRegistry asset_configs_;
    SpreadModel spread_model_;
    ImpactModel impact_model_;
};

}  // namespace transaction_cost
}  // namespace trade_ngin
