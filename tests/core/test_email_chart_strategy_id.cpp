// tests/core/test_email_chart_strategy_id.cpp
//
// The charts of a futures book's daily email query the run's OWN stored rows. The per-strategy
// report body (the overload both futures runners call) passed the literal LIVE_TREND_FOLLOWING to
// every chart, which is CONSERVATIVE's strategy id: a book of two sleeves stores its rows under its
// combined id (BASE: LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST), so BASE's charts showed another
// book's rows.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "../data/test_db_utils.hpp"
#include "trade_ngin/core/email_sender.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

// Records every query the report body sends and answers none of them.
class RecordingDatabase : public MockPostgresDatabase {
public:
    RecordingDatabase() : MockPostgresDatabase("mock://recording") {}
    Result<std::shared_ptr<arrow::Table>> execute_query(const std::string& query) override {
        queries.push_back(query);
        return make_error<std::shared_ptr<arrow::Table>>(ErrorCode::DATABASE_ERROR, "recording only");
    }
    std::vector<std::string> queries;
};

std::string read_source(const std::string& relative) {
    namespace fs = std::filesystem;
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
    return {};
}

}  // namespace

TEST(EmailChartStrategyId, EveryChartOfThePerStrategyBodyQueriesTheIdItIsGiven) {
    const std::string base_id = "LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST";
    auto db = std::make_shared<RecordingDatabase>();
    EmailSender sender{EmailSenderConfig{}};
    (void)sender.generate_trading_report_body(
        StrategyPositionsMap{}, std::unordered_map<std::string, Position>{}, std::nullopt, {},
        StrategyExecutionsMap{}, "2026-04-28", "BASE_PORTFOLIO", true, {}, db, StrategyPositionsMap{},
        {}, {}, {}, base_id);

    int with_id = 0;
    for (const auto& q : db->queries) {
        if (q.find("strategy_id = '") == std::string::npos) continue;
        EXPECT_NE(q.find("strategy_id = '" + base_id + "'"), std::string::npos)
            << "a chart query names another book's strategy id: " << q;
        EXPECT_EQ(q.find("strategy_id = 'LIVE_TREND_FOLLOWING'"), std::string::npos) << q;
        ++with_id;
    }
    EXPECT_GE(with_id, 6) << "the six charts that read stored rows each sent their query";
}

TEST(EmailChartStrategyId, BothFuturesRunnersPassTheirOwnStrategyIdToTheCharts) {
    for (const char* runner :
         {"apps/strategies/live_portfolio_conservative.cpp", "apps/strategies/live_portfolio.cpp"}) {
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";
        const auto begin = src.find("email_sender->generate_trading_report_body(");
        ASSERT_NE(begin, std::string::npos) << runner;
        const std::string call = src.substr(begin, src.find(");", begin) - begin);
        EXPECT_NE(call.find("combined_strategy_id"), std::string::npos)
            << runner << ": the report body is not given the run's strategy id, so its charts "
                         "query the default LIVE_TREND_FOLLOWING";
    }
}
