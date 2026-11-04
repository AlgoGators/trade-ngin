// tests/strategy/test_equity_scenarios.cpp
//
// Three equity scenario pins (W25 of issue #118): a very thin stock, a flat
// stock, and an order above 5 percent of ADV. Strategy and cost manager fed
// from the same bars, a mock database, no runner -- the pattern of
// test_penny_stock_lifecycle.cpp.
//
// Scenario pins: no red is expected today. Each case asserts what the code
// does now, so a change to the cost tiers, the impact buckets, the fractional
// gates or the z-score path cannot move these numbers unnoticed.
//
// One finding is pinned rather than fixed, by instruction: a one-cent move
// after a flat stretch opens a FULL-SIZE position at the 2x volatility cap
// (FlatStock tests below). Whether that is a defect is W25(b), answered in
// the PR write-up; any change to the strategy is a separate item.

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "trade_ngin/instruments/equity.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/strategy/mean_reversion.hpp"
#include "trade_ngin/transaction_cost/asset_cost_config.hpp"
#include "trade_ngin/transaction_cost/impact_model.hpp"
#include "trade_ngin/transaction_cost/transaction_cost_manager.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;
using trade_ngin::transaction_cost::AssetCostConfigRegistry;
using trade_ngin::transaction_cost::ImpactModel;
using trade_ngin::transaction_cost::TransactionCostManager;

class EquityScenarioTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();

        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        ASSERT_TRUE(db_->connect().is_ok());

        strategy_config_.capital_allocation = 100000.0;
        strategy_config_.max_leverage = 1.0;
        strategy_config_.asset_classes = {AssetClass::EQUITIES};
        strategy_config_.frequencies = {DataFrequency::DAILY};
        for (const auto& symbol : {kThin, kFlat}) {
            strategy_config_.trading_params[symbol] = 1.0;
            strategy_config_.position_limits[symbol] = 40000.0;
        }

        mr_config_.lookback_period = 20;
        mr_config_.vol_lookback = 20;
        mr_config_.entry_threshold = 2.0;
        mr_config_.exit_threshold = 0.5;
        mr_config_.risk_target = 0.15;
        mr_config_.position_size = 0.1;
        mr_config_.use_stop_loss = false;
        mr_config_.allow_fractional_shares = true;
        mr_config_.fractional_min_price = 1.0;
        mr_config_.fractional_min_adv = 50000.0;

        risk_limits_.max_position_size = 1e9;
        risk_limits_.max_notional_value = 1e9;
        risk_limits_.max_drawdown = 0.9;
        risk_limits_.max_leverage = 10.0;
    }

    void TearDown() override {
        InstrumentRegistry::instance().clear();
        if (db_) {
            db_->disconnect();
            db_.reset();
        }
        TestBase::TearDown();
    }

    std::unique_ptr<MeanReversionStrategy> create_strategy(
        std::shared_ptr<InstrumentRegistry> registry = nullptr) {
        static int id = 0;
        auto strategy = std::make_unique<MeanReversionStrategy>(
            "TEST_SCENARIO_" + std::to_string(++id), strategy_config_, mr_config_, db_,
            registry);
        EXPECT_TRUE(strategy->initialize().is_ok());
        EXPECT_TRUE(strategy->update_risk_limits(risk_limits_).is_ok());
        EXPECT_TRUE(strategy->start().is_ok());
        return strategy;
    }

    // A registry holding one CASH-account equity (the spec's defaults), so the
    // strategy's short clamp has an instrument to ask.
    std::shared_ptr<InstrumentRegistry> cash_registry(const std::string& symbol) {
        auto& registry = InstrumentRegistry::instance();
        registry.clear();
        EXPECT_TRUE(registry.initialize_without_database().is_ok());
        EquitySpec spec;
        spec.exchange = "NASDAQ";
        spec.currency = "USD";
        registry.register_instrument(symbol, std::make_shared<EquityInstrument>(symbol, spec));
        return std::shared_ptr<InstrumentRegistry>(&registry, [](InstrumentRegistry*) {});
    }

    // 60 bars oscillating around `level`, closing well below the mean so the
    // entry branch fires long. Volume is constant so ADV is exactly `volume`.
    std::vector<Bar> oscillating_series(const std::string& symbol, double level,
                                        double volume) const {
        std::vector<Bar> bars;
        const auto now = std::chrono::system_clock::now();
        for (int i = 0; i < 59; ++i) {
            const double close = (i % 2 == 0) ? level * 0.98 : level * 1.02;
            bars.push_back(bar(symbol, close, volume, now - std::chrono::hours(24 * (60 - i))));
        }
        bars.push_back(bar(symbol, level * 0.90, volume, now));
        return bars;
    }

    // `n - 1` closes at `level`, then one close at `last`. With one outlier
    // among n closes, |z| = sqrt(n - 1) whatever the size of the move.
    std::vector<Bar> flat_series(const std::string& symbol, int n, double level, double last,
                                 double volume) const {
        std::vector<Bar> bars;
        const auto now = std::chrono::system_clock::now();
        for (int i = 0; i < n - 1; ++i) {
            bars.push_back(bar(symbol, level, volume, now - std::chrono::hours(24 * (n - i))));
        }
        bars.push_back(bar(symbol, last, volume, now));
        return bars;
    }

    Bar bar(const std::string& symbol, double close, double volume, Timestamp ts) const {
        Bar b;
        b.symbol = symbol;
        b.timestamp = ts;
        b.open = close;
        b.high = close;
        b.low = close;
        b.close = close;
        b.volume = volume;
        return b;
    }

    static constexpr const char* kThin = "THINLY";  // ADV 5,000 at $20
    static constexpr const char* kFlat = "FLATLY";  // 19 closes of 50.00

    std::shared_ptr<MockPostgresDatabase> db_;
    StrategyConfig strategy_config_;
    RiskLimits risk_limits_;
    MeanReversionConfig mr_config_;
};

// ---------------------------------------------------------------------------
// (a) a very thin stock: ADV 5,000 shares at $20.00
// ---------------------------------------------------------------------------

// ADV 5,000 lands in the last tier (every ADV at or below 100,000 does), and
// in the 80 bps impact bucket (every ADV at or below 20,000 does).
TEST_F(EquityScenarioTest, ThinStockGetsTheIlliquidTierAndThe80BpsBucket) {
    TransactionCostManager tcm;
    std::unordered_map<std::string, std::vector<Bar>> bars_by_symbol;
    bars_by_symbol[kThin] = oscillating_series(kThin, 20.0, 5000.0);
    ASSERT_EQ(tcm.register_equity_costs_from_bars({kThin}, bars_by_symbol), 1);

    const auto config = tcm.get_asset_config(kThin);
    EXPECT_EQ(config.asset_type, AssetType::EQUITY);
    EXPECT_DOUBLE_EQ(config.tick_size, 0.01) << "a $20 stock is not sub-dollar";
    EXPECT_DOUBLE_EQ(config.baseline_spread_ticks, 10.0) << "penny / illiquid tier";
    EXPECT_DOUBLE_EQ(config.max_impact_bps, 300.0);
    EXPECT_DOUBLE_EQ(config.max_total_implicit_bps, 500.0);

    // The 80 bps bucket, asserted through the formula so the bucket and the
    // square root are pinned together: 500 shares is 10% participation, and
    // 80 x sqrt(0.10) = 25.298 bps = $0.050596 a share at $20.00.
    ImpactModel impact;
    const double per_share = impact.calculate_market_impact(500.0, 20.0, 5000.0, config);
    EXPECT_NEAR(per_share, (80.0 * std::sqrt(0.10) / 10000.0) * 20.0, 1e-12);
    EXPECT_NEAR(per_share, 0.0506, 5e-5);
}

// ADV below fractional_min_adv forces whole shares even at a fractional-eligible
// price, and the position limit still binds in SHARES on the floored target.
TEST_F(EquityScenarioTest, ThinStockSizesInWholeSharesAndTheShareLimitBinds) {
    double unclamped = 0.0;
    {
        auto strategy = create_strategy();  // limit 40,000, far above the sized target
        ASSERT_TRUE(strategy->on_data(oscillating_series(kThin, 20.0, 5000.0)).is_ok());
        unclamped = strategy->get_position(kThin);
        ASSERT_NE(unclamped, 0.0) << "the scenario did not open; it proves nothing";
        EXPECT_DOUBLE_EQ(unclamped, std::floor(unclamped))
            << "ADV 5,000 < fractional_min_adv 50,000 must fail closed to whole shares";
        ASSERT_LT(unclamped, strategy_config_.position_limits.at(kThin))
            << "fixture precondition: the default limit must NOT bind";
    }

    const double tight = std::floor(unclamped / 2.0);
    strategy_config_.position_limits[kThin] = tight;
    auto strategy = create_strategy();
    ASSERT_TRUE(strategy->on_data(oscillating_series(kThin, 20.0, 5000.0)).is_ok());
    EXPECT_DOUBLE_EQ(strategy->get_position(kThin), tight)
        << "the clamp must cut the target in share space, not notional";
}

// ---------------------------------------------------------------------------
// (b) a flat stock
// ---------------------------------------------------------------------------

// Twenty identical closes: the standard deviation guard returns z = 0, no
// entry, and zero volatility sizes to 0 anyway. A flat stock is never traded.
TEST_F(EquityScenarioTest, TwentyIdenticalClosesGiveNoEntry) {
    auto strategy = create_strategy();
    ASSERT_TRUE(strategy->on_data(flat_series(kFlat, 25, 50.00, 50.00, 500000.0)).is_ok());
    EXPECT_DOUBLE_EQ(strategy->get_position(kFlat), 0.0);
}

// The edge the issue names: 19 closes of 50.00, then 49.99. The one-cent dip
// is |z| = sqrt(19) = 4.36 sigma (with one outlier among n closes,
// |z| = sqrt(n - 1) whatever the size of the move), far over the 2.0 entry
// threshold, and the near-zero volatility pegs the scalar at its 2x cap: a
// FULL-SIZE DOUBLE position opens on a one-cent move after a flat stretch.
// This pins today's behaviour; whether it is a defect is W25(b) in the PR
// write-up, and any strategy change is a separate item.
TEST_F(EquityScenarioTest, OneCentDipAfterAFlatStretchOpensALongAtTheTwoXCap) {
    auto strategy = create_strategy();
    ASSERT_TRUE(strategy->on_data(flat_series(kFlat, 20, 50.00, 49.99, 500000.0)).is_ok());

    const double target = strategy->get_position(kFlat);
    ASSERT_GT(target, 0.0) << "the 4.36-sigma dip must open a long";

    // vol_scalar = risk_target / max(vol, 0.01) capped at 2; the annualised
    // volatility of 19 log returns (one of them -0.0002) is ~0.00073, so the
    // cap binds exactly. Base value is capital x position_size = $10,000;
    // doubled and fractional-eligible (price $49.99, ADV 500,000):
    EXPECT_NEAR(target, 2.0 * (100000.0 * 0.1) / 49.99, 1e-6)
        << "the volatility cap must bind at exactly 2x on a near-flat series";
}

// The mirror image: a one-cent RISE asks for a 4.36-sigma short, and the
// cash-account clamp sets it to 0 when the strategy has an instrument
// registry (a CASH-mode equity is never shorted).
TEST_F(EquityScenarioTest, OneCentRiseAsksForAShortAndTheCashClampZeroesIt) {
    auto strategy = create_strategy(cash_registry(kFlat));
    ASSERT_TRUE(strategy->on_data(flat_series(kFlat, 20, 50.00, 50.01, 500000.0)).is_ok());
    EXPECT_DOUBLE_EQ(strategy->get_position(kFlat), 0.0)
        << "a CASH-account equity short must be clamped to zero";
}

// And the clamp's dependency, pinned: WITHOUT a registry the same bars
// produce a real short target. The clamp lives behind `registry_ != nullptr`,
// so a caller that constructs the strategy without one gets shorts.
TEST_F(EquityScenarioTest, WithoutARegistryTheSameRiseProducesAShortTarget) {
    auto strategy = create_strategy();  // no registry: the clamp cannot ask
    ASSERT_TRUE(strategy->on_data(flat_series(kFlat, 20, 50.00, 50.01, 500000.0)).is_ok());
    EXPECT_LT(strategy->get_position(kFlat), 0.0)
        << "pins that the short gate depends on the registry being present";
}

// ---------------------------------------------------------------------------
// (c) an order above 5 percent of ADV: ADV 1,000,000 shares at $50.00
// ---------------------------------------------------------------------------

// Impact grows with the square root of participation up to the 10% clamp and
// is flat above it. ADV 1,000,000 sits in the 20 bps bucket (the ultra-liquid
// 10 bps bucket starts strictly above 1,000,000).
TEST_F(EquityScenarioTest, ImpactGrowsAsSqrtParticipationAndFlatlinesAboveTenPercent) {
    const auto config = AssetCostConfigRegistry::get_tiered_equity_config(50.0, 1000000.0);
    ImpactModel impact;

    struct Case {
        double shares;
        double participation;  // after the clamp
    };
    const Case cases[] = {
        {10000.0, 0.01},   // 1%:  20 x sqrt(0.01) = 2.00 bps = $0.0100
        {50000.0, 0.05},   // 5%:  20 x sqrt(0.05) = 4.47 bps = $0.0224
        {100000.0, 0.10},  // 10%: 20 x sqrt(0.10) = 6.32 bps = $0.0316
        {200000.0, 0.10},  // 20%: clamped to 10%, same $0.0316
    };
    for (const auto& c : cases) {
        const double per_share = impact.calculate_market_impact(c.shares, 50.0, 1000000.0, config);
        EXPECT_NEAR(per_share, (20.0 * std::sqrt(c.participation) / 10000.0) * 50.0, 1e-12)
            << c.shares << " shares";
    }

    // The square-root shape, stated as a ratio so the test cannot pass on a
    // linear model: 100x the quantity is 10x the per-share impact.
    const double at_point1pct = impact.calculate_market_impact(1000.0, 50.0, 1000000.0, config);
    const double at_10pct = impact.calculate_market_impact(100000.0, 50.0, 1000000.0, config);
    EXPECT_NEAR(at_10pct / at_point1pct, 10.0, 1e-9);

    // The flat line above the clamp.
    const double at_20pct = impact.calculate_market_impact(200000.0, 50.0, 1000000.0, config);
    EXPECT_DOUBLE_EQ(at_20pct, at_10pct);
}

// Nothing on the equity cost path shrinks an order to a participation limit:
// the clamp caps the PER-SHARE impact, and the whole quantity is billed at
// that capped rate. 200,000 shares against ADV 1,000,000 is costed on all
// 200,000 shares. (No app constructs the ExecutionEngine, so its
// participation limit is not on the equity path; this asserts the cost
// manager's half of that fact.)
TEST_F(EquityScenarioTest, TheFullQuantityIsBilledAboveTheParticipationClamp) {
    TransactionCostManager tcm;
    std::unordered_map<std::string, std::vector<Bar>> bars_by_symbol;
    bars_by_symbol[kThin] = oscillating_series(kThin, 50.0, 1000000.0);
    ASSERT_EQ(tcm.register_equity_costs_from_bars({kThin}, bars_by_symbol), 1);

    const auto cost =
        tcm.calculate_costs(kThin, 200000.0, 50.0, 1000000.0, 1.0, AssetType::EQUITY);

    EXPECT_NEAR(cost.slippage_market_impact, cost.implicit_price_impact * 200000.0 * 1.0, 1e-6)
        << "the dollar slippage must carry the full 200,000 shares, not a shrunk order";
    EXPECT_NEAR(cost.total_transaction_costs,
                cost.commissions_fees + cost.slippage_market_impact, 1e-9);
}
