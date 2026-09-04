// tests/live/test_live_trading_coordinator_keying.cpp
//
// FIX-0 regression cover for the config -> coordinator -> LiveResultsManager storage-key
// path. Live rows are keyed (strategy_id, strategy_name, portfolio_id) and the runner
// declares all three on LiveTradingConfig. Before FIX-0 the coordinator had no
// strategy_name to forward and the equity runner left portfolio_id at the
// LiveTradingConfig default -- BASE_PORTFOLIO, the *futures* book -- so equity rows were
// written under a key no equity read would ever match and each run reloaded an empty
// book. These tests pin the threading at the only seam a unit test can reach: the
// results manager the coordinator actually builds.

#include <gtest/gtest.h>
#include <memory>
#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/live/live_trading_coordinator.hpp"
#include "trade_ngin/storage/live_results_manager.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

class LiveTradingCoordinatorKeyingTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        ASSERT_TRUE(db_->connect().is_ok());
    }
    std::shared_ptr<MockPostgresDatabase> db_;
};

TEST_F(LiveTradingCoordinatorKeyingTest, ConfigStrategyNameAndPortfolioReachResultsManager) {
    LiveTradingConfig config;
    config.strategy_id = "LIVE_EQUITY_MEAN_REVERSION";
    config.strategy_name = "EQUITY_MEAN_REVERSION";
    config.portfolio_id = "EQUITY_PORTFOLIO";

    LiveTradingCoordinator coordinator(db_, InstrumentRegistry::instance(), config);
    ASSERT_TRUE(coordinator.initialize().is_ok());

    const auto* rm = coordinator.get_results_manager();
    ASSERT_NE(rm, nullptr);
    EXPECT_EQ(rm->get_strategy_id(), "LIVE_EQUITY_MEAN_REVERSION");
    // Pre-FIX-0: "LIVE_EQUITY_MEAN_REVERSION" (id reused as name).
    EXPECT_EQ(rm->get_strategy_name(), "EQUITY_MEAN_REVERSION");
    // Pre-FIX-0: "BASE_PORTFOLIO" (the futures book's id, via the config default).
    EXPECT_EQ(rm->get_portfolio_id(), "EQUITY_PORTFOLIO");
}

TEST_F(LiveTradingCoordinatorKeyingTest, UnsetStrategyNameKeepsTheFuturesKeyUnchanged) {
    // The futures runners set strategy_id and portfolio_id only. Their rows have always
    // been keyed (strategy_id, strategy_id, portfolio_id); FIX-0 must not move them.
    LiveTradingConfig config;
    config.strategy_id = "LIVE_TREND_FOLLOWING";
    config.portfolio_id = "BASE_PORTFOLIO";
    EXPECT_TRUE(config.strategy_name.empty());

    LiveTradingCoordinator coordinator(db_, InstrumentRegistry::instance(), config);
    ASSERT_TRUE(coordinator.initialize().is_ok());

    const auto* rm = coordinator.get_results_manager();
    ASSERT_NE(rm, nullptr);
    EXPECT_EQ(rm->get_strategy_id(), "LIVE_TREND_FOLLOWING");
    EXPECT_EQ(rm->get_strategy_name(), "LIVE_TREND_FOLLOWING");
    EXPECT_EQ(rm->get_portfolio_id(), "BASE_PORTFOLIO");
}

TEST_F(LiveTradingCoordinatorKeyingTest, DefaultConfigStillTargetsTheFuturesBook) {
    // Guards the other direction: nothing in FIX-0 may change what an unconfigured
    // LiveTradingConfig means, because the futures apps lean on these defaults.
    LiveTradingConfig config;
    EXPECT_EQ(config.strategy_id, "LIVE_TREND_FOLLOWING");
    EXPECT_EQ(config.portfolio_id, "BASE_PORTFOLIO");
    EXPECT_EQ(config.schema, "trading");
    EXPECT_TRUE(config.strategy_name.empty());
}
