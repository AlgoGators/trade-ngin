// tests/live/test_live_runner_small_fixes.cpp
//
// T-7b-1 C7b: the T-7a review's small fixes on the live runners' side (T-7a_CODE_REVIEW R2 R3 R4
// R9 R10, T-7a_S1_S3_CODE_REVIEW S1-3; R11 is held out, HD 2026-09-24). Each case is the review's
// failure scenario, run through the helper the runners call where one exists, and through the
// runner source (the runners are main()s and cannot be linked here, as
// tests/live/test_day_t_write_ordering.cpp established) where the defect is in the wiring:
//
//   R2   a carried day's CSV note and the email banner derive their reason from the T-1
//        classification: a feed-hole carry names the FEED HOLE and its count (the parent wrote
//        "no session on D ()" and showed no banner), a closure names the first verdict's reason;
//   R3   the held-symbol refusal keys on ANY no-bar verdict older than the tolerance (a feed dead
//        for more than eight weeks is reclassified as a closure and stopped refusing), and a
//        closure older than the tolerance is logged as an ERROR;
//   R4   the STRICT assertion marks today's live_run_metadata row before it returns 1, merged into
//        the row's JSON so a same-day risk refusal mark stays;
//   R9   a held row for a symbol outside the classified universe is logged as such;
//   R10  the execution manager's cost feed is the strategy feed (the JUNK T-1 bar withheld);
//   S1-3 a whole-book carry sums the sleeves into the combined map, and the email's yesterday
//        table is keyed by (sleeve, symbol).

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/data/session_classifier.hpp"
#include "trade_ngin/live/carried_day.hpp"
#include "trade_ngin/live/run_metadata_marks.hpp"
#include "trade_ngin/live/session_book_gate.hpp"

using namespace trade_ngin;
using Day = SessionClassifier::Day;

namespace {

Day ymd_day(const std::string& s) {
    const int y = std::stoi(s.substr(0, 4));
    const unsigned m = static_cast<unsigned>(std::stoi(s.substr(5, 2)));
    const unsigned d = static_cast<unsigned>(std::stoi(s.substr(8, 2)));
    return std::chrono::sys_days{std::chrono::year{y} / std::chrono::month{m} / std::chrono::day{d}};
}

Bar bar_on(const std::string& symbol, const std::string& date, double close, double volume) {
    return Bar(Timestamp(ymd_day(date)), close, close * 1.01, close * 0.99, close, volume, symbol);
}

Position pos(const std::string& symbol, double qty, double avg) {
    Position p;
    p.symbol = symbol;
    p.quantity = Decimal(qty);
    p.average_price = Decimal(avg);
    return p;
}

const HolidayLookup kNoHolidays = [](const std::string&) -> std::optional<HolidayInfo> {
    return std::nullopt;
};

/// `n` weekday bars ending the weekday before `through_exclusive`.
void weekday_history(SessionClassifier& c, const std::string& symbol,
                     const std::string& through_exclusive, double close, double volume,
                     int n = 60) {
    Day d = ymd_day(through_exclusive);
    int added = 0;
    while (added < n) {
        d -= std::chrono::days{1};
        const std::chrono::weekday wd{d};
        if (wd == std::chrono::Saturday || wd == std::chrono::Sunday) continue;
        c.add_bar(bar_on(symbol, SessionClassifier::ymd(d), close, volume));
        ++added;
    }
}

void console_logger() {
    LoggerConfig lc;
    lc.destination = LogDestination::CONSOLE;
    lc.min_level = LogLevel::INFO;
    lc.include_timestamp = false;
    Logger::instance().initialize(lc);
}

/// The first captured log line containing `needle` (empty when none does).
std::string line_with(const std::string& out, const std::string& needle) {
    const auto at = out.find(needle);
    if (at == std::string::npos) return {};
    const auto begin = out.rfind('\n', at);
    const auto end = out.find('\n', at);
    return out.substr(begin == std::string::npos ? 0 : begin + 1,
                      (end == std::string::npos ? out.size() : end) -
                          (begin == std::string::npos ? 0 : begin + 1));
}

size_t count_of(const std::string& text, const std::string& needle) {
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
    return n;
}

/// The dead-Sunday shape: T-1 = Sun 2026-05-17, MES and MYM printed the four Sundays before
/// (feed holes, age 2 from Fri 05-15), ZC never prints on a Sunday (a closure).
T1Classification dead_sunday_t1() {
    SessionClassifier c;
    for (const char* s : {"MES.v.0", "MYM.v.0", "ZC.v.0"}) {
        weekday_history(c, s, "2026-05-16", 100.0, 100000);
    }
    for (int k = 1; k <= 4; ++k) {
        const std::string d = SessionClassifier::ymd(ymd_day("2026-05-17") - std::chrono::days{7 * k});
        c.add_bar(bar_on("MES.v.0", d, 100.0, 30000));
        c.add_bar(bar_on("MYM.v.0", d, 100.0, 5000));
    }
    return classify_t1(c, {"MES.v.0", "MYM.v.0", "ZC.v.0"}, ymd_day("2026-05-17"), kNoHolidays);
}

/// An ordinary Sunday T-1 before June 2026: nothing prints on a Saturday, every symbol a closure.
T1Classification saturday_t1() {
    SessionClassifier c;
    for (const char* s : {"6A.v.0", "MES.v.0", "ZC.v.0"}) {
        weekday_history(c, s, "2026-04-25", 100.0, 100000);
    }
    return classify_t1(c, {"6A.v.0", "MES.v.0", "ZC.v.0"}, ymd_day("2026-04-25"), kNoHolidays);
}

// Runner source ---------------------------------------------------------------------------------

std::filesystem::path find_repo_file(const std::string& relative) {
    namespace fs = std::filesystem;
    fs::path dir = fs::current_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (fs::exists(dir / relative)) return dir / relative;
        dir = dir.parent_path();
    }
    return {};
}

std::string read_source(const std::string& relative) {
    auto path = find_repo_file(relative);
    if (path.empty()) return {};
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

const char* const kFuturesRunners[] = {"apps/strategies/live_portfolio_conservative.cpp",
                                       "apps/strategies/live_portfolio.cpp"};
constexpr auto npos = std::string::npos;

std::string between(const std::string& s, const std::string& from, const std::string& to) {
    const auto a = s.find(from);
    if (a == npos) return {};
    const auto b = s.find(to, a);
    if (b == npos) return {};
    return s.substr(a, b - a);
}

}  // namespace

// =============================================================================================
// R2: the carried day's reason
// =============================================================================================

TEST(C7bCarriedDay, AFeedHoleCarryNamesTheFeedHoleAndItsCount) {
    const auto t1 = dead_sunday_t1();
    ASSERT_EQ(t1.feed_hole, 2u);
    ASSERT_FALSE(t1.any_printed()) << "setup: a whole-book carry";
    const std::string reason = carried_day_reason(t1);
    EXPECT_NE(reason.find("FEED HOLE"), npos) << reason;
    EXPECT_NE(reason.find("2 symbol(s) normally print on this weekday"), npos) << reason;
    EXPECT_EQ(reason.find("closure"), npos) << reason;
}

TEST(C7bCarriedDay, AClosureCarryNamesTheFirstVerdictsReason) {
    const auto t1 = saturday_t1();
    ASSERT_EQ(t1.feed_hole, 0u);
    ASSERT_EQ(t1.closure, 3u);
    const std::string reason = carried_day_reason(t1);
    EXPECT_EQ(reason, "closure: " + t1.verdicts.front().reason) << reason;
    EXPECT_EQ(reason.find("FEED HOLE"), npos);
}

TEST(C7bCarriedDay, TheEmailBannerCarriesTheSameReasonEscaped) {
    const std::string body =
        "<html><body>\n<div class=\"container\">\n<h1>Daily Trading Report</h1>\n</div></body>";
    const std::string reason = carried_day_reason(dead_sunday_t1()) + " <&>";
    const std::string flagged = flag_email_body_for_carried_day(body, reason);
    EXPECT_NE(flagged, body) << "a carried day's email has no banner";
    EXPECT_NE(flagged.find("FEED HOLE"), npos) << flagged;
    EXPECT_NE(flagged.find("&lt;&amp;&gt;"), npos) << "the reason is HTML-escaped";
    EXPECT_LT(flagged.find("id=\"carried-day\""), flagged.find("<h1>"))
        << "the banner opens the report";
}

TEST(C7bRunnerSource, BothTwinsDeriveTheCarriedReasonFromTheClassificationAndFlagTheEmail) {
    std::vector<std::string> blocks;
    for (const char* runner : kFuturesRunners) {
        SCOPED_TRACE(runner);
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found";
        EXPECT_EQ(src.find("\" (a Saturday)\""), npos)
            << "the note still keys on the weekday, blank on a feed-hole carry";
        EXPECT_NE(src.find("carried_day_reason(t1_classification)"), npos);
        const std::string email = between(src, "// Send email with CSV attachments",
                                          "email_sender->send_email(subject, email_body");
        ASSERT_FALSE(email.empty());
        EXPECT_NE(email.find("flag_email_body_for_carried_day(email_body, carried_day_note)"), npos)
            << "the email of a carried day carries no reason";
        blocks.push_back(between(src, "const std::string carried_day_note =", ";\n"));
    }
    ASSERT_EQ(blocks.size(), 2u);
    ASSERT_FALSE(blocks[0].empty());
    EXPECT_EQ(blocks[0], blocks[1]);
}

// =============================================================================================
// R3: the held-symbol refusal keys on any no-bar verdict past the tolerance
// =============================================================================================

TEST(C7bBookGate, AFeedDeadForNineWeeksIsAClosureAndIsStillRefusedWhenHeld) {
    // MYM's last bar is 64 days before T-1 (Thu 2026-04-23): no print on any of the eight
    // preceding Thursdays, so the classifier calls it NO_BAR(closure) "not expected".
    SessionClassifier c;
    weekday_history(c, "MYM.v.0", "2026-02-19", 49000.0, 90000);  // last bar Wed 2026-02-18
    weekday_history(c, "MES.v.0", "2026-04-23", 7100.0, 1500000);
    c.add_bar(bar_on("MES.v.0", "2026-04-23", 7150.0, 1400000));
    const auto t1 = classify_t1(c, {"MES.v.0", "MYM.v.0"}, ymd_day("2026-04-23"), kNoHolidays);
    const auto* mym = t1.find("MYM.v.0");
    ASSERT_NE(mym, nullptr);
    ASSERT_EQ(mym->verdict, SessionVerdict::NO_BAR_CLOSURE) << mym->reason;
    ASSERT_EQ(mym->hole_age_days, 64);
    EXPECT_EQ(t1.max_hole_age_days, 0) << "the feed-hole maximum does not see it";

    const std::unordered_map<std::string, Position> held{{"MYM.v.0", pos("MYM.v.0", 1, 49707)}};
    const auto holes = held_feed_holes_past_tolerance(t1, held, 4);
    ASSERT_EQ(holes.size(), 1u) << "a held symbol with no bar for 64 days is not refused";
    EXPECT_EQ(holes[0].symbol, "MYM.v.0");
    EXPECT_EQ(holes[0].age_days, 64);
    EXPECT_EQ(holes[0].last_bar_date, "2026-02-18");

    // Not held: logged, never refused. A fresh closure (a Saturday, age 1) is never refused.
    EXPECT_TRUE(held_feed_holes_past_tolerance(t1, {}, 4).empty());
    const auto sat = saturday_t1();
    const std::unordered_map<std::string, Position> held6a{{"6A.v.0", pos("6A.v.0", 2, 0.66)}};
    EXPECT_TRUE(held_feed_holes_past_tolerance(sat, held6a, 4).empty());
}

TEST(C7bBookGate, AClosureOlderThanTheToleranceLogsAnErrorAFreshOneAnInfo) {
    SessionClassifier c;
    weekday_history(c, "MYM.v.0", "2026-02-19", 49000.0, 90000);
    weekday_history(c, "ZC.v.0", "2026-04-25", 460.0, 150000);
    const auto t1 = classify_t1(c, {"MYM.v.0", "ZC.v.0"}, ymd_day("2026-04-25"), kNoHolidays);
    ASSERT_EQ(t1.closure, 2u);
    console_logger();
    ::testing::internal::CaptureStdout();
    log_t1_classification(t1, 4);
    const std::string out = ::testing::internal::GetCapturedStdout();
    const std::string mym = line_with(out, "T1_CLASSIFIER closure MYM.v.0 2026-04-25");
    ASSERT_FALSE(mym.empty()) << out;
    EXPECT_NE(mym.find("[ERROR]"), npos) << "a closure 66 days old is an INFO line: " << mym;
    EXPECT_NE(mym.find("past live.data_staleness_tolerance_days=4"), npos) << mym;
    EXPECT_EQ(count_of(out, "[ERROR]"), 1u) << out;
    const std::string zc = line_with(out, "T1_CLASSIFIER closure ZC.v.0 2026-04-25");
    ASSERT_FALSE(zc.empty()) << out;
    EXPECT_NE(zc.find("[INFO]"), npos) << "a one-day closure stays INFO: " << zc;
}

TEST(C7bRunnerSource, BothTwinsTestEveryNoBarAgeAndGiveTheClassifierLogTheTolerance) {
    for (const char* runner : kFuturesRunners) {
        SCOPED_TRACE(runner);
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found";
        EXPECT_EQ(src.find("if (t1_classification.max_hole_age_days > hole_tolerance_days)"), npos)
            << "the refusal only reads the held book when a FEED HOLE is old";
        EXPECT_NE(src.find("if (max_no_bar_age_days(t1_classification) > hole_tolerance_days)"),
                  npos);
        EXPECT_NE(src.find("log_t1_classification(t1_classification,\n"
                           "                              app_config.live.data_staleness_tolerance_days);"),
                  npos);
    }
}

// =============================================================================================
// R4: the STRICT assertion marks the metadata row before it returns
// =============================================================================================

TEST(C7bRunMetadataMark, TheStrictAssertionMarkNamesTheUnpricedChangesAndKeepsEveryKey) {
    const nlohmann::json config = {{"total_capital", 500000.0}, {"use_optimization", true}};
    const auto marked =
        mark_strict_assertion(config, {"TREND_FOLLOWING/GC.v.0", "TREND_FOLLOWING/MYM.v.0"});
    ASSERT_TRUE(marked.contains("strict_assertion")) << marked.dump();
    EXPECT_EQ(marked["strict_assertion"]["unpriced_book_changes"],
              nlohmann::json::array({"TREND_FOLLOWING/GC.v.0", "TREND_FOLLOWING/MYM.v.0"}));
    EXPECT_EQ(marked["total_capital"], 500000.0);
    EXPECT_EQ(marked["use_optimization"], true);
}

TEST(C7bRunnerSource, BothTwinsMarkTheMetadataRowBeforeTheStrictAssertionReturns) {
    std::vector<std::string> blocks;
    for (const char* runner : kFuturesRunners) {
        SCOPED_TRACE(runner);
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found";
        const std::string block =
            between(src, "ERROR(\"STRICT_ASSERTION failed:", "INFO(\"PHASE 4: Total executions");
        ASSERT_FALSE(block.empty());
        const auto mark = block.find("mark_strict_assertion(portfolio_config_json, unpriced_changes)");
        const auto upsert = block.find("db->store_live_run_metadata(");
        const auto ret = block.find("return 1;");
        ASSERT_NE(ret, npos);
        ASSERT_NE(mark, npos) << "the assertion returns leaving an unmarked metadata row";
        // The mark is merged into the JSON the row carries so far: the Q2 block keeps its marked
        // JSON, so a same-day risk_refusal mark is not replaced by the STRICT one.
        const std::string q2 = between(src, "if (auto risk_refusal = portfolio_risk_refusal(",
                                       "if (mark_result.is_error())");
        ASSERT_FALSE(q2.empty());
        EXPECT_NE(q2.find("portfolio_config_json = mark_risk_refusal(portfolio_config_json,"), npos)
            << "the risk refusal mark is written and dropped: a STRICT mark on the same row would "
               "replace it";
        ASSERT_NE(upsert, npos);
        EXPECT_LT(upsert, ret);
        EXPECT_LT(mark, ret);
        blocks.push_back(block);
    }
    ASSERT_EQ(blocks.size(), 2u);
    EXPECT_EQ(blocks[0], blocks[1]);
}

// =============================================================================================
// R9: a held row outside the classified universe
// =============================================================================================

TEST(C7bBookGate, AHeldRowOutsideTheClassifiedUniverseIsLoggedAsSuch) {
    const auto t1 = saturday_t1();
    StrategyBooks prev{{"TREND_FOLLOWING", {{"OLD.v.0", pos("OLD.v.0", 2, 10)}}}};
    StrategyBooks today{{"TREND_FOLLOWING", {}}};
    const auto holds = hold_non_session_symbols(today, prev, t1, Timestamp(ymd_day("2026-04-26")), {});
    ASSERT_EQ(holds.size(), 1u);
    EXPECT_DOUBLE_EQ(today["TREND_FOLLOWING"]["OLD.v.0"].quantity.as_double(), 2.0);
    console_logger();
    ::testing::internal::CaptureStdout();
    log_book_holds(holds);
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_NE(out.find("BOOK_GATE OLD.v.0 (TREND_FOLLOWING): outside the classified universe"), npos)
        << out;
    EXPECT_EQ(out.find("OLD.v.0 (TREND_FOLLOWING): T-1 NO_BAR(closure)"), npos)
        << "a symbol nobody classified is logged as a closure hold: " << out;
}

TEST(C7bBookGate, AClassifiedClosureHoldKeepsItsLine) {
    const auto t1 = saturday_t1();
    StrategyBooks prev{{"TREND_FOLLOWING", {{"MES.v.0", pos("MES.v.0", 1, 7100)}}}};
    StrategyBooks today{{"TREND_FOLLOWING", {{"MES.v.0", pos("MES.v.0", 2, 7100)}}}};
    const auto holds = hold_non_session_symbols(today, prev, t1, Timestamp(ymd_day("2026-04-26")), {});
    ASSERT_EQ(holds.size(), 1u);
    console_logger();
    ::testing::internal::CaptureStdout();
    log_book_holds(holds);
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_NE(out.find("BOOK_GATE MES.v.0 (TREND_FOLLOWING): T-1 NO_BAR(closure) -- book held at "
                       "1.000000 instead of target 2.000000; no order today"),
              npos)
        << out;
}

// =============================================================================================
// R10: the cost feed is the strategy feed
// =============================================================================================

TEST(C7bRunnerSource, BothTwinsFeedTheCostManagerTheStrategyFeedWithTheJunkBarWithheld) {
    std::vector<std::string> blocks;
    for (const char* runner : kFuturesRunners) {
        SCOPED_TRACE(runner);
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found";
        const auto feed_def = src.find("const std::vector<Bar>& strategy_feed_bars =");
        const auto cost = src.find("// UPDATE TRANSACTION COST MANAGER WITH MARKET DATA");
        ASSERT_NE(feed_def, npos);
        ASSERT_NE(cost, npos);
        EXPECT_LT(feed_def, cost) << "the strategy feed is built after the cost feed";
        const std::string block =
            between(src, "// UPDATE TRANSACTION COST MANAGER WITH MARKET DATA",
                    "feed_futures_cost_model(cost_model, strategy_feed_bars, now);");
        ASSERT_FALSE(block.empty());
        EXPECT_NE(block.find("for (const auto& bar : strategy_feed_bars) {"), npos)
            << "the cost manager is fed the JUNK T-1 bar";
        EXPECT_EQ(block.find("for (const auto& bar : all_bars) {"), npos);
        EXPECT_NE(src.find("portfolio->process_market_data(strategy_feed_bars);"), npos);
        blocks.push_back(block);
    }
    ASSERT_EQ(blocks.size(), 2u);
    EXPECT_EQ(blocks[0], blocks[1]);
}

// =============================================================================================
// S1-3: the carried combined map sums the sleeves; the email's yesterday map is per sleeve
// =============================================================================================

TEST(C7bRunnerSource, BothTwinsSumTheSleevesOfACarriedBookAndKeyYesterdaysEmailRowsBySleeve) {
    std::vector<std::string> carry_blocks;
    for (const char* runner : kFuturesRunners) {
        SCOPED_TRACE(runner);
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found";
        const std::string carry = between(src, "// Load previous positions for each strategy and use as current",
                                          "INFO(\"Total positions carried forward: \"");
        ASSERT_FALSE(carry.empty());
        EXPECT_EQ(carry.find("positions[symbol] = pos;"), npos)
            << "the last sleeve's row overwrites the others";
        EXPECT_NE(carry.find("rebuild_combined_positions(positions, strategy_positions_map);"), npos);
        carry_blocks.push_back(carry);

        EXPECT_EQ(src.find("yesterday_positions_finalized[symbol] = pos;"), npos)
            << "the email's yesterday map is keyed by symbol: the last sleeve wins";
        EXPECT_NE(src.find("yesterday_positions_finalized[{strategy_name, symbol}] = pos;"), npos);
        EXPECT_NE(src.find("\"daily_unrealized_pnl, last_update, strategy_name \""), npos)
            << "the query must read the sleeve";
    }
    ASSERT_EQ(carry_blocks.size(), 2u);
    EXPECT_EQ(carry_blocks[0], carry_blocks[1]);
}

TEST(C7bBookGate, TheCombinedMapOfTwoSleevesIsTheirSum) {
    // The helper the carry now calls: two sleeves long one contract each store a combined 2, and
    // opposite sleeves net (the overwrite kept whichever sleeve came last).
    StrategyBooks books{{"TREND_FOLLOWING", {{"6L.v.0", pos("6L.v.0", 1, 0.2)},
                                             {"MES.v.0", pos("MES.v.0", 1, 7100)}}},
                        {"TREND_FOLLOWING_FAST", {{"6L.v.0", pos("6L.v.0", 1, 0.2)},
                                                  {"MES.v.0", pos("MES.v.0", -3, 7100)}}}};
    std::unordered_map<std::string, Position> combined;
    rebuild_combined_positions(combined, books);
    EXPECT_DOUBLE_EQ(combined.at("6L.v.0").quantity.as_double(), 2.0);
    EXPECT_DOUBLE_EQ(combined.at("MES.v.0").quantity.as_double(), -2.0);
}

// =============================================================================================
// The late-bar warning (T-FIX; HD 2026-10-07: a warning, no catch-up, no refusal). A held symbol's
// bar that arrives after the run that settles its date is on no stored row; both runners name it,
// identically, once the stored T-1 books are loaded and before the finalize.
// (The rule itself: tests/live/test_late_bar_warning.cpp.)
// =============================================================================================

TEST(LateBarWarningSource, BothTwinsNameALateBarAfterTheStoredBooksAreLoaded) {
    std::vector<std::string> blocks;
    for (const char* runner : kFuturesRunners) {
        SCOPED_TRACE(runner);
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found";
        const auto books = src.find("previous_strategy_positions[strategy_name] = prev_result.value();");
        const auto call = src.find("find_late_bars(");
        const auto finalize = src.find("PHASE 5: Finalizing Day T-1 PnL per-strategy");
        ASSERT_NE(books, npos);
        ASSERT_NE(finalize, npos);
        ASSERT_NE(call, npos) << "a late bar of a held symbol is lost without a line";
        EXPECT_GT(call, books) << "the held symbols are the stored T-1 books'";
        EXPECT_LT(call, finalize);
        EXPECT_NE(src.find("WARN(late_bar_warning_line(bar, pnl_manager->get_point_value(bar.symbol)));"),
                  npos)
            << "the line is not printed at WARNING with the symbol's point value";
        blocks.push_back(between(src, "// A LATE BAR (HD 2026-10-07", "// BOOK GATE (T-7a C4"));
    }
    ASSERT_EQ(blocks.size(), 2u);
    ASSERT_FALSE(blocks[0].empty());
    EXPECT_EQ(blocks[0], blocks[1]);
}
