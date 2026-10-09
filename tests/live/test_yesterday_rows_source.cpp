// tests/live/test_yesterday_rows_source.cpp
//
// What a futures live run prints of yesterday (the email's "Yesterday's Finalized Position
// Results" table and the attached <T-1>_positions_asof_<T>.csv) is the Day T-1 rows read back
// AFTER the run's finalize write. The runner loads the stored T-1 books early, for the no-session
// hold, and from the commit that moved that load ahead of the finalize (11d03d91) the same early
// rows were handed to the email and the CSV: every realized cell printed the placeholder 0.00
// while the stored row carried the settled figure.
//
// The runners are `main()`s and cannot be linked here, so the order and the arguments are read in
// their source (the approach of test_day_t_write_ordering.cpp).

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

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

// The text of the call that starts at `call`, up to the `);` that ends it.
std::string call_text(const std::string& src, const std::string& call) {
    const auto begin = src.find(call);
    if (begin == std::string::npos) return {};
    return src.substr(begin, src.find(");", begin) - begin);
}

}  // namespace

TEST(YesterdayRowsSource, BothFuturesRunnersPrintYesterdayFromTheRowsReadBackAfterTheFinalize) {
    for (const char* runner :
         {"apps/strategies/live_portfolio_conservative.cpp", "apps/strategies/live_portfolio.cpp"}) {
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";

        const auto early_load = src.find("previous_strategy_positions[strategy_name] = prev_result.value();");
        const auto finalize_write = src.find("Day T-1 positions with finalized PnL for strategy: ");
        const auto read_back = src.find("read_back_finalized_books(");
        const auto csv = src.find("csv_exporter->export_finalized_positions(");
        const auto email = src.find("email_sender->generate_trading_report_body(");
        ASSERT_NE(early_load, std::string::npos) << runner;
        ASSERT_NE(finalize_write, std::string::npos) << runner;
        ASSERT_NE(csv, std::string::npos) << runner;
        ASSERT_NE(email, std::string::npos) << runner;
        ASSERT_NE(read_back, std::string::npos)
            << runner << ": yesterday's rows are not read back after the finalize write";

        // The early load stays ahead of the finalize: the no-session hold needs the stored book.
        EXPECT_LT(early_load, finalize_write) << runner;
        EXPECT_GT(read_back, finalize_write)
            << runner << ": the read-back runs before the finalize write, so it reads the placeholder";
        EXPECT_LT(read_back, csv) << runner;
        EXPECT_LT(read_back, email) << runner;

        for (const char* call : {"csv_exporter->export_finalized_positions(",
                                 "email_sender->generate_trading_report_body("}) {
            const std::string text = call_text(src, call);
            EXPECT_NE(text.find("finalized_strategy_positions"), std::string::npos)
                << runner << ": " << call << " is not handed the rows read back after the finalize";
            EXPECT_EQ(text.find("previous_strategy_positions"), std::string::npos)
                << runner << ": " << call
                << " is handed the books loaded BEFORE the finalize, whose realized P&L is the "
                   "placeholder 0";
        }
    }
}
