#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

// The live equity runner writes positions, executions and live results under the
// portfolio id its config declares. It previously passed the literal
// "BASE_PORTFOLIO" at every storage site while its config said
// EQUITY_MR_PORTFOLIO, so equity rows landed in the futures book's namespace.
//
// The runner is a main(), so it cannot be linked here. What is testable -- and
// what actually guards the fix -- is the config contract the runner now depends
// on: the equity portfolio must declare its own namespace, distinct from the
// futures book. If that ever drifts back, the collision returns.
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

std::string portfolio_id_of(const std::string& relative) {
    auto path = find_repo_file(relative);
    if (path.empty()) return {};
    std::ifstream in(path);
    nlohmann::json j = nlohmann::json::parse(in);
    return j.value("portfolio_id", std::string{});
}

}  // namespace

TEST(EquityPortfolioNamespace, EquityConfigDeclaresItsOwnPortfolioId) {
    const std::string equity_id =
        portfolio_id_of("config_template/portfolios/equity_mr/portfolio.json");
    if (equity_id.empty()) {
        GTEST_SKIP() << "config_template/portfolios/equity_mr/portfolio.json not reachable";
    }

    EXPECT_FALSE(equity_id.empty()) << "equity portfolio must declare a portfolio_id";
    EXPECT_NE(equity_id, "BASE_PORTFOLIO")
        << "equity storage must not share the futures book's namespace";
}

TEST(EquityPortfolioNamespace, EquityAndFuturesPortfolioIdsAreDistinct) {
    const std::string equity_id =
        portfolio_id_of("config_template/portfolios/equity_mr/portfolio.json");
    const std::string base_id =
        portfolio_id_of("config_template/portfolios/base/portfolio.json");
    if (equity_id.empty() || base_id.empty()) {
        GTEST_SKIP() << "config_template portfolio files not reachable";
    }

    EXPECT_NE(equity_id, base_id)
        << "equity and futures portfolios must write to separate namespaces; "
           "sharing one lets a live equity run overwrite futures positions";
}

// ──────────────────────────────────────────────────────────────────────────
// BA-5 / C-1 D3 -- real pins for 9b0d293c.
//
// The two tests above read config_template JSON only. They pass with the fix
// reverted, because reintroducing the "BASE_PORTFOLIO" literal at the runner's
// storage sites does not change any config file. What follows pins the two
// things that can actually regress: the loader must REFUSE a config with no
// portfolio_id (rather than defaulting to the futures book), and the equity
// runner must not carry the literal at all.
// ──────────────────────────────────────────────────────────────────────────

// Linkable pin. validate_config is reached via the `#define private public`
// technique this suite already uses for ConfigLoader internals.
#include <map>
#include <string>
#include <vector>
#define private public
#include "trade_ngin/core/config_loader.hpp"
#undef private

using namespace trade_ngin;

namespace {

AppConfig config_with_portfolio_id(const std::string& id) {
    AppConfig c;
    c.portfolio_id = id;
    c.database.host = "h";
    c.database.username = "u";
    c.database.password = "p";
    c.database.name = "n";
    c.initial_capital = 500000.0;
    c.reserve_capital_pct = 0.10;
    c.strategies_config = nlohmann::json{{"equity_mean_reversion", {{"enabled", true}}}};
    return c;
}

std::string read_repo_text(const std::string& relative) {
    auto path = find_repo_file(relative);
    if (path.empty()) return {};
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

// An absent portfolio_id must be an ERROR, never a silent fall back to the
// futures namespace. This is the half of 9b0d293c that links.
TEST(EquityPortfolioNamespace, MissingPortfolioIdIsRejectedRatherThanDefaulted) {
    auto ok = ConfigLoader::validate_config(config_with_portfolio_id("EQUITY_MR_PORTFOLIO"));
    EXPECT_TRUE(ok.is_ok()) << "a declared portfolio id must validate";

    auto missing = ConfigLoader::validate_config(config_with_portfolio_id(""));
    ASSERT_TRUE(missing.is_error())
        << "an empty portfolio_id must fail validation; defaulting it is how equity rows "
           "landed in the futures book's namespace";
    EXPECT_EQ(missing.error()->code(), ErrorCode::INVALID_DATA);
}

// Source-level pin, because apps/ is not linked into this binary (C-1 F7) and
// the 10 storage sites live in main(). The literal is what regressed; its
// absence from the file is therefore the thing to assert. Comments are stripped
// first so the file may still EXPLAIN the defect it no longer contains.
TEST(EquityPortfolioNamespace, EquityRunnerCarriesNoBasePortfolioLiteral) {
    const std::string src = read_repo_text("apps/strategies/live_equity_mean_reversion.cpp");
    if (src.empty()) {
        GTEST_SKIP() << "apps/strategies/live_equity_mean_reversion.cpp not reachable";
    }

    // Strip // line comments so a comment mentioning BASE_PORTFOLIO does not trip this.
    std::string code;
    code.reserve(src.size());
    for (size_t i = 0; i < src.size();) {
        if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '/') {
            while (i < src.size() && src[i] != '\n') ++i;
        } else {
            code += src[i++];
        }
    }

    // The literal as it would appear in a storage call.
    EXPECT_EQ(code.find("\"BASE_PORTFOLIO\""), std::string::npos)
        << "the equity runner must pass its config's portfolio_id to every storage site; "
           "a \"BASE_PORTFOLIO\" literal here writes equity rows into the futures book";

    // And it must actually thread the config value into the coordinator, which is the
    // single place the old default lived.
    EXPECT_NE(code.find("coordinator_config.portfolio_id = portfolio_id;"), std::string::npos)
        << "the coordinator must be given the equity portfolio id explicitly";
}
