// The live T-1 settlement on the consumed bars (LOOP_SPEC v6.1 sections 2.1, 6.6; T-ROLLX-FIX commit
// 2): the live P&L manager's T-1 finalize books 0 for a symbol whose T-1 bar is a change bar (a roll's
// switch day or either bar of a flip pair) or a WITHHELD bar (K-01), and settles every other symbol
// exactly as before; consumed_t1_settlement (session_book_gate.hpp) names those symbols and books
// each T-1 move against the previous CONSUMED close (a withheld bar in between is skipped).
#include <gtest/gtest.h>
#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <sstream>
#include <thread>
#include "../core/test_base.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/roll_series.hpp"
#include "trade_ngin/instruments/futures.hpp"
#include "trade_ngin/live/session_book_gate.hpp"
// Expose the registry's private members so the singleton can be populated without a database
// (the pattern of test_trend_following.cpp); every std header is pre-loaded above.
#define private public
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/live/live_pnl_manager.hpp"
#undef private

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

Position held(const std::string& symbol, double qty, double avg) {
    Position p;
    p.symbol = symbol;
    p.quantity = Decimal(qty);
    p.average_price = Decimal(avg);
    p.last_update = std::chrono::system_clock::now();
    return p;
}

Bar bar(const std::string& symbol, int day, double close, const std::string& id) {
    Bar b;
    b.symbol = symbol;
    b.timestamp = std::chrono::system_clock::time_point(std::chrono::hours(24 * (20300 + day)));
    b.open = b.high = b.low = b.close = Decimal(close);
    b.volume = 1000.0;
    b.instrument_id = id;
    return b;
}

class ChangeBarPnlTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        auto& registry = InstrumentRegistry::instance();
        for (const auto& [sym, mult] : std::vector<std::pair<std::string, double>>{{"NG", 10000.0}, {"ES", 50.0}}) {
            FuturesSpec spec;
            spec.root_symbol = sym;
            spec.exchange = "CME";
            spec.currency = "USD";
            spec.multiplier = mult;
            spec.tick_size = 0.25;
            spec.commission_per_contract = 2.0;
            spec.initial_margin = 1.0;
            spec.maintenance_margin = 1.0;
            spec.weight = 1.0;
            spec.trading_hours = "09:30-16:00";
            registry.instruments_[sym] = std::make_shared<FuturesInstrument>(sym, spec);
        }
        registry.initialized_ = true;
    }
    void TearDown() override {
        auto& registry = InstrumentRegistry::instance();
        registry.instruments_.clear();
        registry.initialized_ = false;
        TestBase::TearDown();
    }
};

}  // namespace

// NG rolled on T-1 (3.376 -> 3.965, the vendor's switch): its settlement move is 0, ES's is as always.
TEST_F(ChangeBarPnlTest, FinalizeBooksNoMoveForAChangeBarSymbol) {
    LivePnLManager pm(500000.0, InstrumentRegistry::instance());
    const std::vector<Position> book{held("NG.v.0", 2, 3.376), held("ES.v.0", -1, 5000.0)};
    const std::unordered_map<std::string, double> t1{{"NG.v.0", 3.965}, {"ES.v.0", 5050.0}};
    const std::unordered_map<std::string, double> t2{{"NG.v.0", 3.376}, {"ES.v.0", 5000.0}};
    const std::unordered_set<std::string> change{"NG.v.0"};
    auto r = pm.finalize_previous_day(book, t1, t2, 500000.0, 0.0, LivePnLManager::UnrealizedPolicy::SETTLED, change);
    ASSERT_TRUE(r.is_ok());
    const auto& f = r.value();
    EXPECT_DOUBLE_EQ(f.position_realized_pnl.at("NG.v.0"), 0.0) << "the switch's gap is not a move";
    EXPECT_DOUBLE_EQ(f.position_realized_pnl.at("ES.v.0"), -1 * 50.0 * 50.0);
    EXPECT_DOUBLE_EQ(f.finalized_daily_pnl, -2500.0);
    ASSERT_EQ(f.finalized_positions.size(), 2u);
    for (const auto& p : f.finalized_positions) {
        if (p.symbol == "NG.v.0") EXPECT_DOUBLE_EQ(static_cast<double>(p.realized_pnl), 0.0);
    }
    // The control: without the flag the splice step is booked (2 x 0.589 x 10,000).
    auto c = pm.finalize_previous_day(book, t1, t2, 500000.0);
    ASSERT_TRUE(c.is_ok());
    EXPECT_NEAR(c.value().position_realized_pnl.at("NG.v.0"), 2 * (3.965 - 3.376) * 10000.0, 1e-6);
}

// The status of each symbol's last bar in a window: NG's switch on the last bar is a pending change
// (held in the old contract), ES has one id, and a flip's reverting bar is a change bar too.
TEST_F(ChangeBarPnlTest, RollStatusOfAWindowNamesTheChangeBarsAndTheHeldContract) {
    std::vector<Bar> bars;
    for (int d = 1; d <= 5; ++d) {
        bars.push_back(bar("NG.v.0", d, 3.3 + 0.01 * d, d == 5 ? "863" : "864"));
        bars.push_back(bar("ES.v.0", d, 5000.0 + d, "1"));
        bars.push_back(bar("6E.v.0", d, 1.15 + 0.001 * d, d == 4 ? "42040878" : "4274"));
    }
    const auto st = roll_series::roll_status_of(bars);
    EXPECT_TRUE(st.at("NG.v.0").change);
    EXPECT_TRUE(st.at("NG.v.0").pending);
    EXPECT_EQ(st.at("NG.v.0").held_id, "864") << "held in the outgoing contract while pending";
    EXPECT_FALSE(st.at("ES.v.0").change);
    EXPECT_EQ(st.at("ES.v.0").held_id, "1");
    EXPECT_TRUE(st.at("6E.v.0").change) << "day 5 reverts to 4274: the flip's second bar";
    EXPECT_TRUE(st.at("6E.v.0").flip);
    EXPECT_EQ(st.at("6E.v.0").held_id, "4274");
    // Bars given out of order are taken in time order.
    std::vector<Bar> shuffled(bars.rbegin(), bars.rend());
    const auto st2 = roll_series::roll_status_of(shuffled);
    EXPECT_EQ(st2.at("NG.v.0").held_id, "864");
    EXPECT_TRUE(st2.at("NG.v.0").change);
}

// K-01 in the live settlement. Day 3 of ES is withheld (a corrupt print, never consumed): T-1 = day 4
// is booked against day 2's close, the last CONSUMED one, not the withheld day 3's; NG's T-1 is a
// change bar (0); 6B's T-1 bar itself is withheld (0, its row kept); ZN printed no T-1 bar.
TEST_F(ChangeBarPnlTest, TheT1SettlementSkipsAWithheldBarAndZeroesChangeAndWithheldBars) {
    const std::string t1 = SessionClassifier::ymd(SessionClassifier::day_of(bar("ES.v.0", 4, 0, "").timestamp));
    std::vector<Bar> consumed;
    for (int d = 1; d <= 4; ++d) {
        if (d != 3) consumed.push_back(bar("ES.v.0", d, 5000.0 + 10 * d, "1"));
        consumed.push_back(bar("NG.v.0", d, 3.3 + 0.01 * d, d == 4 ? "863" : "864"));
        if (d <= 3) consumed.push_back(bar("6B.v.0", d, 1.30 + 0.001 * d, "1318"));
        if (d <= 2) consumed.push_back(bar("ZN.v.0", d, 110.0 + d, "42002219"));
    }
    const auto status = roll_series::roll_status_of(consumed);
    const std::unordered_map<std::string, double> raw_t2{
        {"ES.v.0", 4999.0 /* the withheld day 3 print */}, {"NG.v.0", 3.33}, {"6B.v.0", 1.303}, {"ZN.v.0", 112.0}};
    const std::unordered_map<std::string, double> raw_t1{{"ES.v.0", 5040.0}, {"NG.v.0", 3.34}, {"6B.v.0", 1.25}};
    const auto s = consumed_t1_settlement(consumed, t1, status, {"6B.v.0"}, raw_t2, raw_t1);
    EXPECT_DOUBLE_EQ(s.t2_close_prices.at("ES.v.0"), 5020.0) << "day 2's close, the last consumed before T-1";
    EXPECT_DOUBLE_EQ(s.t2_close_prices.at("NG.v.0"), 3.33);
    EXPECT_EQ(s.zero_pnl_symbols, (std::unordered_set<std::string>{"NG.v.0", "6B.v.0"}));

    LivePnLManager pm(500000.0, InstrumentRegistry::instance());
    const std::vector<Position> book{held("ES.v.0", 1, 0), held("NG.v.0", 2, 0), held("6B.v.0", 1, 0)};
    auto r = pm.finalize_previous_day(book, raw_t1, s.t2_close_prices, 500000.0, 0.0,
                                      LivePnLManager::UnrealizedPolicy::SETTLED, s.zero_pnl_symbols);
    ASSERT_TRUE(r.is_ok());
    EXPECT_DOUBLE_EQ(r.value().position_realized_pnl.at("ES.v.0"), 1 * (5040.0 - 5020.0) * 50.0);
    EXPECT_DOUBLE_EQ(r.value().position_realized_pnl.at("NG.v.0"), 0.0);
    EXPECT_DOUBLE_EQ(r.value().position_realized_pnl.at("6B.v.0"), 0.0) << "a withheld T-1 bar books nothing";
}
