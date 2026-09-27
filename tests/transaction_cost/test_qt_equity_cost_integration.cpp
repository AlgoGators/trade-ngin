#include <gtest/gtest.h>
#include <unordered_map>
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/transaction_cost/transaction_cost_manager.hpp"

using trade_ngin::AssetType;
using trade_ngin::Bar;
using trade_ngin::Decimal;
using namespace trade_ngin::transaction_cost;

namespace {
void same_costs(const TransactionCostResult& a, const TransactionCostResult& b) {
    EXPECT_DOUBLE_EQ(a.commissions_fees, b.commissions_fees);
    EXPECT_DOUBLE_EQ(a.spread_price_impact, b.spread_price_impact);
    EXPECT_DOUBLE_EQ(a.market_impact_price_impact, b.market_impact_price_impact);
    EXPECT_DOUBLE_EQ(a.implicit_price_impact, b.implicit_price_impact);
    EXPECT_DOUBLE_EQ(a.slippage_market_impact, b.slippage_market_impact);
    EXPECT_DOUBLE_EQ(a.total_transaction_costs, b.total_transaction_costs);
}
AssetCostConfig equity(const std::string& symbol) {
    auto config = AssetCostConfigRegistry::get_equity_default_config();
    config.symbol = symbol;
    return config;
}
Bar bar(double price, double volume) {
    Bar result;
    result.close = Decimal(price);
    result.volume = volume;
    return result;
}
}

TEST(QtEquityCostIntegration, ExplicitEquityFallbackUsesExistingShareSchedule) {
    TransactionCostManager manager;
    CostChargeObservation used;
    auto result = manager.calculate_costs("SYN_EQUITY", 500, 100, 1000000, 1,
                                          AssetType::EQUITY, &used);
    EXPECT_DOUBLE_EQ(result.commissions_fees, 2.5);
    EXPECT_EQ(used.commission_per_unit, 0.005);
    EXPECT_EQ(used.min_commission_per_order, 1);
    EXPECT_EQ(used.max_commission_pct, 0.01);
    EXPECT_FALSE(used.explicit_fee_per_contract.has_value());
    EXPECT_FALSE(used.max_commission_per_order.has_value());
    EXPECT_EQ(used.point_value, 1);
    EXPECT_EQ(used.apply_regulatory_fees, false);
    EXPECT_FALSE(used.sec_fee_per_million.has_value());
    EXPECT_EQ(used.asset_lookup.path, AssetLookupPath::fallback);
    same_costs(result, manager.calculate_costs("SYN_EQUITY", 500, 100, 1000000, 1,
                                              AssetType::EQUITY));
}

TEST(QtEquityCostIntegration, PercentageCeilingAppliesAfterFloorForFractionalShares) {
    TransactionCostManager manager;
    EXPECT_NEAR(manager.calculate_costs("SYN", 0.081001, 230.86, AssetType::EQUITY)
                    .commissions_fees, 0.01 * 0.081001 * 230.86, 1e-12);
    EXPECT_DOUBLE_EQ(manager.calculate_costs("SYN", 7, 300, AssetType::EQUITY)
                         .commissions_fees, 1);
    EXPECT_DOUBLE_EQ(manager.calculate_costs("SYN", 10000, 300, AssetType::EQUITY)
                         .commissions_fees, 50);
    EXPECT_DOUBLE_EQ(manager.calculate_costs("SYN", 0, 300, AssetType::EQUITY)
                         .commissions_fees, 0);
}

TEST(QtEquityCostIntegration, EnabledRegulatoryFeesReadOnlyOnSignedSellBranch) {
    TransactionCostManager manager;
    auto config = equity("TIERED_SYN");
    config.commission_per_unit = 0.0035;
    config.min_commission_per_order = 0.35;
    config.max_commission_pct = -1;
    config.max_commission_per_order = 1000000000;
    config.apply_regulatory_fees = true;
    manager.register_asset_config(config);
    CostChargeObservation used;
    auto buy = manager.calculate_costs(config.symbol, 1000, 50, 1000000, 1,
                                       AssetType::EQUITY, &used);
    EXPECT_FALSE(used.sec_fee_per_million.has_value());
    EXPECT_EQ(used.max_commission_per_order, config.max_commission_per_order);
    auto sell = manager.calculate_costs(config.symbol, -1000, 50, 1000000, 1,
                                        AssetType::EQUITY, &used);
    EXPECT_NEAR(sell.commissions_fees - buy.commissions_fees,
                0.05 * 20.60 + 1000 * 0.000195, 1e-12);
    EXPECT_EQ(used.sec_fee_per_million, config.sec_fee_per_million);
    EXPECT_EQ(used.finra_taf_per_share, config.finra_taf_per_share);
    EXPECT_EQ(used.finra_taf_cap_per_trade, config.finra_taf_cap_per_trade);
    config.apply_regulatory_fees = false;
    manager.register_asset_config(config);
    same_costs(manager.calculate_costs(config.symbol, -1000, 50, AssetType::EQUITY),
               manager.calculate_costs(config.symbol, 1000, 50, AssetType::EQUITY));
}

TEST(QtEquityCostIntegration, EquityRegistrationCannotReplaceFutureOrStripEquityDots) {
    TransactionCostManager manager;
    auto original_future = manager.calculate_costs("ES", 4, 100, 1000000, 1);
    auto original_root = manager.calculate_costs("ES.v.0", 4, 100, 1000000, 1);
    manager.register_asset_config(equity("ES"));
    EXPECT_EQ(manager.get_asset_config("ES", AssetType::EQUITY).point_value, 1);
    EXPECT_EQ(manager.get_asset_config("ES").point_value, 50);
    same_costs(original_future, manager.calculate_costs("ES", 4, 100, 1000000, 1));
    same_costs(original_root, manager.calculate_costs("ES.v.0", 4, 100, 1000000, 1));
    CostChargeObservation used;
    manager.calculate_costs("ES.B", 4, 100, AssetType::EQUITY, &used);
    EXPECT_EQ(used.asset_lookup.path, AssetLookupPath::fallback);
    EXPECT_EQ(used.point_value, 1);
}

TEST(QtEquityCostIntegration, BarRegistrationUsesExistingAdvTiersAndEquityFallback) {
    TransactionCostManager manager;
    std::unordered_map<std::string, std::vector<Bar>> bars{
        {"MEGA", {bar(50, 1), bar(50, 12000000), bar(50, 14000000)}},
        {"PENNY", {bar(0.5, 1000)}}, {"ZERO", {bar(10, 0)}}};
    EXPECT_EQ(manager.register_equity_costs_from_bars(
                  {"MEGA", "PENNY", "ZERO", "MISSING"}, bars, 2), 4);
    auto mega = manager.get_asset_config("MEGA", AssetType::EQUITY);
    EXPECT_EQ(mega.baseline_spread_ticks, 1);
    EXPECT_EQ(mega.max_impact_bps, 50);
    EXPECT_EQ(mega.max_total_implicit_bps, 75);
    auto penny = manager.get_asset_config("PENNY", AssetType::EQUITY);
    EXPECT_EQ(penny.tick_size, 0.0001);
    EXPECT_EQ(penny.baseline_spread_ticks, 10);
    EXPECT_EQ(manager.get_asset_config("ZERO", AssetType::EQUITY).point_value, 1);
    EXPECT_EQ(manager.get_asset_config("MISSING", AssetType::EQUITY).commission_per_unit, 0.005);
    EXPECT_EQ(bars.at("MEGA").size(), 3u);
    EXPECT_EQ(bars.at("MEGA")[0].volume, 1);
    EXPECT_EQ(bars.at("PENNY")[0].close, Decimal(0.5));
}

TEST(QtEquityCostIntegration, EquitySpreadTickAndImplicitCapKeepFutureMathUnchanged) {
    TransactionCostManager manager;
    auto config = equity("EQ_TICK");
    config.baseline_spread_ticks = 0.1;
    config.min_spread_ticks = 0.1;
    config.max_spread_ticks = 5;
    manager.register_asset_config(config);
    CostChargeObservation used;
    auto standard = manager.calculate_costs(config.symbol, 1, 100, 1000000, 1,
                                            AssetType::EQUITY, &used);
    EXPECT_DOUBLE_EQ(standard.spread_price_impact, 0.005);
    EXPECT_EQ(used.spread.tick_constrained, false);
    config.tick_constrained = true;
    manager.register_asset_config(config);
    EXPECT_DOUBLE_EQ(manager.calculate_costs(config.symbol, 1, 100, 1000000, 1,
                                             AssetType::EQUITY).spread_price_impact, 0.0025);
    config.max_total_implicit_bps = 0.001;
    manager.register_asset_config(config);
    auto capped = manager.calculate_costs(config.symbol, 1, 100, 1000000, 1,
                                          AssetType::EQUITY, &used);
    EXPECT_DOUBLE_EQ(capped.implicit_price_impact, 0.00001);
    EXPECT_EQ(used.max_total_implicit_bps, 0.001);
    config.asset_type = AssetType::FUTURE;
    config.symbol = "LEGACY";
    manager.register_asset_config(config);
    auto legacy = manager.calculate_costs(config.symbol, 1, 100, 1000000, 1, &used);
    EXPECT_GT(legacy.implicit_price_impact, 0.00001);
    EXPECT_FALSE(used.max_total_implicit_bps.has_value());
    EXPECT_FALSE(used.spread.tick_constrained.has_value());
}

TEST(QtEquityCostIntegration, LegacyUnknownChargesAndReusedObservationRemainFutureScoped) {
    TransactionCostManager manager;
    CostChargeObservation used;
    manager.calculate_costs("SYN", -10, 50, AssetType::EQUITY, &used);
    auto legacy = manager.calculate_costs("UNKNOWN", 100, 50, &used);
    EXPECT_DOUBLE_EQ(legacy.commissions_fees, 150);
    EXPECT_EQ(used.explicit_fee_per_contract, 1.5);
    EXPECT_EQ(used.point_value, 100);
    EXPECT_FALSE(used.commission_per_unit.has_value());
    EXPECT_FALSE(used.apply_regulatory_fees.has_value());
    EXPECT_FALSE(used.max_total_implicit_bps.has_value());
    EXPECT_EQ(used.input_source, CostInputSource::internally_tracked);
}
