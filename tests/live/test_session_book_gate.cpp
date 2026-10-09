// tests/live/test_session_book_gate.cpp
//
// T-7a commit 4, the live futures runners' half of the per-symbol session rule
// (include/trade_ngin/live/session_book_gate.hpp):
//   * a symbol whose T-1 verdict is not SESSION is HELD at its stored T-1 quantity on every
//     per-strategy book, the key being the verdict and never membership of the T-1 price map (a
//     JUNK symbol has a T-1 price); a held symbol absent from today's target is re-inserted;
//   * executions are generated with PricingPolicy::STRICT, an unpriced symbol is rolled back to its
//     stored row, and no book change may be left without a price;
//   * K-01: every withheld bar of the window (JUNK, a thin first print) is kept out of the strategy
//     feed on every run, and an unconfirmed id change the classifier holds is consumed;
//   * a HELD symbol's feed hole older than the tolerance refuses a true-live run (the run date is
//     the host's date) and only warns on a replay;
//   * in the runners: the abort arm is gone, the Monday agricultural block is gone, the gate and
//     STRICT sit where they must, and the twins carry the same blocks byte for byte.
//
// The worked case is MYM.v.0 on 2026-04-24 (T-4c J1): the strategy wanted 2, the book held 1, and
// MYM had no bar after Mon 04-20 while its siblings printed. Today's runner filled BUY 1 at the
// four-day-old 49,707 through MARK_FALLBACK; the gate holds it at 1 with no order.

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

#include "trade_ngin/data/session_classifier.hpp"
#include "trade_ngin/live/execution_manager.hpp"
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

/// 60 weekdays of history for each symbol up to (not including) `through_exclusive`.
void weekday_history(SessionClassifier& c, const std::string& symbol,
                     const std::string& through_exclusive, double close, double volume) {
    Day d = ymd_day(through_exclusive);
    int n = 0;
    while (n < 60) {
        d -= std::chrono::days{1};
        const std::chrono::weekday wd{d};
        if (wd == std::chrono::Saturday || wd == std::chrono::Sunday) continue;
        c.add_bar(bar_on(symbol, SessionClassifier::ymd(d), close, volume));
        ++n;
    }
}

/// T-1 = Thu 2026-04-23. MES prints (SESSION), MYM's last bar is Mon 04-20 (feed hole, age 3),
/// 6L prints 900 lots against a 17,000 norm (JUNK), ZC prints (SESSION).
T1Classification april_t1() {
    SessionClassifier c;
    weekday_history(c, "MES.v.0", "2026-04-23", 7100.0, 1500000);
    weekday_history(c, "MYM.v.0", "2026-04-21", 49000.0, 90000);
    weekday_history(c, "6L.v.0", "2026-04-23", 0.199, 17000);
    weekday_history(c, "ZC.v.0", "2026-04-23", 460.0, 150000);
    c.add_bar(bar_on("MES.v.0", "2026-04-23", 7150.0, 1400000));
    c.add_bar(bar_on("6L.v.0", "2026-04-23", 0.1992, 107));
    c.add_bar(bar_on("ZC.v.0", "2026-04-23", 462.0, 140000));
    return classify_t1(c, {"MES.v.0", "MYM.v.0", "6L.v.0", "ZC.v.0"}, ymd_day("2026-04-23"),
                       kNoHolidays);
}

Timestamp run_instant() { return Timestamp(ymd_day("2026-04-24")) + std::chrono::hours(4); }

// Runner source
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
// The hold
// =============================================================================================

TEST(SessionBookGate, TheAprilT1IsClassifiedAsTheWorkedCaseSays) {
    const auto t1 = april_t1();
    EXPECT_EQ(t1.find("MES.v.0")->verdict, SessionVerdict::SESSION);
    EXPECT_EQ(t1.find("MYM.v.0")->verdict, SessionVerdict::NO_BAR_FEED_HOLE);
    EXPECT_EQ(t1.find("MYM.v.0")->hole_age_days, 3);
    EXPECT_EQ(t1.find("6L.v.0")->verdict, SessionVerdict::JUNK);
    EXPECT_EQ(t1.find("ZC.v.0")->verdict, SessionVerdict::SESSION);
}

TEST(SessionBookGate, AFeedHoleSymbolIsHeldAndNotFilledAtItsStaleMark) {
    const auto t1 = april_t1();
    const std::unordered_map<std::string, double> t1_prices{
        {"MES.v.0", 7150.0}, {"6L.v.0", 0.1992}, {"ZC.v.0", 462.0}};  // no MYM
    StrategyBooks prev{{"TREND_FOLLOWING", {{"MYM.v.0", pos("MYM.v.0", 1, 49707)},
                                            {"MES.v.0", pos("MES.v.0", 2, 7100)}}}};

    // Today's path without the gate: MARK_FALLBACK fills BUY 1 MYM at the stale 49,707.
    {
        StrategyBooks today{{"TREND_FOLLOWING", {{"MYM.v.0", pos("MYM.v.0", 2, 49707)},
                                                 {"MES.v.0", pos("MES.v.0", 3, 7150)}}}};
        ExecutionManager em;
        auto r = em.generate_daily_executions(today["TREND_FOLLOWING"], prev["TREND_FOLLOWING"],
                                              t1_prices, run_instant());
        ASSERT_TRUE(r.is_ok());
        bool stale_fill = false;
        for (const auto& e : r.value()) {
            if (e.symbol == "MYM.v.0") stale_fill = static_cast<double>(e.fill_price) == 49707.0;
        }
        EXPECT_TRUE(stale_fill) << "the parent's MARK_FALLBACK fill at a four-day-old mark";
    }

    StrategyBooks today{{"TREND_FOLLOWING", {{"MYM.v.0", pos("MYM.v.0", 2, 49707)},
                                             {"MES.v.0", pos("MES.v.0", 3, 7150)}}}};
    const auto holds = hold_non_session_symbols(today, prev, t1, run_instant(), {});
    ASSERT_EQ(holds.size(), 1u);
    EXPECT_EQ(holds[0].symbol, "MYM.v.0");
    EXPECT_EQ(holds[0].verdict, SessionVerdict::NO_BAR_FEED_HOLE);
    EXPECT_DOUBLE_EQ(holds[0].held_quantity, 1.0);
    EXPECT_DOUBLE_EQ(holds[0].target_quantity, 2.0);
    EXPECT_DOUBLE_EQ(today["TREND_FOLLOWING"]["MYM.v.0"].quantity.as_double(), 1.0);
    EXPECT_DOUBLE_EQ(static_cast<double>(today["TREND_FOLLOWING"]["MYM.v.0"].average_price),
                     49707.0)
        << "the held row keeps its last mark";
    EXPECT_DOUBLE_EQ(today["TREND_FOLLOWING"]["MES.v.0"].quantity.as_double(), 3.0)
        << "every other symbol trades normally";

    ExecutionManager em;
    auto r = execute_strategy_day_strict(em, today["TREND_FOLLOWING"], prev["TREND_FOLLOWING"],
                                         t1_prices, run_instant());
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().unpriced.empty());
    ASSERT_EQ(r.value().executions.size(), 1u);
    EXPECT_EQ(r.value().executions[0].symbol, "MES.v.0");
    EXPECT_TRUE(unpriced_book_changes(today, prev, t1_prices).empty());
}

TEST(SessionBookGate, AJunkSymbolIsHeldAlthoughItHasAT1Price) {
    // The whole point of keying on the verdict (T-CLASSIFIER_ADVERSARIAL D1): 6L's junk print IS
    // in the T-1 price map, so a gate keyed on the map would trade it at the junk close.
    const auto t1 = april_t1();
    const std::unordered_map<std::string, double> t1_prices{
        {"MES.v.0", 7150.0}, {"6L.v.0", 0.1992}, {"ZC.v.0", 462.0}};
    StrategyBooks prev{{"TREND_FOLLOWING", {{"6L.v.0", pos("6L.v.0", 2, 0.199)}}}};
    StrategyBooks today{{"TREND_FOLLOWING", {{"6L.v.0", pos("6L.v.0", 3, 0.199)}}}};
    const auto holds = hold_non_session_symbols(today, prev, t1, run_instant(), {});
    ASSERT_EQ(holds.size(), 1u);
    EXPECT_EQ(holds[0].verdict, SessionVerdict::JUNK);
    EXPECT_DOUBLE_EQ(today["TREND_FOLLOWING"]["6L.v.0"].quantity.as_double(), 2.0);
    ExecutionManager em;
    auto r = execute_strategy_day_strict(em, today["TREND_FOLLOWING"], prev["TREND_FOLLOWING"],
                                         t1_prices, run_instant());
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().executions.empty()) << "no order at the junk print";
}

// D-A (T-ROLLX-FIX commit 4; LOOP_SPEC v6.2 section 6.1, D37): the change-bar hold on EVERY
// rebalance. ZC's T-1 bar (Thu 04-23) is a full-volume contract switch: its verdict is SESSION and
// its last consumed bar is a pending change. On a day the risk gate does not cut, nothing but this
// hold stands between the strategy's new target and an order at the change bar's close. The runner
// passes the D37 set (roll_status holds()) to the book gate: ZC is held at its stored quantity on
// both sleeves, opened from flat on neither, re-inserted where the target dropped it, and no order
// is generated; MES, a SESSION symbol with no pending change, trades.
TEST(SessionBookGate, ASessionChangeBarIsHeldOnADayWithoutACut) {
    const auto t1 = april_t1();
    ASSERT_TRUE(t1.is_session("ZC.v.0")) << "the verdict alone would let it trade";
    auto zc = [](const std::string& date, double close, const std::string& id) {
        Bar b = bar_on("ZC.v.0", date, close, 150000);
        b.instrument_id = id;
        return b;
    };
    const auto status = roll_series::roll_status_of(
        {zc("2026-04-21", 459.0, "ZCK6"), zc("2026-04-22", 460.0, "ZCK6"), zc("2026-04-23", 462.0, "ZCN6")});
    std::unordered_set<std::string> change_bar_holds;
    for (const auto& [symbol, st] : status) {
        if (st.holds()) change_bar_holds.insert(symbol);
    }
    ASSERT_EQ(change_bar_holds.count("ZC.v.0"), 1u);

    const std::unordered_map<std::string, double> t1_prices{{"MES.v.0", 7150.0}, {"ZC.v.0", 462.0}};
    StrategyBooks prev{{"TREND_FOLLOWING", {{"ZC.v.0", pos("ZC.v.0", 2, 460.0)}, {"MES.v.0", pos("MES.v.0", 1, 7100.0)}}},
                       {"TREND_FOLLOWING_FAST", {{"ZC.v.0", pos("ZC.v.0", 1, 460.0)}}},
                       {"FLAT_SLEEVE", {}}};
    StrategyBooks today{{"TREND_FOLLOWING", {{"ZC.v.0", pos("ZC.v.0", 3, 462.0)}, {"MES.v.0", pos("MES.v.0", 2, 7150.0)}}},
                        {"TREND_FOLLOWING_FAST", {}},
                        {"FLAT_SLEEVE", {{"ZC.v.0", pos("ZC.v.0", 1, 462.0)}}}};
    const auto holds = hold_non_session_symbols(today, prev, t1, run_instant(), change_bar_holds);
    ASSERT_EQ(holds.size(), 3u);
    for (const auto& h : holds) {
        EXPECT_EQ(h.symbol, "ZC.v.0");
        EXPECT_TRUE(h.change_bar) << "held by the change-bar rule, its T-1 a SESSION";
    }
    EXPECT_DOUBLE_EQ(today["TREND_FOLLOWING"]["ZC.v.0"].quantity.as_double(), 2.0);
    EXPECT_DOUBLE_EQ(today["TREND_FOLLOWING_FAST"]["ZC.v.0"].quantity.as_double(), 1.0) << "re-inserted";
    EXPECT_DOUBLE_EQ(today["FLAT_SLEEVE"]["ZC.v.0"].quantity.as_double(), 0.0) << "never opened from flat";
    EXPECT_DOUBLE_EQ(today["TREND_FOLLOWING"]["MES.v.0"].quantity.as_double(), 2.0) << "MES trades";
    for (const std::string sleeve : {"TREND_FOLLOWING", "TREND_FOLLOWING_FAST", "FLAT_SLEEVE"}) {
        ExecutionManager em;
        auto r = execute_strategy_day_strict(em, today[sleeve], prev[sleeve], t1_prices, run_instant());
        ASSERT_TRUE(r.is_ok());
        for (const auto& e : r.value().executions) EXPECT_NE(e.symbol, "ZC.v.0") << "no order on the change bar";
    }
}

TEST(SessionBookGate, EveryPerStrategyBookIsHeldEachAtItsOwnStoredQuantity) {
    const auto t1 = april_t1();
    StrategyBooks prev{{"TREND_FOLLOWING", {{"MYM.v.0", pos("MYM.v.0", 1, 49707)}}},
                       {"TREND_FOLLOWING_FAST", {{"MYM.v.0", pos("MYM.v.0", -2, 49707)}}}};
    StrategyBooks today{{"TREND_FOLLOWING", {{"MYM.v.0", pos("MYM.v.0", 3, 49707)}}},
                        {"TREND_FOLLOWING_FAST", {{"MYM.v.0", pos("MYM.v.0", 0, 49707)}}}};
    const auto holds = hold_non_session_symbols(today, prev, t1, run_instant(), {});
    ASSERT_EQ(holds.size(), 2u);
    EXPECT_EQ(holds[0].strategy_name, "TREND_FOLLOWING");
    EXPECT_EQ(holds[1].strategy_name, "TREND_FOLLOWING_FAST");
    EXPECT_DOUBLE_EQ(today["TREND_FOLLOWING"]["MYM.v.0"].quantity.as_double(), 1.0);
    EXPECT_DOUBLE_EQ(today["TREND_FOLLOWING_FAST"]["MYM.v.0"].quantity.as_double(), -2.0);
    std::unordered_map<std::string, Position> combined;
    rebuild_combined_positions(combined, today);
    EXPECT_DOUBLE_EQ(combined["MYM.v.0"].quantity.as_double(), -1.0);
}

TEST(SessionBookGate, AHeldSymbolAbsentFromTodaysTargetIsReinsertedNotClosedOut) {
    const auto t1 = april_t1();
    StrategyBooks prev{{"TREND_FOLLOWING", {{"MYM.v.0", pos("MYM.v.0", 1, 49707)}}}};
    StrategyBooks today{{"TREND_FOLLOWING", {}}};
    const auto holds = hold_non_session_symbols(today, prev, t1, run_instant(), {});
    ASSERT_EQ(holds.size(), 1u);
    EXPECT_TRUE(holds[0].reinserted);
    ASSERT_TRUE(today["TREND_FOLLOWING"].count("MYM.v.0"));
    EXPECT_DOUBLE_EQ(today["TREND_FOLLOWING"]["MYM.v.0"].quantity.as_double(), 1.0);
    EXPECT_EQ(today["TREND_FOLLOWING"]["MYM.v.0"].last_update, run_instant());
    ExecutionManager em;
    auto r = execute_strategy_day_strict(em, today["TREND_FOLLOWING"], prev["TREND_FOLLOWING"],
                                         {{"MES.v.0", 7150.0}}, run_instant());
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().executions.empty()) << "no close-out at a stale mark";
}

TEST(SessionBookGate, APositionOpenedFromFlatWithoutASessionStaysFlat) {
    // L1/L2 of T-4c: ZC and ZM on Mon 2026-03-02 were opened from flat at Friday's close; the
    // Monday agricultural block only reverted a symbol with a stored row.
    SessionClassifier c;
    weekday_history(c, "ZC.v.0", "2026-03-02", 448.25, 150000);
    const auto t1 = classify_t1(c, {"ZC.v.0"}, ymd_day("2026-03-01"), kNoHolidays);
    ASSERT_EQ(t1.find("ZC.v.0")->verdict, SessionVerdict::NO_BAR_CLOSURE);
    StrategyBooks prev{{"TREND_FOLLOWING", {}}};
    StrategyBooks today{{"TREND_FOLLOWING", {{"ZC.v.0", pos("ZC.v.0", 1, 448.25)}}}};
    const auto holds = hold_non_session_symbols(today, prev, t1, run_instant(), {});
    ASSERT_EQ(holds.size(), 1u);
    EXPECT_DOUBLE_EQ(today["TREND_FOLLOWING"]["ZC.v.0"].quantity.as_double(), 0.0);
}

TEST(SessionBookGate, AnUnchangedHeldSymbolAndASessionSymbolAreLeftAlone) {
    const auto t1 = april_t1();
    StrategyBooks prev{{"TREND_FOLLOWING", {{"MYM.v.0", pos("MYM.v.0", 1, 49707)},
                                            {"ZC.v.0", pos("ZC.v.0", 1, 460)}}}};
    StrategyBooks today{{"TREND_FOLLOWING", {{"MYM.v.0", pos("MYM.v.0", 1, 49707)},
                                             {"ZC.v.0", pos("ZC.v.0", 4, 462)}}}};
    EXPECT_TRUE(hold_non_session_symbols(today, prev, t1, run_instant(), {}).empty());
    EXPECT_DOUBLE_EQ(today["TREND_FOLLOWING"]["ZC.v.0"].quantity.as_double(), 4.0);
}

TEST(SessionBookGate, ASymbolNobodyClassifiedIsHeld) {
    const auto t1 = april_t1();
    StrategyBooks prev{{"TREND_FOLLOWING", {}}};
    StrategyBooks today{{"TREND_FOLLOWING", {{"NEW.v.0", pos("NEW.v.0", 1, 10)}}}};
    const auto holds = hold_non_session_symbols(today, prev, t1, run_instant(), {});
    ASSERT_EQ(holds.size(), 1u);
    EXPECT_DOUBLE_EQ(today["TREND_FOLLOWING"]["NEW.v.0"].quantity.as_double(), 0.0);
}

// =============================================================================================
// STRICT, the rollback and the assertion
// =============================================================================================

TEST(SessionBookGate, StrictRollsAnUnpricedChangeBackToTheStoredRow) {
    // Bypass the gate: a changed symbol with no T-1 price reaches the execution step.
    StrategyBooks prev{{"TREND_FOLLOWING", {{"MYM.v.0", pos("MYM.v.0", 1, 49707)},
                                            {"PL.v.0", pos("PL.v.0", 1, 1000)}}}};
    StrategyBooks today{{"TREND_FOLLOWING",
                         {{"MYM.v.0", pos("MYM.v.0", 2, 49707)}, {"GC.v.0", pos("GC.v.0", 1, 4000)}}}};
    // PL absent today (a close-out) and unpriced; GC opened from flat, unpriced.
    const std::unordered_map<std::string, double> prices{{"MES.v.0", 7150.0}};
    EXPECT_EQ(unpriced_book_changes(today, prev, prices),
              (std::vector<std::string>{"TREND_FOLLOWING/GC.v.0", "TREND_FOLLOWING/MYM.v.0",
                                        "TREND_FOLLOWING/PL.v.0"}));
    ExecutionManager em;
    auto r = execute_strategy_day_strict(em, today["TREND_FOLLOWING"], prev["TREND_FOLLOWING"],
                                         prices, run_instant());
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().executions.empty()) << "STRICT never fills at a mark";
    EXPECT_EQ(r.value().unpriced.size(), 3u);
    EXPECT_EQ(r.value().rolled_back.size(), 3u);
    EXPECT_DOUBLE_EQ(today["TREND_FOLLOWING"]["MYM.v.0"].quantity.as_double(), 1.0);
    EXPECT_DOUBLE_EQ(today["TREND_FOLLOWING"]["PL.v.0"].quantity.as_double(), 1.0);
    EXPECT_EQ(today["TREND_FOLLOWING"].count("GC.v.0"), 0u) << "never held: dropped";
    EXPECT_TRUE(unpriced_book_changes(today, prev, prices).empty())
        << "after the rollback no book change is left without a price";
}

// LOOP_SPEC section 5.2: a forecast-sign close is its OWN fill to flat, stored ahead of the symbol's
// move to today's quantity (exec_id and order_id suffixed _SC); every other symbol trades once.
TEST(SessionBookGate, AForecastSignCloseIsBookedAsItsOwnFillAheadOfTheMove) {
    StrategyBooks prev{{"TREND_FOLLOWING", {{"MBT.v.0", pos("MBT.v.0", 1, 76000)},
                                            {"MES.v.0", pos("MES.v.0", 2, 7100)}}}};
    StrategyBooks today{{"TREND_FOLLOWING", {{"MBT.v.0", pos("MBT.v.0", -1, 76035)},
                                             {"MES.v.0", pos("MES.v.0", 3, 7150)}}}};
    const std::unordered_map<std::string, double> prices{{"MBT.v.0", 76035.0}, {"MES.v.0", 7150.0}};
    ExecutionManager em;
    auto r = execute_strategy_day_strict(em, today["TREND_FOLLOWING"], prev["TREND_FOLLOWING"],
                                         prices, run_instant(), {{"MBT.v.0", -1.0}});
    ASSERT_TRUE(r.is_ok());
    const auto& execs = r.value().executions;
    ASSERT_EQ(execs.size(), 3u) << "the close, the move from flat, and MES's one fill";
    EXPECT_TRUE(is_sign_close(execs[0]));
    EXPECT_EQ(execs[0].symbol, "MBT.v.0");
    EXPECT_EQ(execs[0].side, Side::SELL);
    EXPECT_DOUBLE_EQ(static_cast<double>(execs[0].filled_quantity), 1.0);
    EXPECT_DOUBLE_EQ(static_cast<double>(execs[0].fill_price), 76035.0);
    EXPECT_EQ(execs[0].exec_id.substr(execs[0].exec_id.size() - 3), "_SC");
    int mbt_moves = 0, mes_moves = 0;
    for (size_t k = 1; k < execs.size(); ++k) {
        EXPECT_FALSE(is_sign_close(execs[k]));
        if (execs[k].symbol == "MBT.v.0") {
            ++mbt_moves;
            EXPECT_EQ(execs[k].side, Side::SELL);
            EXPECT_DOUBLE_EQ(static_cast<double>(execs[k].filled_quantity), 1.0)
                << "from flat to -1, not one fill of two";
            EXPECT_EQ(execs[k].exec_id + "_SC", execs[0].exec_id)
                << "the two fills share the day's id, the close suffixed";
        } else {
            ++mes_moves;
            EXPECT_DOUBLE_EQ(static_cast<double>(execs[k].filled_quantity), 1.0);
        }
    }
    EXPECT_EQ(mbt_moves, 1);
    EXPECT_EQ(mes_moves, 1);
    EXPECT_TRUE(r.value().unpriced.empty());
    EXPECT_DOUBLE_EQ(today["TREND_FOLLOWING"]["MBT.v.0"].quantity.as_double(), -1.0);

    // With no sign close the step is the plain one: MBT trades once, two contracts.
    StrategyBooks today2{{"TREND_FOLLOWING", {{"MBT.v.0", pos("MBT.v.0", -1, 76035)},
                                              {"MES.v.0", pos("MES.v.0", 3, 7150)}}}};
    ExecutionManager em2;
    auto plain = execute_strategy_day_strict(em2, today2["TREND_FOLLOWING"], prev["TREND_FOLLOWING"],
                                             prices, run_instant(), {});
    ASSERT_TRUE(plain.is_ok());
    ASSERT_EQ(plain.value().executions.size(), 2u);
    for (const auto& e : plain.value().executions) {
        EXPECT_FALSE(is_sign_close(e));
        if (e.symbol == "MBT.v.0") EXPECT_DOUBLE_EQ(static_cast<double>(e.filled_quantity), 2.0);
    }

    // A close that cannot be priced did not happen: the symbol goes back to its stored row.
    StrategyBooks today3{{"TREND_FOLLOWING", {{"MBT.v.0", pos("MBT.v.0", -1, 76035)},
                                              {"MES.v.0", pos("MES.v.0", 3, 7150)}}}};
    ExecutionManager em3;
    auto unpriced = execute_strategy_day_strict(em3, today3["TREND_FOLLOWING"], prev["TREND_FOLLOWING"],
                                                {{"MES.v.0", 7150.0}}, run_instant(),
                                                {{"MBT.v.0", -1.0}});
    ASSERT_TRUE(unpriced.is_ok());
    for (const auto& e : unpriced.value().executions) EXPECT_NE(e.symbol, "MBT.v.0");
    EXPECT_DOUBLE_EQ(today3["TREND_FOLLOWING"]["MBT.v.0"].quantity.as_double(), 1.0);
    EXPECT_EQ(r.value().rolled_back.size(), 0u);
    EXPECT_EQ(unpriced.value().rolled_back, (std::vector<std::string>{"MBT.v.0"}));
}

// =============================================================================================
// The JUNK feed and the feed-hole refusal
// =============================================================================================

// LOOP_SPEC v6.1 section 2.1 (K-01): the runners feed the window's CONSUMED bars. A JUNK bar is
// withheld on whatever date of the window it sits (the parent withheld only the T-1 bar and fed an
// older one as history), a thin first print likewise, and an unconfirmed id change the classifier
// holds is consumed (the change bar, held under D37).
TEST(SessionBookGate, EveryWithheldBarOfTheWindowIsKeptOutOfTheFeed) {
    SessionClassifier c;
    weekday_history(c, "6L.v.0", "2026-04-15", 0.199, 17000);
    weekday_history(c, "MES.v.0", "2026-04-23", 7100.0, 1500000);
    std::vector<Bar> bars = {bar_on("6L.v.0", "2026-04-14", 0.1985, 16000),
                             bar_on("6L.v.0", "2026-04-15", 0.1990, 107),    // JUNK, an old date
                             bar_on("6L.v.0", "2026-04-16", 0.1991, 18000),
                             bar_on("6L.v.0", "2026-04-23", 0.1992, 107),    // JUNK, T-1
                             bar_on("MES.v.0", "2026-04-23", 7150.0, 1400000),
                             bar_on("ZT.v.0", "2026-04-23", 104.1, 10)};     // thin first print
    c.add_bars(bars);
    std::vector<SymbolDayVerdict> withheld;
    const auto feed = k01_consumed_bars(c, bars, &withheld);
    ASSERT_EQ(feed.size(), 3u);
    EXPECT_EQ(feed[0].symbol, "6L.v.0");
    EXPECT_EQ(SessionClassifier::ymd(SessionClassifier::day_of(feed[0].timestamp)), "2026-04-14");
    EXPECT_EQ(SessionClassifier::ymd(SessionClassifier::day_of(feed[1].timestamp)), "2026-04-16");
    EXPECT_EQ(feed[2].symbol, "MES.v.0");
    ASSERT_EQ(withheld.size(), 3u);
    EXPECT_EQ(withheld[0].symbol + " " + withheld[0].date, "6L.v.0 2026-04-15");
    EXPECT_EQ(withheld[1].symbol + " " + withheld[1].date, "6L.v.0 2026-04-23");
    EXPECT_EQ(withheld[2].symbol + " " + withheld[2].date, "ZT.v.0 2026-04-23");
    for (const auto& v : withheld) EXPECT_TRUE(v.k01_withheld()) << v.symbol << " " << v.reason;
}

TEST(SessionBookGate, AnIdChangeHoldIsConsumedNotWithheld) {
    SessionClassifier c;
    weekday_history(c, "NG.v.0", "2026-04-23", 3.3, 200000);
    for (Day d = ymd_day("2026-03-01"); d < ymd_day("2026-04-23"); d += std::chrono::days{1}) {
        c.add_instrument_id("NG.v.0", d, "864");
    }
    const Bar change = bar_on("NG.v.0", "2026-04-23", 3.9, 9000);  // thin on a new id
    c.add_bar(change);
    c.add_instrument_id("NG.v.0", ymd_day("2026-04-23"), "863");
    std::vector<SymbolDayVerdict> withheld;
    const auto feed = k01_consumed_bars(c, {change}, &withheld);
    const auto v = c.classify_symbol_day("NG.v.0", ymd_day("2026-04-23"), kNoHolidays);
    ASSERT_EQ(v.verdict, SessionVerdict::JUNK) << v.reason;
    EXPECT_TRUE(v.id_change_hold);
    EXPECT_FALSE(v.k01_withheld());
    EXPECT_EQ(feed.size(), 1u) << "the change bar is consumed (and held: its verdict is not SESSION)";
    EXPECT_TRUE(withheld.empty());
}

TEST(SessionBookGate, TheFeedHoleRefusalKeysOnAHeldSymbolPastTheTolerance) {
    SessionClassifier c;
    weekday_history(c, "MYM.v.0", "2026-04-21", 49000.0, 90000);  // last bar 04-20
    weekday_history(c, "PL.v.0", "2026-04-21", 1000.0, 20000);
    // T-1 = Sun 04-26 after four Sundays of prints: age 6 for both.
    for (int k = 1; k <= 4; ++k) {
        const std::string d = SessionClassifier::ymd(ymd_day("2026-04-26") - std::chrono::days{7 * k});
        c.add_bar(bar_on("MYM.v.0", d, 49000.0, 5000));
        c.add_bar(bar_on("PL.v.0", d, 1000.0, 500));
    }
    const auto t1 = classify_t1(c, {"MYM.v.0", "PL.v.0"}, ymd_day("2026-04-26"), kNoHolidays);
    ASSERT_EQ(t1.feed_hole, 2u);
    EXPECT_EQ(t1.max_hole_age_days, 6);

    const std::unordered_map<std::string, Position> held{{"MYM.v.0", pos("MYM.v.0", 1, 49707)}};
    auto holes = held_feed_holes_past_tolerance(t1, held, 4);
    ASSERT_EQ(holes.size(), 1u) << "PL is not held: its hole is logged, never refused";
    EXPECT_EQ(holes[0].symbol, "MYM.v.0");
    EXPECT_EQ(holes[0].age_days, 6);
    EXPECT_EQ(holes[0].last_bar_date, "2026-04-20");
    EXPECT_TRUE(held_feed_holes_past_tolerance(t1, held, 6).empty()) << "6 days is not past 6";
}

TEST(SessionBookGate, TheRefusalIsForTheHostsOwnDateOnly) {
    const auto host = std::chrono::system_clock::now();
    EXPECT_TRUE(run_date_is_host_date(host, host));
    EXPECT_FALSE(run_date_is_host_date(host - std::chrono::hours(48), host))
        << "a replay of a past date is never refused";
}

// =============================================================================================
// The runners
// =============================================================================================

TEST(SessionBookGateSource, TheAbortArmAndTheMondayAgriculturalBlockAreGone) {
    for (const char* runner : kFuturesRunners) {
        SCOPED_TRACE(runner);
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found";
        EXPECT_EQ(src.find("DATA ISSUE DETECTED - ABORTING"), npos);
        EXPECT_EQ(src.find("is_non_trading_day"), npos);
        EXPECT_EQ(src.find("MONDAY AGRICULTURAL FUTURES"), npos);
        EXPECT_EQ(src.find("AGRICULTURAL_FUTURES_BASE"), npos);
        EXPECT_EQ(src.find("is_monday"), npos);
        EXPECT_NE(src.find("log_whole_book_carry(t1_classification);"), npos)
            << "a whole-book carry must log its reason";
    }
}

TEST(SessionBookGateSource, TheGateHoldsOnTheVerdictBeforeStrictExecutions) {
    for (const char* runner : kFuturesRunners) {
        SCOPED_TRACE(runner);
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found";
        const auto classify = src.find("classify_t1(session_classifier, symbols,");
        const auto upsert = src.find("db->store_live_run_metadata(");
        const auto feed = src.find("portfolio->process_market_data(strategy_feed_bars);");
        const auto extract = src.find("strategy_positions_map = portfolio->get_strategy_positions();");
        const auto gate = src.find("book_holds = hold_non_session_symbols(strategy_positions_map,");
        const auto finalize = src.find("pnl_manager->finalize_previous_day(");
        const auto strict = src.find("execute_strategy_day_strict(*execution_manager,");
        const auto assertion = src.find("unpriced_book_changes(");
        const auto store_exec = src.find("db->store_executions(");
        const auto persisted = src.find("report_config_json[\"t1_classification\"]");
        for (auto at : {classify, upsert, feed, extract, gate, finalize, strict, assertion,
                        store_exec, persisted}) {
            ASSERT_NE(at, npos);
        }
        EXPECT_LT(classify, upsert);
        EXPECT_LT(feed, extract);
        EXPECT_LT(extract, gate) << "the gate must hold the PM's final per-strategy books";
        EXPECT_LT(gate, finalize);
        EXPECT_LT(gate, strict);
        EXPECT_LT(strict, assertion);
        EXPECT_LT(assertion, store_exec) << "the assertion must run before anything is stored";
        EXPECT_EQ(src.find("generate_daily_executions("), npos)
            << "the runner still generates executions without the STRICT helper";
        EXPECT_EQ(src.find("PricingPolicy::MARK_FALLBACK"), npos);
    }
}

TEST(SessionBookGateSource, TheTwinsCarryTheSameClassifierGateAndStrictBlocks) {
    const std::string cons = read_source(kFuturesRunners[0]);
    const std::string base = read_source(kFuturesRunners[1]);
    if (cons.empty() || base.empty()) GTEST_SKIP() << "runner source not found";
    const std::vector<std::pair<std::string, std::string>> blocks = {
        {"bool is_yesterday_holiday = holiday_checker.is_holiday(", "// STORE LIVE RUN METADATA"},
        {"if (early_previous_day_close_prices.empty() || no_t1_bar_consumed) {",
         "// UPDATE TRANSACTION COST MANAGER WITH MARKET DATA"},
        {"// JUNK (T-7a C4):", "// T-RISK-ARCH Q2 (ruled yes)"},
        {"// PREVIOUS DAY PER-STRATEGY BOOKS (Option A)", "// PHASE 5: PER-STRATEGY DAY T-1"},
        {"// PricingPolicy::STRICT (T-7a C4)", "// Store executions for each strategy"},
        {"// T-7a C4: the day's T-1 classification", "// Create SQL insert for live_results"},
    };
    for (const auto& [from, to] : blocks) {
        SCOPED_TRACE(from);
        const std::string a = between(cons, from, to);
        const std::string b = between(base, from, to);
        ASSERT_FALSE(a.empty());
        EXPECT_EQ(a, b) << "live_portfolio_conservative.cpp and live_portfolio.cpp are twins";
    }
}
