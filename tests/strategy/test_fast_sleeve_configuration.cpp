// The FAST sleeve is a configuration of the one trend class (LOOP_SPEC v6.2 section 2.5, D21):
//   - TrendFollowingStrategy on the four fast EMA pairs (2,8), (4,16), (8,32), (16,64); the (1,4)
//     pair is dropped;
//   - no separate fast or slow trend class exists, in the library or in a runner's factory;
//   - the shipped BASE book configures its FAST sleeve on those four pairs.
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "trade_ngin/strategy/trend_following.hpp"

using namespace trade_ngin;

namespace {

// The checkout this test runs in: the nearest directory at or above the working directory that holds
// the build file and the config template. Every path below is resolved inside it and nowhere else (a
// git worktree sits inside another checkout, whose files are not this tree's).
std::filesystem::path repo_root() {
    namespace fs = std::filesystem;
    fs::path dir = fs::current_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (fs::exists(dir / "CMakeLists.txt") && fs::exists(dir / "config_template")) return dir;
        if (dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
    return {};
}

std::filesystem::path repo_path(const std::string& relative) {
    const auto root = repo_root();
    if (root.empty() || !std::filesystem::exists(root / relative)) return {};
    return root / relative;
}

std::string read_all(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

size_t count_of(const std::string& text, const std::string& needle) {
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
    return n;
}

const std::vector<std::pair<int, int>> kFastPairs = {{2, 8}, {4, 16}, {8, 32}, {16, 64}};

}  // namespace

TEST(FastSleeveConfiguration, TheFastConfigurationIsTheFourFastPairs) {
    const TrendFollowingConfig fast = fast_trend_following_config();
    EXPECT_EQ(fast.ema_windows, kFastPairs) << "the (1,4) pair is dropped from FAST (D21)";
    EXPECT_EQ(fast.vol_lookback_short, 16);
    EXPECT_DOUBLE_EQ(fast.risk_target, 0.25);
    // Everything else is the trend class's own default.
    const TrendFollowingConfig trend;
    EXPECT_DOUBLE_EQ(fast.idm, trend.idm);
    EXPECT_EQ(fast.vol_lookback_long, trend.vol_lookback_long);
    EXPECT_EQ(fast.fdm, trend.fdm);
}

TEST(FastSleeveConfiguration, TheShippedBaseBookRunsFastOnTheFourPairs) {
    const auto path = repo_path("config_template/portfolios/base/portfolio.json");
    if (path.empty()) GTEST_SKIP() << "config_template not found from " << std::filesystem::current_path();
    const auto book = nlohmann::json::parse(read_all(path));
    const auto& fast = book.at("strategies").at("TREND_FOLLOWING_FAST");
    std::vector<std::pair<int, int>> pairs;
    for (const auto& w : fast.at("config").at("ema_windows")) {
        pairs.emplace_back(w.at(0).get<int>(), w.at(1).get<int>());
    }
    EXPECT_EQ(pairs, kFastPairs) << "the template's FAST sleeve still lists a pair D21 dropped";
}

TEST(FastSleeveConfiguration, NoSeparateFastOrSlowTrendClassExists) {
    if (repo_root().empty()) GTEST_SKIP() << "repository root not found from " << std::filesystem::current_path();
    for (const auto* gone :
         {"src/strategy/trend_following_fast.cpp", "src/strategy/trend_following_slow.cpp",
          "include/trade_ngin/strategy/trend_following_fast.hpp",
          "include/trade_ngin/strategy/trend_following_slow.hpp"}) {
        EXPECT_TRUE(repo_path(gone).empty()) << gone << " still exists";
    }
    const auto exporter = repo_path("src/live/csv_exporter.cpp");
    const auto coordinator = repo_path("src/backtest/backtest_coordinator.cpp");
    if (exporter.empty() || coordinator.empty()) GTEST_SKIP() << "library sources not found";
    for (const auto& path : {exporter, coordinator}) {
        const std::string src = read_all(path);
        EXPECT_EQ(src.find("TrendFollowingSlowStrategy"), std::string::npos) << path;
        EXPECT_EQ(src.find("TrendFollowingFastStrategy"), std::string::npos) << path;
    }
}

TEST(FastSleeveConfiguration, EveryRunnerBuildsBothSleevesFromTheOneTrendClass) {
    for (const auto* runner :
         {"apps/strategies/live_portfolio.cpp", "apps/strategies/live_portfolio_conservative.cpp",
          "apps/backtest/bt_portfolio.cpp", "apps/backtest/bt_portfolio_conservative.cpp"}) {
        const auto path = repo_path(runner);
        if (path.empty()) GTEST_SKIP() << "runner source not found: " << runner;
        const std::string src = read_all(path);
        // The factory still accepts the configured type name of the FAST sleeve...
        EXPECT_EQ(count_of(src, "strategy_type == \"TrendFollowingFastStrategy\""), 1u) << runner;
        // ...and builds it, like the TREND sleeve, as a TrendFollowingStrategy.
        EXPECT_EQ(count_of(src, "fast_trend_following_config()"), 1u) << runner;
        EXPECT_EQ(count_of(src, "make_shared<trade_ngin::TrendFollowingStrategy>("), 2u) << runner;
        EXPECT_EQ(src.find("TrendFollowingFastStrategy>"), std::string::npos) << runner;
        EXPECT_EQ(src.find("TrendFollowingFastConfig"), std::string::npos) << runner;
        EXPECT_EQ(src.find("TrendFollowingSlow"), std::string::npos) << runner;
    }
}
