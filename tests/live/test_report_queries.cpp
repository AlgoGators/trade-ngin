// tests/live/test_report_queries.cpp
//
// T-8D-2 R55, the part the runners own: the live runners' read of yesterday's position rows for
// the daily report filters by strategy_id and portfolio_id and, since trading.positions carries
// a portfolio_type, reads the system book only, as the chart queries do. The query text is built
// by a function called here; that the three runners use it is a labelled source guard.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "trade_ngin/live/report_queries.hpp"

using namespace trade_ngin;

namespace {

const auto npos = std::string::npos;

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

size_t count_of(const std::string& text, const std::string& needle) {
    size_t n = 0;
    for (size_t at = text.find(needle); at != npos; at = text.find(needle, at + 1))
        ++n;
    return n;
}

}  // namespace

// The futures form: seven columns, the sleeve name last (the runner keys a row by sleeve and
// symbol), one book by both ids, the system book only, the rows of one UTC date.
TEST(EmailQueriesYesterdayPositions, TheFuturesReadFiltersByBothIdsAndTheSystemBook) {
    EXPECT_EQ(yesterday_positions_report_query("LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST",
                                               "BASE_PORTFOLIO", "2026-04-24", true),
              "SELECT symbol, quantity, average_price, daily_realized_pnl, daily_unrealized_pnl, "
              "last_update, strategy_name FROM trading.positions "
              "WHERE strategy_id = 'LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST' "
              "AND portfolio_id = 'BASE_PORTFOLIO' AND portfolio_type = 'system' "
              "AND DATE(last_update) = '2026-04-24'");
}

// The equity form: the same six columns the runner read before, the same filters.
TEST(EmailQueriesYesterdayPositions, TheEquityReadFiltersByBothIdsAndTheSystemBook) {
    const std::string query = yesterday_positions_report_query(
        "LIVE_EQUITY_MEAN_REVERSION", "EQUITY_MR_PORTFOLIO", "2026-06-15", false);
    EXPECT_EQ(query,
              "SELECT symbol, quantity, average_price, daily_realized_pnl, daily_unrealized_pnl, "
              "last_update FROM trading.positions "
              "WHERE strategy_id = 'LIVE_EQUITY_MEAN_REVERSION' "
              "AND portfolio_id = 'EQUITY_MR_PORTFOLIO' AND portfolio_type = 'system' "
              "AND DATE(last_update) = '2026-06-15'");
    EXPECT_EQ(query.find("strategy_name"), npos);
}

// Each of the three filters is in the text once, whatever the book.
TEST(EmailQueriesYesterdayPositions, EveryFilterIsPresentOnce) {
    for (const bool with_sleeve : {true, false}) {
        const std::string query =
            yesterday_positions_report_query("S", "P", "2026-01-02", with_sleeve);
        EXPECT_EQ(count_of(query, "strategy_id = 'S'"), 1u);
        EXPECT_EQ(count_of(query, "portfolio_id = 'P'"), 1u);
        EXPECT_EQ(count_of(query, "portfolio_type = 'system'"), 1u);
        EXPECT_EQ(count_of(query, "DATE(last_update) = '2026-01-02'"), 1u);
    }
}

// READS SOURCE (a labelled wiring and twin guard; the query text is executed above). It also
// pins the runners' other report read, yesterday's trading.live_results row: that table has no
// portfolio_type, and the query names both ids.
TEST(EmailQueriesTwinSourceGuard, TheRunnersReadYesterdaysRowsThroughTheFunction) {
    for (const char* runner : {"apps/strategies/live_portfolio_conservative.cpp",
                               "apps/strategies/live_portfolio.cpp"}) {
        SCOPED_TRACE(runner);
        const std::string src = read_source(runner);
        if (src.empty())
            GTEST_SKIP() << "runner source not found from the test working directory";
        EXPECT_EQ(count_of(src,
                           "trade_ngin::yesterday_positions_report_query(\n"
                           "                            combined_strategy_id, "
                           "coordinator_config.portfolio_id,\n"
                           "                            yesterday_date_for_email, true, qt_read_book);"),
                  1u);
        EXPECT_EQ(src.find("daily_unrealized_pnl, last_update, strategy_name \""), npos)
            << "the literal query without the portfolio_type filter";
        EXPECT_NE(
            src.find("\"FROM trading.live_results \"\n"
                     "                            \"WHERE strategy_id = '\" +\n"
                     "                            combined_strategy_id + \"' AND portfolio_id = "
                     "'\" +\n"
                     "                            coordinator_config.portfolio_id + \"' AND "
                     "date = '\" +\n"
                     "                            yesterday_date_for_email +"),
            npos);
    }
    const std::string equity = read_source("apps/strategies/live_equity_mean_reversion.cpp");
    if (equity.empty())
        GTEST_SKIP() << "runner source not found from the test working directory";
    EXPECT_EQ(count_of(equity,
                       "trade_ngin::yesterday_positions_report_query(\n"
                       "                    kEquityStrategyId, portfolio_id, "
                       "yesterday_date_for_email, false);"),
              1u);
    EXPECT_EQ(equity.find("daily_unrealized_pnl, last_update \""), npos);
    EXPECT_NE(
        equity.find("\"WHERE strategy_id = 'LIVE_EQUITY_MEAN_REVERSION' AND portfolio_id = '\" "
                    "+ portfolio_id + \"' AND date = '\" + yesterday_date_for_email + \"' \""),
        npos);
}

// T-EMAIL Q11: the quantity a closed position had is the latest stored row before the settled day,
// one row per symbol, of this book's system portfolio; a quote in a symbol cannot end the list.
TEST(ReportQueries, TheQuantityBeforeQueryReadsOneBookAndOneRowPerSymbolBeforeTheDate) {
    const std::string q = trade_ngin::quantity_before_report_query(
        "LIVE_EQUITY_MEAN_REVERSION", "EQUITY_MR", "2026-05-04", {"ABT", "D'D"});
    EXPECT_NE(q.find("strategy_id = 'LIVE_EQUITY_MEAN_REVERSION'"), std::string::npos) << q;
    EXPECT_NE(q.find("portfolio_id = 'EQUITY_MR'"), std::string::npos) << q;
    EXPECT_NE(q.find("portfolio_type = 'system'"), std::string::npos) << q;
    EXPECT_NE(q.find("DATE(last_update) < '2026-05-04'"), std::string::npos) << q;
    EXPECT_NE(q.find("DISTINCT ON (symbol)"), std::string::npos) << q;
    EXPECT_NE(q.find("ORDER BY symbol, last_update DESC"), std::string::npos) << q;
    EXPECT_NE(q.find("IN ('ABT', 'DD')"), std::string::npos) << q;
}
