// T-7a C2b: the live execution id is EXEC_<symbol>_<YYYYMMDD>.
//
// Before this commit ExecutionManager built EXEC_<symbol>_<run instant in ms>_<n>, n being
// the fill's index in that call's output, i.e. the iteration order of an unordered_map.
// The same fills could be numbered differently, and one fill appearing or disappearing
// renumbered unrelated fills of the same day (T-4c F13: six fills re-keyed when one MYM
// execution moved from 04-24 to 05-02).
//
// These tests go through the public generate_daily_executions (and, for the equity
// runner, LiveDailyCycle::execute_day_t) only, so they compile unchanged on the parent
// source and fail there on the id's value.
//
// Timestamps are fixed instants so the expected date does not depend on the host's zone:
//   2026-04-24 05:00:00Z  the futures runners' `now` (local midnight on a US-Central host)
//   2026-06-10 00:00:00Z  the equity runner's `now` (the CLI date parsed as UTC midnight)

#include <gtest/gtest.h>

#include <chrono>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/live/execution_manager.hpp"
#include "trade_ngin/live/live_daily_cycle.hpp"

using namespace trade_ngin;

namespace {

const Timestamp kFuturesNow = std::chrono::system_clock::from_time_t(1777006800);  // 04-24 05:00Z
const Timestamp kEquityNow = std::chrono::system_clock::from_time_t(1781049600);   // 06-10 00:00Z

Position pos(const std::string& symbol, double qty, double price) {
    Position p;
    p.symbol = symbol;
    p.quantity = Quantity(qty);
    p.average_price = Decimal(price);
    p.unrealized_pnl = Decimal(0.0);
    p.realized_pnl = Decimal(0.0);
    return p;
}

using Book = std::unordered_map<std::string, Position>;
using Prices = std::unordered_map<std::string, double>;

// A day like the conservative chain's: opens, resizes, a flip and full closes.
struct Day {
    Book current;
    Book previous;
    Prices prices;
};

Day six_fill_day() {
    Day d;
    const std::vector<std::string> syms = {"6A.v.0", "6C.v.0", "6L.v.0", "MBT.v.0", "MES.v.0",
                                           "MYM.v.0"};
    double px = 100.0;
    for (const auto& s : syms) d.prices[s] = (px += 7.0);
    d.current["6A.v.0"] = pos("6A.v.0", 2.0, 0.0);    // open
    d.current["6C.v.0"] = pos("6C.v.0", -1.0, 0.0);   // flip long -> short
    d.previous["6C.v.0"] = pos("6C.v.0", 1.0, 0.0);
    d.current["6L.v.0"] = pos("6L.v.0", 3.0, 0.0);    // resize
    d.previous["6L.v.0"] = pos("6L.v.0", 1.0, 0.0);
    d.current["MES.v.0"] = pos("MES.v.0", 1.0, 0.0);  // open
    d.previous["MBT.v.0"] = pos("MBT.v.0", 1.0, 0.0);  // full close (second loop)
    d.previous["MYM.v.0"] = pos("MYM.v.0", 2.0, 0.0);  // full close (second loop)
    return d;
}

std::unordered_map<std::string, std::string> ids_by_symbol(const std::vector<ExecutionReport>& v) {
    std::unordered_map<std::string, std::string> m;
    for (const auto& e : v) m[e.symbol] = e.exec_id;
    return m;
}

}  // namespace

// RED on the parent: EXEC_6A.v.0_1777006800000_<n>.
TEST(ExecIdDeterminism, FuturesIdIsExecSymbolRunDate) {
    ExecutionManager em;
    Day d = six_fill_day();
    auto r = em.generate_daily_executions(d.current, d.previous, d.prices, kFuturesNow);
    ASSERT_TRUE(r.is_ok());
    ASSERT_EQ(r.value().size(), 6u);
    for (const auto& e : r.value()) {
        EXPECT_EQ(e.exec_id, "EXEC_" + e.symbol + "_20260424") << e.symbol;
        EXPECT_EQ(e.order_id, "DAILY_" + e.symbol + "_20260424") << e.symbol;
    }
}

// The id is order_id with the prefix swapped: they share one date string.
TEST(ExecIdDeterminism, ExecIdSharesTheOrderIdDate) {
    ExecutionManager em;
    Day d = six_fill_day();
    auto r = em.generate_daily_executions(d.current, d.previous, d.prices, kFuturesNow);
    ASSERT_TRUE(r.is_ok());
    for (const auto& e : r.value()) {
        ASSERT_EQ(e.order_id.rfind("DAILY_", 0), 0u);
        EXPECT_EQ(e.exec_id, "EXEC_" + e.order_id.substr(6)) << e.symbol;
    }
}

// T-4c F13's case. RED on the parent: the removed fill is the one numbered _0, so the five
// that remain held suffixes {1..5} and now hold {0..4}; at least one must be renumbered.
TEST(ExecIdDeterminism, RemovingOneExecutionDoesNotRenumberAnother) {
    ExecutionManager em;
    Day d = six_fill_day();
    auto full = em.generate_daily_executions(d.current, d.previous, d.prices, kFuturesNow);
    ASSERT_TRUE(full.is_ok());
    ASSERT_EQ(full.value().size(), 6u);

    const std::string removed = full.value().front().symbol;  // the fill the parent numbered _0
    d.current.erase(removed);
    d.previous.erase(removed);
    auto less = em.generate_daily_executions(d.current, d.previous, d.prices, kFuturesNow);
    ASSERT_TRUE(less.is_ok());
    ASSERT_EQ(less.value().size(), 5u);

    const auto before = ids_by_symbol(full.value());
    for (const auto& e : less.value()) {
        EXPECT_EQ(e.exec_id, before.at(e.symbol))
            << e.symbol << " was renumbered when " << removed << " stopped trading";
    }
}

// A fill's id does not depend on what else traded that day or in which order the maps
// were built. RED on the parent: alone it is always _0.
TEST(ExecIdDeterminism, IdIndependentOfOtherExecutionsThatDay) {
    ExecutionManager em;
    Day d = six_fill_day();
    auto all = em.generate_daily_executions(d.current, d.previous, d.prices, kFuturesNow);
    ASSERT_TRUE(all.is_ok());
    const auto in_company = ids_by_symbol(all.value());

    for (const auto& [symbol, id] : in_company) {
        Book cur, prev;
        if (d.current.count(symbol)) cur[symbol] = d.current.at(symbol);
        if (d.previous.count(symbol)) prev[symbol] = d.previous.at(symbol);
        auto alone = em.generate_daily_executions(cur, prev, d.prices, kFuturesNow);
        ASSERT_TRUE(alone.is_ok());
        ASSERT_EQ(alone.value().size(), 1u);
        EXPECT_EQ(alone.value()[0].exec_id, id) << symbol;
    }

    // Same book, maps built in the reverse insertion order.
    Book cur_rev, prev_rev;
    std::vector<std::string> keys;
    for (const auto& [s, _] : d.current) keys.push_back(s);
    for (auto it = keys.rbegin(); it != keys.rend(); ++it) cur_rev[*it] = d.current.at(*it);
    keys.clear();
    for (const auto& [s, _] : d.previous) keys.push_back(s);
    for (auto it = keys.rbegin(); it != keys.rend(); ++it) prev_rev[*it] = d.previous.at(*it);
    auto rev = em.generate_daily_executions(cur_rev, prev_rev, d.prices, kFuturesNow);
    ASSERT_TRUE(rev.is_ok());
    EXPECT_EQ(ids_by_symbol(rev.value()), in_company);
}

// The id names the run DATE, not the instant: the futures `now` (05:00Z) and a 00:00Z
// `now` of the same UTC day give the same id. RED on the parent (the ms differ).
TEST(ExecIdDeterminism, IdCarriesTheDateNotTheInstant) {
    ExecutionManager em;
    Book cur{{"MES.v.0", pos("MES.v.0", 1.0, 0.0)}};
    Prices px{{"MES.v.0", 7250.0}};
    const Timestamp midnight_utc = std::chrono::system_clock::from_time_t(1777006800 - 5 * 3600);
    auto a = em.generate_daily_executions(cur, {}, px, kFuturesNow);
    auto b = em.generate_daily_executions(cur, {}, px, midnight_utc);
    auto c = em.generate_daily_executions(cur, {}, px, kFuturesNow + std::chrono::hours(24));
    ASSERT_TRUE(a.is_ok() && b.is_ok() && c.is_ok());
    EXPECT_EQ(a.value()[0].exec_id, "EXEC_MES.v.0_20260424");
    EXPECT_EQ(b.value()[0].exec_id, a.value()[0].exec_id);
    EXPECT_EQ(c.value()[0].exec_id, "EXEC_MES.v.0_20260425");
}

// The equity runner reaches the same generator through LiveDailyCycle::execute_day_t.
// RED on the parent: EXEC_AAPL_1781049600000_<n>.
TEST(ExecIdDeterminism, EquityDayTIdIsExecSymbolRunDate) {
    ExecutionManager em;
    Book positions{{"AAPL", pos("AAPL", 21.5, 0.0)}, {"MSFT", pos("MSFT", 3.0, 0.0)}};
    Book previous{{"MSFT", pos("MSFT", 5.0, 400.0)}, {"GOOGL", pos("GOOGL", 4.0, 150.0)}};
    Prices t1{{"AAPL", 290.55}, {"MSFT", 410.0}, {"GOOGL", 160.0}};
    auto r = LiveDailyCycle::execute_day_t(em, positions, previous, t1, {}, kEquityNow);
    ASSERT_TRUE(r.is_ok());
    ASSERT_EQ(r.value().executions.size(), 3u);
    for (const auto& e : r.value().executions) {
        EXPECT_EQ(e.exec_id, "EXEC_" + e.symbol + "_20260610") << e.symbol;
    }
}

// Guard (GREEN on both sides): one call never emits a symbol twice, so the ids of one
// call are unique without a counter. This is the invariant the id's uniqueness under
// trading.executions' key (portfolio_id, strategy_id, strategy_name, date, exec_id) rests on.
TEST(ExecIdDeterminism, OneCallEmitsEachSymbolAtMostOnce) {
    ExecutionManager em;
    Day d = six_fill_day();
    auto r = em.generate_daily_executions(d.current, d.previous, d.prices, kFuturesNow);
    ASSERT_TRUE(r.is_ok());
    std::set<std::string> symbols, ids;
    for (const auto& e : r.value()) {
        symbols.insert(e.symbol);
        ids.insert(e.exec_id);
    }
    EXPECT_EQ(symbols.size(), r.value().size());
    EXPECT_EQ(ids.size(), r.value().size());
}
