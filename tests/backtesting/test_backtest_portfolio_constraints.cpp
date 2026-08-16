#include <gtest/gtest.h>
#include <chrono>
#include <map>
#include <memory>
#include <vector>
#include "../core/test_base.hpp"
#include "trade_ngin/backtest/backtest_portfolio_constraints.hpp"

using namespace trade_ngin;
using namespace trade_ngin::backtest;
using namespace trade_ngin::testing;

namespace {

Bar make_bar(const std::string& symbol, double close, std::chrono::system_clock::time_point ts) {
    Bar bar;
    bar.symbol = symbol;
    bar.timestamp = ts;
    bar.open = Decimal(close);
    bar.high = Decimal(close);
    bar.low = Decimal(close);
    bar.close = Decimal(close);
    bar.volume = 1000.0;
    return bar;
}

Position make_pos(const std::string& sym, double qty) {
    return Position(sym, Quantity(qty), Price(100.0), Decimal(0.0), Decimal(0.0), Timestamp{});
}

PortfolioConstraintsConfig default_config() {
    PortfolioConstraintsConfig c;
    c.use_risk_management = false;
    c.use_optimization = false;
    c.max_history_length = 252;
    c.min_periods_for_covariance = 20;
    c.default_variance = 0.01;
    return c;
}

std::shared_ptr<RiskManager> make_loose_risk_manager() {
    RiskConfig rc;
    rc.var_limit = 1.0;
    rc.jump_risk_limit = 1.0;
    rc.max_correlation = 1.0;
    rc.max_gross_leverage = 1e6;
    rc.max_net_leverage = 1e6;
    rc.capital = 1'000'000.0;
    rc.confidence_level = 0.99;
    rc.lookback_period = 252;
    return std::make_shared<RiskManager>(rc);
}

std::shared_ptr<DynamicOptimizer> make_default_optimizer() {
    DynamicOptConfig c;
    return std::make_shared<DynamicOptimizer>(c);
}

}  // namespace

class BacktestPortfolioConstraintsTest : public TestBase {};

TEST_F(BacktestPortfolioConstraintsTest, ConfigOnlyConstructorLeavesEverythingDisabled) {
    PortfolioConstraintsConfig cfg = default_config();
    cfg.use_risk_management = true;
    cfg.use_optimization = true;
    BacktestPortfolioConstraints bpc(cfg);
    EXPECT_FALSE(bpc.is_risk_management_enabled());  // null risk manager
    EXPECT_FALSE(bpc.is_optimization_enabled());     // null optimizer
}

TEST_F(BacktestPortfolioConstraintsTest, EnabledRequiresBothConfigFlagAndDependency) {
    PortfolioConstraintsConfig cfg = default_config();
    BacktestPortfolioConstraints bpc(cfg, make_loose_risk_manager(), make_default_optimizer());
    // Flags off: still disabled.
    EXPECT_FALSE(bpc.is_risk_management_enabled());
    EXPECT_FALSE(bpc.is_optimization_enabled());
}

TEST_F(BacktestPortfolioConstraintsTest, FullConstructorEnablesWhenFlagAndDepBothPresent) {
    PortfolioConstraintsConfig cfg = default_config();
    cfg.use_risk_management = true;
    cfg.use_optimization = true;
    BacktestPortfolioConstraints bpc(cfg, make_loose_risk_manager(), make_default_optimizer());
    EXPECT_TRUE(bpc.is_risk_management_enabled());
    EXPECT_TRUE(bpc.is_optimization_enabled());
}

TEST_F(BacktestPortfolioConstraintsTest, SettersFlipEnabledFlags) {
    PortfolioConstraintsConfig cfg = default_config();
    cfg.use_risk_management = true;
    cfg.use_optimization = true;
    BacktestPortfolioConstraints bpc(cfg);
    EXPECT_FALSE(bpc.is_risk_management_enabled());
    bpc.set_risk_manager(make_loose_risk_manager());
    EXPECT_TRUE(bpc.is_risk_management_enabled());
    bpc.set_optimizer(make_default_optimizer());
    EXPECT_TRUE(bpc.is_optimization_enabled());
}

TEST_F(BacktestPortfolioConstraintsTest, ApplyConstraintsNoOpWhenEverythingDisabled) {
    BacktestPortfolioConstraints bpc(default_config());
    std::vector<Bar> bars;
    std::map<std::string, Position> positions = {{"ES", make_pos("ES", 5.0)}};
    std::vector<RiskResult> risk_metrics;
    auto result = bpc.apply_constraints(bars, positions, risk_metrics);
    ASSERT_TRUE(result.is_ok());
    EXPECT_TRUE(risk_metrics.empty());
    EXPECT_DOUBLE_EQ(static_cast<double>(positions.at("ES").quantity), 5.0);
}

TEST_F(BacktestPortfolioConstraintsTest, ApplyRiskManagementWithoutManagerReturnsError) {
    BacktestPortfolioConstraints bpc(default_config());
    std::vector<Bar> bars;
    std::map<std::string, Position> positions = {{"ES", make_pos("ES", 5.0)}};
    auto result = bpc.apply_risk_management(bars, positions);
    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_DATA);
}

TEST_F(BacktestPortfolioConstraintsTest, ApplyOptimizationWithoutOptimizerReturnsError) {
    BacktestPortfolioConstraints bpc(default_config());
    std::map<std::string, Position> positions = {
        {"ES", make_pos("ES", 5.0)},
        {"NQ", make_pos("NQ", 3.0)},
    };
    auto result = bpc.apply_optimization(positions);
    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_DATA);
}

TEST_F(BacktestPortfolioConstraintsTest, UpdateHistoricalReturnsHandlesEmptyBars) {
    BacktestPortfolioConstraints bpc(default_config());
    bpc.update_historical_returns({});
    EXPECT_EQ(bpc.get_history_length("ES"), 0u);
}

TEST_F(BacktestPortfolioConstraintsTest, UpdateHistoricalReturnsBuildsReturnsAfterTwoBars) {
    BacktestPortfolioConstraints bpc(default_config());
    auto t0 = std::chrono::system_clock::now();
    bpc.update_historical_returns({make_bar("ES", 100.0, t0)});
    EXPECT_EQ(bpc.get_history_length("ES"), 0u);  // 1 price → 0 returns
    bpc.update_historical_returns({make_bar("ES", 110.0, t0 + std::chrono::hours(24))});
    EXPECT_EQ(bpc.get_history_length("ES"), 1u);  // 2 prices → 1 return
}

TEST_F(BacktestPortfolioConstraintsTest, UpdateHistoricalReturnsTrimsToMaxHistoryLength) {
    PortfolioConstraintsConfig cfg = default_config();
    cfg.max_history_length = 5;
    BacktestPortfolioConstraints bpc(cfg);
    auto t0 = std::chrono::system_clock::now();
    for (int i = 0; i < 20; ++i) {
        bpc.update_historical_returns(
            {make_bar("ES", 100.0 + i, t0 + std::chrono::hours(24 * i))});
    }
    EXPECT_LE(bpc.get_history_length("ES"), 5u);
}

TEST_F(BacktestPortfolioConstraintsTest, UpdateHistoricalReturnsSkipsNonPositivePriorPrice) {
    BacktestPortfolioConstraints bpc(default_config());
    auto t0 = std::chrono::system_clock::now();
    bpc.update_historical_returns({make_bar("ES", 0.0, t0)});  // zero is non-positive
    bpc.update_historical_returns({make_bar("ES", 100.0, t0 + std::chrono::hours(24))});
    bpc.update_historical_returns({make_bar("ES", 110.0, t0 + std::chrono::hours(48))});
    // First price 0 → second-step return skipped due to prev<=0 guard. Third step → 1 return.
    EXPECT_EQ(bpc.get_history_length("ES"), 1u);
}

TEST_F(BacktestPortfolioConstraintsTest, CovarianceWithEmptySymbolsReturnsEmptyMatrix) {
    BacktestPortfolioConstraints bpc(default_config());
    auto cov = bpc.calculate_covariance_matrix({});
    EXPECT_TRUE(cov.empty());
}

TEST_F(BacktestPortfolioConstraintsTest, CovarianceFallsBackToDiagonalWhenInsufficientData) {
    PortfolioConstraintsConfig cfg = default_config();
    cfg.min_periods_for_covariance = 10;
    cfg.default_variance = 0.04;
    BacktestPortfolioConstraints bpc(cfg);
    auto cov = bpc.calculate_covariance_matrix({"ES", "NQ"});
    ASSERT_EQ(cov.size(), 2u);
    EXPECT_DOUBLE_EQ(cov[0][0], 0.04);
    EXPECT_DOUBLE_EQ(cov[1][1], 0.04);
    EXPECT_DOUBLE_EQ(cov[0][1], 0.0);
    EXPECT_DOUBLE_EQ(cov[1][0], 0.0);
}

TEST_F(BacktestPortfolioConstraintsTest, CovarianceComputesRealMatrixWithSufficientData) {
    PortfolioConstraintsConfig cfg = default_config();
    cfg.min_periods_for_covariance = 5;
    BacktestPortfolioConstraints bpc(cfg);
    auto t0 = std::chrono::system_clock::now();

    // Feed 12 bars → 11 returns each, well above the min (5).
    std::vector<double> es_prices{100, 102, 101, 103, 105, 104, 106, 108, 107, 109, 111, 110};
    std::vector<double> nq_prices{200, 199, 201, 200, 202, 201, 203, 202, 204, 203, 205, 204};
    for (size_t i = 0; i < es_prices.size(); ++i) {
        bpc.update_historical_returns(
            {make_bar("ES", es_prices[i], t0 + std::chrono::hours(24 * i)),
             make_bar("NQ", nq_prices[i], t0 + std::chrono::hours(24 * i))});
    }

    auto cov = bpc.calculate_covariance_matrix({"ES", "NQ"});
    ASSERT_EQ(cov.size(), 2u);
    ASSERT_EQ(cov[0].size(), 2u);
    EXPECT_GT(cov[0][0], 0.0);                  // ES variance positive
    EXPECT_GT(cov[1][1], 0.0);                  // NQ variance positive
    EXPECT_DOUBLE_EQ(cov[0][1], cov[1][0]);     // Symmetric
}

TEST_F(BacktestPortfolioConstraintsTest, ResetClearsHistoricalState) {
    BacktestPortfolioConstraints bpc(default_config());
    auto t0 = std::chrono::system_clock::now();
    bpc.update_historical_returns({make_bar("ES", 100.0, t0)});
    bpc.update_historical_returns({make_bar("ES", 110.0, t0 + std::chrono::hours(24))});
    ASSERT_EQ(bpc.get_history_length("ES"), 1u);

    bpc.reset();
    EXPECT_EQ(bpc.get_history_length("ES"), 0u);
}

TEST_F(BacktestPortfolioConstraintsTest, ApplyConstraintsRunsRiskWhenEnabled) {
    PortfolioConstraintsConfig cfg = default_config();
    cfg.use_risk_management = true;
    BacktestPortfolioConstraints bpc(cfg, make_loose_risk_manager(), nullptr);
    ASSERT_TRUE(bpc.is_risk_management_enabled());

    auto t0 = std::chrono::system_clock::now();
    std::vector<Bar> bars;
    for (int i = 0; i < 30; ++i) {
        bars.push_back(make_bar("ES", 100.0 + i * 0.1, t0 + std::chrono::hours(24 * i)));
    }
    std::map<std::string, Position> positions = {{"ES", make_pos("ES", 1.0)}};
    std::vector<RiskResult> metrics;
    auto result = bpc.apply_constraints(bars, positions, metrics);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(metrics.size(), 1u);
    // Loose limits → no scaling expected
    EXPECT_DOUBLE_EQ(static_cast<double>(positions.at("ES").quantity), 1.0);
}

TEST_F(BacktestPortfolioConstraintsTest, ApplyConstraintsSkipsOptimizationWithSinglePosition) {
    PortfolioConstraintsConfig cfg = default_config();
    cfg.use_optimization = true;
    BacktestPortfolioConstraints bpc(cfg, nullptr, make_default_optimizer());
    ASSERT_TRUE(bpc.is_optimization_enabled());

    std::vector<Bar> bars;
    std::map<std::string, Position> positions = {{"ES", make_pos("ES", 5.0)}};
    std::vector<RiskResult> metrics;
    // With only one position the optimizer branch is bypassed (size > 1 guard).
    auto result = bpc.apply_constraints(bars, positions, metrics);
    EXPECT_TRUE(result.is_ok());
    EXPECT_DOUBLE_EQ(static_cast<double>(positions.at("ES").quantity), 5.0);
}
