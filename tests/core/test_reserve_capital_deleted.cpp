// J3 (OPT-N4): the reserve-capital knob is deleted. Nothing ever sized on it: the last
// reader went on 2026-02-11 (d833f189), and the figure was only written to config, one log
// line and the run-metadata JSON. HD ruled delete, history rows left as they are.
//
// These tests pin the deletion and compile against the parent source as well, so each one
// is shown RED there:
//   - PortfolioConfig::to_json() (stored verbatim in backtest.run_metadata.portfolio_config
//     and inside .hyperparameters) carries no "reserve_capital";
//   - a stored object that still has the key (every row before the cut-over) still parses;
//   - a portfolio.json that still carries "reserve_capital_pct" keeps loading (production
//     configs must still start), even with a value the old [0, 1) check refused, and the run
//     logs ONE WARN naming the key as deleted;
//   - the config summary line no longer prints reserve_pct=, and AppConfig::to_json() no
//     longer emits the key;
//   - no runner or backtest app writes "reserve_capital" into run metadata, and no template
//     portfolio.json carries the key.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "../core/test_base.hpp"
#include "trade_ngin/core/config_loader.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

constexpr auto npos = std::string::npos;

size_t count_of(const std::string& text, const std::string& needle) {
    size_t n = 0;
    for (size_t at = text.find(needle); at != npos; at = text.find(needle, at + 1)) ++n;
    return n;
}

std::filesystem::path find_repo_file(const std::string& relative) {
    namespace fs = std::filesystem;
    fs::path dir = fs::current_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (fs::exists(dir / relative)) return dir / relative;
        dir = dir.parent_path();
    }
    return {};
}

std::string read_repo_text(const std::string& relative) {
    auto path = find_repo_file(relative);
    if (path.empty()) return {};
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void write_json(const std::filesystem::path& p, const nlohmann::json& j) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p);
    f << j.dump(2);
}

// The smallest config set the schema-2 loader accepts (the same shape as
// test_config_loader.cpp's fixture).
void write_config_set(const std::filesystem::path& base, const nlohmann::json& portfolio_extra) {
    write_json(base / "defaults.json",
               {{"database", {{"host", "h"}, {"port", "5432"}, {"username", "u"},
                              {"password", "p"}, {"name", "n"}, {"num_connections", 5}}},
                {"execution", {{"commission_rate", 0.0005}, {"slippage_bps", 1.0},
                               {"position_limit_backtest", 1000.0},
                               {"position_limit_live", 500.0}}},
                {"optimization", {{"tau", 1.0}, {"capital", 500000.0},
                                  {"cost_penalty_scalar", 50}, {"asymmetric_risk_buffer", 0.1},
                                  {"max_iterations", 100}, {"convergence_threshold", 1e-6},
                                  {"use_buffering", true}, {"buffer_size_factor", 0.05}}},
                {"backtest", {{"lookback_years", 2}, {"store_trade_details", true}}},
                {"live", {{"historical_days", 300}}},
                {"strategy_defaults", {{"max_strategy_allocation", 1.0},
                                       {"min_strategy_allocation", 0.1},
                                       {"fdm", nlohmann::json::array({{1, 1.0}, {2, 1.03}})}}}});
    nlohmann::json portfolio = {
        {"portfolio_id", "J3_TEST_PORTFOLIO"},
        {"initial_capital", 1'000'000.0},
        {"use_optimization", true},
        {"strategies", {{"TREND_FOLLOWING", {{"weight", 1.0}, {"allocation", 1.0}}}}},
    };
    for (auto& [k, v] : portfolio_extra.items()) portfolio[k] = v;
    write_json(base / "portfolios" / "j3" / "portfolio.json", portfolio);
    nlohmann::json carver = {
        {"id", "carver"},          {"type", "carver"},
        {"var_limit", 0.15},       {"jump_risk_limit", 0.10},
        {"max_correlation", 0.7},  {"max_gross_leverage", 4.0},
        {"max_net_leverage", 2.0}, {"confidence_level", 0.99},
        {"lookback_period", 252},  {"lookback_unit", "dates"},
        {"min_gate_dates", 21},    {"missing_symbol_policy", "ignore"},
        {"_missing_symbol_policy_reason", "unit test"},
    };
    nlohmann::json reporting = {
        {"type", "carver"},        {"window", "all_bars"},
        {"var_limit", 0.15},       {"jump_risk_limit", 0.10},
        {"max_correlation", 0.7},  {"max_gross_leverage", 4.0},
        {"max_net_leverage", 2.0}, {"confidence_level", 0.99},
        {"lookback_period", 252},
    };
    write_json(base / "portfolios" / "j3" / "risk.json",
               {{"schema", 2},
                {"modules", nlohmann::json::array({carver})},
                {"risk_reporting", reporting},
                {"max_drawdown", 0.4},
                {"max_leverage", 4.0}});
    write_json(base / "portfolios" / "j3" / "email.json",
               {{"smtp_host", "smtp.test.com"},
                {"smtp_port", 587},
                {"username", "u"},
                {"password", "p"},
                {"from_email", "f@test.com"},
                {"to_emails", nlohmann::json::array({"a@test.com"})}});
}

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) out.push_back(line);
    return out;
}

const std::string kWarnNeedle = "\" was deleted by J3 and is no longer read";

}  // namespace

// =============================================================================================
// The stored JSON: backtest.run_metadata.portfolio_config (and hyperparameters.portfolio_config)
// is PortfolioConfig::to_json() verbatim.
// =============================================================================================

TEST(ReserveCapitalDeleted, PortfolioConfigToJsonWritesNoReserveKey) {
    PortfolioConfig c;
    c.total_capital = Decimal(500000.0);
    const auto j = c.to_json();
    EXPECT_FALSE(j.contains("reserve_capital"))
        << "PortfolioConfig::to_json() still writes reserve_capital; it is stored verbatim in "
           "backtest.run_metadata.portfolio_config and .hyperparameters: "
        << j.dump();
    EXPECT_DOUBLE_EQ(j.at("total_capital").get<double>(), 500000.0);
}

TEST(ReserveCapitalDeleted, AStoredObjectThatStillCarriesTheKeyParsesAndIsNotWrittenBack) {
    // Every run_metadata row written before the cut-over carries the key (history is left).
    const nlohmann::json historical = {{"total_capital", 500000.0},
                                       {"reserve_capital", 50000.0},
                                       {"max_strategy_allocation", 1.0},
                                       {"min_strategy_allocation", 0.0},
                                       {"use_optimization", true}};
    PortfolioConfig c;
    ASSERT_NO_THROW(c.from_json(historical));
    EXPECT_DOUBLE_EQ(static_cast<double>(c.total_capital), 500000.0);
    EXPECT_TRUE(c.use_optimization);
    EXPECT_FALSE(c.to_json().contains("reserve_capital"))
        << "a historical reserve_capital read back from a stored row is written out again";
}

// =============================================================================================
// The loader: a deployed portfolio.json that still carries the key keeps loading and says so.
// =============================================================================================

class ReserveCapitalDeletedLoader : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        base_ = std::filesystem::temp_directory_path() /
                ("trade_ngin_j3_" + std::to_string(::getpid()) + "_" +
                 ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(base_);
        LoggerConfig lc;
        lc.destination = LogDestination::CONSOLE;
        lc.min_level = LogLevel::INFO;
        lc.include_timestamp = false;
        Logger::instance().initialize(lc);
    }
    void TearDown() override {
        std::filesystem::remove_all(base_);
        TestBase::TearDown();
    }

    /// Loads the set and returns (error text or "", captured log lines).
    std::pair<std::string, std::vector<std::string>> load(const nlohmann::json& portfolio_extra,
                                                          AppConfig* out = nullptr) {
        write_config_set(base_, portfolio_extra);
        ::testing::internal::CaptureStdout();
        auto r = ConfigLoader::load(base_, "j3");
        auto lines = lines_of(::testing::internal::GetCapturedStdout());
        if (r.is_error()) return {r.error()->what(), lines};
        if (out) *out = r.value();
        return {"", lines};
    }

    std::filesystem::path base_;
};

TEST_F(ReserveCapitalDeletedLoader, ALeftoverKeyStillLoadsIsIgnoredAndWarnsOnce) {
    // 1.5 is outside the deleted [0, 1) check: the parent source REFUSED this config.
    const auto [error, lines] = load({{"reserve_capital_pct", 1.5}});
    ASSERT_EQ(error, "") << "a portfolio.json still carrying reserve_capital_pct must load "
                            "(production configs keep the key until an operator deletes it)";
    size_t warns = 0;
    for (const auto& l : lines) {
        if (l.find("\"reserve_capital_pct" + kWarnNeedle) != npos) {
            ++warns;
            EXPECT_NE(l.find("[WARNING]"), npos) << l;
            EXPECT_NE(l.find("config for J3_TEST_PORTFOLIO: "), npos) << l;
        }
    }
    EXPECT_EQ(warns, 1u) << "the run must say ONCE that the key is deleted and ignored";
}

TEST_F(ReserveCapitalDeletedLoader, TheMetadataKeyNameInAConfigIsAlsoNamedOnce) {
    const auto [error, lines] = load({{"reserve_capital", 50000.0}});
    ASSERT_EQ(error, "");
    size_t warns = 0;
    for (const auto& l : lines) warns += count_of(l, "\"reserve_capital" + kWarnNeedle);
    EXPECT_EQ(warns, 1u);
}

TEST_F(ReserveCapitalDeletedLoader, AConfigWithoutTheKeyLoadsWithNoWarning) {
    const auto [error, lines] = load(nlohmann::json::object());
    ASSERT_EQ(error, "");
    for (const auto& l : lines) EXPECT_EQ(l.find(kWarnNeedle), npos) << l;
}

TEST_F(ReserveCapitalDeletedLoader, TheConfigSummaryAndAppConfigJsonNoLongerCarryIt) {
    AppConfig c;
    const auto [error, lines] = load({{"reserve_capital_pct", 0.1}}, &c);
    ASSERT_EQ(error, "");
    size_t summaries = 0;
    for (const auto& l : lines) {
        EXPECT_EQ(l.find("reserve_pct="), npos) << l;
        if (l.find("Config summary: portfolio_id=J3_TEST_PORTFOLIO") != npos) {
            ++summaries;
            EXPECT_NE(l.find("Config summary: portfolio_id=J3_TEST_PORTFOLIO, "
                             "initial_capital=1000000.000000"),
                      npos)
                << l;
            EXPECT_EQ(l.size() - l.find("Config summary:"),
                      std::string("Config summary: portfolio_id=J3_TEST_PORTFOLIO, "
                                  "initial_capital=1000000.000000")
                          .size())
                << "the summary line ends at initial_capital: " << l;
        }
    }
    EXPECT_EQ(summaries, 1u);
    EXPECT_FALSE(c.to_json().contains("reserve_capital_pct"));
}

// =============================================================================================
// The writers: no runner or backtest app writes the key; no template carries it.
// =============================================================================================

TEST(ReserveCapitalDeletedSource, NoRunnerOrBacktestAppWritesTheKey) {
    for (const char* app : {"apps/strategies/live_portfolio_conservative.cpp",
                            "apps/strategies/live_portfolio.cpp",
                            "apps/strategies/live_equity_mean_reversion.cpp",
                            "apps/backtest/bt_portfolio_conservative.cpp",
                            "apps/backtest/bt_portfolio.cpp",
                            "apps/backtest/bt_equity_mean_reversion.cpp",
                            "apps/backtest/bt_equity_validation.cpp"}) {
        SCOPED_TRACE(app);
        const std::string src = read_repo_text(app);
        if (src.empty()) GTEST_SKIP() << "app source not found from the test working directory";
        EXPECT_EQ(count_of(src, "reserve_capital"), 0u)
            << "the app still computes or stores the reserve (live_run_metadata.portfolio_config "
               "on a runner)";
    }
}

TEST(ReserveCapitalDeletedSource, NoTemplatePortfolioCarriesTheKey) {
    for (const char* file : {"config_template/portfolios/base/portfolio.json",
                             "config_template/portfolios/conservative/portfolio.json",
                             "config_template/portfolios/equity_mr/portfolio.json",
                             "config_template/examples/risk_modules/sleeve_assignment.portfolio.json"}) {
        SCOPED_TRACE(file);
        const std::string text = read_repo_text(file);
        if (text.empty()) GTEST_SKIP() << "template not found from the test working directory";
        EXPECT_EQ(count_of(text, "reserve_capital"), 0u);
    }
}
