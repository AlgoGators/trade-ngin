// Coverage for live_metrics_calculator.cpp. Pure math, easy to test by
// computing expected values by hand.

#include <gtest/gtest.h>
#include <cmath>
#include <vector>
#include "trade_ngin/live/live_metrics_calculator.hpp"

using namespace trade_ngin;

class LiveMetricsCalculatorTest : public ::testing::Test {
protected:
    LiveMetricsCalculator c;
};

// ===== Returns =====

TEST_F(LiveMetricsCalculatorTest, DailyReturnZeroForNonPositivePrevious) {
    EXPECT_DOUBLE_EQ(c.calculate_daily_return(100.0, 0.0), 0.0);
    EXPECT_DOUBLE_EQ(c.calculate_daily_return(100.0, -1.0), 0.0);
}

TEST_F(LiveMetricsCalculatorTest, DailyReturnIsPercent) {
    EXPECT_DOUBLE_EQ(c.calculate_daily_return(1000.0, 100000.0), 1.0);
}

TEST_F(LiveMetricsCalculatorTest, TotalReturnZeroForNonPositiveInitial) {
    EXPECT_DOUBLE_EQ(c.calculate_total_return(110000.0, 0.0), 0.0);
}

TEST_F(LiveMetricsCalculatorTest, TotalReturnIsPercent) {
    EXPECT_DOUBLE_EQ(c.calculate_total_return(110000.0, 100000.0), 10.0);
}

TEST_F(LiveMetricsCalculatorTest, AnnualizedReturnZeroForZeroDays) {
    EXPECT_DOUBLE_EQ(c.calculate_annualized_return(0.1, 0), 0.0);
}

TEST_F(LiveMetricsCalculatorTest, AnnualizedReturnSingleDayReturnsTotal) {
    EXPECT_DOUBLE_EQ(c.calculate_annualized_return(0.05, 1), 5.0);
}

TEST_F(LiveMetricsCalculatorTest, AnnualizedReturnAfter252DaysIsTotal) {
    EXPECT_NEAR(c.calculate_annualized_return(0.10, 252), 10.0, 1e-9);
}

// ===== Leverage / margin =====

TEST_F(LiveMetricsCalculatorTest, GrossLeverageZeroForNonPositivePortfolio) {
    EXPECT_DOUBLE_EQ(c.calculate_gross_leverage(100000.0, 0.0), 0.0);
}

TEST_F(LiveMetricsCalculatorTest, GrossLeverageIsRatio) {
    EXPECT_DOUBLE_EQ(c.calculate_gross_leverage(200000.0, 100000.0), 2.0);
}

TEST_F(LiveMetricsCalculatorTest, EquityToMarginZeroForNonPositiveMargin) {
    EXPECT_DOUBLE_EQ(c.calculate_equity_to_margin_ratio(100000.0, 0.0), 0.0);
}

TEST_F(LiveMetricsCalculatorTest, EquityToMarginIsRatio) {
    EXPECT_DOUBLE_EQ(c.calculate_equity_to_margin_ratio(120000.0, 30000.0), 4.0);
}

TEST_F(LiveMetricsCalculatorTest, MarginCushion) {
    EXPECT_DOUBLE_EQ(c.calculate_margin_cushion(2.0), 100.0);
    EXPECT_DOUBLE_EQ(c.calculate_margin_cushion(1.0), 0.0);
}

TEST_F(LiveMetricsCalculatorTest, CashAvailableIsDifference) {
    EXPECT_DOUBLE_EQ(c.calculate_cash_available(100000.0, 30000.0), 70000.0);
}

// ===== Position PnL =====

TEST_F(LiveMetricsCalculatorTest, PositionPnLLongProfit) {
    // 2 contracts, entry 4500, current 4510, multiplier 50 → 2 * 10 * 50 = 1000
    EXPECT_DOUBLE_EQ(c.calculate_position_pnl(2.0, 4500.0, 4510.0, 50.0), 1000.0);
}

TEST_F(LiveMetricsCalculatorTest, PositionPnLShortProfitOnPriceDrop) {
    // -3 contracts, entry 4500, current 4490 → -3 * -10 * 50 = 1500
    EXPECT_DOUBLE_EQ(c.calculate_position_pnl(-3.0, 4500.0, 4490.0, 50.0), 1500.0);
}

TEST_F(LiveMetricsCalculatorTest, TotalPositionPnLAggregatesPositions) {
    std::vector<PositionPnL> v;
    PositionPnL a; a.pnl = 100.0; v.push_back(a);
    PositionPnL b; b.pnl = -50.0; v.push_back(b);
    PositionPnL d; d.pnl = 25.0; v.push_back(d);
    EXPECT_DOUBLE_EQ(c.calculate_total_position_pnl(v), 75.0);
}

TEST_F(LiveMetricsCalculatorTest, NetPnLIsGrossMinusCommissions) {
    EXPECT_DOUBLE_EQ(c.calculate_net_pnl(1000.0, 50.0), 950.0);
}

// ===== Risk metrics =====

TEST_F(LiveMetricsCalculatorTest, SharpeOfEmptyOrSingleIsZero) {
    EXPECT_DOUBLE_EQ(c.calculate_sharpe_ratio({}, 0.02), 0.0);
    EXPECT_DOUBLE_EQ(c.calculate_sharpe_ratio({0.01}, 0.02), 0.0);
}

TEST_F(LiveMetricsCalculatorTest, SharpeOfFlatReturnsIsZero) {
    EXPECT_DOUBLE_EQ(c.calculate_sharpe_ratio({0.01, 0.01, 0.01, 0.01}, 0.0), 0.0);
}

TEST_F(LiveMetricsCalculatorTest, SharpeIsNonZeroForVariableReturns) {
    auto s = c.calculate_sharpe_ratio({0.01, -0.005, 0.02, 0.015}, 0.0);
    EXPECT_GT(std::abs(s), 0.0);
}

TEST_F(LiveMetricsCalculatorTest, SortinoOfEmptyIsZero) {
    EXPECT_DOUBLE_EQ(c.calculate_sortino_ratio({}, 0.0), 0.0);
}

TEST_F(LiveMetricsCalculatorTest, SortinoZeroWhenNoDownside) {
    auto s = c.calculate_sortino_ratio({0.01, 0.02, 0.03}, 0.0);
    EXPECT_DOUBLE_EQ(s, 0.0);
}

TEST_F(LiveMetricsCalculatorTest, MaxDrawdownEmptyIsZero) {
    EXPECT_DOUBLE_EQ(c.calculate_max_drawdown({}), 0.0);
}

TEST_F(LiveMetricsCalculatorTest, MaxDrawdownComputesPercent) {
    // peak 100 → trough 80 = 20%
    EXPECT_DOUBLE_EQ(c.calculate_max_drawdown({100, 90, 80, 90}), 20.0);
}

TEST_F(LiveMetricsCalculatorTest, VolatilityEmptyOrSingleIsZero) {
    EXPECT_DOUBLE_EQ(c.calculate_volatility({}), 0.0);
    EXPECT_DOUBLE_EQ(c.calculate_volatility({0.01}), 0.0);
}

TEST_F(LiveMetricsCalculatorTest, VolatilityScalesBySqrt252) {
    auto v = c.calculate_volatility({0.01, -0.01});
    // std dev of {0.01, -0.01} with sample formula = sqrt(((0.01)^2 + (-0.01)^2)/(2-1)) = sqrt(0.0002)
    // annualized = sqrt(0.0002) * sqrt(252)
    EXPECT_NEAR(v, std::sqrt(0.0002) * std::sqrt(252.0), 1e-9);
}

TEST_F(LiveMetricsCalculatorTest, Var95EmptyIsZero) {
    EXPECT_DOUBLE_EQ(c.calculate_var_95({}), 0.0);
}

TEST_F(LiveMetricsCalculatorTest, Var95Returns5thPercentile) {
    std::vector<double> returns;
    for (int i = 1; i <= 100; ++i) returns.push_back(static_cast<double>(i));
    // 5% of 100 = 5 → returns the 5th element after sorting (which is 5).
    auto v = c.calculate_var_95(returns);
    EXPECT_DOUBLE_EQ(v, 6.0);  // index = 5 → element value 6
}

TEST_F(LiveMetricsCalculatorTest, CVaR95EmptyIsZero) {
    EXPECT_DOUBLE_EQ(c.calculate_cvar_95({}), 0.0);
}

TEST_F(LiveMetricsCalculatorTest, CVaR95AveragesBelowCutoff) {
    std::vector<double> returns;
    for (int i = 1; i <= 100; ++i) returns.push_back(static_cast<double>(i));
    // cutoff = 5 → average of [1..5] = 3
    auto cv = c.calculate_cvar_95(returns);
    EXPECT_DOUBLE_EQ(cv, 3.0);
}

// ===== Composite =====

TEST_F(LiveMetricsCalculatorTest, CalculateAllMetricsPopulatesFields) {
    auto m = c.calculate_all_metrics(/*daily_pnl=*/1000, /*prev_pv=*/100000,
                                      /*current_pv=*/101000, /*initial_cap=*/100000,
                                      /*gross_notional=*/200000, /*margin=*/30000,
                                      /*trading_days=*/10, /*tx_costs=*/0.0);
    EXPECT_DOUBLE_EQ(m.daily_return, 1.0);     // 1000/100000 * 100
    EXPECT_DOUBLE_EQ(m.total_return, 1.0);     // 1000/100000 * 100
    EXPECT_DOUBLE_EQ(m.gross_leverage, 200000.0 / 101000.0);
    EXPECT_DOUBLE_EQ(m.equity_to_margin_ratio, 101000.0 / 30000.0);
    EXPECT_DOUBLE_EQ(m.cash_available, 71000.0);
    EXPECT_EQ(m.trading_days, 10);
    EXPECT_DOUBLE_EQ(m.daily_pnl, 1000.0);
    EXPECT_DOUBLE_EQ(m.total_pnl, 1000.0);
}

TEST_F(LiveMetricsCalculatorTest, CalculateFinalizationMetricsPopulatesRealized) {
    auto m = c.calculate_finalization_metrics(/*realized=*/500, /*day_before=*/100000,
                                                /*current=*/100500, /*initial=*/100000,
                                                /*gross_notional=*/150000, /*margin=*/20000,
                                                /*days=*/5, /*commissions=*/10);
    EXPECT_DOUBLE_EQ(m.realized_pnl, 500.0);
    EXPECT_DOUBLE_EQ(m.daily_pnl, 500.0);
    EXPECT_DOUBLE_EQ(m.total_pnl, 500.0);
    EXPECT_EQ(m.trading_days, 5);
}

// ===== Utility =====

TEST_F(LiveMetricsCalculatorTest, WinRateZeroForNoTrades) {
    EXPECT_DOUBLE_EQ(c.calculate_win_rate(0, 0), 0.0);
}

TEST_F(LiveMetricsCalculatorTest, WinRateIsPercent) {
    EXPECT_DOUBLE_EQ(c.calculate_win_rate(3, 7), 30.0);
}

TEST_F(LiveMetricsCalculatorTest, ProfitFactorRatio) {
    EXPECT_DOUBLE_EQ(c.calculate_profit_factor(2000.0, 1000.0), 2.0);
}

TEST_F(LiveMetricsCalculatorTest, ProfitFactorLargeWhenNoLosses) {
    EXPECT_GT(c.calculate_profit_factor(1000.0, 0.0), 100.0);
}

TEST_F(LiveMetricsCalculatorTest, ProfitFactorZeroWhenNoActivity) {
    EXPECT_DOUBLE_EQ(c.calculate_profit_factor(0.0, 0.0), 0.0);
}
