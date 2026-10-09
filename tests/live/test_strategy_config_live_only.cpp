// QT plan E2, ruling 4: settings changes are live-only; backtests keep file config.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <sstream>
#include <string>

namespace {

std::filesystem::path repo_root() {
    namespace fs = std::filesystem;
    const fs::path from_source = fs::path(__FILE__).parent_path().parent_path().parent_path();
    if (fs::exists(from_source / "apps" / "backtest")) return from_source;
    fs::path dir = fs::current_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (fs::exists(dir / "apps" / "backtest")) return dir;
        dir = dir.parent_path();
    }
    return {};
}

std::string read_file(const std::filesystem::path& p) {
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

// ===== Ruling 4: settings changes are live-only =====

// A static check over the app sources: every backtest app loads its files with the
// two-argument ConfigLoader::load and never reads strategy_config; every live runner resolves its
// settings through resolve_live_settings and writes the settings used.
TEST(StrategyConfigLiveOnly, BacktestAppsNeverReadStrategyConfigAndLiveRunnersDo) {
    namespace fs = std::filesystem;
    const fs::path root = repo_root();
    ASSERT_FALSE(root.empty());
    const std::regex two_arg_load(R"(ConfigLoader::load\(\s*"[^"]*"\s*,\s*"[^"]*"\s*\))");
    const std::regex any_load(R"(ConfigLoader::load\()");
    int backtests = 0;
    for (const auto& entry : fs::directory_iterator(root / "apps" / "backtest")) {
        if (entry.path().extension() != ".cpp") continue;
        ++backtests;
        const std::string src = read_file(entry.path());
        const std::string name = entry.path().filename().string();
        for (const char* forbidden : {"trading.strategy_config", "resolve_live_settings",
                                      "get_active_strategy_config", "live_settings.hpp",
                                      "store_settings_used"}) {
            EXPECT_EQ(src.find(forbidden), std::string::npos)
                << name << " mentions " << forbidden << " (ruling 4: backtests keep file config)";
        }
        const auto loads = std::distance(std::sregex_iterator(src.begin(), src.end(), any_load),
                                         std::sregex_iterator());
        const auto plain = std::distance(std::sregex_iterator(src.begin(), src.end(), two_arg_load),
                                         std::sregex_iterator());
        EXPECT_EQ(loads, plain) << name << ": a ConfigLoader::load call that is not file-only";
    }
    EXPECT_GE(backtests, 4) << "the backtest apps were not found";
    for (const char* runner : {"live_portfolio.cpp", "live_portfolio_conservative.cpp",
                               "live_equity_mean_reversion.cpp"}) {
        const std::string src = read_file(root / "apps" / "strategies" / runner);
        EXPECT_NE(src.find("get_active_strategy_config("), std::string::npos) << runner;
        EXPECT_NE(src.find("resolve_live_settings("), std::string::npos) << runner;
        EXPECT_NE(src.find("store_settings_used("), std::string::npos) << runner;
    }
}
