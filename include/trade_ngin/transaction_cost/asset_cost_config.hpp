#pragma once

#include <map>
#include <optional>
#include <string>

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

    // Spread parameters (in ticks)
    double baseline_spread_ticks = 1.0;  // Typical quoted spread
    double min_spread_ticks = 1.0;       // Floor for spread
    double max_spread_ticks = 10.0;      // Cap for spread

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
    AssetCostConfig get_config(const std::string& symbol) const;

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

private:
    std::map<std::string, AssetCostConfig> configs_;

    void initialize_default_configs();
};

}  // namespace transaction_cost
}  // namespace trade_ngin
