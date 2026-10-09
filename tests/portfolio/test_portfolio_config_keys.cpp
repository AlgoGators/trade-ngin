// LOOP_SPEC sections 7.5 and 7.5.1: the design keys a futures book records with every run, in
// backtest.run_metadata.portfolio_config and live_run_metadata.portfolio_config. Both are
// PortfolioConfig::to_json() (the backtest runners add strategy_allocations and strategy_names,
// the live runners their marks). This test pins EVERY key and value of the shipped CONSERVATIVE
// book's object, built from the tracked config_template exactly as the four futures runners build
// it, and that a book with no overlay sleeve still writes the object it always wrote.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "trade_ngin/core/config_loader.hpp"
#include "trade_ngin/portfolio/loop_config.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"

using namespace trade_ngin;

namespace {

std::filesystem::path repo_file(const std::string& relative) {
    namespace fs = std::filesystem;
    fs::path dir = fs::current_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (fs::exists(dir / relative)) return dir / relative;
        dir = dir.parent_path();
    }
    return {};
}

std::string read_all(const std::filesystem::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The PortfolioConfig the futures runners hand the PortfolioManager, from a loaded book.
PortfolioConfig runner_config(const AppConfig& app, const std::string& first_sleeve, double tau) {
    PortfolioConfig pc;
    pc.total_capital = app.initial_capital;
    pc.use_optimization = app.use_optimization;
    pc.max_strategy_allocation = app.strategy_defaults.max_strategy_allocation;
    pc.min_strategy_allocation = app.strategy_defaults.min_strategy_allocation;
    pc.opt_config = app.opt_config;
    pc.risk_config = app.risk_config;
    pc.risk_modules = app.risk_schema.portfolio;
    apply_loop_config(app, pc);
    pc.overlay_sleeve = first_sleeve;
    pc.overlay_tau = tau;
    return pc;
}

std::vector<std::string> keys_of(const nlohmann::json& j) {
    std::vector<std::string> out;
    for (const auto& item : j.items()) out.push_back(item.key());
    return out;
}

}  // namespace

TEST(PortfolioConfigKeys, TheFuturesBookRecordsEveryDesignKeyAndNoRetiredOne) {
    const auto tmpl = repo_file("config_template/defaults.json");
    ASSERT_FALSE(tmpl.empty()) << "config_template/ not found; this test must not skip";
    auto loaded = ConfigLoader::load(tmpl.parent_path(), "conservative");
    ASSERT_TRUE(loaded.is_ok()) << (loaded.is_error() ? loaded.error()->what() : "");
    ASSERT_TRUE(ConfigLoader::require_loop_keys(loaded.value()).is_ok());
    const nlohmann::json j = runner_config(loaded.value(), "TREND_FOLLOWING", 0.20).to_json();

    // Every key, by level (nlohmann orders keys by name).
    EXPECT_EQ(keys_of(j), (std::vector<std::string>{
                              "allow_fractional_positions", "equity_slow_rule", "max_strategy_allocation",
                              "min_strategy_allocation", "opt_config", "oracle_hash_list", "risk_config",
                              "sizing_mode", "total_capital", "use_optimization", "version"}));
    EXPECT_EQ(keys_of(j.at("opt_config")),
              (std::vector<std::string>{"b_sigma_floor", "capital", "convergence_threshold",
                                        "cost_penalty_scalar", "max_iterations", "sign_close_band",
                                        "use_buffering", "version"}));
    EXPECT_EQ(keys_of(j.at("risk_config")),
              (std::vector<std::string>{"R_jump_max", "R_max", "R_shock_max", "capital",
                                        "confidence_level", "lookback_period", "max_gross_leverage",
                                        "max_net_leverage", "per_name_cap", "trim_max", "version"}));
    EXPECT_EQ(keys_of(j.at("equity_slow_rule")), (std::vector<std::string>{"pairs", "symbols"}));

    // Every value of section 7.5.1's table.
    EXPECT_DOUBLE_EQ(j.at("total_capital").get<double>(), 500000.0) << "S_0, reused, not duplicated";
    EXPECT_DOUBLE_EQ(j.at("opt_config").at("cost_penalty_scalar").get<double>(), 100.0);
    EXPECT_DOUBLE_EQ(j.at("opt_config").at("sign_close_band").get<double>(), 2.0);
    EXPECT_DOUBLE_EQ(j.at("opt_config").at("b_sigma_floor").get<double>(), 0.05);
    EXPECT_DOUBLE_EQ(j.at("risk_config").at("max_gross_leverage").get<double>(), 8.0);
    EXPECT_DOUBLE_EQ(j.at("risk_config").at("max_net_leverage").get<double>(), 6.0);
    EXPECT_DOUBLE_EQ(j.at("risk_config").at("R_max").get<double>(), 2.25);
    EXPECT_DOUBLE_EQ(j.at("risk_config").at("R_jump_max").get<double>(), 4.5);
    EXPECT_DOUBLE_EQ(j.at("risk_config").at("R_shock_max").get<double>(), 4.0);
    EXPECT_DOUBLE_EQ(j.at("risk_config").at("per_name_cap").get<double>(), 2.0);
    EXPECT_EQ(j.at("risk_config").at("trim_max").get<int>(), 5);
    EXPECT_EQ(j.at("sizing_mode").get<std::string>(), "half_compounding");
    EXPECT_EQ(j.at("equity_slow_rule").dump(),
              "{\"pairs\":[[32,128],[64,256]],\"symbols\":[\"M2K\",\"MES\",\"MNQ\",\"MYM\"]}");
    EXPECT_EQ(j.at("oracle_hash_list").get<std::string>(), "oracle_sha256_frozen_v6_4.txt");
    // The keys the object carried before and keeps.
    EXPECT_EQ(j.at("use_optimization").get<bool>(), true);
    EXPECT_EQ(j.at("allow_fractional_positions").get<bool>(), false);
    EXPECT_DOUBLE_EQ(j.at("max_strategy_allocation").get<double>(), 1.0);
    EXPECT_DOUBLE_EQ(j.at("min_strategy_allocation").get<double>(), 0.1);
    EXPECT_DOUBLE_EQ(j.at("opt_config").at("capital").get<double>(), 500000.0);
    EXPECT_EQ(j.at("opt_config").at("max_iterations").get<int>(), 100);
    EXPECT_DOUBLE_EQ(j.at("opt_config").at("convergence_threshold").get<double>(), 1e-6);
    EXPECT_EQ(j.at("opt_config").at("use_buffering").get<bool>(), true);
    EXPECT_DOUBLE_EQ(j.at("risk_config").at("confidence_level").get<double>(), 0.99);
    EXPECT_EQ(j.at("risk_config").at("lookback_period").get<int>(), 252);
    EXPECT_DOUBLE_EQ(j.at("risk_config").at("capital").get<double>(), 500000.0);
    // No retired key, and no key the spec says is recorded elsewhere.
    for (const char* retired : {"tau", "asymmetric_risk_buffer", "buffer_size_factor"}) {
        EXPECT_FALSE(j.at("opt_config").contains(retired)) << retired;
    }
    for (const char* retired : {"var_limit", "jump_risk_limit", "max_correlation"}) {
        EXPECT_FALSE(j.at("risk_config").contains(retired)) << retired;
    }
    for (const char* elsewhere : {"sizing_capital", "idm", "statistics_K", "starting_capital"}) {
        EXPECT_FALSE(j.contains(elsewhere)) << elsewhere;
    }

    // BASE records the same keys (its values are CONSERVATIVE's).
    auto base = ConfigLoader::load(tmpl.parent_path(), "base");
    ASSERT_TRUE(base.is_ok()) << (base.is_error() ? base.error()->what() : "");
    const nlohmann::json b = runner_config(base.value(), "TREND_FOLLOWING", 0.20).to_json();
    EXPECT_EQ(keys_of(b), keys_of(j));
    EXPECT_EQ(b.at("opt_config").dump(), j.at("opt_config").dump());
    EXPECT_EQ(b.at("risk_config").dump(), j.at("risk_config").dump());
    EXPECT_EQ(b.at("equity_slow_rule").dump(), j.at("equity_slow_rule").dump());
}

// A book with no overlay sleeve (the equity book) writes the object it always wrote: the same
// keys, the optimiser's and the gate's old ones included, and none of the loop's.
TEST(PortfolioConfigKeys, ABookWithNoOverlaySleeveWritesTheObjectItAlwaysWrote) {
    PortfolioConfig pc{100000.0, 1.0, 0.0, false};
    pc.allow_fractional_positions = true;
    const nlohmann::json j = pc.to_json();
    EXPECT_EQ(keys_of(j), (std::vector<std::string>{
                              "allow_fractional_positions", "max_strategy_allocation",
                              "min_strategy_allocation", "opt_config", "risk_config", "total_capital",
                              "use_optimization", "version"}));
    EXPECT_EQ(keys_of(j.at("opt_config")),
              (std::vector<std::string>{"asymmetric_risk_buffer", "buffer_size_factor", "capital",
                                        "convergence_threshold", "cost_penalty_scalar", "max_iterations",
                                        "tau", "use_buffering", "version"}));
    EXPECT_EQ(keys_of(j.at("risk_config")),
              (std::vector<std::string>{"capital", "confidence_level", "jump_risk_limit",
                                        "lookback_period", "max_correlation", "max_gross_leverage",
                                        "max_net_leverage", "var_limit", "version"}));
}

// Both live runners write that object as live_run_metadata.portfolio_config (their marks are
// added to it), and both backtest runners write it as backtest.run_metadata.portfolio_config.
TEST(PortfolioConfigKeys, TheFourFuturesRunnersWriteTheObject) {
    for (const char* runner : {"apps/strategies/live_portfolio_conservative.cpp",
                               "apps/strategies/live_portfolio.cpp",
                               "apps/backtest/bt_portfolio_conservative.cpp",
                               "apps/backtest/bt_portfolio.cpp"}) {
        const auto path = repo_file(runner);
        ASSERT_FALSE(path.empty()) << runner;
        const std::string src = read_all(path);
        EXPECT_NE(src.find("nlohmann::json portfolio_config_json = portfolio_config.to_json();"),
                  std::string::npos)
            << runner;
        EXPECT_EQ(src.find("portfolio_config_json[\"total_capital\"] ="), std::string::npos) << runner;
        EXPECT_NE(src.find("apply_loop_config(app_config, portfolio_config);"), std::string::npos) << runner;
    }
}
