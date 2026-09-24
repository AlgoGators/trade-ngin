// tests/backtest/test_backtest_junk_signal_feed.cpp
//
// T-7b-1 commit 7a (HD 2026-09-24; T-7a_CODE_REVIEW R1, T-7a ADVERSARY F4). The futures backtest
// withholds a JUNK signal-group bar from the strategies and the PortfolioManager's feed exactly as
// the live runners do.
//
// Live (live_portfolio_conservative.cpp / live_portfolio.cpp, the "JUNK (T-7a C4)" block before
// process_market_data): each run is a fresh process that feeds the whole 730-day window; a JUNK
// symbol's T-1 bar is removed from that feed (withhold_junk_t1_bars), so its signal is not updated
// today, and on the NEXT run the same bar is T-2 and is fed with the rest of the history, in date
// order. The backtest used to feed the whole signal group and hold only the fill and the book.
//
// The backtest's equivalent is a ONE-CYCLE DELAYED FEED: on the cycle whose signal group holds a
// JUNK bar for X, X's bar is withheld from process_market_data (every other symbol's bar is fed as
// before); on the next cycle X's withheld bar is fed ahead of X's next bar, in date order (if that
// bar is JUNK too it is withheld in turn). The hold of X's fill and book is unchanged.
//
// The per-symbol state the delayed feed leaves (the trend sleeve's price history, volatility and
// forecast; the PortfolioManager's date-keyed history and returns) equals feeding every bar in
// order, which is what live's next run computes from its window: pinned below as controls.

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

TEST(JunkDelayedSignalFeed, AJunkBarIsWithheldAndEveryOtherBarIsFedInItsOrder) {
    const auto f = junk_delayed_signal_feed({bar("AA", 1, 10), bar("XX", 1, 20, true), bar("BB", 1, 30)},
                                            {"XX"}, {});
    EXPECT_EQ(keys(f.feed), (Keys{"AA@1", "BB@1"}));
    EXPECT_EQ(keys(f.withheld), (Keys{"XX@1"}));
    EXPECT_TRUE(f.released.empty());
}

TEST(JunkDelayedSignalFeed, TheWithheldBarIsFedOnTheNextCycleAheadOfTheSymbolsNewBar) {
    const auto f = junk_delayed_signal_feed({bar("AA", 2, 11), bar("XX", 2, 21), bar("BB", 2, 31)}, {},
                                            {bar("XX", 1, 20, true)});
    EXPECT_EQ(keys(f.feed), (Keys{"AA@2", "XX@1", "XX@2", "BB@2"}))
        << "the old bar right before the symbol's new one, in date order; the others untouched";
    EXPECT_EQ(keys(f.released), (Keys{"XX@1"}));
    EXPECT_TRUE(f.withheld.empty());
}

TEST(JunkDelayedSignalFeed, AJunkBarAgainWithholdsTheNewBarWhileTheOlderOneIsFed) {
    const auto f = junk_delayed_signal_feed({bar("AA", 2, 11), bar("XX", 2, 21, true)}, {"XX"},
                                            {bar("XX", 1, 20, true)});
    EXPECT_EQ(keys(f.feed), (Keys{"AA@2", "XX@1"}));
    EXPECT_EQ(keys(f.withheld), (Keys{"XX@2"}));
    EXPECT_EQ(keys(f.released), (Keys{"XX@1"}));
}

TEST(JunkDelayedSignalFeed, ACarriedBarWhoseSymbolHasNoBarInTheGroupIsStillFed) {
    // Live's next run feeds the junk bar with the history whether or not the symbol printed again.
    const auto f = junk_delayed_signal_feed({bar("AA", 2, 11), bar("BB", 2, 31)}, {},
                                            {bar("XX", 1, 20, true)});
    EXPECT_EQ(keys(f.feed), (Keys{"AA@2", "BB@2", "XX@1"}));
    EXPECT_EQ(keys(f.released), (Keys{"XX@1"}));
}

TEST(JunkDelayedSignalFeed, AnAllJunkGroupGivesAnEmptyFeed) {
    const auto f = junk_delayed_signal_feed({bar("XX", 1, 20, true)}, {"XX"}, {});
    EXPECT_TRUE(f.feed.empty());
    EXPECT_EQ(keys(f.withheld), (Keys{"XX@1"}));
}

// Control (passes on the parent too): with no JUNK bar and nothing carried the feed is the group.
TEST(JunkDelayedSignalFeed, NoJunkAndNothingCarriedFeedsTheGroupUnchanged) {
    const auto f = junk_delayed_signal_feed({bar("BB", 1, 30), bar("AA", 1, 10)}, {}, {});
    EXPECT_EQ(keys(f.feed), (Keys{"BB@1", "AA@1"}));
    EXPECT_TRUE(f.withheld.empty());
    EXPECT_TRUE(f.released.empty());
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

TEST_F(BacktestJunkSignalFeedTest, TheCoordinatorFeedsAJunkBarOneCycleLateInDateOrder) {
    make(true);
    run_four_groups(false);
    ASSERT_EQ(strat_->calls.size(), 3u);
    EXPECT_EQ(strat_->calls[0], (Keys{"AA@0", "XX@0"}));
    EXPECT_EQ(strat_->calls[1], (Keys{"AA@1"})) << "XX's JUNK bar is withheld on its own cycle";
    EXPECT_EQ(strat_->calls[2], (Keys{"AA@2", "XX@1", "XX@2"}))
        << "and fed on the next cycle, ahead of XX's next bar";
    EXPECT_EQ(pm_->closes_by_date_.at("XX").size(), 3u) << "the PM's own history has day 1 again";
}

TEST_F(BacktestJunkSignalFeedTest, TheRuleAppliesDuringWarmUp) {
    make(true);
    run_four_groups(true);
    ASSERT_EQ(strat_->calls.size(), 3u);
    EXPECT_EQ(strat_->calls[1], (Keys{"AA@1"}));
    EXPECT_EQ(strat_->calls[2], (Keys{"AA@2", "XX@1", "XX@2"}));
}

// Control (passes on the parent too): the equity backtest (no session hold) feeds the full group.
TEST_F(BacktestJunkSignalFeedTest, WithoutTheSessionHoldTheFullGroupIsFed) {
    make(false);
    run_four_groups(false);
    ASSERT_EQ(strat_->calls.size(), 3u);
    EXPECT_EQ(strat_->calls[1], (Keys{"AA@1", "XX@1"}));
    EXPECT_EQ(strat_->calls[2], (Keys{"AA@2", "XX@2"}));
}

TEST_F(BacktestJunkSignalFeedTest, AnAllJunkSignalGroupFeedsNothingAndTheCycleCompletes) {
    make(true);
    cycle(0, {bar("XX", 0, 20)}, false);
    cycle(1, {bar("XX", 1, 21, true)}, false);
    cycle(2, {bar("XX", 2, 22)}, false);  // signal group {XX@1}: all junk, nothing to feed
    cycle(3, {bar("XX", 3, 23)}, false);
    ASSERT_EQ(strat_->calls.size(), 2u) << "no strategy call on the all-junk cycle";
    EXPECT_EQ(strat_->calls[0], (Keys{"XX@0"}));
    EXPECT_EQ(strat_->calls[1], (Keys{"XX@1", "XX@2"}));
    EXPECT_EQ(equity_.size(), 4u) << "every cycle still books its equity point";
}

// ------------------------------------------------------------------------------------------------
// Controls: the delayed feed leaves the same per-symbol state as feeding every bar in order
// ------------------------------------------------------------------------------------------------

namespace {

double close_of(const std::string& symbol, int d) {
    const double phase = symbol == "ES" ? 0.0 : 1.3;
    return 4000.0 * (1.0 + 0.03 * std::sin(0.11 * d + phase) + 0.0004 * d);
}

}  // namespace

class JunkFeedStateEqualityTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();
        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        ASSERT_TRUE(db_->connect().is_ok());
        auto& registry = InstrumentRegistry::instance();
        for (const auto& symbol : {"MES", "MNQ"}) {
            FuturesSpec spec;
            spec.root_symbol = symbol;
            spec.exchange = "CME";
            spec.currency = "USD";
            spec.multiplier = 5.0;
            spec.tick_size = 0.25;
            spec.commission_per_contract = 2.0;
            spec.initial_margin = 10000.0;
            spec.maintenance_margin = 8000.0;
            spec.weight = 1.0;
            spec.trading_hours = "09:30-16:00";
            registry.instruments_[symbol] = std::make_shared<FuturesInstrument>(symbol, spec);
        }
        registry.initialized_ = true;
    }
    void TearDown() override {
        auto& registry = InstrumentRegistry::instance();
        registry.instruments_.clear();
        registry.initialized_ = false;
        db_.reset();
        StateManager::reset_instance();
        TestBase::TearDown();
    }

    std::shared_ptr<TrendFollowingStrategy> make_trend() {
        static int n = 0;
        StrategyConfig sc;
        sc.capital_allocation = 1'000'000.0;
        sc.max_leverage = 100.0;
        sc.asset_classes = {AssetClass::FUTURES};
        sc.frequencies = {DataFrequency::DAILY};
        for (const auto& s : {"ES", "NQ"}) {
            sc.trading_params[s] = 5.0;
            sc.position_limits[s] = 1000.0;
        }
        TrendFollowingConfig tc;
        tc.weight = 1.0 / 30.0;
        tc.risk_target = 0.2;
        tc.idm = 2.5;
        tc.use_position_buffering = true;
        tc.ema_windows = {{2, 8}, {4, 16}, {8, 32}, {16, 64}, {32, 128}};
        tc.vol_lookback_short = 32;
        tc.vol_lookback_long = 252;
        tc.fdm = {{1, 1.0}, {2, 1.03}, {3, 1.08}, {4, 1.13}, {5, 1.19}, {6, 1.26}};
        auto& registry = InstrumentRegistry::instance();
        auto registry_ptr = std::shared_ptr<InstrumentRegistry>(&registry, [](InstrumentRegistry*) {});
        auto s = std::make_shared<TrendFollowingStrategy>("JF_TREND_" + std::to_string(++n), sc, tc,
                                                          db_, registry_ptr);
        EXPECT_TRUE(s->initialize().is_ok());
        RiskLimits limits;
        limits.max_position_size = 1000.0;
        limits.max_notional_value = 1e9;
        limits.max_drawdown = 0.9;
        limits.max_leverage = 100.0;
        EXPECT_TRUE(s->update_risk_limits(limits).is_ok());
        EXPECT_TRUE(s->start().is_ok());
        return s;
    }

    std::shared_ptr<MockPostgresDatabase> db_;
};

// The trend sleeve: ES's day-J bar withheld and fed with day J+1 ends in the same price history,
// volatility and forecast for ES as feeding it on its own day; NQ (fed normally) is untouched.
TEST_F(JunkFeedStateEqualityTest, TheTrendSleevesStateAfterTheDelayedFeedEqualsInOrderFeeding) {
    auto in_order = make_trend();
    auto delayed = make_trend();
    const int J = 280, LAST = 282;
    for (int d = 0; d <= LAST; ++d) {
        const Bar es = bar("ES", d, close_of("ES", d));
        const Bar nq = bar("NQ", d, close_of("NQ", d));
        ASSERT_TRUE(in_order->on_data({es, nq}).is_ok());
        if (d == J) {
            ASSERT_TRUE(delayed->on_data({nq}).is_ok());  // ES withheld
        } else if (d == J + 1) {
            ASSERT_TRUE(delayed->on_data({bar("ES", J, close_of("ES", J)), es, nq}).is_ok());
        } else {
            ASSERT_TRUE(delayed->on_data({es, nq}).is_ok());
        }
    }
    for (const std::string sym : {"ES", "NQ"}) {
        const auto* a = in_order->get_instrument_data(sym);
        const auto* b = delayed->get_instrument_data(sym);
        ASSERT_NE(a, nullptr);
        ASSERT_NE(b, nullptr);
        EXPECT_EQ(std::vector<double>(a->price_history.begin(), a->price_history.end()),
                  std::vector<double>(b->price_history.begin(), b->price_history.end()))
            << sym;
        EXPECT_EQ(a->current_volatility, b->current_volatility) << sym;
        EXPECT_EQ(a->current_raw_forecast, b->current_raw_forecast) << sym;
        EXPECT_EQ(a->current_forecast, b->current_forecast) << sym;
    }
}

// The PortfolioManager's own history (T-6c commit B, one close per symbol per date, 756 cap):
// the same closes and returns either way; on the withholding cycle the date intersection (S3)
// loses the junk date for every symbol, as live's does (adversary F4), and gets it back next cycle.
TEST_F(JunkFeedStateEqualityTest, ThePmHistoryAfterTheDelayedFeedEqualsInOrderFeeding) {
    PortfolioManager in_order(plain_config(), "PM_JF_IN_ORDER");
    PortfolioManager delayed(plain_config(), "PM_JF_DELAYED");
    auto s1 = make_trend();
    auto s2 = make_trend();
    ASSERT_TRUE(in_order.add_strategy(s1, 1.0, false).is_ok());
    ASSERT_TRUE(delayed.add_strategy(s2, 1.0, false).is_ok());
    const int J = 30, LAST = 32;
    for (int d = 0; d <= LAST; ++d) {
        const Bar es = bar("ES", d, close_of("ES", d));
        const Bar nq = bar("NQ", d, close_of("NQ", d));
        ASSERT_TRUE(in_order.process_market_data({es, nq}).is_ok());
        if (d == J) {
            ASSERT_TRUE(delayed.process_market_data({nq}).is_ok());
            const auto aligned = delayed.date_aligned_returns(delayed.closes_by_date_);
            EXPECT_EQ(aligned.at("ES").size(), static_cast<size_t>(J - 1))
                << "the junk date leaves the intersection for every symbol";
            EXPECT_EQ(aligned.at("NQ").size(), static_cast<size_t>(J - 1));
        } else if (d == J + 1) {
            ASSERT_TRUE(
                delayed.process_market_data({bar("ES", J, close_of("ES", J)), es, nq}).is_ok());
        } else {
            ASSERT_TRUE(delayed.process_market_data({es, nq}).is_ok());
        }
    }
    EXPECT_EQ(in_order.closes_by_date_, delayed.closes_by_date_);
    EXPECT_EQ(in_order.historical_returns_, delayed.historical_returns_);
    const auto a = in_order.date_aligned_returns(in_order.closes_by_date_);
    const auto b = delayed.date_aligned_returns(delayed.closes_by_date_);
    EXPECT_EQ(a, b);
    EXPECT_EQ(b.at("ES").size(), static_cast<size_t>(LAST));
}

// ------------------------------------------------------------------------------------------------
// The fill price on a release cycle (C7a amended, lead ruling C7a_HALT option 1)
// ------------------------------------------------------------------------------------------------
//
// On a release cycle the PM is fed a symbol's withheld JUNK bar ahead of its new bar, in date
// order. A fill of that symbol is priced at its LATEST-dated bar in the feed (the signal group's
// close), never at the older released junk bar. Before the fix the PM took the FIRST bar of the
// symbol in the vector, which on C7a's feed was the junk close (btfut: 6L 2025-03-25 at 0.17455,
// 2025-07-15 at 0.17915; M2K and MYM 2025-12-30).

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
