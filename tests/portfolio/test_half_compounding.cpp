// LOOP_SPEC v6.2 section 3.1 (D19): Carver's half compounding is the sizing capital of the futures
// book in both engines. This file uses only what existed before the commit that brings it
// (backtest_sizing_equity, the runners' and templates' text), so it compiles on the commit before
// and fails there: that run is its proof that it tests the change.
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "trade_ngin/portfolio/sizing_capital.hpp"

using namespace trade_ngin;

namespace {

Timestamp day(int d) {
    return std::chrono::system_clock::from_time_t(1767571200) + std::chrono::hours(24 * d);
}

std::filesystem::path repo_root() {
    std::filesystem::path root = std::filesystem::current_path();
    while (!(std::filesystem::exists(root / "CMakeLists.txt") &&
             std::filesystem::exists(root / "config_template"))) {
        if (root == root.parent_path()) return {};
        root = root.parent_path();
    }
    return root;
}

std::string read_all(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

// A backtest whose account makes a profit and then a loss: the capital it sizes on follows
// min(S_0, capital + net) row by row.
TEST(HalfCompoundingBacktest, AProfitThenALossFollowsTheRecursionRowByRow) {
    const double s0 = 500'000.0;
    const std::vector<double> rows = {500'000.0, 503'125.5, 507'900.0, 504'000.0,
                                      498'250.25, 501'000.0, 509'100.0, 508'000.0};
    std::vector<std::pair<Timestamp, double>> curve;
    double capital = s0;
    for (size_t k = 0; k < rows.size(); ++k) {
        if (k > 0) capital = std::min(s0, capital + (rows[k] - rows[k - 1]));
        curve.emplace_back(day(static_cast<int>(k)), rows[k]);
        EXPECT_NEAR(backtest_sizing_equity(curve, s0), capital, 1e-6) << "row " << k;
    }
}

// A profit above the starting capital is set aside and never sized on.
TEST(HalfCompoundingBacktest, AProfitIsNotSizedOn) {
    const double s0 = 500'000.0;
    EXPECT_DOUBLE_EQ(backtest_sizing_equity({{day(0), s0}, {day(1), 503'125.5}}, s0), s0);
    EXPECT_NEAR(backtest_sizing_equity({{day(0), s0}, {day(1), 507'900.0}, {day(2), 504'000.0}}, s0),
                s0 - 3'900.0, 1e-6)
        << "a loss from a high comes off the starting capital, not off the high";
}

// Both live runners size the book on the capital the sizing read returns, log the unsettled day,
// and the backtest sizes on the half compounding of its own curve.
TEST(HalfCompoundingWiring, BothEnginesSizeOnTheHalfCompoundedCapital) {
    const auto root = repo_root();
    ASSERT_FALSE(root.empty());
    for (const char* runner : {"apps/strategies/live_portfolio_conservative.cpp",
                               "apps/strategies/live_portfolio.cpp"}) {
        const std::string src = read_all(root / runner);
        ASSERT_FALSE(src.empty()) << runner;
        EXPECT_NE(src.find("portfolio->set_sizing_capital(sizing_read.capital.capital)"),
                  std::string::npos)
            << runner;
        EXPECT_EQ(src.find("portfolio->set_sizing_capital(sizing_equity.equity)"), std::string::npos)
            << runner << " still sizes on the account's compounding value";
        EXPECT_NE(src.find("WARN(sizing_capital_unsettled_log_line("), std::string::npos) << runner;
    }
    const std::string coordinator = read_all(root / "src/backtest/backtest_coordinator.cpp");
    EXPECT_NE(coordinator.find("backtest_half_compounding(equity_curve, initial_capital)"),
              std::string::npos);
}

// Both futures books' templates carry the sizing mode and the starting capital, equal to the book's
// initial capital.
TEST(HalfCompoundingWiring, TheFuturesTemplatesCarryTheSizingKeys) {
    const auto root = repo_root();
    ASSERT_FALSE(root.empty());
    for (const char* book : {"conservative", "base"}) {
        std::ifstream in(root / "config_template" / "portfolios" / book / "portfolio.json");
        ASSERT_TRUE(in.good()) << book;
        const nlohmann::json portfolio = nlohmann::json::parse(in);
        ASSERT_TRUE(portfolio.contains("sizing_mode")) << book;
        EXPECT_EQ(portfolio["sizing_mode"], "half_compounding") << book;
        ASSERT_TRUE(portfolio.contains("starting_capital")) << book;
        EXPECT_DOUBLE_EQ(portfolio["starting_capital"].get<double>(),
                         portfolio["initial_capital"].get<double>())
            << book;
        EXPECT_DOUBLE_EQ(portfolio["starting_capital"].get<double>(), 500'000.0) << book;
    }
}
