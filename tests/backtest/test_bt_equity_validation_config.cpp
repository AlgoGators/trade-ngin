// tests/backtest/test_bt_equity_validation_config.cpp
//
// T2.6 residue + H4 -- the validation harness must validate the strategy the CONFIG
// describes, not one it has hard-coded.
//
// `bt_equity_validation` recomputes the framework's SMA, standard deviation, volatility and
// z-score by hand and reports PASS or FAIL. It was doing that against nine parameter literals
// (`lookback_period = 20`, `entry_threshold = 2.0`, …) and a ten-name candidate ticker list,
// while every other equity app read `strategies_config`. The literals equalled the config
// value for value -- the candidate list is character for character the template's `symbols`
// array -- which is precisely what made it dangerous: change `lookback_period` in the
// gitignored config and the validator would have kept recomputing 20, agreed with itself, and
// printed PASS while the strategy under test used the new number.
//
// The app is a `main()` and cannot be linked here, so this pins the two halves that are
// testable: the builder's answer on the real tracked config (the numbers the app now uses),
// and the absence of the literals from the app's source (the approach
// tests/core/test_equity_portfolio_namespace.cpp and tests/live/test_day_t_write_ordering.cpp
// take for the same reason).

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "trade_ngin/strategy/equity_strategy_builder.hpp"

using namespace trade_ngin;

namespace {

std::filesystem::path find_repo_file(const std::string& relative) {
    namespace fs = std::filesystem;
    fs::path dir = fs::current_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (fs::exists(dir / relative)) return dir / relative;
        dir = dir.parent_path();
    }
    return {};
}

std::string read_repo_file(const std::string& relative) {
    auto path = find_repo_file(relative);
    if (path.empty()) return {};
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

TEST(BtEquityValidationConfig, TheBuilderReproducesEveryLiteralTheAppUsedToCarry) {
    const std::string text =
        read_repo_file("config_template/portfolios/equity_mr/portfolio.json");
    if (text.empty()) GTEST_SKIP() << "config template not found from the test working directory";
    const auto j = nlohmann::json::parse(text);

    auto entries = apps::collect_enabled_equity_strategies(j.at("strategies"), "enabled_backtest");
    ASSERT_TRUE(entries.is_ok()) << entries.error()->what();
    ASSERT_FALSE(entries.value().empty());
    const auto& entry = entries.value().front();
    EXPECT_EQ(entry.type, "MeanReversionStrategy");

    const auto mr = apps::build_mean_reversion_config(entry.def.at("config"));

    // The nine literals that stood in bt_equity_validation.cpp, in the order they stood in.
    EXPECT_EQ(mr.lookback_period, 20);
    EXPECT_DOUBLE_EQ(mr.entry_threshold, 2.0);
    EXPECT_DOUBLE_EQ(mr.exit_threshold, 0.5);
    EXPECT_DOUBLE_EQ(mr.risk_target, 0.15);
    EXPECT_DOUBLE_EQ(mr.position_size, 0.1);
    EXPECT_EQ(mr.vol_lookback, 20);
    EXPECT_TRUE(mr.use_stop_loss);
    EXPECT_DOUBLE_EQ(mr.stop_loss_pct, 0.05);
    // These two were pinned `true` in the app's second config block regardless of the config.
    EXPECT_TRUE(mr.allow_fractional_shares);

    EXPECT_DOUBLE_EQ(j.at("initial_capital").get<double>(), 100000.0);
}

TEST(BtEquityValidationConfig, TheCandidateTickerListWasTheConfigsSymbolsAllAlong) {
    // H4. Not a coincidence worth preserving: the literal list in the app was
    // {"AAPL","MSFT","AMZN","GOOGL","META","TMUS","NSC","ABT","ABEV","ABM"} and the template's
    // `symbols` array is the same ten names in the same order. Sourcing them from the config
    // is therefore value-neutral on the template AND on the runtime config, whose first three
    // names are also AAPL, MSFT, AMZN -- the app takes the first three with enough bars.
    const std::string text =
        read_repo_file("config_template/portfolios/equity_mr/portfolio.json");
    if (text.empty()) GTEST_SKIP() << "config template not found from the test working directory";
    const auto j = nlohmann::json::parse(text);
    const auto symbols =
        j.at("strategies").at("MEAN_REVERSION").at("symbols").get<std::vector<std::string>>();

    const std::vector<std::string> old_hardcoded_list = {
        "AAPL", "MSFT", "AMZN", "GOOGL", "META", "TMUS", "NSC", "ABT", "ABEV", "ABM"};
    EXPECT_EQ(symbols, old_hardcoded_list);
}

TEST(BtEquityValidationConfig, TheAppNoLongerCarriesTheLiterals) {
    const std::string src = read_repo_file("apps/backtest/bt_equity_validation.cpp");
    if (src.empty()) GTEST_SKIP() << "app source not found from the test working directory";

    // It reads the config through the same two entry points every other equity app uses.
    EXPECT_NE(src.find("collect_enabled_equity_strategies(\n            app_config.strategies_config, \"enabled_backtest\")"),
              std::string::npos)
        << "the validator must collect its strategy from strategies_config";
    EXPECT_NE(src.find("apps::build_mean_reversion_config(validation_entry->def[\"config\"])"),
              std::string::npos)
        << "and build its parameters with the shared builder";

    // And the literals are gone. Each of these is the exact line that stood in the app.
    for (const auto& gone : {"int lookback_period = 20;",
                             "double entry_threshold = 2.0;",
                             "double exit_threshold = 0.5;",
                             "double risk_target = 0.15;",
                             "double position_size_pct = 0.1;",
                             "int vol_lookback = 20;",
                             "double stop_loss_pct = 0.05;",
                             "double initial_capital = 100000.0;",
                             "double position_limit = 1000.0;",
                             "mr_config.use_stop_loss = true;",
                             "mr_config.allow_fractional_shares = true;",
                             "candidate_symbols = {\"AAPL\""}) {
        EXPECT_EQ(src.find(gone), std::string::npos)
            << "bt_equity_validation.cpp has re-acquired the literal: " << gone;
    }

    // The parameters are PRINTED, which is how a config change is verified in the app's own
    // output (the E3 audit's stated verification for T2.6).
    EXPECT_NE(src.find("Strategy parameters (from config, strategy '"), std::string::npos);
}
