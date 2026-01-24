#pragma once

#include <memory>
#include <string>
#include <vector>

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

        Config() : explicit_fee_per_contract(1.75), spread_config(), impact_config() {}
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
        double reference_price) const;

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
     * @return Detailed cost breakdown
     */
    TransactionCostResult calculate_costs(
        const std::string& symbol,
        double quantity,
        double reference_price,
        double adv,
        double volatility_multiplier) const;

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
