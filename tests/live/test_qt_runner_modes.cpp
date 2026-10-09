// QT runner modes on both futures runners (docs/design/qt-contract.md). The behaviour is checked
// against a database by scripts/qt_runner_check.sh; these pin the wiring in the source, as the
// other twin-runner tests do.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "trade_ngin/live/qt_desk.hpp"

namespace {
namespace fs = std::filesystem;
constexpr auto npos = std::string::npos;
const char* kRunners[] = {"apps/strategies/live_portfolio_conservative.cpp",
                          "apps/strategies/live_portfolio.cpp"};

std::string read_source(const std::string& relative) {
    fs::path dir = fs::current_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (fs::exists(dir / relative)) {
            std::ifstream in(dir / relative);
            std::ostringstream ss;
            ss << in.rdbuf();
            return ss.str();
        }
        dir = dir.parent_path();
    }
    return std::string();
}
}  // namespace

TEST(QtRunnerModes, ModeNames) {
    using trade_ngin::qt::Mode;
    EXPECT_EQ(trade_ngin::qt::mode_name(Mode::FINALIZE_SYSTEM), "finalize-system");
    EXPECT_EQ(trade_ngin::qt::mode_name(Mode::PUBLISH), "publish");
}

// Ruling 17: each book is finalised into its own rows. The model run of an editable portfolio
// starts from qt and runs --finalize-system first, which reads system and writes Day T-1 only.
TEST(QtRunnerModes, TheSystemBooksDayTMinusOneIsFinalisedOnAnEditablePortfolio) {
    for (const char* runner : kRunners) {
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found";
        EXPECT_NE(src.find("if (qt_mode == qt::Mode::MODEL && qt_read_book == \"qt\") {\n"
                           "            const int rc = qt::run_finalize_system("),
                  npos)
            << runner;
        EXPECT_NE(src.find("qt_mode == qt::Mode::MODEL || qt_mode == qt::Mode::FINALIZE_SYSTEM;"),
                  npos)
            << runner << ": --finalize-system writes Day T-1";
        EXPECT_NE(src.find("qt_mode != qt::Mode::PUBLISH && qt_mode != qt::Mode::FINALIZE_SYSTEM;"),
                  npos)
            << runner << ": --finalize-system writes no Day T row";
        EXPECT_NE(src.find("if (qt_desk_editable && qt_mode != qt::Mode::FINALIZE_SYSTEM) {"), npos)
            << runner << ": --finalize-system reads the system book";
    }
}

// Ruling 29: a non-trading day is published by the model run itself, with no e-mail.
TEST(QtRunnerModes, ANonTradingDayIsPublishedByTheModelRun) {
    for (const char* runner : kRunners) {
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found";
        const auto rule = src.find("const bool qt_non_trading_day = skip_strategy_processing ||");
        ASSERT_NE(rule, npos) << runner;
        EXPECT_NE(src.find("\"system:non-trading-day\"", rule), npos) << runner;
        EXPECT_NE(src.find("if (qt_desk_editable && qt_mode == qt::Mode::MODEL) send_email = false;"),
                  npos)
            << runner << ": ruling 15";
    }
}

// Ruling 16: a close is stored as a zero-quantity row in qt.
TEST(QtRunnerModes, ACloseIsStoredAsAZeroQuantityRow) {
    for (const char* runner : kRunners) {
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found";
        EXPECT_NE(src.find("if (!has_quantity && qt_zero_keep.count(strategy_name + \"|\" + symbol) == 0) {"),
                  npos)
            << runner;
    }
}
