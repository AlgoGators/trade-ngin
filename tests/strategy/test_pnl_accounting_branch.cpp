#include <gtest/gtest.h>
#include <memory>
#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "trade_ngin/strategy/mean_reversion.hpp"
#include "trade_ngin/strategy/trend_following.hpp"
#include "trade_ngin/strategy/types.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

// Phase 4 audit test T4.6 — §1.14 backtest coordinator P&L semantics.
//
// Contract test: the coordinator's branch reads strategy->get_pnl_accounting().method
// and only stamps realized_pnl when REALIZED_ONLY (futures: settled daily).
// MIXED / UNREALIZED_ONLY (equities) leaves realized_pnl untouched at the
// coordinator level -- on_execution writes realized when positions actually close.
//
// This test pins the contract by verifying:
// 1. Each strategy's accounting method is what the coordinator expects.
// 2. The PnL accounting accessor returns the same value over successive calls.
//
// A true integration test of the coordinator's branch requires the full bar
// loop, portfolio, and execution path -- captured by the broader smoke runs
// rather than a unit test. This contract test catches regressions in the
// accessor / setter contract that the coordinator's branch depends on.

namespace {

class PnLAccountingBranchTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();
        db_ = std::make_shared<MockPostgresDatabase>("mock://pnl_branch_test");
        ASSERT_TRUE(db_->connect().is_ok());
    }

    std::shared_ptr<MockPostgresDatabase> db_;
};

}  // namespace

TEST_F(PnLAccountingBranchTest, MeanReversionUsesMixed) {
    StrategyConfig cfg;
    cfg.capital_allocation = 100000.0;
    cfg.max_leverage = 2.0;
    cfg.max_drawdown = 0.3;
    cfg.asset_classes = {AssetClass::EQUITIES};
    cfg.frequencies = {DataFrequency::DAILY};
    cfg.trading_params["AAPL"] = 1.0;
    cfg.position_limits["AAPL"] = 1000.0;

    MeanReversionConfig mr;
    mr.lookback_period = 20;
    mr.vol_lookback = 20;
    mr.entry_threshold = 2.0;
    mr.exit_threshold = 0.5;
    mr.risk_target = 0.15;
    mr.position_size = 0.1;

    MeanReversionStrategy strat("TEST_MR_PNL", cfg, mr, db_);
    ASSERT_TRUE(strat.initialize().is_ok());

    EXPECT_EQ(strat.get_pnl_accounting().method, PnLAccountingMethod::MIXED)
        << "Equity mean reversion must declare MIXED accounting so the "
           "backtest coordinator skips daily realized_pnl writes (Phase 4 §1.14).";
}

TEST_F(PnLAccountingBranchTest, TrendFollowingUsesRealizedOnly) {
    StrategyConfig cfg;
    cfg.capital_allocation = 100000.0;
    cfg.max_leverage = 2.0;
    cfg.max_drawdown = 0.3;
    cfg.asset_classes = {AssetClass::FUTURES};
    cfg.frequencies = {DataFrequency::DAILY};
    cfg.trading_params["ES"] = 1.0;
    cfg.position_limits["ES"] = 10.0;

    TrendFollowingConfig tfc;

    TrendFollowingStrategy strat("TEST_TF_PNL", cfg, tfc, db_);
    ASSERT_TRUE(strat.initialize().is_ok());

    EXPECT_EQ(strat.get_pnl_accounting().method, PnLAccountingMethod::REALIZED_ONLY)
        << "Futures trend following must declare REALIZED_ONLY accounting so "
           "the backtest coordinator writes daily MTM into realized_pnl "
           "(futures settle daily) per Phase 4 §1.14.";
}

TEST_F(PnLAccountingBranchTest, AccessorIsStableAcrossCalls) {
    StrategyConfig cfg;
    cfg.capital_allocation = 100000.0;
    cfg.max_leverage = 2.0;
    cfg.max_drawdown = 0.3;
    cfg.asset_classes = {AssetClass::EQUITIES};
    cfg.frequencies = {DataFrequency::DAILY};
    cfg.trading_params["AAPL"] = 1.0;
    cfg.position_limits["AAPL"] = 1000.0;

    MeanReversionStrategy strat("TEST_MR_STABLE", cfg, MeanReversionConfig{}, db_);
    ASSERT_TRUE(strat.initialize().is_ok());

    auto method_a = strat.get_pnl_accounting().method;
    auto method_b = strat.get_pnl_accounting().method;
    auto method_c = strat.get_pnl_accounting().method;
    EXPECT_EQ(method_a, method_b);
    EXPECT_EQ(method_b, method_c);
}
