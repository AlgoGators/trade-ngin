// PM-price-history: PortfolioManager::process_market_data copied each strategy's
// price history BEFORE feeding that strategy the bars.
//
// update_historical_returns() reads strategy->get_price_history(), and a
// strategy's price history is built by its own on_data(). Calling it first
// therefore read the PREVIOUS cycle's state: empty on the first cycle, one cycle
// stale on every cycle after. The only consumer of what it produces --
// price_history_ and the historical_returns_ derived from it -- is
// optimize_positions(), which runs later in the very same call. So the optimiser
// was building its covariance from data that did not include the bars it was
// optimising against, and on a first cycle from no data at all.
//
// The single-feed collapse (7d69fe89) is what made this reachable. While every
// strategy was fed twice per cycle, the history was already populated by the
// time the early call ran.
//
// The test uses a strategy whose history exists ONLY after on_data, which is the
// contract every real strategy has (TrendFollowing and MeanReversion both build
// theirs from the bars they are given). One cycle, then ask what the manager
// holds:
//   before the fix -- empty, because the copy happened before the feed
//   after  the fix -- this cycle's prices
//
// Private members are reached the same way test_portfolio_manager_internals.cpp
// does it.

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

// A strategy whose price history is a pure function of the bars it has been
// fed -- which is the contract PortfolioManager depends on and the reason the
// call order matters. MockStrategy cannot be used here: it inherits
// BaseStrategy::get_price_history, which returns an empty map unconditionally,
// so it would report "empty" whichever order the manager used.
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
            Position pos;
            pos.symbol = bar.symbol;
            pos.quantity = 1.0;
            pos.average_price = bar.close;
            pos.last_update = bar.timestamp;
            positions_[bar.symbol] = pos;
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

        StrategyConfig sc;
        sc.capital_allocation = 1'000'000.0;
        sc.max_leverage = 2.0;
        sc.asset_classes = {AssetClass::EQUITIES};
        sc.frequencies = {DataFrequency::DAILY};
        sc.trading_params["ES"] = 1.0;
        sc.position_limits["ES"] = 10000.0;
        strategy_ = std::make_shared<HistoryRecordingStrategy>(
            "HIST_" + std::to_string(n), sc, db_);
        ASSERT_TRUE(strategy_->initialize().is_ok());
        ASSERT_TRUE(strategy_->start().is_ok());
        ASSERT_TRUE(manager_->add_strategy(strategy_, 0.3).is_ok());
    }

    void TearDown() override {
        manager_.reset();
        strategy_.reset();
        db_.reset();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        trade_ngin::testing::TestBase::TearDown();
    }

    std::shared_ptr<MockPostgresDatabase> db_;
    std::unique_ptr<PortfolioManager> manager_;
    std::shared_ptr<HistoryRecordingStrategy> strategy_;
};

// The core claim: after ONE cycle, the manager holds THAT cycle's history.
TEST_F(PriceHistoryOrderingTest, FirstCycleHistoryIsVisibleToTheManager) {
    std::vector<Bar> bars{bar_at("ES", 0, 100.0)};
    ASSERT_TRUE(manager_->process_market_data(bars).is_ok());

    ASSERT_EQ(strategy_->feeds(), 1) << "the strategy must have been fed exactly once";
    ASSERT_EQ(manager_->price_history_.count("ES"), 1u)
        << "the manager read the strategy's price history BEFORE feeding it, so it copied "
           "the previous cycle's state -- which on cycle one is nothing at all (PM-price-history)";
    EXPECT_EQ(manager_->price_history_.at("ES").size(), 1u);
    EXPECT_DOUBLE_EQ(manager_->price_history_.at("ES").front(), 100.0);
}

// And it is not merely non-empty: it is current, not one cycle behind.
TEST_F(PriceHistoryOrderingTest, HistoryIsCurrentRatherThanOneCycleStale) {
    ASSERT_TRUE(manager_->process_market_data({bar_at("ES", 0, 100.0)}).is_ok());
    ASSERT_TRUE(manager_->process_market_data({bar_at("ES", 1, 110.0)}).is_ok());
    ASSERT_TRUE(manager_->process_market_data({bar_at("ES", 2, 120.0)}).is_ok());

    ASSERT_EQ(manager_->price_history_.count("ES"), 1u);
    const auto& prices = manager_->price_history_.at("ES");
    ASSERT_EQ(prices.size(), 3u)
        << "the manager is holding " << prices.size()
        << " of 3 prices, so it is reading the history one cycle late (PM-price-history)";
    EXPECT_DOUBLE_EQ(prices.back(), 120.0)
        << "the newest price the manager holds is not the newest bar it was given";
}

// The returns the optimiser actually consumes follow from the same read, so pin
// them too: three prices are two returns, and their values are exact.
TEST_F(PriceHistoryOrderingTest, DerivedReturnsCoverEveryBarOfThisCycle) {
    ASSERT_TRUE(manager_->process_market_data({bar_at("ES", 0, 100.0)}).is_ok());
    ASSERT_TRUE(manager_->process_market_data({bar_at("ES", 1, 110.0)}).is_ok());
    ASSERT_TRUE(manager_->process_market_data({bar_at("ES", 2, 121.0)}).is_ok());

    ASSERT_EQ(manager_->historical_returns_.count("ES"), 1u);
    const auto& rets = manager_->historical_returns_.at("ES");
    ASSERT_EQ(rets.size(), 2u);
    EXPECT_NEAR(rets[0], 0.10, 1e-12);
    EXPECT_NEAR(rets[1], 0.10, 1e-12);
}
