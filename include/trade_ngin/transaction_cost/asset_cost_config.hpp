#pragma once

#include <map>
#include <optional>
#include <string>
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/transaction_cost/consumption.hpp"

namespace trade_ngin {
namespace transaction_cost {

/**
 * @brief Per-asset transaction cost configuration
 *
 * Contains microstructure parameters needed for transaction cost calculation:
 * - Spread parameters (tick-based)
 * - Impact caps
 * - Instrument metadata (tick_size, point_value)
 */
struct AssetCostConfig {
    std::string symbol;
    AssetType asset_type = AssetType::FUTURE;
    bool tick_constrained = false;

    // Existing equity schedule from main08b15c00; futures do not read these.
    double commission_per_unit = -1.0;
    double min_commission_per_order = 0.0;
    double max_commission_per_order = 1e9;
    double max_commission_pct = -1.0;
    double sec_fee_per_million = 20.60;
    double finra_taf_per_share = 0.000195;
    double finra_taf_cap_per_trade = 9.79;
    bool apply_regulatory_fees = false;

    // Spread parameters (in ticks)
    double baseline_spread_ticks = 1.0;  // Typical quoted spread
    double min_spread_ticks = 1.0;       // Floor for spread
    double max_spread_ticks = 10.0;      // Cap for spread
    // Multiplier applied to spread cost conversion (price impact).
    // - 0.5 models crossing half the quoted spread (aggressive/marketable orders)
    // - < 0.5 models providing liquidity with adverse selection (limit orders)
    double spread_cost_multiplier = 0.5;

    // Impact parameters
    double max_impact_bps = 100.0;  // Cap for market impact in basis points

    // Instrument metadata
    double tick_size = 0.01;     // Minimum price increment
    double point_value = 1.0;    // Dollar value per point (contract multiplier)

    // Optional: max total implicit cost cap
    double max_total_implicit_bps = 200.0;
};

/**
 * @brief Registry of asset cost configurations
 *
 * Provides per-symbol cost parameters with sensible defaults for common
 * futures contracts. Unknown symbols fall back to conservative defaults.
 */
class AssetCostConfigRegistry {
public:
    AssetCostConfigRegistry();

    /**
     * @brief Get configuration for a symbol
     * @param symbol The instrument symbol
     * @return Config for the symbol, or default config if not found
     */
    AssetCostConfig get_config(const std::string& symbol,
                               AssetLookupObservation* observation = nullptr) const;
    AssetCostConfig get_config(const std::string& symbol, AssetType asset_type,
                               AssetLookupObservation* observation = nullptr) const;

    /**
     * @brief Register or update configuration for a symbol
     * @param config The configuration to register
     */
    void register_config(const AssetCostConfig& config);

    /**
     * @brief Check if a symbol has explicit configuration
     */
    bool has_config(const std::string& symbol) const;

    /**
     * @brief Get default configuration for unknown symbols
     */
    static AssetCostConfig get_default_config();
    static AssetCostConfig get_equity_default_config();
    static AssetCostConfig get_tiered_equity_config(double price, double adv);

private:
    std::map<std::string, AssetCostConfig> configs_;
    // An explicitly typed equity cannot overwrite a futures root with that ticker.
    std::map<std::string, AssetCostConfig> equity_configs_;

    void initialize_default_configs();
};

}  // namespace transaction_cost
}  // namespace trade_ngin
