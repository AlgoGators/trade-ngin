// tests/backtest/test_backtest_k01_feed.cpp
//
// LOOP_SPEC v6.1 section 2.1, K-01 (LOCKED; T-ROLLX-FIX commit 1): in the futures backtest a JUNK
// bar and a thin first print of the signal group are WITHHELD from every consumer and never fed
// later; an unconfirmed instrument-id change the classifier holds is CONSUMED (it is the change bar
// of section 2.2). This file drives BacktestCoordinator::process_portfolio_day through the APIs the
// parent 9fe44f1a already has, so each test fails there on the behaviour, not on a missing name:
// the parent fed a withheld bar on the next cycle ahead of its symbol's next bar (T-7b-1 7a) and
// withheld the id-change bar like any JUNK bar.

#include <gtest/gtest.h>
#include "../risk/risk_module_test_helpers.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
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

// Day d at 00:00 UTC, counted from Monday 2026-01-05 (d = 0..3 are Monday..Thursday).
Timestamp wday(int d) { return Timestamp(std::chrono::seconds(1767571200LL + 86400LL * d)); }

// A bar; `locked` makes high == low, which the session classifier calls JUNK whatever the volume.
Bar bar(const std::string& symbol, int d, double close, bool locked = false,
        double volume = 100000.0) {
    Bar b;
    b.symbol = symbol;
    b.timestamp = wday(d);
    b.open = Decimal(close);
    b.high = Decimal(locked ? close : close * 1.01);
    b.low = Decimal(locked ? close : close * 0.99);
    b.close = Decimal(close);
    b.volume = volume;
    return b;
}

using Keys = std::vector<std::string>;

// "SYM@d" for each bar, d the weekday index, in feed order.
Keys keys(const std::vector<Bar>& bars) {
    Keys out;
    for (const auto& b : bars) {
        const auto secs =
            std::chrono::duration_cast<std::chrono::seconds>(b.timestamp.time_since_epoch()).count();
        out.push_back(b.symbol + "@" + std::to_string((secs - 1767571200LL) / 86400LL));
    }
    return out;
}

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

class BacktestK01FeedTest : public TestBase {
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

    void make() {
        static int n = 0;
        BacktestCoordinatorConfig cc;
        cc.initial_capital = 1'000'000.0;
        cc.store_results = false;
        cc.store_trade_details = false;
        cc.portfolio_id = "K01_FEED_TEST";
        coord_ = std::make_unique<BacktestCoordinator>(db_, &InstrumentRegistry::instance(), cc);
        ASSERT_TRUE(coord_->initialize().is_ok());
        coord_->reset_portfolio_state();
        coord_->session_hold_enabled_ = true;  // what run_portfolio sets for AssetClass::FUTURES
        pm_ = std::make_shared<PortfolioManager>(plain_config(), "PM_K01_" + std::to_string(++n));
        StrategyConfig sc;
        sc.capital_allocation = 1'000'000.0;
        sc.max_leverage = 10.0;
        sc.asset_classes = {AssetClass::FUTURES};
        sc.frequencies = {DataFrequency::DAILY};
        strat_ = std::make_shared<RecordingStrategy>("K01_S", sc, db_);
        ASSERT_TRUE(strat_->initialize().is_ok());
        ASSERT_TRUE(strat_->start().is_ok());
        ASSERT_TRUE(pm_->add_strategy(strat_, 1.0, false).is_ok());
    }

    void cycle(int d, const std::vector<Bar>& group, bool warmup) {
        auto r = coord_->process_portfolio_day(wday(d), group, pm_, execs_, equity_, risk_, warmup,
                                               1'000'000.0);
        ASSERT_TRUE(r.is_ok()) << r.error()->what();
    }

    void id(const std::string& symbol, int d, const std::string& instrument_id) {
        coord_->session_classifier_.add_instrument_id(symbol, SessionClassifier::day_of(wday(d)),
                                                      instrument_id);
    }

    std::shared_ptr<MockPostgresDatabase> db_;
    std::unique_ptr<BacktestCoordinator> coord_;
    std::shared_ptr<PortfolioManager> pm_;
    std::shared_ptr<RecordingStrategy> strat_;
    std::vector<ExecutionReport> execs_;
    std::vector<std::pair<Timestamp, double>> equity_;
    std::vector<RiskResult> risk_;
};

// XX's day-1 bar is locked (JUNK): withheld on its own cycle and never fed afterwards.
TEST_F(BacktestK01FeedTest, AWithheldJunkBarIsNeverFedOnALaterCycle) {
    make();
    cycle(0, {bar("AA", 0, 10), bar("XX", 0, 20)}, false);  // first group: stored, not processed
    cycle(1, {bar("AA", 1, 11), bar("XX", 1, 21, true)}, false);
    cycle(2, {bar("AA", 2, 12), bar("XX", 2, 22)}, false);
    cycle(3, {bar("AA", 3, 13), bar("XX", 3, 23)}, false);
    ASSERT_EQ(strat_->calls.size(), 3u);
    EXPECT_EQ(strat_->calls[0], (Keys{"AA@0", "XX@0"}));
    EXPECT_EQ(strat_->calls[1], (Keys{"AA@1"})) << "XX's JUNK bar is withheld on its own cycle";
    EXPECT_EQ(strat_->calls[2], (Keys{"AA@2", "XX@2"}))
        << "and never fed later: no return is formed across it (K-01)";
    EXPECT_EQ(pm_->closes_by_date_.at("XX").size(), 2u)
        << "the PortfolioManager's own history never holds day 1";
}

TEST_F(BacktestK01FeedTest, TheRuleAppliesDuringWarmUp) {
    make();
    cycle(0, {bar("AA", 0, 10), bar("XX", 0, 20)}, true);
    cycle(1, {bar("AA", 1, 11), bar("XX", 1, 21, true)}, true);
    cycle(2, {bar("AA", 2, 12), bar("XX", 2, 22)}, true);
    cycle(3, {bar("AA", 3, 13), bar("XX", 3, 23)}, true);
    ASSERT_EQ(strat_->calls.size(), 3u);
    EXPECT_EQ(strat_->calls[1], (Keys{"AA@1"}));
    EXPECT_EQ(strat_->calls[2], (Keys{"AA@2", "XX@2"}));
}

TEST_F(BacktestK01FeedTest, AnAllWithheldSignalGroupFeedsNothingAndItsBarIsNeverFed) {
    make();
    cycle(0, {bar("XX", 0, 20)}, false);
    cycle(1, {bar("XX", 1, 21, true)}, false);
    cycle(2, {bar("XX", 2, 22)}, false);  // signal group {XX@1}: withheld, nothing to feed
    cycle(3, {bar("XX", 3, 23)}, false);
    ASSERT_EQ(strat_->calls.size(), 2u) << "no strategy call on the all-withheld cycle";
    EXPECT_EQ(strat_->calls[0], (Keys{"XX@0"}));
    EXPECT_EQ(strat_->calls[1], (Keys{"XX@2"}));
    EXPECT_EQ(equity_.size(), 4u) << "every cycle still books its equity point";
}

// A symbol's first bar printing 10 lots has no norm yet and sits under the 50-lot floor: a thin
// first print, withheld like a JUNK bar and never fed.
TEST_F(BacktestK01FeedTest, AThinFirstPrintIsWithheldAndNeverFed) {
    make();
    cycle(0, {bar("AA", 0, 10)}, false);
    cycle(1, {bar("AA", 1, 11), bar("YY", 1, 50, false, 10.0)}, false);
    cycle(2, {bar("AA", 2, 12), bar("YY", 2, 51)}, false);
    cycle(3, {bar("AA", 3, 13), bar("YY", 3, 52)}, false);
    ASSERT_EQ(strat_->calls.size(), 3u);
    EXPECT_EQ(strat_->calls[1], (Keys{"AA@1"})) << "YY's thin first print is withheld";
    EXPECT_EQ(strat_->calls[2], (Keys{"AA@2", "YY@2"})) << "and never fed later";
}

// XX moves from contract A to B on day 1 on a thin print (5,000 lots against a 100,000 norm): the
// classifier's instrument-id limb holds it (JUNK by the id change). That bar is the change bar of
// section 2.2: CONSUMED on its own cycle (held there, no fill), not withheld and not fed late.
TEST_F(BacktestK01FeedTest, AnIdChangeHoldIsConsumedOnItsOwnCycle) {
    make();
    id("XX", 0, "A");
    id("XX", 1, "B");
    id("XX", 2, "B");
    id("XX", 3, "B");
    cycle(0, {bar("AA", 0, 10), bar("XX", 0, 20)}, false);
    cycle(1, {bar("AA", 1, 11), bar("XX", 1, 25, false, 5000.0)}, false);
    cycle(2, {bar("AA", 2, 12), bar("XX", 2, 26)}, false);
    cycle(3, {bar("AA", 3, 13), bar("XX", 3, 27)}, false);
    ASSERT_EQ(strat_->calls.size(), 3u);
    EXPECT_EQ(strat_->calls[1], (Keys{"AA@1", "XX@1"}))
        << "the id-change bar is fed on its own cycle (consumed, held)";
    EXPECT_EQ(strat_->calls[2], (Keys{"AA@2", "XX@2"}));
}
