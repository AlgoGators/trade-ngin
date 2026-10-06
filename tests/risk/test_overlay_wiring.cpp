// LOOP_SPEC v6.2 section 4: the risk overlay reads the book in capital terms. This file reads only
// the repository's text and the two futures templates, so it compiles on the commit before the one
// that brings it and fails there: that run is its proof that it tests the change.
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

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

// Both futures templates carry section 12's limits: the three risk limits as ratios to tau on the
// carver module, and L_max 8.0 and L_net_max 6.0 as its (and the reporting block's) leverage limits.
TEST(OverlayWiring, TheFuturesTemplatesCarrySection12sLimits) {
    const auto root = repo_root();
    ASSERT_FALSE(root.empty());
    for (const char* book : {"conservative", "base"}) {
        std::ifstream in(root / "config_template" / "portfolios" / book / "risk.json");
        ASSERT_TRUE(in.good()) << book;
        const nlohmann::json risk = nlohmann::json::parse(in);
        const nlohmann::json* carver = nullptr;
        for (const auto& module : risk.at("modules")) {
            if (module.value("type", "") == "carver") carver = &module;
        }
        ASSERT_NE(carver, nullptr) << book;
        ASSERT_TRUE(carver->contains("R_max")) << book;
        EXPECT_DOUBLE_EQ(carver->at("R_max").get<double>(), 2.25) << book;
        EXPECT_DOUBLE_EQ(carver->value("R_jump_max", 0.0), 4.5) << book;
        EXPECT_DOUBLE_EQ(carver->value("R_shock_max", 0.0), 4.0) << book;
        EXPECT_DOUBLE_EQ(carver->at("max_gross_leverage").get<double>(), 8.0) << book;
        EXPECT_DOUBLE_EQ(carver->at("max_net_leverage").get<double>(), 6.0) << book;
        EXPECT_DOUBLE_EQ(risk.at("risk_reporting").at("max_gross_leverage").get<double>(), 8.0) << book;
        EXPECT_DOUBLE_EQ(risk.at("risk_reporting").at("max_net_leverage").get<double>(), 6.0) << book;
    }
}

// The four futures runners name the book's first sleeve as the overlay's source and pass its risk
// target; the PortfolioManager's one pass builds the overlay's window from that sleeve's own series
// and reads the limits off the book's carver module, which itself reads no book and logs no
// OVERLAY line (LOOP_SPEC section 4: the overlay answers once per rebalance, inside the pass).
TEST(OverlayWiring, TheOverlayIsFedFromTheFirstSleeveAndReadByTheOnePass) {
    const auto root = repo_root();
    ASSERT_FALSE(root.empty());
    for (const char* runner : {"apps/backtest/bt_portfolio_conservative.cpp", "apps/backtest/bt_portfolio.cpp",
                               "apps/strategies/live_portfolio_conservative.cpp",
                               "apps/strategies/live_portfolio.cpp"}) {
        const std::string src = read_all(root / runner);
        ASSERT_FALSE(src.empty()) << runner;
        EXPECT_NE(src.find("portfolio_config.overlay_sleeve = "), std::string::npos) << runner;
        EXPECT_NE(src.find("portfolio_config.overlay_tau = trend_config.risk_target;"), std::string::npos)
            << runner;
        EXPECT_NE(src.find("apply_loop_config(app_config, portfolio_config);"), std::string::npos) << runner;
    }
    const std::string pm = read_all(root / "src/portfolio/portfolio_manager.cpp");
    EXPECT_NE(pm.find("overlay::build_inputs(in.tau, symbols, views)"), std::string::npos);
    EXPECT_NE(pm.find("carver->overlay_limits()"), std::string::npos);
    EXPECT_NE(pm.find("one_pass::rebalance(in)"), std::string::npos);
    EXPECT_EQ(pm.find("for (int lap"), std::string::npos);
    EXPECT_EQ(pm.find("max_iterations = 5"), std::string::npos) << "the lap loop is gone";
    const std::string module = read_all(root / "src/risk/carver_risk_module.cpp");
    EXPECT_EQ(module.find("OVERLAY"), std::string::npos);
    EXPECT_EQ(module.find("RISK_LEVERAGE_ROUNDED"), std::string::npos);
}
