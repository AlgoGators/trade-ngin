#include <gtest/gtest.h>
#include "trade_ngin/transaction_cost/transaction_cost_manager.hpp"

using namespace trade_ngin::transaction_cost;

namespace {
AssetCostConfig custom_asset(const std::string& symbol) {
    AssetCostConfig a;
    a.symbol = symbol;
    a.baseline_spread_ticks = 2.0;
    a.min_spread_ticks = 0.5;
    a.max_spread_ticks = 4.0;
    a.spread_cost_multiplier = 0.3;
    a.tick_size = 1.25;
    a.point_value = 40.0;
    a.max_impact_bps = 90.0;
    a.max_total_implicit_bps = 0.01;  // Unused by existing charge math.
    return a;
}
}  // namespace

TEST(TransactionCostConsumption, ExplicitInputsCaptureActualReadsAndCostFields) {
    TransactionCostManager::Config c;
    c.explicit_fee_per_contract = 2.75;
    c.impact_config.min_adv = 150.5;
    c.impact_config.min_participation = 0.02;
    c.impact_config.max_participation = 0.08;
    TransactionCostManager m(c);
    m.register_asset_config(custom_asset("TEST"));
    CostChargeObservation used;
    auto r = m.calculate_costs("TEST", -4, 125, 20000, 2, &used);
    EXPECT_DOUBLE_EQ(r.commissions_fees, 11);
    EXPECT_DOUBLE_EQ(r.spread_price_impact, 1.5);
    EXPECT_NEAR(r.market_impact_price_impact, 0.1414213562373095, 1e-12);
    EXPECT_NEAR(r.implicit_price_impact, 1.6414213562373095, 1e-12);
    EXPECT_NEAR(r.slippage_market_impact, 262.6274169979695, 1e-10);
    EXPECT_NEAR(r.total_transaction_costs, 273.6274169979695, 1e-10);
    EXPECT_EQ(used.input_source, CostInputSource::explicit_values);
    EXPECT_EQ(used.quantity, -4);
    EXPECT_EQ(used.reference_price, 125);
    EXPECT_EQ(used.adv_argument, 20000);
    EXPECT_EQ(used.volatility_multiplier_argument, 2);
    EXPECT_FALSE(used.retrieved_adv.has_value());
    EXPECT_FALSE(used.volatility.lambda.has_value());
    EXPECT_EQ(used.explicit_fee_per_contract, 2.75);
    EXPECT_EQ(used.point_value, 40);
    EXPECT_EQ(used.asset_lookup.path, AssetLookupPath::exact_symbol);
    EXPECT_EQ(used.spread.baseline_spread_ticks, 2);
    EXPECT_EQ(used.impact.min_adv, 150.5);
    EXPECT_EQ(used.impact.max_participation, 0.08);
    EXPECT_EQ(used.impact.max_impact_bps, 90);
    EXPECT_EQ(used.impact.selected_k_bps, 80);
}

TEST(TransactionCostConsumption, TrackedFallbackAndReuseDoNotInventVolatilityReads) {
    TransactionCostManager m;
    CostChargeObservation used;
    m.calculate_costs("UNKNOWN", 0, 100, 50000, 2, &used);
    auto r = m.calculate_costs("UNKNOWN", 0, 100, &used);
    EXPECT_DOUBLE_EQ(r.total_transaction_costs, 0);
    EXPECT_EQ(used.input_source, CostInputSource::internally_tracked);
    EXPECT_EQ(used.retrieved_adv, 0);
    EXPECT_EQ(used.retrieved_volatility_multiplier, 1);
    EXPECT_EQ(used.effective_adv, 100000);
    EXPECT_EQ(used.effective_volatility_multiplier, 1);
    EXPECT_FALSE(used.adv_argument.has_value());
    EXPECT_FALSE(used.volatility.lambda.has_value());
    EXPECT_EQ(used.asset_lookup.path, AssetLookupPath::fallback);
    EXPECT_EQ(used.explicit_fee_per_contract, 1.5);
    EXPECT_EQ(used.point_value, 100);
    EXPECT_TRUE(used.spread.tick_size.has_value());
    EXPECT_TRUE(used.impact.max_impact_bps.has_value());
}

TEST(TransactionCostConsumption, RootLookupAndHistoryGuardKeepSeparateEvidence) {
    TransactionCostManager::Config c;
    c.spread_config.lookback_days = 2;
    c.spread_config.lambda = 0.4;
    c.impact_config.adv_lookback_days = 2;
    TransactionCostManager m(c);
    m.register_asset_config(custom_asset("ROOT"));
    MarketDataObservation history;
    m.update_market_data("ROOT.v.0", 100, 100, 100, &history);
    EXPECT_EQ(history.volume.adv_lookback_days, 2u);
    EXPECT_EQ(history.log_returns.lookback_days, 2u);
    m.update_market_data("ROOT.v.0", 200, 0, 100, &history);
    EXPECT_EQ(history.volume.adv_lookback_days, 2u);
    EXPECT_FALSE(history.log_returns.lookback_days.has_value());
    m.update_market_data("ROOT.v.0", 300, 110, 100, &history);
    CostChargeObservation used;
    auto observed = m.calculate_costs("ROOT.v.0", 2, 100, &used);
    EXPECT_EQ(used.asset_lookup.path, AssetLookupPath::pre_dot_root);
    EXPECT_EQ(used.retrieved_adv, 250);
    EXPECT_EQ(used.volatility.lambda, 0.4);
    EXPECT_EQ(used.point_value, 40);
    TransactionCostManager plain(c);
    plain.register_asset_config(custom_asset("ROOT"));
    plain.update_market_data("ROOT.v.0", 100, 100, 100);
    plain.update_market_data("ROOT.v.0", 200, 0, 100);
    plain.update_market_data("ROOT.v.0", 300, 110, 100);
    auto baseline = plain.calculate_costs("ROOT.v.0", 2, 100);
    EXPECT_DOUBLE_EQ(observed.commissions_fees, baseline.commissions_fees);
    EXPECT_DOUBLE_EQ(observed.spread_price_impact, baseline.spread_price_impact);
    EXPECT_DOUBLE_EQ(observed.market_impact_price_impact, baseline.market_impact_price_impact);
    EXPECT_DOUBLE_EQ(observed.implicit_price_impact, baseline.implicit_price_impact);
    EXPECT_DOUBLE_EQ(observed.slippage_market_impact, baseline.slippage_market_impact);
    EXPECT_DOUBLE_EQ(observed.total_transaction_costs, baseline.total_transaction_costs);
    EXPECT_DOUBLE_EQ(m.get_adv("ROOT.v.0"), plain.get_adv("ROOT.v.0"));
    EXPECT_DOUBLE_EQ(m.get_volatility_multiplier("ROOT.v.0"),
                     plain.get_volatility_multiplier("ROOT.v.0"));
    auto explicit_observed = m.calculate_costs("ROOT.v.0", 2, 100, 250, 1, &used);
    auto explicit_baseline = plain.calculate_costs("ROOT.v.0", 2, 100, 250, 1);
    EXPECT_DOUBLE_EQ(explicit_observed.commissions_fees, explicit_baseline.commissions_fees);
    EXPECT_DOUBLE_EQ(explicit_observed.spread_price_impact, explicit_baseline.spread_price_impact);
    EXPECT_DOUBLE_EQ(explicit_observed.market_impact_price_impact,
                     explicit_baseline.market_impact_price_impact);
    EXPECT_DOUBLE_EQ(explicit_observed.implicit_price_impact, explicit_baseline.implicit_price_impact);
    EXPECT_DOUBLE_EQ(explicit_observed.slippage_market_impact,
                     explicit_baseline.slippage_market_impact);
    EXPECT_DOUBLE_EQ(explicit_observed.total_transaction_costs,
                     explicit_baseline.total_transaction_costs);
    EXPECT_EQ(used.input_source, CostInputSource::explicit_values);
    EXPECT_FALSE(used.retrieved_adv.has_value());
    EXPECT_FALSE(used.volatility.lambda.has_value());
    EXPECT_EQ(used.adv_argument, 250);
}
