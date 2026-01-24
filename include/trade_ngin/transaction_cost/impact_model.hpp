#pragma once

#include <cmath>
#include <deque>
#include <map>
#include <string>

#include "trade_ngin/transaction_cost/asset_cost_config.hpp"

namespace trade_ngin {
namespace transaction_cost {

/**
 * @brief Square-root market impact model
 *
 * Implements the standard square-root impact model:
 *   impact_bps = k_bps * sqrt(participation)
 *   where participation = |qty| / ADV
 *
 * Key features:
 * - ADV-based coefficient selection (liquidity buckets)
 * - Rolling ADV tracking with configurable lookback
 * - Impact capping to prevent blowups
 */
class ImpactModel {
public:
    /**
     * @brief Configuration for impact model
     */
    struct Config {
        size_t adv_lookback_days;  // Days for rolling ADV calculation
        double min_adv;            // Floor for ADV to prevent division issues
        double min_participation;  // Floor for participation rate
        double max_participation;  // Cap for participation rate (10%)

        Config()
            : adv_lookback_days(20), min_adv(100.0), min_participation(0.0), max_participation(0.1) {}
    };

    explicit ImpactModel(const Config& config = Config());

    /**
     * @brief Calculate market impact per contract in price units
     *
     * @param quantity Absolute trade quantity (|qty|)
     * @param reference_price Reference fill price
     * @param adv Average daily volume (20-day rolling)
     * @param asset_config Asset configuration (for caps)
     * @return Market impact in price units per contract
     *
     * Formula:
     *   participation = |qty| / ADV
     *   impact_bps = k_bps(ADV) * sqrt(participation)
     *   impact_bps = min(impact_bps, max_impact_bps)
     *   market_impact_price = (impact_bps / 10000) * ref_price
     */
    double calculate_market_impact(
        double quantity,
        double reference_price,
        double adv,
        const AssetCostConfig& asset_config) const;

    /**
     * @brief Get impact coefficient based on ADV bucket
     *
     * Liquidity buckets:
     *   ADV > 1,000,000: k = 10 bps (ultra liquid)
     *   ADV > 200,000:   k = 20 bps (liquid)
     *   ADV > 50,000:    k = 40 bps (medium)
     *   ADV > 20,000:    k = 60 bps (thin)
     *   Otherwise:       k = 80 bps (very thin)
     *
     * @param adv Average daily volume
     * @return Impact coefficient in basis points
     */
    double get_impact_k_bps(double adv) const;

    /**
     * @brief Update rolling volume for ADV calculation
     *
     * Call this daily with the day's volume.
     *
     * @param symbol Instrument symbol
     * @param volume Today's trading volume
     */
    void update_volume(const std::string& symbol, double volume);

    /**
     * @brief Get current ADV for a symbol
     *
     * @param symbol Instrument symbol
     * @return Rolling ADV, or 0 if no data
     */
    double get_adv(const std::string& symbol) const;

    /**
     * @brief Check if we have sufficient volume history
     *
     * @param symbol Instrument symbol
     * @return True if we have at least min_days of volume data
     */
    bool has_sufficient_data(const std::string& symbol, size_t min_days = 5) const;

    /**
     * @brief Clear stored data for a symbol
     */
    void clear_symbol_data(const std::string& symbol);

    /**
     * @brief Clear all stored data
     */
    void clear_all();

private:
    Config config_;

    // Rolling volume per symbol (deque for efficient front removal)
    std::map<std::string, std::deque<double>> symbol_volumes_;
};

}  // namespace transaction_cost
}  // namespace trade_ngin
