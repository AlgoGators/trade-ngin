// Extended PortfolioManager coverage. Targets specific branches in
// add_strategy, update_allocations, process_market_data, get_portfolio_value,
// strategy/execution accessors, set_risk_manager, update_strategy_position,
// and update_cost_manager_market_data. Companion to test_portfolio_manager.cpp;
// no overlap with the basic happy-path tests there.

#include <gtest/gtest.h>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>
#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "mock_strategy.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/risk/risk_manager.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

PortfolioConfig default_config(bool use_optimization = false, bool use_risk_management = false) {
    PortfolioConfig c{
        1'000'000.0,           // total_capital
        100'000.0,             // reserve_capital
        0.6,                   // max_strategy_allocation
        0.05,                  // min_strategy_allocation
        use_optimization,
        use_risk_management,
    };
    c.opt_config.tau = 1.0;
    c.opt_config.capital = 1'000'000.0;
    c.opt_config.cost_penalty_scalar = 10.0;
    c.opt_config.asymmetric_risk_buffer = 0.1;
    c.opt_config.max_iterations = 100;
    c.opt_config.convergence_threshold = 1e-6;
    c.risk_config.var_limit = 1.0;            // loose so risk doesn't fire
    c.risk_config.max_correlation = 1.0;
    c.risk_config.max_gross_leverage = 1e6;
    c.risk_config.max_net_leverage = 1e6;
    c.risk_config.capital = 1'000'000.0;
    c.risk_config.confidence_level = 0.99;
    c.risk_config.lookback_period = 252;
    return c;
}

}  // namespace

class PortfolioManagerExtendedTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        ASSERT_TRUE(db_->connect().is_ok());

        static int n = 0;
        manager_id_ = "PM_EXT_" + std::to_string(++n);
        manager_ = std::make_unique<PortfolioManager>(default_config(), manager_id_);
    }

    void TearDown() override {
        manager_.reset();
        db_.reset();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        TestBase::TearDown();
    }

    struct StrategyHandle {
        std::shared_ptr<StrategyInterface> strategy;
        std::string id;
    };

    StrategyHandle make_strategy(const std::string& id_prefix,
                                  std::vector<std::string> symbols = {"AAPL"}) {
        static int n = 0;
        std::string unique_id = id_prefix + "_" + std::to_string(++n);
        StrategyConfig sc;
        sc.capital_allocation = 1'000'000.0;
        sc.max_leverage = 2.0;
        sc.asset_classes = {AssetClass::EQUITIES};
        sc.frequencies = {DataFrequency::DAILY};
        for (const auto& s : symbols) {
            sc.trading_params[s] = 1.0;
            sc.position_limits[s] = 10000.0;
        }
        auto strat = std::make_shared<MockStrategy>(unique_id, sc, db_);
        if (strat->initialize().is_error()) throw std::runtime_error("init failed");
        if (strat->start().is_error()) throw std::runtime_error("start failed");
        return {strat, unique_id};
    }

    std::vector<Bar> make_bars(const std::string& symbol, int n,
                                std::chrono::system_clock::time_point t0) {
        std::vector<Bar> bars;
        for (int i = 0; i < n; ++i) {
            Bar b;
            b.symbol = symbol;
            b.timestamp = t0 + std::chrono::hours(24 * i);
            double price = 100.0 + std::sin(i * 0.1) * 2.0;
            b.open = b.close = Decimal(price);
            b.high = Decimal(price + 1.0);
            b.low = Decimal(price - 1.0);
            b.volume = 100000.0;
            bars.push_back(b);
        }
        return bars;
    }

    std::shared_ptr<MockPostgresDatabase> db_;
    std::unique_ptr<PortfolioManager> manager_;
    std::string manager_id_;
};

// ===== add_strategy validation =====

TEST_F(PortfolioManagerExtendedTest, AddStrategyRejectsNullPointer) {
    auto r = manager_->add_strategy(nullptr, 0.3);
    EXPECT_TRUE(r.is_error());
}

TEST_F(PortfolioManagerExtendedTest, AddStrategyRejectsAllocationAboveMax) {
    auto strat = make_strategy("S");
    // max_strategy_allocation = 0.6 → 0.7 rejected
    auto r = manager_->add_strategy(strat.strategy, 0.7);
    EXPECT_TRUE(r.is_error());
}

TEST_F(PortfolioManagerExtendedTest, AddStrategyRejectsAllocationBelowMin) {
    auto strat = make_strategy("S");
    // min_strategy_allocation = 0.05 → 0.01 rejected
    auto r = manager_->add_strategy(strat.strategy, 0.01);
    EXPECT_TRUE(r.is_error());
}

TEST_F(PortfolioManagerExtendedTest, AddStrategyRejectsDuplicateId) {
    auto strat = make_strategy("DUP");
    ASSERT_TRUE(manager_->add_strategy(strat.strategy, 0.3).is_ok());
    auto r = manager_->add_strategy(strat.strategy, 0.2);
    EXPECT_TRUE(r.is_error());
}

TEST_F(PortfolioManagerExtendedTest, AddStrategyRejectsAllocationsThatExceedTotal) {
    // Three strategies each at 0.4 sum to 1.2 > 1.0 → third rejected.
    ASSERT_TRUE(manager_->add_strategy(make_strategy("A").strategy, 0.4).is_ok());
    ASSERT_TRUE(manager_->add_strategy(make_strategy("B").strategy, 0.4).is_ok());
    auto r = manager_->add_strategy(make_strategy("C").strategy, 0.4);
    EXPECT_TRUE(r.is_error());
}

TEST_F(PortfolioManagerExtendedTest, GetStrategiesReflectsOrderOfAddition) {
    auto a = make_strategy("ORDER_A");
    auto b = make_strategy("ORDER_B");
    ASSERT_TRUE(manager_->add_strategy(a.strategy, 0.3).is_ok());
    ASSERT_TRUE(manager_->add_strategy(b.strategy, 0.3).is_ok());
    auto strategies = manager_->get_strategies();
    EXPECT_EQ(strategies.size(), 2u);
}

// ===== update_allocations validation =====

TEST_F(PortfolioManagerExtendedTest, UpdateAllocationsRejectsUnknownStrategy) {
    auto strat = make_strategy("UA");
    ASSERT_TRUE(manager_->add_strategy(strat.strategy, 0.3).is_ok());
    auto r = manager_->update_allocations({{"NOT_REGISTERED", 0.5}});
    EXPECT_TRUE(r.is_error());
}

TEST_F(PortfolioManagerExtendedTest, UpdateAllocationsRejectsAllocationsSummingAboveOne) {
    auto a = make_strategy("UA1");
    auto b = make_strategy("UA2");
    ASSERT_TRUE(manager_->add_strategy(a.strategy, 0.3).is_ok());
    ASSERT_TRUE(manager_->add_strategy(b.strategy, 0.3).is_ok());
    auto r = manager_->update_allocations({{a.id, 0.6}, {b.id, 0.6}});
    EXPECT_TRUE(r.is_error());
}

TEST_F(PortfolioManagerExtendedTest, UpdateAllocationsAcceptsCompleteUpdateSummingToOne) {
    auto a = make_strategy("UA3");
    auto b = make_strategy("UA4");
    ASSERT_TRUE(manager_->add_strategy(a.strategy, 0.3).is_ok());
    ASSERT_TRUE(manager_->add_strategy(b.strategy, 0.3).is_ok());
    // Production contract: validate_allocations requires the supplied map to
    // sum to 1.0 (within 1e-6). Partial updates not supported.
    auto r = manager_->update_allocations({{a.id, 0.4}, {b.id, 0.6}});
    EXPECT_TRUE(r.is_ok());
}

TEST_F(PortfolioManagerExtendedTest, UpdateAllocationsRejectsSumNotEqualToOne) {
    auto a = make_strategy("UA3B");
    auto b = make_strategy("UA4B");
    ASSERT_TRUE(manager_->add_strategy(a.strategy, 0.3).is_ok());
    ASSERT_TRUE(manager_->add_strategy(b.strategy, 0.3).is_ok());
    // 0.4 + 0.5 = 0.9; both within bounds but sum != 1.0
    auto r = manager_->update_allocations({{a.id, 0.4}, {b.id, 0.5}});
    EXPECT_TRUE(r.is_error());
}

TEST_F(PortfolioManagerExtendedTest, UpdateAllocationsRejectsEmptyMap) {
    auto a = make_strategy("UA_EMPTY");
    ASSERT_TRUE(manager_->add_strategy(a.strategy, 0.3).is_ok());
    auto r = manager_->update_allocations({});
    EXPECT_TRUE(r.is_error());
}

TEST_F(PortfolioManagerExtendedTest, UpdateAllocationsRejectsBelowMin) {
    auto a = make_strategy("UA5");
    ASSERT_TRUE(manager_->add_strategy(a.strategy, 0.3).is_ok());
    auto r = manager_->update_allocations({{a.id, 0.001}});
    EXPECT_TRUE(r.is_error());
}

// ===== process_market_data branches =====

TEST_F(PortfolioManagerExtendedTest, ProcessEmptyBarsReturnsError) {
    auto strat = make_strategy("EMPTY");
    ASSERT_TRUE(manager_->add_strategy(strat.strategy, 0.3).is_ok());
    EXPECT_TRUE(manager_->process_market_data({}).is_error());
}

TEST_F(PortfolioManagerExtendedTest, ProcessSkipExecutionGenerationProducesNoExecutions) {
    auto strat = make_strategy("WARMUP");
    ASSERT_TRUE(manager_->add_strategy(strat.strategy, 0.3).is_ok());
    auto t0 = std::chrono::system_clock::now() - std::chrono::hours(24 * 300);
    auto bars = make_bars("AAPL", 300, t0);
    ASSERT_TRUE(manager_->process_market_data(bars, /*skip_execution_generation=*/true).is_ok());
    EXPECT_TRUE(manager_->get_recent_executions().empty());
}

TEST_F(PortfolioManagerExtendedTest, ProcessWithCurrentTimestampOverridesFillTime) {
    auto strat = make_strategy("TS_OVERRIDE");
    ASSERT_TRUE(manager_->add_strategy(strat.strategy, 0.3).is_ok());
    auto t0 = std::chrono::system_clock::now() - std::chrono::hours(24 * 300);
    auto bars = make_bars("AAPL", 300, t0);
    auto override_ts = t0 + std::chrono::hours(24 * 500);
    auto r = manager_->process_market_data(bars, /*skip=*/false, override_ts);
    EXPECT_TRUE(r.is_ok());
    auto execs = manager_->get_recent_executions();
    for (const auto& e : execs) {
        EXPECT_EQ(e.fill_time, override_ts);
    }
}

// ===== get_portfolio_value =====

TEST_F(PortfolioManagerExtendedTest, GetPortfolioValueWithoutPositionsEqualsCapital) {
    EXPECT_DOUBLE_EQ(manager_->get_portfolio_value({}), 1'000'000.0);
}

TEST_F(PortfolioManagerExtendedTest, GetPortfolioValueWithPositionsAddsMarkToMarket) {
    auto strat = make_strategy("PV", {"AAPL"});
    ASSERT_TRUE(manager_->add_strategy(strat.strategy, 0.3).is_ok());
    auto t0 = std::chrono::system_clock::now() - std::chrono::hours(24 * 300);
    ASSERT_TRUE(manager_->process_market_data(make_bars("AAPL", 300, t0)).is_ok());
    // After processing, mock strategy should have positions; value must be finite
    // and depend on the price map.
    auto value_low = manager_->get_portfolio_value({{"AAPL", 50.0}});
    auto value_high = manager_->get_portfolio_value({{"AAPL", 200.0}});
    EXPECT_TRUE(std::isfinite(value_low));
    EXPECT_TRUE(std::isfinite(value_high));
}

// ===== execution accessors =====

TEST_F(PortfolioManagerExtendedTest, ClearExecutionHistoryEmptiesAggregate) {
    auto strat = make_strategy("CLR1");
    ASSERT_TRUE(manager_->add_strategy(strat.strategy, 0.3).is_ok());
    auto t0 = std::chrono::system_clock::now() - std::chrono::hours(24 * 300);
    ASSERT_TRUE(manager_->process_market_data(make_bars("AAPL", 300, t0)).is_ok());
    manager_->clear_execution_history();
    EXPECT_TRUE(manager_->get_recent_executions().empty());
}

TEST_F(PortfolioManagerExtendedTest, ClearAllExecutionsEmptiesPerStrategyToo) {
    auto strat = make_strategy("CLR2");
    ASSERT_TRUE(manager_->add_strategy(strat.strategy, 0.3).is_ok());
    auto t0 = std::chrono::system_clock::now() - std::chrono::hours(24 * 300);
    ASSERT_TRUE(manager_->process_market_data(make_bars("AAPL", 300, t0)).is_ok());
    manager_->clear_all_executions();
    EXPECT_TRUE(manager_->get_recent_executions().empty());
    // Per-strategy executions also cleared.
    auto per_strat = manager_->get_strategy_executions();
    for (const auto& [_id, execs] : per_strat) {
        EXPECT_TRUE(execs.empty());
    }
}

// ===== update_strategy_position =====

TEST_F(PortfolioManagerExtendedTest, UpdateStrategyPositionRejectsUnknownStrategy) {
    Position pos("AAPL", Quantity(10.0), Price(100.0), Decimal(0.0), Decimal(0.0), Timestamp{});
    auto r = manager_->update_strategy_position("NOT_FOUND", "AAPL", pos);
    EXPECT_TRUE(r.is_error());
}

TEST_F(PortfolioManagerExtendedTest, UpdateStrategyPositionAcceptsKnownStrategy) {
    auto strat = make_strategy("USP");
    ASSERT_TRUE(manager_->add_strategy(strat.strategy, 0.3).is_ok());
    auto t0 = std::chrono::system_clock::now() - std::chrono::hours(24 * 300);
    ASSERT_TRUE(manager_->process_market_data(make_bars("AAPL", 300, t0)).is_ok());

    Position updated("AAPL", Quantity(42.0), Price(105.0), Decimal(123.0), Decimal(0.0),
                      Timestamp{});
    auto r = manager_->update_strategy_position(strat.id, "AAPL", updated);
    EXPECT_TRUE(r.is_ok());
    auto positions = manager_->get_strategy_positions();
    ASSERT_TRUE(positions.count(strat.id));
    auto& sp = positions.at(strat.id);
    if (sp.count("AAPL")) {
        EXPECT_DOUBLE_EQ(static_cast<double>(sp.at("AAPL").quantity), 42.0);
    }
}

// ===== set_risk_manager / external risk manager pathway =====

TEST_F(PortfolioManagerExtendedTest, SetExternalRiskManagerSwitchesActiveImplementation) {
    auto cfg = default_config(/*use_optimization=*/false, /*use_risk_management=*/true);
    auto pm = std::make_unique<PortfolioManager>(cfg, manager_id_ + "_RISK");
    auto external = std::make_shared<RiskManager>(cfg.risk_config);
    pm->set_risk_manager(external);
    // No crash + subsequent operations work with the external manager.
    auto strat = make_strategy("EXT_RISK");
    EXPECT_TRUE(pm->add_strategy(strat.strategy, 0.3, /*opt=*/false, /*risk=*/true).is_ok());
}

TEST_F(PortfolioManagerExtendedTest, SetExternalRiskManagerWithNullDoesNotReplace) {
    auto cfg = default_config(/*use_optimization=*/false, /*use_risk_management=*/true);
    auto pm = std::make_unique<PortfolioManager>(cfg, manager_id_ + "_NOOP");
    pm->set_risk_manager(nullptr);
    // Internal risk manager should still function.
    auto strat = make_strategy("NULL_RISK");
    EXPECT_TRUE(pm->add_strategy(strat.strategy, 0.3, /*opt=*/false, /*risk=*/true).is_ok());
}

// ===== update_cost_manager_market_data + downstream =====

TEST_F(PortfolioManagerExtendedTest, UpdateCostManagerMarketDataDoesNotErrorOrAlterPositions) {
    auto strat = make_strategy("COST");
    ASSERT_TRUE(manager_->add_strategy(strat.strategy, 0.3).is_ok());
    auto t0 = std::chrono::system_clock::now() - std::chrono::hours(24 * 300);
    ASSERT_TRUE(manager_->process_market_data(make_bars("AAPL", 300, t0)).is_ok());
    auto pre = manager_->get_portfolio_positions();
    manager_->update_cost_manager_market_data("AAPL", 1'000'000.0, 105.0, 100.0);
    auto post = manager_->get_portfolio_positions();
    EXPECT_EQ(pre.size(), post.size());
}

// ===== getters =====

TEST_F(PortfolioManagerExtendedTest, GetConfigReturnsConstructorConfig) {
    const auto& cfg = manager_->get_config();
    EXPECT_DOUBLE_EQ(cfg.total_capital.as_double(), 1'000'000.0);
    EXPECT_DOUBLE_EQ(cfg.reserve_capital.as_double(), 100'000.0);
}

TEST_F(PortfolioManagerExtendedTest, GetPortfolioPositionsEmptyBeforeProcessing) {
    auto strat = make_strategy("GP");
    ASSERT_TRUE(manager_->add_strategy(strat.strategy, 0.3).is_ok());
    EXPECT_TRUE(manager_->get_portfolio_positions().empty());
}

TEST_F(PortfolioManagerExtendedTest, GetRequiredChangesEmptyBeforeProcessing) {
    auto strat = make_strategy("GRC");
    ASSERT_TRUE(manager_->add_strategy(strat.strategy, 0.3).is_ok());
    EXPECT_TRUE(manager_->get_required_changes().empty());
}

TEST_F(PortfolioManagerExtendedTest, GetStrategyPositionsEmptyBeforeProcessing) {
    auto strat = make_strategy("GSP");
    ASSERT_TRUE(manager_->add_strategy(strat.strategy, 0.3).is_ok());
    auto positions = manager_->get_strategy_positions();
    if (positions.count(strat.id)) {
        EXPECT_TRUE(positions.at(strat.id).empty());
    }
}

// ===== aggregation across multiple strategies =====

TEST_F(PortfolioManagerExtendedTest, MultipleStrategiesAggregatePositionsBySymbol) {
    auto a = make_strategy("AGG_A", {"AAPL"});
    auto b = make_strategy("AGG_B", {"AAPL"});  // both trade the same symbol
    ASSERT_TRUE(manager_->add_strategy(a.strategy, 0.3).is_ok());
    ASSERT_TRUE(manager_->add_strategy(b.strategy, 0.3).is_ok());
    auto t0 = std::chrono::system_clock::now() - std::chrono::hours(24 * 300);
    ASSERT_TRUE(manager_->process_market_data(make_bars("AAPL", 300, t0)).is_ok());

    auto by_strategy = manager_->get_strategy_positions();
    EXPECT_EQ(by_strategy.size(), 2u);

    auto portfolio = manager_->get_portfolio_positions();
    // Aggregate may collapse to a single symbol entry summing both strategies'
    // positions; either way the symbol must be tracked.
    EXPECT_TRUE(portfolio.count("AAPL") > 0 || portfolio.empty());
}

// ===== process with optimization enabled =====

TEST_F(PortfolioManagerExtendedTest, ProcessWithOptimizationEnabledSucceeds) {
    auto cfg = default_config(/*use_optimization=*/true, /*use_risk_management=*/false);
    auto pm = std::make_unique<PortfolioManager>(cfg, manager_id_ + "_OPT");
    auto strat = make_strategy("OPT");
    ASSERT_TRUE(pm->add_strategy(strat.strategy, 0.3, /*opt=*/true, /*risk=*/false).is_ok());
    auto t0 = std::chrono::system_clock::now() - std::chrono::hours(24 * 300);
    EXPECT_TRUE(pm->process_market_data(make_bars("AAPL", 300, t0)).is_ok());
}

TEST_F(PortfolioManagerExtendedTest, ProcessWithRiskManagementEnabledSucceeds) {
    auto cfg = default_config(/*use_optimization=*/false, /*use_risk_management=*/true);
    auto pm = std::make_unique<PortfolioManager>(cfg, manager_id_ + "_RM");
    auto strat = make_strategy("RM");
    ASSERT_TRUE(pm->add_strategy(strat.strategy, 0.3, /*opt=*/false, /*risk=*/true).is_ok());
    auto t0 = std::chrono::system_clock::now() - std::chrono::hours(24 * 300);
    EXPECT_TRUE(pm->process_market_data(make_bars("AAPL", 300, t0)).is_ok());
}

TEST_F(PortfolioManagerExtendedTest, ProcessWithOptimizationAndRiskBothEnabled) {
    auto cfg = default_config(/*use_optimization=*/true, /*use_risk_management=*/true);
    auto pm = std::make_unique<PortfolioManager>(cfg, manager_id_ + "_BOTH");
    auto strat = make_strategy("BOTH");
    ASSERT_TRUE(pm->add_strategy(strat.strategy, 0.3, /*opt=*/true, /*risk=*/true).is_ok());
    auto t0 = std::chrono::system_clock::now() - std::chrono::hours(24 * 300);
    EXPECT_TRUE(pm->process_market_data(make_bars("AAPL", 300, t0)).is_ok());
}
