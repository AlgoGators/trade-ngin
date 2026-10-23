// Pins the two StrategyInterface hooks added for NAV-sized strategies:
//   on_portfolio_value(double)  -- default no-op
//   required_warmup_days()      -- default 0
// Every strategy that does not override them must be unaffected by the coordinator
// calling them, so this is the guard that keeps the futures and MeanReversion paths
// byte-identical.
#include <gtest/gtest.h>
#include <memory>
#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "../portfolio/mock_strategy.hpp"
#include "trade_ngin/strategy/base_strategy.hpp"
#include "trade_ngin/strategy/mean_reversion.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

StrategyConfig make_config() {
    StrategyConfig sc;
    sc.capital_allocation = 100000.0;
    sc.asset_classes = {AssetClass::EQUITIES};
    sc.frequencies = {DataFrequency::DAILY};
    sc.trading_params["AAPL"] = 1.0;
    sc.position_limits["AAPL"] = 1000.0;
    return sc;
}

class OverridingStrategy : public MockStrategy {
public:
    using MockStrategy::MockStrategy;
    void on_portfolio_value(double nav) override { seen_nav = nav; }
    int required_warmup_days() const override { return 37; }
    double seen_nav{-1.0};
};

}  // namespace

class StrategyInterfaceHooksTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
    }
    std::shared_ptr<MockPostgresDatabase> db_;
};

TEST_F(StrategyInterfaceHooksTest, DefaultsAreNoOps) {
    auto strategy = std::make_shared<MockStrategy>("HOOK_DEFAULTS", make_config(), db_);
    const auto config_before = strategy->get_config().capital_allocation;
    const auto positions_before = strategy->get_positions().size();

    strategy->on_portfolio_value(123456.0);  // must not throw or change state
    EXPECT_EQ(strategy->required_warmup_days(), 0);
    EXPECT_DOUBLE_EQ(strategy->get_config().capital_allocation, config_before);
    EXPECT_EQ(strategy->get_positions().size(), positions_before);
}

TEST_F(StrategyInterfaceHooksTest, MeanReversionKeepsTheDefaults) {
    MeanReversionConfig mr;
    auto strategy =
        std::make_shared<MeanReversionStrategy>("HOOK_MR", make_config(), mr, db_, nullptr);
    EXPECT_EQ(strategy->required_warmup_days(), 0);
    strategy->on_portfolio_value(1.0e6);
    EXPECT_DOUBLE_EQ(strategy->get_config().capital_allocation, 100000.0);
}

TEST_F(StrategyInterfaceHooksTest, OverridesAreReachedThroughTheInterface) {
    auto strategy = std::make_shared<OverridingStrategy>("HOOK_OVERRIDE", make_config(), db_);
    std::shared_ptr<StrategyInterface> iface = strategy;
    iface->on_portfolio_value(250000.0);
    EXPECT_DOUBLE_EQ(strategy->seen_nav, 250000.0);
    EXPECT_EQ(iface->required_warmup_days(), 37);
}
