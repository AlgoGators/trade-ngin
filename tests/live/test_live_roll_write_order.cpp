// tests/live/test_live_roll_write_order.cpp
//
// The roll write order: the parts that live in the two
// runners' main() and in the backtest's catch blocks, where no fixture can drive them: pinned on the
// source text of both twins. Their behaviour is proven by the replays (a run killed before the leg
// store, between the leg store and the Day T-1 re-write, and after it; a failing executions store on
// a roll day; an unplaced contract).
//
//   1   the day's executions, the ROLL legs first of all, are stored BEFORE the Day T-1 rows are
//       re-written: the stored row's contract is the state the legs are booked from, so a run
//       stopped anywhere leaves either that state or the legs a re-run reads;
//   3   a failed executions store while ROLL legs are owed is a ROLL_LEG STOP, exit 1, before any
//       position or result is written, and before any execution row of any sleeve;
//   4   what earlier runs booked of a late roll's moves is read from the stored rows;
//   7   the unplaced-contract refusal names its remedy and says that it repeats;
//   8   a stored book that cannot be read refuses the run (never skipped silently);
//   10  the backtest's exception paths stop on an owed roll as its error return does;
//   12  the two classifier-history ERROR lines carry the classifier's family.
//
// And: the arguments that would default to "nothing" (and so silently size, settle or hold on
// the raw maps) are required.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "trade_ngin/live/live_data_loader.hpp"
#include "trade_ngin/live/live_roll_legs.hpp"
#include "trade_ngin/live/live_sizing_read.hpp"
#include "trade_ngin/live/session_book_gate.hpp"
#include "trade_ngin/portfolio/sizing_capital.hpp"

using namespace trade_ngin;

namespace {

constexpr auto npos = std::string::npos;

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

const char* const kRunners[] = {"apps/strategies/live_portfolio_conservative.cpp",
                                "apps/strategies/live_portfolio.cpp"};

// The text between two markers, [from, to).
std::string between(const std::string& src, const std::string& from, const std::string& to) {
    const auto a = src.find(from);
    if (a == npos) return {};
    const auto b = src.find(to, a);
    if (b == npos) return {};
    return src.substr(a, b - a);
}

// Finding 9: whether a call with exactly these argument types compiles.
template <class... A>
constexpr bool kSettles = requires(A... a) { consumed_t1_settlement(a...); };
template <class... A>
constexpr bool kHolds = requires(A... a) { hold_non_session_symbols(a...); };
template <class... A>
constexpr bool kSizes = requires(A... a) { live_sizing_equity(a...); };
template <class... A>
constexpr bool kReadsSizing = requires(A... a) { read_live_sizing_equity(a...); };

using Closes = const std::unordered_map<std::string, double>&;
using Symbols = const std::unordered_set<std::string>&;
using PointValue = const std::function<double(const std::string&)>&;

}  // namespace

// 1. In both runners: PHASE 5 computes the finalized Day T-1 rows and writes none; the executions
// are stored (a sleeve with ROLL legs through the one-transaction replace); only then are the Day
// T-1 rows written, and the Day T rows after them.
TEST(LiveRollWriteOrder, TheDaysExecutionsAreStoredBeforeTheDayT1RowsAreReWritten) {
    for (const char* runner : kRunners) {
        const std::string src = read_source(runner);
        ASSERT_FALSE(src.empty()) << runner;
        const std::string phase5 = between(src, "// PHASE 5: PER-STRATEGY DAY T-1 FINALIZATION",
                                           "// STEP 2: CREATE TODAY'S (Day T) POSITIONS WITH ZERO PnL");
        ASSERT_FALSE(phase5.empty()) << runner;
        EXPECT_EQ(phase5.find("store_positions("), npos)
            << runner << ": PHASE 5 re-writes the Day T-1 rows before the legs are stored";
        EXPECT_NE(phase5.find("t1_position_rewrites.emplace_back(strategy_name,"), npos) << runner;

        const auto replace_at = src.find("db->replace_roll_day_executions(");
        const auto store_at = src.find("{{strategy_name, executions}}, combined_strategy_id, portfolio_id, now,");
        const auto t1_rows_at = src.find("db->store_positions(finalized_positions,");
        const auto day_t_rows_at = src.find("db->store_positions(strategy_positions_vec,");
        ASSERT_NE(replace_at, npos) << runner;
        ASSERT_NE(store_at, npos) << runner;
        ASSERT_NE(t1_rows_at, npos) << runner;
        ASSERT_NE(day_t_rows_at, npos) << runner;
        EXPECT_LT(replace_at, t1_rows_at) << runner << ": the legs are stored first";
        EXPECT_LT(store_at, t1_rows_at) << runner << ": and the day's other executions";
        EXPECT_LT(t1_rows_at, day_t_rows_at) << runner;
        EXPECT_EQ(src.find("db->store_positions(finalized_positions,", t1_rows_at + 1), npos)
            << runner << ": one write of the Day T-1 rows";
        // The write is the loop over what PHASE 5 computed.
        const std::string write = between(src, "for (const auto& [strategy_name, finalized_positions] : "
                                               "t1_position_rewrites) {",
                                          "======= Daily Position Report =======");
        EXPECT_NE(write.find("db->store_positions(finalized_positions,"), npos) << runner;
        // No sweep is committed apart from its store. Every sleeve's rows of the day
        // are replaced by the one-transaction call; the runners call no bare delete.
        EXPECT_EQ(src.find("db->delete_day_executions("), npos) << runner;
        EXPECT_EQ(src.find("db->delete_roll_executions("), npos) << runner;
        EXPECT_EQ(src.find("db->delete_stale_executions("), npos) << runner;
        EXPECT_EQ(src.find("db->store_executions("), npos) << runner;
    }
}

// 3. In both runners: the replace of the sleeves with ROLL legs that fails is a ROLL_LEG STOP that
// returns 1 before the Day T-1 rows, the Day T rows and the results. The
// sleeves with legs are stored FIRST, in name order, all in one transaction, so the STOP leaves no
// execution row of ANY sleeve of the run; the sweep and the stores of the sleeves without legs come
// after it. (A STOP returned from inside a loop over an unordered map of sleeves would leave a
// sleeve without legs iterated earlier with its executions already committed.)
TEST(LiveRollWriteOrder, AFailedExecutionsStoreWithRollLegsOwedStopsBeforePositionsAndResults) {
    for (const char* runner : kRunners) {
        const std::string src = read_source(runner);
        ASSERT_FALSE(src.empty()) << runner;
        const auto replace_at = src.find("db->replace_roll_day_executions(");
        ASSERT_NE(replace_at, npos) << runner;
        EXPECT_EQ(src.find("db->replace_roll_day_executions(\n                    leg_sleeves,", replace_at + 1), npos)
            << runner << ": one call, one transaction, for every sleeve with legs";
        const std::string leg_store = between(src, "db->replace_roll_day_executions(",
                                              "auto replace_sleeve_day = [&](const std::string& strategy_name,");
        ASSERT_FALSE(leg_store.empty()) << runner << ": the legs are stored before the other sleeves are swept";
        EXPECT_EQ(leg_store.find("leg_sleeves, combined_strategy_id, portfolio_id, now, \"trading.executions\", qt_write_book);"),
                  leg_store.find("leg_sleeves,"))
            << runner;
        EXPECT_NE(leg_store.find("leg_sleeves,"), npos) << runner;
        const auto failed_at = leg_store.find("if (replaced.is_error()) {");
        const auto stop_at = leg_store.find("ERROR(\"ROLL_LEG STOP: the day's executions could not be stored (\"");
        const auto exit_at = leg_store.find("return 1;");
        ASSERT_NE(failed_at, npos) << runner;
        ASSERT_NE(stop_at, npos) << runner;
        ASSERT_NE(exit_at, npos) << runner;
        EXPECT_LT(failed_at, stop_at) << runner;
        EXPECT_LT(stop_at, exit_at) << runner;
        EXPECT_NE(leg_store.find("ROLL legs are owed. Refusing to run: no execution, no position and no"), npos)
            << runner;
        // The batch: every sleeve with legs, sorted by name.
        const std::string batch = between(src, "std::vector<std::pair<std::string, std::vector<ExecutionReport>>> leg_sleeves;",
                                          "db->replace_roll_day_executions(");
        ASSERT_FALSE(batch.empty()) << runner;
        EXPECT_NE(batch.find("if (roll_legs_of(strategy_name) > 0) leg_sleeves.emplace_back(strategy_name, executions);"),
                  npos)
            << runner;
        EXPECT_NE(batch.find("std::sort(leg_sleeves.begin(), leg_sleeves.end(),"), npos) << runner;
        EXPECT_NE(batch.find("return a.first < b.first;"), npos) << runner;
        // Every other execution write of the run comes after the legs' store, and skips those sleeves.
        const auto other_at = src.find("{{strategy_name, executions}}, combined_strategy_id, portfolio_id, now,");
        ASSERT_NE(other_at, npos) << runner;
        EXPECT_LT(replace_at, other_at) << runner;
        const std::string others = between(src, "// Store executions for each strategy without ROLL legs",
                                           "replace_sleeve_day(strategy_name, executions);");
        EXPECT_NE(others.find("if (roll_legs_of(strategy_name) > 0) continue;"), npos) << runner;
        EXPECT_LT(replace_at, src.find("db->store_positions(finalized_positions,")) << runner;
        EXPECT_LT(replace_at,
                  src.find("// STEP 4: UPDATE Day T-1 live_results AND equity_curve WITH FINALIZED PnL"))
            << runner;
    }
}

// What earlier runs booked of a late roll's moves is read
// from the stored rows, in both runners, above the sizing read: one read of the rows dated after the
// stored state, late_booked_points per late symbol, the booked points added to settle_from. A
// failed read and rows that cannot decide are each a ROLL_LEG STOP that returns 1 (above the
// live_run_metadata row: nothing written), the second with its remedy.
TEST(LiveRollWriteOrder, ALateRollsBookedMovesAreReadFromTheStoredRows) {
    for (const char* runner : kRunners) {
        const std::string src = read_source(runner);
        ASSERT_FALSE(src.empty()) << runner;
        const std::string late = between(src, "if (!roll_state.late.empty()) {",
                                         "const std::vector<ConfirmedRoll>& confirmed_rolls");
        ASSERT_FALSE(late.empty()) << runner << ": the late sum is not read against the stored rows";
        const auto read_at = late.find("db->get_stored_realised_rows(");
        ASSERT_NE(read_at, npos) << runner;
        EXPECT_EQ(late.find("db->get_stored_realised_rows(", read_at + 1), npos) << runner << ": one read";
        EXPECT_NE(late.find("combined_strategy_id, portfolio_id, booked_after, t1_classification.t1_date,"), npos)
            << runner;
        const auto unreadable_at = late.find("if (stored_rows.is_error()) {");
        ASSERT_NE(unreadable_at, npos) << runner;
        EXPECT_NE(late.find("ERROR(\"ROLL_LEG STOP: the stored rows after \" + booked_after +", unreadable_at), npos)
            << runner;
        EXPECT_LT(late.find("return 1;", unreadable_at), late.find("late_booked_points(")) << runner;
        const auto undecided_at = late.find("if (!booked.undecided.empty()) {");
        ASSERT_NE(undecided_at, npos) << runner;
        EXPECT_NE(late.find("ERROR(\"ROLL_LEG STOP \" + symbol + \": the stored rows cannot tell what \"", undecided_at),
                  npos)
            << runner;
        EXPECT_NE(late.find("Remedy: correct daily_realized_pnl on the", undecided_at), npos) << runner;
        EXPECT_NE(late.find("Every run refuses until then.", undecided_at), npos) << runner;
        const auto applied_at = late.find("late.settle_from += booked.points;");
        ASSERT_NE(applied_at, npos) << runner;
        EXPECT_LT(late.find("return 1;", undecided_at), applied_at) << runner;
        EXPECT_NE(late.find("pnl_manager->get_point_value(symbol)"), npos) << runner;
        EXPECT_LT(src.find("late.settle_from += booked.points;"),
                  src.find("const ConsumedT1Settlement t1_settlement = consumed_t1_settlement("))
            << runner << ": the settlement, and the sizing read after it, take the corrected sum";
        EXPECT_LT(src.find("late.settle_from += booked.points;"), src.find("// STORE LIVE RUN METADATA")) << runner;
    }
}

// 7. The unplaced-contract refusal stays a ROLL_LEG STOP that returns 1, and its text names the
// remedy (restore the bars or their ids, or clear the row's instrument_id, after which the run
// legs only a roll its T-1 bar confirms) and says the refusal repeats until then.
TEST(LiveRollWriteOrder, TheUnplacedContractRefusalNamesItsRemedy) {
    for (const char* runner : kRunners) {
        const std::string src = read_source(runner);
        ASSERT_FALSE(src.empty()) << runner;
        const std::string refusal = between(src, "if (!roll_state.unplaced.empty()) {",
                                            "const std::vector<ConfirmedRoll>& confirmed_rolls");
        ASSERT_FALSE(refusal.empty()) << runner;
        EXPECT_NE(refusal.find("ROLL_LEG STOP: the contract recorded on the stored T-1 row is at no consumed"),
                  npos)
            << runner;
        EXPECT_NE(refusal.find("stored T-1 trading.positions row does not appear on the"), npos) << runner;
        EXPECT_NE(refusal.find("Remedy: restore the symbol's bars or their instrument ids"), npos) << runner;
        EXPECT_NE(refusal.find("instrument_id to NULL, after which the run legs only a roll its T-1 bar"), npos)
            << runner;
        EXPECT_NE(refusal.find("Every run refuses until then."), npos) << runner;
        EXPECT_NE(refusal.find("return 1;"), npos) << runner;
    }
}

// 8. A stored Day T-1 book that cannot be read is a ROLL_LEG STOP and a refusal (exit 1, above the
// live_run_metadata row: nothing written), never a sleeve skipped in silence.
TEST(LiveRollWriteOrder, AFailedStoredBookReadRefusesTheRun) {
    for (const char* runner : kRunners) {
        const std::string src = read_source(runner);
        ASSERT_FALSE(src.empty()) << runner;
        const std::string state = between(src, "LiveRollState roll_state;",
                                          "auto stored_legs = db->get_stored_roll_contracts(");
        ASSERT_FALSE(state.empty()) << runner;
        EXPECT_EQ(state.find("if (stored_book.is_error()) continue;"), npos)
            << runner << ": a failed read is skipped in silence";
        const auto failed_at = state.find("if (stored_book.is_error()) {");
        ASSERT_NE(failed_at, npos) << runner;
        const auto stop_at = state.find("ERROR(\"ROLL_LEG STOP: sleeve \" + sleeve + \"'s stored Day T-1 book is "
                                        "unreadable (\"",
                                        failed_at);
        ASSERT_NE(stop_at, npos) << runner;
        const auto exit_at = state.find("return 1;", stop_at);
        ASSERT_NE(exit_at, npos) << runner;
        EXPECT_LT(exit_at, state.find("for (const auto& [symbol, position] : stored_book.value()) {")) << runner;
        EXPECT_LT(src.find("LiveRollState roll_state;"), src.find("// STORE LIVE RUN METADATA")) << runner;
    }
}

// 12. The classifier-history load's two ERROR lines carry the live classifier's family
// (T1_CLASSIFIER), as every other line of the T-1 verdicts does.
TEST(LiveRollWriteOrder, TheClassifierHistoryErrorsCarryTheClassifierFamily) {
    for (const char* runner : kRunners) {
        const std::string src = read_source(runner);
        ASSERT_FALSE(src.empty()) << runner;
        const std::string load = between(src, "const Timestamp k01_history_start = k01_classifier_history_start(",
                                         "session_classifier.add_bars(k01_classifier_history(");
        ASSERT_FALSE(load.empty()) << runner;
        EXPECT_NE(load.find("ERROR(\"T1_CLASSIFIER history: failed to load the bars before the window: \""), npos)
            << runner;
        EXPECT_NE(load.find("ERROR(\"T1_CLASSIFIER history: failed to convert the bars before the window: \""),
                  npos)
            << runner;
        size_t errors = 0, in_family = 0;
        for (size_t at = load.find("ERROR(\""); at != npos; at = load.find("ERROR(\"", at + 1)) {
            ++errors;
            if (load.compare(at, 28, "ERROR(\"T1_CLASSIFIER history") == 0) ++in_family;
        }
        EXPECT_EQ(errors, 2u) << runner;
        EXPECT_EQ(in_family, errors) << runner << ": an ERROR line of the history load carries no family";
    }
}

// 10. The backtest: an exception while a roll is owed (the tracker has consumed the confirming bar,
// the legs are not booked yet) fails the run with a ROLL_LEG STOP, in the day's own catch and in
// the run loop's, exactly as the PortfolioManager's error return does; neither warns and goes on.
TEST(LiveRollWriteOrder, TheBacktestsExceptionPathsStopOnAnOwedRoll) {
    const std::string src = read_source("src/backtest/backtest_coordinator.cpp");
    ASSERT_FALSE(src.empty());
    // The run loop's catch.
    const std::string loop_catch = between(src, "if (process_result.is_error() && roll_leg_stop_) {",
                                           "// Save positions daily if storage is enabled");
    ASSERT_FALSE(loop_catch.empty());
    const auto catch_at = loop_catch.find("} catch (const std::exception& e) {");
    ASSERT_NE(catch_at, npos);
    const auto check_at = loop_catch.find("roll_owed_stop(e.what()); stop.is_error() || roll_leg_stop_", catch_at);
    const auto fail_at = loop_catch.find("return make_error<BacktestResults>(", catch_at);
    const auto warn_at = loop_catch.find("WARN(\"Exception processing portfolio data: \"", catch_at);
    ASSERT_NE(check_at, npos) << "the run loop's catch does not test an owed roll";
    ASSERT_NE(fail_at, npos);
    ASSERT_NE(warn_at, npos);
    EXPECT_LT(check_at, fail_at);
    EXPECT_LT(fail_at, warn_at) << "the stop comes before the warning that lets the run go on";
    // The day's catch.
    const auto day_at = src.find("Result<void> BacktestCoordinator::process_portfolio_day(");
    ASSERT_NE(day_at, npos);
    const auto day_catch_at = src.find("if (auto stop = roll_owed_stop(e.what()); stop.is_error()) return stop;", day_at);
    const auto day_error_at = src.find("std::string(\"Error processing portfolio data: \") + e.what()", day_at);
    ASSERT_NE(day_catch_at, npos) << "the day's catch does not test an owed roll";
    ASSERT_NE(day_error_at, npos);
    EXPECT_LT(day_catch_at, day_error_at);
    // The owed rolls are recorded when the tracker has consumed the confirming bars and cleared
    // when the legs are booked; the stop sets roll_leg_stop_, which fails the run.
    const auto owed_at = src.find("cycle_rolls_owed_.push_back(symbol + \" (\" + strategy_id + \") confirmed \" +", day_at);
    const auto legs_at = src.find("portfolio->insert_executions_at(strategy_id, at, legs);", day_at);
    const auto cleared_at = src.find("cycle_rolls_owed_.clear();  // booked", day_at);
    ASSERT_NE(owed_at, npos);
    ASSERT_NE(legs_at, npos);
    ASSERT_NE(cleared_at, npos);
    EXPECT_LT(owed_at, src.find("auto data_result = portfolio->process_market_data(*signal_feed, is_warmup,", day_at));
    EXPECT_LT(legs_at, cleared_at);
    const std::string stop = between(src, "Result<void> BacktestCoordinator::roll_owed_stop(", "\n}\n");
    EXPECT_NE(stop.find("roll_leg_stop_ = true;"), npos);
    EXPECT_NE(stop.find("\"ROLL_LEG STOP \" + owed + \": the cycle failed (\""), npos);
}

// 9. The settlement's raw T-1 map, the sizing reads' zero-settlement set and the hold's change-bar
// set are required arguments. Each defaulted to an empty container, and a caller that left it out
// compiled and silently settled on an empty T-1 map, sized on bars that settle 0, or traded a
// change bar on a day without a cut.
TEST(NoSilentDefaults, TheSettlementSizingAndHoldInputsAreRequired) {
    using Bars = const std::vector<Bar>&;
    using Status = const std::unordered_map<std::string, roll_series::RollTracker::Status>&;
    using Names = const std::vector<std::string>&;
    using Late = const std::unordered_map<std::string, LateRollSettlement>&;
    EXPECT_FALSE((kSettles<Bars, const std::string&, Status, Names, Closes>))
        << "consumed_t1_settlement compiles without the raw T-1 map";
    EXPECT_FALSE((kSettles<Bars, const std::string&, Status, Names, Closes, Closes>))
        << "consumed_t1_settlement compiles without the late rolls (commit 6)";
    EXPECT_TRUE((kSettles<Bars, const std::string&, Status, Names, Closes, Closes, Late>));

    EXPECT_FALSE((kHolds<StrategyBooks&, const StrategyBooks&, const T1Classification&, const Timestamp&>))
        << "hold_non_session_symbols compiles without the change-bar set";
    EXPECT_TRUE((kHolds<StrategyBooks&, const StrategyBooks&, const T1Classification&, const Timestamp&, Symbols>));

    using Books = const std::vector<std::unordered_map<std::string, Position>>&;
    EXPECT_FALSE((kSizes<bool, double, double, Books, Closes, Closes, PointValue>))
        << "live_sizing_equity compiles without the zero-settlement set";
    EXPECT_TRUE((kSizes<bool, double, double, Books, Closes, Closes, PointValue, Symbols>));

    EXPECT_FALSE((kReadsSizing<LiveDataLoader&, DatabaseInterface&, const std::string&, const std::string&, Names,
                               const Timestamp&, double, Closes, Closes, PointValue, const LiveSizingCalendar&>))
        << "read_live_sizing_equity compiles without the zero-settlement set";
    EXPECT_FALSE((kReadsSizing<LiveDataLoader&, DatabaseInterface&, const std::string&, const std::string&, Names,
                               const Timestamp&, double, Closes, Closes, PointValue, Symbols>))
        << "read_live_sizing_equity compiles without the bar calendar its settled test reads";
    EXPECT_TRUE((kReadsSizing<LiveDataLoader&, DatabaseInterface&, const std::string&, const std::string&, Names,
                              const Timestamp&, double, Closes, Closes, PointValue, Symbols,
                              const LiveSizingCalendar&>));
}

// With migration 040, each runner attaches a per-contract roll's split to the legs
// it just made, from the roll's points and the contract's multiplier (the one the P&L reads), before the
// legs are logged and inserted, so the stored closing leg carries it; a roll with no points (the vendor's
// series) attaches nothing.
TEST(LiveRollWriteOrder, EachRunnerAttachesTheRollSplitToTheLegsItMakes) {
    for (const char* runner : kRunners) {
        const std::string src = read_source(runner);
        ASSERT_FALSE(src.empty()) << runner;
        const std::string block = between(src, "legs = roll_series::make_roll_legs(",
                                          "roll_legs.insert(roll_legs.end(), legs.begin(), legs.end());");
        ASSERT_FALSE(block.empty()) << runner;
        const auto gate = block.find("if (r.roll_bar_points) {");
        ASSERT_NE(gate, npos) << runner;
        const auto pv = block.find("const double point_value = pnl_manager->get_point_value(r.symbol);", gate);
        ASSERT_NE(pv, npos) << runner;
        const auto attach = block.find("roll_series::attach_roll_split(legs, q, *r.roll_bar_points, point_value);", pv);
        ASSERT_NE(attach, npos) << runner;
        EXPECT_LT(attach, block.find("for (const auto& leg : legs) {")) << runner << ": before the legs are logged";
        EXPECT_NE(block.find("INFO(\"ROLL_SPLIT \" + strategy_name", attach), npos) << runner;
    }
}

// With the source on, each runner sets the rule's log floor to its T-1 (the bar a
// run consumes for the first time; runs go on consecutive days) right after configuring the source.
TEST(LiveRollWriteOrder, EachRunnerLogsTheRulesFallbacksFromItsT1) {
    for (const char* runner : kRunners) {
        const std::string src = read_source(runner);
        ASSERT_FALSE(src.empty()) << runner;
        const auto configured = src.find("contract_rolls::configure_per_contract_source(");
        ASSERT_NE(configured, npos) << runner;
        const auto floor = src.find("const std::string log_floor = core::format_utc_date(now - std::chrono::hours(24));",
                                    configured);
        ASSERT_NE(floor, npos) << runner;
        EXPECT_NE(src.find("PerContractSource::instance().set_log_floor(contract_rolls::parse_ymd(log_floor));", floor),
                  npos)
            << runner;
        EXPECT_LT(floor, src.find("auto symbols_result = db->get_symbols(")) << runner << ": before any bar is loaded";
    }
}

// With the source on, each runner hands the report the roll footnote's symbols and the roll view,
// asked through report_roll_query (executed by test_report_roll_query.cpp) with the run's roll status.
// A source guard: it reads the runners' text.
TEST(LiveRollWriteOrder, EachRunnerHandsTheReportItsRollView) {
    for (const char* runner : kRunners) {
        const std::string src = read_source(runner);
        ASSERT_FALSE(src.empty()) << runner;
        const std::string block = between(src, "report_context.settled_zero_kinds.withheld = t1_settlement.withheld_zero_symbols;",
                                          "if (has_today_row_metrics) {");
        ASSERT_FALSE(block.empty()) << runner;
        EXPECT_NE(block.find("if (contract_rolls::PerContractSource::instance().enabled()) {"), npos) << runner;
        EXPECT_NE(block.find("report_context.settled_zero_kinds.per_contract_roll =\n"
                             "                            t1_settlement.per_contract_roll_symbols;"), npos) << runner;
        EXPECT_NE(block.find("report_context.rolls = db->report_roll_view("), npos) << runner;
        EXPECT_NE(block.find("report_roll_query(yesterday_date_for_email, positions, strategy_positions_map,"), npos)
            << runner;
        EXPECT_NE(block.find("all_strategy_executions, roll_status),"), npos) << runner;
        EXPECT_NE(block.find("report_tick_size);"), npos) << runner;
    }
}

// Both live runners book no ROLL legs and owe none for a micro's change of price
// source (ConfirmedRoll::price_source_change): the leg loop and the owed-roll stop both skip it, and the run
// logs it as a PRICE_SOURCE_CHANGE line with its gap instead of ROLL_CONFIRMED.
TEST(LiveRollWriteOrder, AChangeOfPriceSourceIsNeitherLeggedNorOwed) {
    for (const char* runner : {"apps/strategies/live_portfolio.cpp", "apps/strategies/live_portfolio_conservative.cpp"}) {
        const std::string src = read_source(runner);
        ASSERT_FALSE(src.empty()) << runner;
        EXPECT_NE(src.find("                    for (const auto& r : confirmed_rolls) {\n"
                           "                        if (r.price_source_change) continue;  // no legs, no cost\n"
                           "                        const auto held = prev_positions_map.find(r.symbol);"),
                  std::string::npos)
            << runner << ": the leg loop skips it";
        EXPECT_NE(src.find("                for (const auto& r : confirmed_rolls) {\n"
                           "                    if (r.price_source_change) continue;  // nothing to leg"),
                  std::string::npos)
            << runner << ": the F-3 stop does not wait for its legs";
        const size_t logged = src.find("INFO(\"PRICE_SOURCE_CHANGE \" + r.symbol");
        const size_t confirmed = src.find("INFO(\"ROLL_CONFIRMED \" + r.symbol");
        ASSERT_NE(logged, std::string::npos) << runner;
        EXPECT_LT(logged, confirmed) << runner << ": logged in the same loop, before the rolls' line";
    }
}
