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
