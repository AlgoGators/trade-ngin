#include <gtest/gtest.h>
#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/transaction_cost/transaction_cost_manager.hpp"

using namespace trade_ngin;
using namespace trade_ngin::transaction_cost;

// Regression test for audit finding §1.1 — Phase 1a-ii wire-up. Before this
// fix, every traded equity needed an explicit hardcoded config in
// TransactionCostManager. The new register_equity_costs_from_bars helper
// computes ADV from recent bars and registers the appropriate tier.

namespace {

std::vector<Bar> synthetic_equity_bars(const std::string& symbol,
                                       int days,
                                       double price,
                                       double volume) {
    std::vector<Bar> bars;
    bars.reserve(days);
    auto now = std::chrono::system_clock::now();
    for (int i = 0; i < days; ++i) {
        Bar b;
        b.symbol = symbol;
        b.timestamp = now - std::chrono::hours(24 * (days - i));
        b.open = price;
        b.high = price * 1.005;
        b.low = price * 0.995;
        b.close = price;
        b.volume = volume;
        bars.push_back(b);
    }
    return bars;
}

}  // namespace

// NVDA-shaped mega-cap: ADV >> 10M, price > $100 → mega-cap tier.
// Expected from get_tiered_equity_config:
//   point_value = 1.0 (per share, not per "contract")
//   commission_per_unit = 0.005 ($0.005/share, IBKR Pro)
//   apply_regulatory_fees = true (SEC + FINRA on sells)
//   baseline_spread_ticks = 1.0 (mega-cap = tight spread)
TEST(ADVClassifierWireupTest, MegaCapSymbolGetsTier1Config) {
    TransactionCostManager::Config tcm_config;
    TransactionCostManager tcm(tcm_config);

    std::unordered_map<std::string, std::vector<Bar>> bars_by_symbol;
    bars_by_symbol["NVDA"] = synthetic_equity_bars("NVDA", /*days=*/30,
                                                    /*price=*/500.0,
                                                    /*volume=*/50'000'000.0);

    int registered = tcm.register_equity_costs_from_bars({"NVDA"}, bars_by_symbol);
    EXPECT_EQ(registered, 1);

    AssetCostConfig cfg = tcm.get_asset_config("NVDA");
    EXPECT_EQ(cfg.asset_type, AssetType::EQUITY);
    EXPECT_DOUBLE_EQ(cfg.point_value, 1.0);
    EXPECT_DOUBLE_EQ(cfg.commission_per_unit, 0.005);
    EXPECT_TRUE(cfg.apply_regulatory_fees);
    EXPECT_LE(cfg.baseline_spread_ticks, 2.0)
        << "Mega-cap should have tight spread (≤2 ticks); got "
        << cfg.baseline_spread_ticks;
}

// Symbol with no bars gets skipped (warning logged); not registered.
TEST(ADVClassifierWireupTest, SymbolWithNoBarsIsSkipped) {
    TransactionCostManager::Config tcm_config;
    TransactionCostManager tcm(tcm_config);

    std::unordered_map<std::string, std::vector<Bar>> bars_by_symbol;
    // No entry for "MSFT".

    int registered = tcm.register_equity_costs_from_bars({"MSFT"}, bars_by_symbol);
    EXPECT_EQ(registered, 0);
}

// Symbol with bars but zero volume is skipped (degenerate ADV).
TEST(ADVClassifierWireupTest, ZeroVolumeIsSkipped) {
    TransactionCostManager::Config tcm_config;
    TransactionCostManager tcm(tcm_config);

    std::unordered_map<std::string, std::vector<Bar>> bars_by_symbol;
    bars_by_symbol["GOOG"] = synthetic_equity_bars("GOOG", 20, 150.0, /*volume=*/0.0);

    int registered = tcm.register_equity_costs_from_bars({"GOOG"}, bars_by_symbol);
    EXPECT_EQ(registered, 0);
}

// Smaller bar history than the lookback window still computes ADV from what
// is available (capped by bars.size()) and registers a config.
TEST(ADVClassifierWireupTest, ShortHistoryStillRegisters) {
    TransactionCostManager::Config tcm_config;
    TransactionCostManager tcm(tcm_config);

    std::unordered_map<std::string, std::vector<Bar>> bars_by_symbol;
    bars_by_symbol["AAPL"] = synthetic_equity_bars("AAPL", /*days=*/3,
                                                    /*price=*/180.0,
                                                    /*volume=*/30'000'000.0);

    int registered = tcm.register_equity_costs_from_bars(
        {"AAPL"}, bars_by_symbol, /*adv_lookback_days=*/20);
    EXPECT_EQ(registered, 1);
    AssetCostConfig cfg = tcm.get_asset_config("AAPL");
    EXPECT_EQ(cfg.asset_type, AssetType::EQUITY);
}

// Multiple symbols at once.
TEST(ADVClassifierWireupTest, MultipleSymbolsBatch) {
    TransactionCostManager::Config tcm_config;
    TransactionCostManager tcm(tcm_config);

    std::unordered_map<std::string, std::vector<Bar>> bars_by_symbol;
    bars_by_symbol["AAPL"] = synthetic_equity_bars("AAPL", 30, 180.0, 30'000'000.0);
    bars_by_symbol["MSFT"] = synthetic_equity_bars("MSFT", 30, 400.0, 20'000'000.0);
    bars_by_symbol["NVDA"] = synthetic_equity_bars("NVDA", 30, 500.0, 50'000'000.0);

    int registered = tcm.register_equity_costs_from_bars(
        {"AAPL", "MSFT", "NVDA"}, bars_by_symbol);
    EXPECT_EQ(registered, 3);
    EXPECT_EQ(tcm.get_asset_config("AAPL").asset_type, AssetType::EQUITY);
    EXPECT_EQ(tcm.get_asset_config("MSFT").asset_type, AssetType::EQUITY);
    EXPECT_EQ(tcm.get_asset_config("NVDA").asset_type, AssetType::EQUITY);
}
