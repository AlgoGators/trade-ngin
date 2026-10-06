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
// target; the PortfolioManager hands the rebalance's inputs to its modules; the carver module reads
// the book through the overlay and logs the OVERLAY line.
TEST(OverlayWiring, TheOverlayIsFedFromTheFirstSleeveAndReadByTheModule) {
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
    }
    const std::string pm = read_all(root / "src/portfolio/portfolio_manager.cpp");
    EXPECT_NE(pm.find("overlay::build_inputs(config_.overlay_tau, symbols, series)"), std::string::npos);
    EXPECT_NE(pm.find("ctx.overlay_inputs = overlay_inputs_set_ ? &overlay_inputs_ : nullptr;"),
              std::string::npos);
    const std::string module = read_all(root / "src/risk/carver_risk_module.cpp");
    EXPECT_NE(module.find("overlay::evaluate(inputs, quantities, capital, overlay_ratios_)"),
              std::string::npos);
    EXPECT_NE(module.find("\"OVERLAY lap=\""), std::string::npos);
}
