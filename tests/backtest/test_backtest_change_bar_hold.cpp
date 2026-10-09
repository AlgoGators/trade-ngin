// tests/backtest/test_backtest_change_bar_hold.cpp
//
// LOOP_SPEC v6.1 sections 2.1 and 2.2, D37 (T-ROLLX-FIX commit 1): in the futures backtest a symbol
// whose LAST consumed bar is pending (a change bar, either bar of a flip, an id-less bar inside a
// pending roll: code review D2) is HELD at the rebalance: no fill, the book stays at its filled
// quantity; the confirming bar ends the hold. The status is taken on the CONSUMED sequence (a
// withheld bar never walks it) and persists across cycles that consume no bar of the symbol.
//
// The book is rebalanced by the one pass (LOOP_SPEC sections 4 to 6), which carries the hold: the
// coordinator leaves a pending symbol out of the session set and the pass fixes its row. The sleeve
// targets 3n contracts of XX on its n-th call (a contract of XX is a tenth of the book, so every
// step is beyond the no-trade buffer): every cycle that is not held fills, and a held cycle fills
// none.

#include <gtest/gtest.h>
#include "../risk/risk_module_test_helpers.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "../portfolio/one_pass_test_fixture.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/strategy/base_strategy.hpp"

#define private public
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/backtest/backtest_coordinator.hpp"
#undef private

using namespace trade_ngin;
using namespace trade_ngin::backtest;
using namespace trade_ngin::testing;

namespace {

Timestamp wday(int d) { return Timestamp(std::chrono::seconds(1767571200LL + 86400LL * d)); }

Bar bar(const std::string& symbol, int d, double close, const std::string& id, bool locked = false) {
    Bar b;
    b.symbol = symbol;
    b.timestamp = wday(d);
    b.open = Decimal(close);
    b.high = Decimal(locked ? close : close * 1.01);
    b.low = Decimal(locked ? close : close * 0.99);
    b.close = Decimal(close);
    b.volume = 100000.0;
    b.instrument_id = id;
    return b;
}

class StepStrategy : public OverlayStubStrategy {
public:
    StepStrategy(std::string id, StrategyConfig config, std::shared_ptr<DatabaseInterface> db)
        : OverlayStubStrategy(std::move(id), std::move(config), std::move(db)) {
        metadata_.name = "Step Strategy";
    }
    Result<void> on_data(const std::vector<Bar>& data) override {
        ++calls_;
        Row& row = rows["XX"];
        row.multiplier = 1000.0;
        row.optimal = 3.0 * static_cast<double>(calls_);
        row.forecast = 10.0;
        for (const auto& b : data) {
            if (b.symbol == "XX") row.close = static_cast<double>(b.close);
        }
        return Result<void>();
    }

private:
    size_t calls_{0};
};

}  // namespace

class BacktestChangeBarHoldTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        ASSERT_TRUE(db_->connect().is_ok());
        static int n = 0;
        BacktestCoordinatorConfig cc;
        cc.initial_capital = 1'000'000.0;
        cc.store_results = false;
        cc.store_trade_details = false;
        cc.portfolio_id = "HOLD_TEST";
        coord_ = std::make_unique<BacktestCoordinator>(db_, &InstrumentRegistry::instance(), cc);
        ASSERT_TRUE(coord_->initialize().is_ok());
        coord_->reset_portfolio_state();
        coord_->session_hold_enabled_ = true;
        pm_ = std::make_shared<PortfolioManager>(one_pass_config("HOLD_S", 1'000'000.0),
                                                 "PM_HOLD_" + std::to_string(++n));
        pm_->set_backtest_mode(true);
        StrategyConfig sc;
        sc.capital_allocation = 1'000'000.0;
        sc.max_leverage = 10.0;
        sc.asset_classes = {AssetClass::FUTURES};
        sc.frequencies = {DataFrequency::DAILY};
        auto s = std::make_shared<StepStrategy>("HOLD_S", sc, db_);
        ASSERT_TRUE(s->initialize().is_ok());
        ASSERT_TRUE(s->start().is_ok());
        ASSERT_TRUE(pm_->add_strategy(s, 1.0, true).is_ok());
    }
    void TearDown() override {
        coord_.reset();
        pm_.reset();
        db_.reset();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        TestBase::TearDown();
    }

    // Feeds XX's bars one group a day (day 0 first) and returns, for each cycle after the first,
    // whether it filled (1) or was held (0) (cycle d's fills sit on day d's timestamp): the STRATEGY
    // fills only (a confirmed roll's two ROLL legs are not the strategy's trading).
    std::vector<double> run(const std::vector<Bar>& xx) {
        std::vector<double> filled;
        for (size_t d = 0; d < xx.size(); ++d) {
            size_t before = 0;
            for (const auto& [sid, ex] : pm_->get_strategy_executions()) before += ex.size();
            auto r = coord_->process_portfolio_day(wday(static_cast<int>(d)), {xx[d]}, pm_, execs_,
                                                   equity_, risk_, false, 1'000'000.0);
            EXPECT_TRUE(r.is_ok()) << r.error()->what();
            double q = 0.0;
            for (const auto& [sid, ex] : pm_->get_strategy_executions()) {
                for (size_t i = 0; i < ex.size(); ++i) {
                    if (i >= before && ex[i].execution_type == ExecutionType::STRATEGY) {
                        q += static_cast<double>(ex[i].filled_quantity);
                    }
                }
            }
            if (d > 0) filled.push_back(q > 0.0 ? 1.0 : 0.0);
        }
        return filled;
    }

    std::shared_ptr<MockPostgresDatabase> db_;
    std::unique_ptr<BacktestCoordinator> coord_;
    std::shared_ptr<PortfolioManager> pm_;
    std::vector<ExecutionReport> execs_;
    std::vector<std::pair<Timestamp, double>> equity_;
    std::vector<RiskResult> risk_;
};

// Days 0-1 on A, day 2 the change bar onto B, day 3 confirms B. The cycle whose signal bar is day 2
// (cycle 3) is held; every other cycle fills.
TEST_F(BacktestChangeBarHoldTest, ThePendingChangeBarHoldsAndTheConfirmReleases) {
    const auto f = run({bar("XX", 0, 100, "A"), bar("XX", 1, 101, "A"), bar("XX", 2, 120, "B"),
                        bar("XX", 3, 121, "B"), bar("XX", 4, 122, "B")});
    EXPECT_EQ(f, (std::vector<double>{1, 1, 0, 1})) << "cycle 3 (signal bar day 2) held; cycle 4 trades";
}

// D2: an id-less bar right after the change bar leaves the roll pending: held on its cycle too.
TEST_F(BacktestChangeBarHoldTest, AnIdLessBarInsideAPendingRollIsHeld) {
    const auto f = run({bar("XX", 0, 100, "A"), bar("XX", 1, 101, "A"), bar("XX", 2, 120, "B"),
                        bar("XX", 3, 121, ""), bar("XX", 4, 122, "B"), bar("XX", 5, 123, "B")});
    EXPECT_EQ(f, (std::vector<double>{1, 1, 0, 0, 1}))
        << "cycles 3 (change bar) and 4 (id-less, still pending) held; cycle 5 (confirm) trades";
}

// A flip: day 2 onto B, day 3 back on A. Both bars are held; the next consumed bar releases.
TEST_F(BacktestChangeBarHoldTest, BothBarsOfAFlipAreHeld) {
    const auto f = run({bar("XX", 0, 100, "A"), bar("XX", 1, 101, "A"), bar("XX", 2, 90, "B"),
                        bar("XX", 3, 102, "A"), bar("XX", 4, 103, "A")});
    EXPECT_EQ(f, (std::vector<double>{1, 1, 0, 0}));
}

// K-01 in the roll status: day 2 is a locked print (JUNK, withheld) carrying another contract's id.
// It never walks the tracker, so nothing is pending and no cycle is held by D37 (cycle 3 is held by
// the JUNK verdict itself: its signal bar is the withheld one).
TEST_F(BacktestChangeBarHoldTest, AWithheldPrintNeverStartsAHold) {
    const auto f = run({bar("XX", 0, 100, "A"), bar("XX", 1, 101, "A"), bar("XX", 2, 90, "B", true),
                        bar("XX", 3, 102, "A"), bar("XX", 4, 103, "A")});
    EXPECT_EQ(f, (std::vector<double>{1, 1, 0, 1}))
        << "only the cycle whose signal bar is the withheld print is held; day 3 is no flip";
}
