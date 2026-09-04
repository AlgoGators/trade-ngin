// tests/live/test_feed_ingestion_assertion.cpp
//
// E2-F43 / BA-17 -- a strategy that did not ingest the day's bars must stop the run.
//
// PortfolioManager::process_market_data (src/portfolio/portfolio_manager.cpp:217-222) does
// this:
//
//     auto result = info.strategy->on_data(data);
//     if (result.is_error()) { ERROR(...); std::cerr << ...; }     // <-- and carries on
//     info.target_positions = info.strategy->get_target_positions();
//
// so an on_data failure produces an ERROR line, an exit code of 0, and targets read off
// instrument data the strategy never updated. A live process starts with an empty
// instrument_data_ every session, so those targets are ZERO for every symbol -- and a zero
// target map against a held book is not "no change", it is a full-book SELL at the T-1
// closes. Before E2-F28 collapsed the double feed, the first feed returned that error to the
// runner and the run exited 1. Removing it removed the only caller that checked.
//
// These tests pin the check the runner now performs before it reads a single target.

#include <gtest/gtest.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/live/live_daily_cycle.hpp"

using namespace trade_ngin;

namespace {

Timestamp at(int y, int m, int d) {
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}

Bar bar_at(const std::string& symbol, Timestamp ts, double close = 100.0) {
    Bar b;
    b.symbol = symbol;
    b.timestamp = ts;
    b.open = Price(close);
    b.high = Price(close);
    b.low = Price(close);
    b.close = Price(close);
    b.volume = 1000.0;
    return b;
}

// Three symbols, bars on three consecutive sessions, newest 04-17.
std::vector<Bar> three_days_of_bars() {
    std::vector<Bar> bars;
    for (const auto& sym : {"AAPL", "MSFT", "TMUS"}) {
        bars.push_back(bar_at(sym, at(2026, 4, 15)));
        bars.push_back(bar_at(sym, at(2026, 4, 16)));
        bars.push_back(bar_at(sym, at(2026, 4, 17)));
    }
    return bars;
}

}  // namespace

TEST(FeedIngestionAssertion, AFullFeedVerifiesEverySymbolAndFailsNone) {
    const std::vector<std::string> universe = {"AAPL", "MSFT", "TMUS"};
    const std::unordered_map<std::string, Timestamp> last_update = {
        {"AAPL", at(2026, 4, 17)}, {"MSFT", at(2026, 4, 17)}, {"TMUS", at(2026, 4, 17)}};

    const auto r = LiveDailyCycle::verify_strategy_ingested_bars(universe,
                                                                three_days_of_bars(),
                                                                last_update);
    EXPECT_TRUE(r.not_ingested.empty());
    EXPECT_EQ(r.verified, 3u);
    EXPECT_EQ(r.no_bars, 0u);
}

TEST(FeedIngestionAssertion, AnEmptyStrategyStateIsCaughtRatherThanSellingTheBook) {
    // The exact shape of the defect: on_data errored, the process's instrument_data_ was
    // never touched, every symbol is missing. get_target_positions() returns nothing and the
    // runner would sell everything.
    const std::vector<std::string> universe = {"AAPL", "MSFT", "TMUS"};
    const auto r = LiveDailyCycle::verify_strategy_ingested_bars(universe,
                                                                three_days_of_bars(), {});
    ASSERT_EQ(r.not_ingested.size(), 3u);
    EXPECT_EQ(r.verified, 0u);
    for (const auto& detail : r.not_ingested) {
        EXPECT_NE(detail.find("no instrument data"), std::string::npos) << detail;
    }
}

TEST(FeedIngestionAssertion, OneSymbolStuckOnAnOlderBarIsNamed) {
    // The partial case, which is the one a "did anything happen at all" check would miss:
    // two symbols ingested today, MSFT is still on yesterday's bar. Its target would be
    // computed from a z-score one session out of date against a book marked at today's
    // close.
    const std::vector<std::string> universe = {"AAPL", "MSFT", "TMUS"};
    const std::unordered_map<std::string, Timestamp> last_update = {
        {"AAPL", at(2026, 4, 17)},
        {"MSFT", at(2026, 4, 16)},  // stale by one session
        {"TMUS", at(2026, 4, 17)}};

    const auto r = LiveDailyCycle::verify_strategy_ingested_bars(universe,
                                                                three_days_of_bars(),
                                                                last_update);
    ASSERT_EQ(r.not_ingested.size(), 1u);
    EXPECT_NE(r.not_ingested[0].find("MSFT"), std::string::npos);
    // The message carries both timestamps, so the log says WHICH bar it stopped on.
    EXPECT_NE(r.not_ingested[0].find("2026-04-16"), std::string::npos) << r.not_ingested[0];
    EXPECT_NE(r.not_ingested[0].find("2026-04-17"), std::string::npos) << r.not_ingested[0];
    EXPECT_EQ(r.verified, 2u);
}

TEST(FeedIngestionAssertion, ASymbolWithNoBarsIsCountedNotFailed) {
    // A held name whose ticker stopped printing is a DATA GAP, owned by the runner's T-1
    // price checks ("Missing T-1 price for symbol with a non-zero position") and by the
    // widened-price resolver. Failing it here would stop the book on a condition the run is
    // designed to handle, so it is counted separately and reported.
    const std::vector<std::string> universe = {"AAPL", "MSFT", "TMUS", "RAL"};
    const std::unordered_map<std::string, Timestamp> last_update = {
        {"AAPL", at(2026, 4, 17)}, {"MSFT", at(2026, 4, 17)}, {"TMUS", at(2026, 4, 17)}};

    const auto r = LiveDailyCycle::verify_strategy_ingested_bars(universe,
                                                                three_days_of_bars(),
                                                                last_update);
    EXPECT_TRUE(r.not_ingested.empty());
    EXPECT_EQ(r.verified, 3u);
    EXPECT_EQ(r.no_bars, 1u);
}

TEST(FeedIngestionAssertion, NothingVerifiableIsReportedAsVacuousRatherThanAsAPass) {
    // Every symbol in the universe has no bars. `not_ingested` is empty, so a check that
    // only looked at that list would pass -- while proving nothing whatsoever about what the
    // strategy saw. verified == 0 is what the runner turns into a refusal.
    const std::vector<std::string> universe = {"RAL", "MRP"};
    const auto r = LiveDailyCycle::verify_strategy_ingested_bars(universe,
                                                                three_days_of_bars(), {});
    EXPECT_TRUE(r.not_ingested.empty());
    EXPECT_EQ(r.verified, 0u);
    EXPECT_EQ(r.no_bars, 2u);
}

TEST(FeedIngestionAssertion, TheNewestBarWinsRegardlessOfTheOrderTheBarsArriveIn) {
    // all_bars comes back from the loader in whatever order the query produced; the check
    // must take the maximum per symbol, not the last one seen.
    std::vector<Bar> shuffled = {bar_at("AAPL", at(2026, 4, 17)),
                                 bar_at("AAPL", at(2026, 4, 15)),
                                 bar_at("AAPL", at(2026, 4, 16))};
    const auto ok = LiveDailyCycle::verify_strategy_ingested_bars(
        {"AAPL"}, shuffled, {{"AAPL", at(2026, 4, 17)}});
    EXPECT_TRUE(ok.not_ingested.empty());
    EXPECT_EQ(ok.verified, 1u);

    const auto bad = LiveDailyCycle::verify_strategy_ingested_bars(
        {"AAPL"}, shuffled, {{"AAPL", at(2026, 4, 16)}});
    EXPECT_EQ(bad.not_ingested.size(), 1u);
}
