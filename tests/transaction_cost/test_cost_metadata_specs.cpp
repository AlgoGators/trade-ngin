// CM1: the transaction cost model prices a future with the metadata's contract specs, the same
// source the strategy sizes with (metadata.contract_metadata through the InstrumentRegistry), not
// with a point value and tick of its own. These tests use only the registry and calculate_costs /
// get_asset_config, so they build on the source before CM1 as well, where they fail: the cost
// model read the per-symbol table in asset_cost_config.cpp (6E point_value 125000, ZT tick 1/128)
// whatever the metadata said.

#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <unordered_map>

#include "../core/test_base.hpp"
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/instruments/equity.hpp"
#include "trade_ngin/instruments/futures.hpp"
#include "trade_ngin/instruments/instrument.hpp"
#include "trade_ngin/instruments/option.hpp"
#include "trade_ngin/transaction_cost/transaction_cost_manager.hpp"

// Reach into the registry singleton so each test starts from an empty map and leaves the map
// as it found it. Every header the registry's header includes is included above, so the define
// below reaches the registry's own declaration only.
#define private public
#include "trade_ngin/instruments/instrument_registry.hpp"
#undef private

using namespace trade_ngin;
using namespace trade_ngin::transaction_cost;
using namespace trade_ngin::testing;

namespace {

// A futures row as the metadata gives it: contract size, tick in price units, tick in dollars.
std::shared_ptr<FuturesInstrument> contract(const std::string& root, double contract_size,
                                            double tick) {
    FuturesSpec s;
    s.root_symbol = root;
    s.exchange = "CME";
    s.currency = "USD";
    s.multiplier = contract_size;
    s.tick_size = tick;
    s.commission_per_contract = 0.0;
    s.initial_margin = 2500.0;
    s.maintenance_margin = 2500.0;
    s.weight = 1.0;
    return std::make_shared<FuturesInstrument>(root, s);
}

}  // namespace

class CostMetadataSpecs : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        auto& registry = InstrumentRegistry::instance();
        saved_ = registry.instruments_;
        registry.instruments_.clear();
    }
    void TearDown() override {
        InstrumentRegistry::instance().instruments_ = saved_;
        TestBase::TearDown();
    }
    std::unordered_map<std::string, std::shared_ptr<Instrument>> saved_;
};

// The metadata says one 6E contract is 12,500 euros (a micro-sized row). The strategy sizes on
// 12,500, so the fill's spread and impact dollars must be priced on 12,500 too:
// implicit price impact x |qty| x 12,500. Before CM1 they were x 125,000, ten times the book's.
TEST_F(CostMetadataSpecs, ExecutionCostFollowsAChangedMetadataMultiplier) {
    auto& registry = InstrumentRegistry::instance();
    TransactionCostManager tcm;

    registry.register_instrument("6E", contract("6E", 12500.0, 0.00005));
    const auto micro = tcm.calculate_costs("6E.v.0", 3.0, 1.10, 1.0e6, 1.0);
    EXPECT_DOUBLE_EQ(micro.slippage_market_impact, micro.implicit_price_impact * 3.0 * 12500.0)
        << "the spread and impact dollars must use the metadata's 12,500, not the cost table's "
           "125,000";

    registry.register_instrument("6E", contract("6E", 125000.0, 0.00005));
    const auto full = tcm.calculate_costs("6E.v.0", 3.0, 1.10, 1.0e6, 1.0);
    EXPECT_DOUBLE_EQ(full.slippage_market_impact, full.implicit_price_impact * 3.0 * 125000.0);

    // Same trade, same price units: only the contract size moved, so the implicit dollars scale
    // by exactly 12,500 / 125,000 and the per-contract fee does not move.
    EXPECT_DOUBLE_EQ(micro.implicit_price_impact, full.implicit_price_impact);
    EXPECT_NEAR(micro.slippage_market_impact / full.slippage_market_impact, 0.1, 1e-12);
    EXPECT_DOUBLE_EQ(micro.commissions_fees, full.commissions_fees);
}

// The config the cost model reports for a symbol is the one it prices with.
TEST_F(CostMetadataSpecs, ReportedAssetConfigCarriesTheMetadataPointValue) {
    InstrumentRegistry::instance().register_instrument("6E", contract("6E", 12500.0, 0.0001));
    TransactionCostManager tcm;
    const auto cfg = tcm.get_asset_config("6E.v.0");
    EXPECT_DOUBLE_EQ(cfg.point_value, 12500.0);
    EXPECT_DOUBLE_EQ(cfg.tick_size, 0.0001);
}

// ZT: the metadata's tick is 1/256 of a point (0.00390625, $7.8125 at 2,000; 1/8 of 1/32); the
// cost table carried 1/128 (ZF's). The spread term is spread_cost_multiplier (0.25) x ticks (1)
// x tick, so with the metadata's tick it is half the table's: $1.953125 less per contract.
TEST_F(CostMetadataSpecs, SpreadFollowsTheMetadataTick) {
    InstrumentRegistry::instance().register_instrument("ZT", contract("ZT", 2000.0, 0.00390625));
    TransactionCostManager tcm;
    const auto cost = tcm.calculate_costs("ZT.v.0", 1.0, 104.0, 1.0e6, 1.0);
    EXPECT_DOUBLE_EQ(cost.spread_price_impact, 0.25 * 1.0 * 0.00390625);
    EXPECT_DOUBLE_EQ(cost.slippage_market_impact, cost.implicit_price_impact * 1.0 * 2000.0);
}

// The cost model looks the contract up by the symbol without its continuous-contract suffix, as
// the strategies and the P&L managers do, so "ES.v.0" prices the ES row the book was sized on
// (50), not the MES row the registry's micro remap would hand a suffixed lookup (5).
TEST_F(CostMetadataSpecs, SuffixedSymbolPricesTheRowTheStrategySizesOn) {
    auto& registry = InstrumentRegistry::instance();
    registry.register_instrument("ES", contract("ES", 50.0, 0.25));
    registry.register_instrument("MES", contract("MES", 5.0, 0.25));
    TransactionCostManager tcm;
    EXPECT_DOUBLE_EQ(tcm.get_asset_config("ES.v.0").point_value, 50.0);
    EXPECT_DOUBLE_EQ(tcm.get_asset_config("MES.v.0").point_value, 5.0);
}
