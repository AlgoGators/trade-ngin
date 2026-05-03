// Regression tests for the multi-strategy aggregation bug.
//
// Bug: get_portfolio_positions() applies info.allocation a second time
// (Σ qᵢ × allocᵢ), which double-discounts strategy positions because each
// strategy already sized for its own capital slice. The broker actually
// holds the simple sum (Σ qᵢ), and that's what risk/margin/CSV consumers
// must operate on. The fix in live_portfolio.cpp / backtest_coordinator.cpp /
// portfolio_manager.cpp:apply_risk_management swaps the input map for a
// per-strategy sum.
//
// These tests pin down the invariants the fix relies on so the bug cannot
// silently re-introduce itself.

#include <gtest/gtest.h>
#include <thread>
#include "../data/test_db_utils.hpp"
#include "../order/test_utils.hpp"
#include "mock_strategy.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

class PortfolioAggregationTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::instance().reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        PortfolioConfig config{
            1'000'000.0,  // total_capital
            100'000.0,    // reserve_capital
            0.9,          // max_strategy_allocation (allow 0.7)
            0.05,         // min_strategy_allocation (allow 0.3)
            false,        // use_optimization
            false         // use_risk_management
        };
        config.opt_config.tau = 1.0;
        config.opt_config.capital = config.total_capital.as_double();
        config.opt_config.cost_penalty_scalar = 10;
        config.opt_config.asymmetric_risk_buffer = 0.1;
        config.opt_config.max_iterations = 100;
        config.opt_config.convergence_threshold = 1e-6;
        config.risk_config.var_limit = 1.0;
        config.risk_config.max_correlation = 1.0;
        config.risk_config.capital = config.total_capital;
        config.risk_config.confidence_level = 0.99;
        config.risk_config.lookback_period = 252;

        static int n = 0;
        manager_id_ = "PM_AGG_" + std::to_string(++n);
        manager_ = std::make_unique<PortfolioManager>(config, manager_id_);
        ASSERT_TRUE(manager_ != nullptr);

        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        ASSERT_TRUE(db_->connect().is_ok());
    }

    void TearDown() override {
        manager_.reset();
        db_.reset();
        StateManager::instance().reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        TestBase::TearDown();
    }

    std::shared_ptr<StrategyInterface> make_strategy(const std::string& prefix) {
        static int n = 0;
        std::string uid = prefix + "_" + std::to_string(++n);
        StrategyConfig sc;
        sc.capital_allocation = 1'000'000.0;
        sc.max_leverage = 4.0;
        sc.asset_classes = {AssetClass::EQUITIES};
        sc.frequencies = {DataFrequency::DAILY};
        sc.trading_params["AAPL"] = 1.0;
        sc.position_limits["AAPL"] = 10000.0;
        auto strat = std::make_shared<MockStrategy>(uid, sc, db_);
        EXPECT_TRUE(strat->initialize().is_ok());
        EXPECT_TRUE(strat->start().is_ok());
        return strat;
    }

    static Position make_position(const std::string& symbol, double qty, double price = 100.0) {
        Position p;
        p.symbol = symbol;
        p.quantity = Decimal(qty);
        p.average_price = Decimal(price);
        p.last_update = std::chrono::system_clock::now();
        return p;
    }

    std::unique_ptr<PortfolioManager> manager_;
    std::shared_ptr<MockPostgresDatabase> db_;
    std::string manager_id_;
};

// Pin the bug: get_portfolio_positions() applies allocation twice. Two
// strategies at 70/30 each holding qty=1 produce 1.0 in the aggregated view,
// while the broker actually holds 2.
TEST_F(PortfolioAggregationTest, GetPortfolioPositionsIsUnderScaled) {
    auto s1 = make_strategy("TF");
    auto s2 = make_strategy("TF_FAST");
    ASSERT_TRUE(manager_->add_strategy(s1, 0.7, false, false).is_ok());
    ASSERT_TRUE(manager_->add_strategy(s2, 0.3, false, false).is_ok());

    ASSERT_TRUE(manager_
                    ->update_strategy_position(s1->get_metadata().id, "AAPL",
                                               make_position("AAPL", 1.0))
                    .is_ok());
    ASSERT_TRUE(manager_
                    ->update_strategy_position(s2->get_metadata().id, "AAPL",
                                               make_position("AAPL", 1.0))
                    .is_ok());

    auto strat_positions = manager_->get_strategy_positions();
    ASSERT_EQ(strat_positions.size(), 2u);

    // Each strategy stores the integer quantity it was assigned.
    EXPECT_DOUBLE_EQ(
        static_cast<double>(strat_positions[s1->get_metadata().id]["AAPL"].quantity), 1.0);
    EXPECT_DOUBLE_EQ(
        static_cast<double>(strat_positions[s2->get_metadata().id]["AAPL"].quantity), 1.0);

    // Buggy aggregated view: 1×0.7 + 1×0.3 = 1.0 (under-scaled). This is
    // what feeds the unfixed CSV/margin/risk paths and what the surgical
    // fixes in live_portfolio / backtest_coordinator now bypass by summing
    // per-strategy directly.
    auto agg = manager_->get_portfolio_positions();
    ASSERT_EQ(agg.count("AAPL"), 1u);
    EXPECT_DOUBLE_EQ(static_cast<double>(agg["AAPL"].quantity), 1.0);
}

// The invariant the fix relies on: simple per-strategy sum yields the broker
// truth and stays integer when each strategy's quantity is integer.
TEST_F(PortfolioAggregationTest, PerStrategySumMatchesBrokerTruth) {
    auto s1 = make_strategy("TF");
    auto s2 = make_strategy("TF_FAST");
    ASSERT_TRUE(manager_->add_strategy(s1, 0.7, false, false).is_ok());
    ASSERT_TRUE(manager_->add_strategy(s2, 0.3, false, false).is_ok());

    ASSERT_TRUE(manager_
                    ->update_strategy_position(s1->get_metadata().id, "AAPL",
                                               make_position("AAPL", 4.0))
                    .is_ok());
    ASSERT_TRUE(manager_
                    ->update_strategy_position(s2->get_metadata().id, "AAPL",
                                               make_position("AAPL", -8.0))
                    .is_ok());

    // Per-strategy sum: 4 + (-8) = -4. This is the signed broker net,
    // matches the MBT-disagreement-day pattern observed in trading.positions.
    std::unordered_map<std::string, Position> portfolio_sum;
    for (const auto& [_, pos_map] : manager_->get_strategy_positions()) {
        for (const auto& [symbol, pos] : pos_map) {
            auto it = portfolio_sum.find(symbol);
            if (it == portfolio_sum.end()) {
                portfolio_sum[symbol] = pos;
            } else {
                it->second.quantity += pos.quantity;
            }
        }
    }
    ASSERT_EQ(portfolio_sum.count("AAPL"), 1u);
    EXPECT_DOUBLE_EQ(static_cast<double>(portfolio_sum["AAPL"].quantity), -4.0);

    // Buggy view on the same disagreement: 4×0.7 + (-8)×0.3 = +0.4. Sign
    // flipped, magnitude collapsed — this is the failure mode that produced
    // the MBT 0.071 ratio in trading.live_results.margin_posted.
    auto agg = manager_->get_portfolio_positions();
    ASSERT_EQ(agg.count("AAPL"), 1u);
    EXPECT_NEAR(static_cast<double>(agg["AAPL"].quantity), 0.4, 1e-9);
}

// Same-direction agreement day: per-strategy sum gives the broker truth,
// aggregated view returns Σ alloc² × Q_full when Q_strategy ∝ alloc.
TEST_F(PortfolioAggregationTest, AgreementDaySumMatchesBroker) {
    auto s1 = make_strategy("TF");
    auto s2 = make_strategy("TF_FAST");
    ASSERT_TRUE(manager_->add_strategy(s1, 0.7, false, false).is_ok());
    ASSERT_TRUE(manager_->add_strategy(s2, 0.3, false, false).is_ok());

    ASSERT_TRUE(manager_
                    ->update_strategy_position(s1->get_metadata().id, "AAPL",
                                               make_position("AAPL", 7.0))
                    .is_ok());
    ASSERT_TRUE(manager_
                    ->update_strategy_position(s2->get_metadata().id, "AAPL",
                                               make_position("AAPL", 3.0))
                    .is_ok());

    double sum_q = 0.0;
    for (const auto& [_, pos_map] : manager_->get_strategy_positions()) {
        for (const auto& [symbol, pos] : pos_map) {
            if (symbol == "AAPL") sum_q += static_cast<double>(pos.quantity);
        }
    }
    EXPECT_DOUBLE_EQ(sum_q, 10.0);

    // Aggregated: 7×0.7 + 3×0.3 = 4.9 + 0.9 = 5.8 (Σ alloc² × Q_full
    // pattern: 0.58 × 10 = 5.8).
    auto agg = manager_->get_portfolio_positions();
    EXPECT_NEAR(static_cast<double>(agg["AAPL"].quantity), 5.8, 1e-9);
}
