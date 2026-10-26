// Carry in the trend strategy (trade-ngin #185): the hand-over from portfolio.json to the first
// sleeve, the runners' calls, and the sleeve's own checks of its carry settings.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "trade_ngin/strategy/sleeve_config.hpp"
#include "trade_ngin/strategy/trend_following.hpp"

using namespace trade_ngin;

TEST(CarryConfig, TheHandOverCarriesTheBlockOnlyWhenCarryIsOn) {
    AppConfig app;
    app.carry.present = true;
    app.carry.weight = 0.4;
    app.carry.spans = {20, 60, 120};
    app.carry.scalar = 25.0;
    TrendFollowingConfig handed;
    hand_over_carry(app, handed);
    EXPECT_EQ(handed.carry_weight, 0.4);
    EXPECT_EQ(handed.carry_spans, (std::vector<int>{20, 60, 120}));
    EXPECT_EQ(handed.carry_scalar, 25.0);
    // weight 0 and an absent block hand over carry off
    app.carry.weight = 0.0;
    TrendFollowingConfig off;
    off.carry_weight = 0.4;
    hand_over_carry(app, off);
    EXPECT_EQ(off.carry_weight, 0.0);
    TrendFollowingConfig absent;
    hand_over_carry(AppConfig{}, absent);
    EXPECT_EQ(absent.carry_weight, 0.0);
    EXPECT_EQ(absent.carry_spans, (std::vector<int>{5, 20, 60, 120}));
}

// Every futures runner hands the block to its first trend sleeve, once, beside the trading-rule
// removals, and tells the bar source to carry the quotes when carry is on.
TEST(CarryConfig, EveryFuturesRunnerCallsTheHandOverAndTurnsTheQuotesOn) {
    std::filesystem::path root = std::filesystem::current_path();
    while (!(std::filesystem::exists(root / "CMakeLists.txt") &&
             std::filesystem::exists(root / "config_template"))) {
        ASSERT_NE(root, root.parent_path()) << "repository root not found";
        root = root.parent_path();
    }
    auto count = [](const std::string& text, const std::string& what) {
        std::size_t n = 0;
        for (std::size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++n;
        return n;
    };
    for (const char* runner : {"apps/backtest/bt_portfolio.cpp", "apps/backtest/bt_portfolio_conservative.cpp",
                               "apps/strategies/live_portfolio.cpp",
                               "apps/strategies/live_portfolio_conservative.cpp"}) {
        std::ifstream in(root / runner);
        ASSERT_TRUE(in.good()) << runner;
        std::stringstream text;
        text << in.rdbuf();
        const std::string source = text.str();
        EXPECT_EQ(count(source, "hand_over_carry(app_config, trend_config);"), 1u) << runner;
        EXPECT_EQ(count(source, "carry_weight ="), 0u) << runner << " sets the weight itself";
        const std::size_t removals = source.find("hand_over_trading_rule_removals(app_config, trend_config);");
        const std::size_t carry = source.find("hand_over_carry(app_config, trend_config);");
        const std::size_t fast = source.find("strategy_type == \"TrendFollowingFastStrategy\"");
        ASSERT_NE(removals, std::string::npos) << runner;
        EXPECT_LT(removals, carry) << runner;
        EXPECT_LT(carry, fast) << runner;
        EXPECT_EQ(count(source, "app_config.per_contract_rolls, app_config.per_contract_switch_bar,\n"
                                "                app_config.carry.enabled());"),
                  1u)
            << runner;
    }
}

TEST(CarryConfig, TheSleeveRefusesCarrySettingsItCannotRun) {
    TrendFollowingConfig ok;
    ok.carry_weight = 0.4;
    EXPECT_TRUE(TrendFollowingStrategy::validate_trend_config(ok).is_ok());
    ok.rule_removals = {{"ZR", {{2, 8}, {4, 16}, {8, 32}}}};
    EXPECT_TRUE(TrendFollowingStrategy::validate_trend_config(ok).is_ok()) << "7 rules: 1.29";
    TrendFollowingConfig off;  // the default is off and always valid
    EXPECT_EQ(off.carry_weight, 0.0);
    EXPECT_TRUE(TrendFollowingStrategy::validate_trend_config(off).is_ok());
    auto refused = [](TrendFollowingConfig c, const std::string& named) {
        auto r = TrendFollowingStrategy::validate_trend_config(c);
        ASSERT_TRUE(r.is_error());
        EXPECT_NE(std::string(r.error()->what()).find(named), std::string::npos) << r.error()->what();
    };
    TrendFollowingConfig c;
    c.carry_weight = 1.2;
    refused(c, "from 0 to 1");
    c.carry_weight = -0.1;
    refused(c, "from 0 to 1");
    c.carry_weight = 0.4;
    c.carry_spans = {};
    refused(c, "no span");
    c.carry_spans = {20, 5};
    refused(c, "fastest first");
    c.carry_spans = {5, 20};
    c.carry_scalar = 0.0;
    refused(c, "scalar");
    // 20 trend pairs are not supported anyway; a rule count table 52 does not print is refused:
    // six trend speeds and 17 carry spans are 23 rules.
    c.carry_scalar = 30.0;
    c.carry_spans.clear();
    for (int s = 1; s <= 17; ++s) c.carry_spans.push_back(s * 5);
    refused(c, "no multiplier for 23 trading rules");
}
