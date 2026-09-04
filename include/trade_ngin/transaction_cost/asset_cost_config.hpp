#pragma once

#include <map>
#include <optional>
#include <string>
#include "trade_ngin/core/types.hpp"

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

    // Asset type for asset-class-aware defaults
    AssetType asset_type = AssetType::FUTURE;  // default preserves backward compat

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
    bool tick_constrained = false;  // Nov 2025 Rule 612: half-penny tick for TWAQS <= $0.015

    // Per-unit commission override (-1.0 = use manager's global fee_per_contract)
    // For equities: set to ~0.005 (IBKR Pro $0.005/share)
    // For futures: leave as -1.0 to use the global explicit_fee_per_contract
    double commission_per_unit = -1.0;

    // Min/max commission per order (only applied when commission_per_unit >= 0)
    double min_commission_per_order = 0.0;
    double max_commission_per_order = 1e9;  // effectively no cap by default

    // Percentage-based commission cap (-1.0 = use flat max_commission_per_order)
    // IBKR Tiered: 0.5% of trade value; IBKR Fixed: 1.0% of trade value
    double max_commission_pct = -1.0;

    // Regulatory fees (equity sell-side only)
    // Configurable per-symbol so historical backtests can use period-correct rates
    double sec_fee_per_million = 20.60;       // SEC fee: $20.60 per $1M of sell proceeds (FY2026)
    double finra_taf_per_share = 0.000195;    // FINRA TAF: $0.000195/share on sells (2026)
    double finra_taf_cap_per_trade = 9.79;    // FINRA TAF cap per trade (2026)
    bool apply_regulatory_fees = false;        // Only true for equities

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
     * @param asset_type Optional asset type for fallback defaults (EQUITY gets equity defaults)
     * @return Config for the symbol, or asset-class-appropriate default if not found
     */
    AssetCostConfig get_config(const std::string& symbol,
                               AssetType asset_type = AssetType::NONE) const;

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
     * @brief Get default configuration for unknown futures
     */
    static AssetCostConfig get_default_config();

    /**
     * @brief Get default configuration for unknown equities
     * point_value=1.0, commission=$0.005/share, IBKR Pro structure
     */
    static AssetCostConfig get_equity_default_config();

    /**
     * @brief Get tiered equity config based on market data
     * Classifies by ADV into mega/large/mid/small/penny tiers
     * Calibrated to Almgren (2005) and standard TCA literature
     * @param price Current stock price
     * @param adv Average daily volume in shares
     * @return Tier-appropriate cost config
     */
    static AssetCostConfig get_tiered_equity_config(double price, double adv);

private:
    std::map<std::string, AssetCostConfig> configs_;

    void initialize_default_configs();
};

}  // namespace transaction_cost
}  // namespace trade_ngin
