// tests/backtest/test_backtest_junk_signal_feed.cpp
//
// The futures backtest's signal feed under LOOP_SPEC v6.1 section 2.1, K-01 (LOCKED; T-ROLLX-FIX
// commit 1; it supersedes T-7b-1 commit 7a's one-cycle delayed feed): a JUNK bar and a thin first
// print of the signal group are withheld from the strategies, the PortfolioManager and the cost
// models and are never fed later (k01_signal_feed). The coordinator's behaviour is pinned in
// test_backtest_k01_feed.cpp; this file pins the pure helper and, as controls, the PortfolioManager's
// fill price when one feed carries two bars of a symbol (C7a amended): it is the latest-dated bar's.

#include <gtest/gtest.h>
#include "../risk/risk_module_test_helpers.hpp"

#include <chrono>
#include <cmath>
#include <deque>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "trade_ngin/backtest/junk_signal_feed.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/instruments/futures.hpp"
#include "trade_ngin/strategy/base_strategy.hpp"

#define private public
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/backtest/backtest_coordinator.hpp"
#undef private

#include "trade_ngin/strategy/trend_following.hpp"

using namespace trade_ngin;
using namespace trade_ngin::backtest;
using namespace trade_ngin::testing;

namespace {

// Day d at 00:00 UTC, counted from Monday 2026-01-05 (d = 0..3 are Monday..Thursday).
Timestamp wday(int d) { return Timestamp(std::chrono::seconds(1767571200LL + 86400LL * d)); }

// A bar; `locked` makes high == low, which the session classifier calls JUNK whatever the volume.
Bar bar(const std::string& symbol, int d, double close, bool locked = false) {
    Bar b;
    b.symbol = symbol;
    b.timestamp = wday(d);
    b.open = Decimal(close);
    b.high = Decimal(locked ? close : close * 1.01);
    b.low = Decimal(locked ? close : close * 0.99);
    b.close = Decimal(close);
    b.volume = 100000.0;
    return b;
}

// "SYM@d" for each bar, d the weekday index, in feed order.
std::vector<std::string> keys(const std::vector<Bar>& bars) {
    std::vector<std::string> out;
    for (const auto& b : bars) {
        const auto secs =
            std::chrono::duration_cast<std::chrono::seconds>(b.timestamp.time_since_epoch()).count();
        out.push_back(b.symbol + "@" + std::to_string((secs - 1767571200LL) / 86400LL));
    }
    return out;
}

using Keys = std::vector<std::string>;

}  // namespace

// ------------------------------------------------------------------------------------------------
// The rule, on the pure helper
// ------------------------------------------------------------------------------------------------

TEST(K01SignalFeed, AWithheldSymbolsBarLeavesTheFeedAndEveryOtherBarKeepsItsOrder) {
    const auto f = k01_signal_feed({bar("AA", 1, 10), bar("XX", 1, 20, true), bar("BB", 1, 30)},
                                   {"XX"});
    EXPECT_EQ(keys(f.feed), (Keys{"AA@1", "BB@1"}));
    EXPECT_EQ(keys(f.withheld), (Keys{"XX@1"}));
}

TEST(K01SignalFeed, NothingIsCarriedToTheNextCycle) {
    // The helper has no carried input at all: the next cycle's feed is that cycle's own group.
    const auto day1 = k01_signal_feed({bar("AA", 1, 10), bar("XX", 1, 20, true)}, {"XX"});
    const auto day2 = k01_signal_feed({bar("AA", 2, 11), bar("XX", 2, 21)}, {});
    EXPECT_EQ(keys(day1.feed), (Keys{"AA@1"}));
    EXPECT_EQ(keys(day2.feed), (Keys{"AA@2", "XX@2"}));
    EXPECT_TRUE(day2.withheld.empty());
}

TEST(K01SignalFeed, AnAllWithheldGroupGivesAnEmptyFeed) {
    const auto f = k01_signal_feed({bar("XX", 1, 20, true)}, {"XX"});
    EXPECT_TRUE(f.feed.empty());
    EXPECT_EQ(keys(f.withheld), (Keys{"XX@1"}));
}

// Control (passes on the parent's helper too): nothing withheld feeds the group unchanged.
TEST(K01SignalFeed, NothingWithheldFeedsTheGroupUnchanged) {
    const auto f = k01_signal_feed({bar("BB", 1, 30), bar("AA", 1, 10)}, {});
    EXPECT_EQ(keys(f.feed), (Keys{"BB@1", "AA@1"}));
    EXPECT_TRUE(f.withheld.empty());
}

// ------------------------------------------------------------------------------------------------
// The coordinator applies it (futures only, warm-up included)
// ------------------------------------------------------------------------------------------------

namespace {

class RecordingStrategy : public BaseStrategy {
public:
    RecordingStrategy(std::string id, StrategyConfig config, std::shared_ptr<DatabaseInterface> db)
        : BaseStrategy(std::move(id), std::move(config),
                       std::static_pointer_cast<trade_ngin::PostgresDatabase>(db)) {
        metadata_.name = "Recording Strategy";
    }
    Result<void> on_data(const std::vector<Bar>& data) override {
        calls.push_back(keys(data));
        return Result<void>();
    }
    std::unordered_map<std::string, Position> get_target_positions() const override { return {}; }
    std::vector<Keys> calls;
};

PortfolioConfig plain_config() {
    PortfolioConfig c{1'000'000.0, 1.0, 0.0, false};
    c.opt_config.capital = 1'000'000.0;
    c.risk_config.capital = 1'000'000.0;
    c.risk_modules = {test_none_module()};
    return c;
}

}  // namespace

class BacktestJunkSignalFeedTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        ASSERT_TRUE(db_->connect().is_ok());
    }
    void TearDown() override {
        coord_.reset();
        pm_.reset();
        db_.reset();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        TestBase::TearDown();
    }

    void make(bool futures) {
        static int n = 0;
        BacktestCoordinatorConfig cc;
        cc.initial_capital = 1'000'000.0;
        cc.store_results = false;
        cc.store_trade_details = false;
        cc.portfolio_id = "JUNK_FEED_TEST";
        coord_ = std::make_unique<BacktestCoordinator>(db_, &InstrumentRegistry::instance(), cc);
        ASSERT_TRUE(coord_->initialize().is_ok());
        coord_->reset_portfolio_state();
        coord_->session_hold_enabled_ = futures;  // what run_portfolio sets for AssetClass::FUTURES
        pm_ = std::make_shared<PortfolioManager>(plain_config(), "PM_JF_" + std::to_string(++n));
        StrategyConfig sc;
        sc.capital_allocation = 1'000'000.0;
        sc.max_leverage = 10.0;
        sc.asset_classes = {AssetClass::FUTURES};
        sc.frequencies = {DataFrequency::DAILY};
        strat_ = std::make_shared<RecordingStrategy>("JF_S", sc, db_);
        ASSERT_TRUE(strat_->initialize().is_ok());
        ASSERT_TRUE(strat_->start().is_ok());
        ASSERT_TRUE(pm_->add_strategy(strat_, 1.0, false).is_ok());
    }

    void cycle(int d, const std::vector<Bar>& group, bool warmup) {
        auto r = coord_->process_portfolio_day(wday(d), group, pm_, execs_, equity_, risk_, warmup,
                                               1'000'000.0);
        ASSERT_TRUE(r.is_ok()) << r.error()->what();
    }

    // Four groups; XX's day-1 bar is locked (JUNK).
    void run_four_groups(bool warmup) {
        cycle(0, {bar("AA", 0, 10), bar("XX", 0, 20)}, warmup);  // first group: stored, not processed
        cycle(1, {bar("AA", 1, 11), bar("XX", 1, 21, true)}, warmup);
        cycle(2, {bar("AA", 2, 12), bar("XX", 2, 22)}, warmup);
        cycle(3, {bar("AA", 3, 13), bar("XX", 3, 23)}, warmup);
    }

    std::shared_ptr<MockPostgresDatabase> db_;
    std::unique_ptr<BacktestCoordinator> coord_;
    std::shared_ptr<PortfolioManager> pm_;
    std::shared_ptr<RecordingStrategy> strat_;
    std::vector<ExecutionReport> execs_;
    std::vector<std::pair<Timestamp, double>> equity_;
    std::vector<RiskResult> risk_;
};

// ------------------------------------------------------------------------------------------------
// Controls: the fill price when one feed carries two bars of a symbol (C7a amended, lead ruling
// C7a_HALT option 1)
// ------------------------------------------------------------------------------------------------
//
// K-01 retired the release cycle that fed a withheld bar ahead of its symbol's new bar, but the
// PortfolioManager's rule stands for any feed with two bars of a symbol (a live window, a catch-up):
// a fill is priced at the symbol's LATEST-dated bar, never at an older bar in the vector.

namespace {

// Target 1 contract of XX on the first call, 3 on every later call.
class TwoStepStrategy : public BaseStrategy {
public:
    TwoStepStrategy(std::string id, StrategyConfig config, std::shared_ptr<DatabaseInterface> db)
        : BaseStrategy(std::move(id), std::move(config),
                       std::static_pointer_cast<trade_ngin::PostgresDatabase>(db)) {
        metadata_.name = "Two Step Strategy";
    }
    Result<void> on_data(const std::vector<Bar>& data) override {
        (void)data;
        ++calls_;
        return Result<void>();
    }
    std::unordered_map<std::string, Position> get_target_positions() const override {
        Position p;
        p.symbol = "XX";
        p.quantity = Decimal(calls_ <= 1 ? 1.0 : 3.0);
        p.average_price = Decimal(100.0);
        p.last_update = wday(0);
        return {{"XX", p}};
    }

private:
    size_t calls_{0};
};

}  // namespace

class ReleaseCycleFillPriceTest : public BacktestJunkSignalFeedTest {
protected:
    std::shared_ptr<PortfolioManager> make_pm() {
        static int n = 0;
        auto pm = std::make_shared<PortfolioManager>(plain_config(), "PM_JF_FILL_" + std::to_string(++n));
        StrategyConfig sc;
        sc.capital_allocation = 1'000'000.0;
        sc.max_leverage = 10.0;
        sc.asset_classes = {AssetClass::FUTURES};
        sc.frequencies = {DataFrequency::DAILY};
        auto s = std::make_shared<TwoStepStrategy>("JF_FILL_S", sc, db_);
        EXPECT_TRUE(s->initialize().is_ok());
        EXPECT_TRUE(s->start().is_ok());
        EXPECT_TRUE(pm->add_strategy(s, 1.0, false).is_ok());
        return pm;
    }
    // get_strategy_executions() returns a copy, so the report is returned by value.
    static std::optional<ExecutionReport> last_fill(const PortfolioManager& pm, const std::string& symbol) {
        std::optional<ExecutionReport> out;
        const auto all = pm.get_strategy_executions();
        for (const auto& [sid, reports] : all) {
            (void)sid;
            for (const auto& r : reports) {
                if (r.symbol == symbol) out = r;
            }
        }
        return out;
    }
};

TEST_F(ReleaseCycleFillPriceTest, AFillOnAReleaseCycleIsPricedAtTheSignalGroupsCloseNotTheJunkBar) {
    auto pm = make_pm();
    const std::unordered_set<std::string> xx{"XX"};
    ASSERT_TRUE(pm->process_market_data({bar("XX", 0, 100.0)}, false, wday(1), &xx).is_ok());
    // The release cycle: XX's withheld day-1 JUNK bar (close 90) ahead of its day-2 bar (close 110).
    ASSERT_TRUE(pm->process_market_data({bar("XX", 1, 90.0, true), bar("XX", 2, 110.0)}, false,
                                        wday(3), &xx)
                    .is_ok());
    const auto fill = last_fill(*pm, "XX");
    ASSERT_TRUE(fill.has_value());
    EXPECT_DOUBLE_EQ(static_cast<double>(fill->filled_quantity), 2.0);
    EXPECT_DOUBLE_EQ(static_cast<double>(fill->fill_price), 110.0)
        << "priced at the symbol's latest-dated bar, not at the released junk close 90";
    const auto recent = pm->get_recent_executions();
    ASSERT_FALSE(recent.empty());
    EXPECT_DOUBLE_EQ(static_cast<double>(recent.back().fill_price), 110.0)
        << "the portfolio-level execution takes the same price";
}

// Control (passes on the parent too): one bar per symbol, the fill is priced at it.
TEST_F(ReleaseCycleFillPriceTest, WithOneBarPerSymbolTheFillIsPricedAtThatBar) {
    auto pm = make_pm();
    const std::unordered_set<std::string> xx{"XX"};
    ASSERT_TRUE(pm->process_market_data({bar("XX", 0, 100.0)}, false, wday(1), &xx).is_ok());
    ASSERT_TRUE(pm->process_market_data({bar("XX", 2, 110.0)}, false, wday(3), &xx).is_ok());
    const auto fill = last_fill(*pm, "XX");
    ASSERT_TRUE(fill.has_value());
    EXPECT_DOUBLE_EQ(static_cast<double>(fill->fill_price), 110.0);
}
