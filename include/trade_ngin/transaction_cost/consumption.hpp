#pragma once

#include <cstddef>
#include <optional>

namespace trade_ngin {
namespace transaction_cost {

enum class AssetLookupPath { exact_symbol, pre_dot_root, fallback };
enum class CostInputSource { internally_tracked, explicit_values };

struct AssetLookupObservation {
    std::optional<AssetLookupPath> path;
};

struct SpreadPriceObservation {
    std::optional<bool> tick_constrained;
    std::optional<double> baseline_spread_ticks;
    std::optional<double> min_spread_ticks;
    std::optional<double> max_spread_ticks;
    std::optional<double> spread_cost_multiplier;
    std::optional<double> tick_size;
};

struct VolatilityObservation {
    std::optional<bool> calculation_reached;  // Observation only; no pricing policy change.
    std::optional<double> lambda;
    std::optional<double> min_multiplier;
    std::optional<double> max_multiplier;
};

struct SpreadHistoryObservation {
    std::optional<size_t> lookback_days;
};

struct ImpactPriceObservation {
    std::optional<double> min_adv;
    std::optional<double> min_participation;
    std::optional<double> max_participation;
    std::optional<double> max_impact_bps;
    std::optional<double> selected_k_bps;  // Derived code-defined bucket, not a setting.
};

struct ImpactHistoryObservation {
    std::optional<size_t> adv_lookback_days;
};

struct CostChargeObservation {
    std::optional<CostInputSource> input_source;
    std::optional<double> quantity;
    std::optional<double> reference_price;
    std::optional<double> adv_argument;
    std::optional<double> volatility_multiplier_argument;
    std::optional<double> retrieved_adv;
    std::optional<double> retrieved_volatility_multiplier;
    std::optional<double> effective_adv;
    std::optional<double> effective_volatility_multiplier;
    std::optional<double> explicit_fee_per_contract;
    std::optional<double> commission_per_unit;
    std::optional<double> min_commission_per_order;
    std::optional<double> max_commission_per_order;
    std::optional<double> max_commission_pct;
    std::optional<bool> apply_regulatory_fees;
    std::optional<double> sec_fee_per_million;
    std::optional<double> finra_taf_per_share;
    std::optional<double> finra_taf_cap_per_trade;
    std::optional<double> max_total_implicit_bps;
    std::optional<double> point_value;
    AssetLookupObservation asset_lookup;
    VolatilityObservation volatility;
    SpreadPriceObservation spread;
    ImpactPriceObservation impact;
};

struct MarketDataObservation {
    ImpactHistoryObservation volume;
    SpreadHistoryObservation log_returns;
};

}  // namespace transaction_cost
}  // namespace trade_ngin
