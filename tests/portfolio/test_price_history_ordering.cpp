// PortfolioManager cycle behaviour.
//
// PM-price-history (E3, STAGE3_PLAN §25a.2 / §28b.3): process_market_data reads
// each strategy's price history AFTER the on_data loop, so the history the
// optimiser covaries includes the bars of this call -- its newest bar is the bar
// the strategy signals from. The tests below pin that order with a strategy whose
// history is exactly the bars it has been fed.
//
// The file also holds the on_data-swallowed-futures guard.

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <thread>
#include <string>
#include <unordered_map>
#include <vector>

#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"

#define private public
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#undef private

#include "trade_ngin/strategy/base_strategy.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

Bar bar_at(const std::string& symbol, int day_offset, double close) {
    Bar b;
    b.symbol = symbol;
    b.timestamp = std::chrono::system_clock::now() + std::chrono::hours(24 * day_offset);
    b.open = Decimal(close);
    b.high = Decimal(close);
    b.low = Decimal(close);
    b.close = Decimal(close);
    b.volume = 1000.0;
    return b;
}

PortfolioConfig plain_config() {
    // Optimisation and risk OFF: this test is about WHEN the history is read,
    // not about what the optimiser then does with it. Leaving them on would make
    // the assertions depend on the whole solver.
    PortfolioConfig c{1'000'000.0, 100'000.0, 0.6, 0.05, false, false};
    c.opt_config.capital = 1'000'000.0;
    c.risk_config.capital = 1'000'000.0;
    return c;
}

// A strategy whose price history is exactly the closes it has been fed by
// on_data, which is the contract every real strategy has (TrendFollowing and
// MeanReversion build theirs from the bars they are given). MockStrategy cannot
// be used: BaseStrategy::get_price_history returns an empty map whichever order
// the manager reads it in.
class HistoryRecordingStrategy : public BaseStrategy {
public:
    HistoryRecordingStrategy(std::string id, StrategyConfig config,
                             std::shared_ptr<DatabaseInterface> db)
        : BaseStrategy(std::move(id), std::move(config),
                       std::static_pointer_cast<trade_ngin::PostgresDatabase>(db)) {
        metadata_.name = "History Recording Strategy";
    }

    Result<void> on_data(const std::vector<Bar>& data) override {
        auto base = BaseStrategy::on_data(data);
        if (base.is_error()) return base;
        for (const auto& bar : data) {
            history_[bar.symbol].push_back(bar.close.as_double());
        }
        ++feeds_;
        return Result<void>();
    }

    std::unordered_map<std::string, std::vector<double>> get_price_history() const override {
        return history_;
    }

    int feeds() const { return feeds_; }

private:
    std::unordered_map<std::string, std::vector<double>> history_;
    int feeds_{0};
};

StrategyConfig es_config() {
    StrategyConfig sc;
    sc.capital_allocation = 1'000'000.0;
    sc.max_leverage = 2.0;
    sc.asset_classes = {AssetClass::EQUITIES};
    sc.frequencies = {DataFrequency::DAILY};
    sc.trading_params["ES"] = 1.0;
    sc.position_limits["ES"] = 10000.0;
    return sc;
}

}  // namespace

class PriceHistoryOrderingTest : public trade_ngin::testing::TestBase {
protected:
    void SetUp() override {
        trade_ngin::testing::TestBase::SetUp();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        ASSERT_TRUE(db_->connect().is_ok());
        static int n = 0;
        manager_ = std::make_unique<PortfolioManager>(plain_config(),
                                                      "PM_HISTORY_" + std::to_string(++n));

    }

    void TearDown() override {
        manager_.reset();
        db_.reset();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        trade_ngin::testing::TestBase::TearDown();
    }

    std::shared_ptr<MockPostgresDatabase> db_;
    std::unique_ptr<PortfolioManager> manager_;
};

// ===== PM-price-history: the history is read after on_data =====
//
// The backtest feeds one day's bars per call. A strategy that already holds day
// D-1 is fed day D by ONE process_market_data call on a fresh manager: the
// history the manager holds after that call must end at D, and its returns must
// include day D's return. Read before the on_data loop, the manager copies the
// history as it stood before D was fed -- one price, no return -- which is the
// one-bar lag T-4e measured in the futures backtest (covariance through t-2
// while signalling from t-1).
TEST_F(PriceHistoryOrderingTest, OneCallOnAFreshManagerReadsTheReturnOfTheDayItFeeds) {
    static int k = 0;
    auto strategy = std::make_shared<HistoryRecordingStrategy>(
        "HIST_D_" + std::to_string(++k), es_config(), db_);
    ASSERT_TRUE(strategy->initialize().is_ok());
    ASSERT_TRUE(strategy->start().is_ok());
    // Day D-1: the strategy's own accumulated history, before this manager exists.
    ASSERT_TRUE(strategy->on_data({bar_at("ES", -1, 100.0)}).is_ok());
    ASSERT_TRUE(manager_->add_strategy(strategy, 0.3).is_ok());

    // Day D, one call.
    ASSERT_TRUE(manager_->process_market_data({bar_at("ES", 0, 110.0)}).is_ok());

    ASSERT_EQ(strategy->feeds(), 2) << "D-1 directly, D through the manager, nothing else";
    ASSERT_EQ(manager_->price_history_.count("ES"), 1u);
    EXPECT_EQ(manager_->price_history_.at("ES"), (std::vector<double>{100.0, 110.0}))
        << "the manager read the strategy's history before feeding it day D, so its newest "
           "price is D-1's, not the bar the strategy signals from (PM-price-history)";
    ASSERT_EQ(manager_->historical_returns_.count("ES"), 1u)
        << "no returns at all for ES: day D's return is missing (PM-price-history)";
    const auto& rets = manager_->historical_returns_.at("ES");
    ASSERT_EQ(rets.size(), 1u) << "day D's return is missing (PM-price-history)";
    EXPECT_NEAR(rets.back(), 0.10, 1e-12) << "the newest return is not day D's (110 / 100 - 1)";
}

// And across cycles, the way the backtest runs: after each call the newest
// return is that call's, not the previous call's.
TEST_F(PriceHistoryOrderingTest, EveryCallsNewestReturnIsThatCallsBar) {
    static int k = 0;
    auto strategy = std::make_shared<HistoryRecordingStrategy>(
        "HIST_SEQ_" + std::to_string(++k), es_config(), db_);
    ASSERT_TRUE(strategy->initialize().is_ok());
    ASSERT_TRUE(strategy->start().is_ok());
    ASSERT_TRUE(manager_->add_strategy(strategy, 0.3).is_ok());

    const std::vector<double> closes{100.0, 110.0, 121.0, 108.9};
    for (size_t d = 0; d < closes.size(); ++d) {
        ASSERT_TRUE(manager_->process_market_data({bar_at("ES", static_cast<int>(d), closes[d])})
                        .is_ok());
        ASSERT_EQ(manager_->price_history_.count("ES"), 1u) << "after call " << d;
        EXPECT_EQ(manager_->price_history_.at("ES").size(), d + 1)
            << "after call " << d << " the manager holds the history one call late";
        if (d == 0) continue;
        ASSERT_EQ(manager_->historical_returns_.count("ES"), 1u) << "after call " << d;
        const auto& rets = manager_->historical_returns_.at("ES");
        ASSERT_EQ(rets.size(), d) << "after call " << d << " the newest return is missing";
        EXPECT_NEAR(rets.back(), closes[d] / closes[d - 1] - 1.0, 1e-12)
            << "after call " << d << " the newest return is not this call's bar";
    }
}

// ===== on_data-swallowed-futures =====
//
// PortfolioManager::process_market_data used to LOG an on_data failure and then
// call get_target_positions() anyway. What comes back from a strategy that did
// not ingest the bars is either the previous cycle's targets or -- from a
// process whose instrument data starts empty -- zero for every symbol. Zero
// targets against a held book is a full-book liquidation, not a no-op, and it
// would have been produced with an ERROR in the log and exit code 0.
//
// The equity runner grew an assertion around this (BA-17 / E2-F43); the futures
// runners never had one. The refusal now lives at the shared site, so it covers
// both.
//
// The strategy below fails on_data while still holding a position map, which is
// exactly the dangerous shape: there is something to liquidate, and the targets
// that would have been read do not reflect the bars.
namespace {

class FailingOnDataStrategy : public BaseStrategy {
public:
    FailingOnDataStrategy(std::string id, StrategyConfig config,
                          std::shared_ptr<DatabaseInterface> db)
        : BaseStrategy(std::move(id), std::move(config),
                       std::static_pointer_cast<trade_ngin::PostgresDatabase>(db)) {
        metadata_.name = "Failing OnData Strategy";
    }

    Result<void> on_data(const std::vector<Bar>&) override {
        ++calls_;
        return make_error<void>(ErrorCode::MARKET_DATA_ERROR,
                                "simulated ingest failure", "FailingOnDataStrategy");
    }

    // Deliberately non-empty and deliberately NOT derived from any bar: this is
    // the stale target map the old code would have shipped.
    std::unordered_map<std::string, Position> get_target_positions() const override {
        std::unordered_map<std::string, Position> t;
        Position p;
        p.symbol = "ES";
        p.quantity = 0.0;  // a full-book SELL against any held position
        t["ES"] = p;
        ++target_reads_;
        return t;
    }

    int calls() const { return calls_; }
    int target_reads() const { return target_reads_; }

private:
    int calls_{0};
    mutable int target_reads_{0};
};

}  // namespace

TEST_F(PriceHistoryOrderingTest, AFailedOnDataStopsTheCycleBeforeTargetsAreRead) {
    StrategyConfig sc;
    sc.capital_allocation = 1'000'000.0;
    sc.max_leverage = 2.0;
    sc.asset_classes = {AssetClass::EQUITIES};
    sc.frequencies = {DataFrequency::DAILY};
    sc.trading_params["ES"] = 1.0;
    sc.position_limits["ES"] = 10000.0;

    static int m = 0;
    auto failing = std::make_shared<FailingOnDataStrategy>(
        "FAILS_" + std::to_string(++m), sc, db_);
    ASSERT_TRUE(failing->initialize().is_ok());
    ASSERT_TRUE(failing->start().is_ok());
    ASSERT_TRUE(manager_->add_strategy(failing, 0.3).is_ok());

    auto r = manager_->process_market_data({bar_at("ES", 0, 100.0)});

    ASSERT_TRUE(r.is_error())
        << "process_market_data returned success after a strategy failed to ingest the bars; "
           "the targets it went on to read are stale or empty (on_data-swallowed-futures)";
    EXPECT_NE(std::string(r.error()->what()).find("did not see this cycle's prices"),
              std::string::npos)
        << "the failure is reported, but not as the ingest failure it is: " << r.error()->what();

    EXPECT_EQ(failing->calls(), 1);
    EXPECT_EQ(failing->target_reads(), 0)
        << "get_target_positions() was called on a strategy whose on_data had just failed";
}
