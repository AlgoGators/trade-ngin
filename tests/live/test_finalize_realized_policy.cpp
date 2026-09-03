// tests/live/test_finalize_realized_policy.cpp
//
// E2-F19 / E2-F20: what the Day T-1 finalization leaves in realized_pnl, per
// settlement model.
//
// LivePnLManager::finalize_previous_day writes the settlement move
// qty x (close(T-1) - close(T-2)) x point_value into every finalized row's
// realized_pnl. Under SETTLED (futures) that IS the day's realized -- a position
// entered at close(T-2) and settled at close(T-1) realizes its whole move daily.
// Under MARK_TO_MARKET (equities) it is a mark, and writing it over the trade
// realized the day's own run recorded is how a column named "daily_realized_pnl"
// came to hold price moves.
//
// The equity runner puts the LOADED T-1 figure back (LiveDailyCycle::
// restore_loaded_realized) rather than editing the shared finalizer, so the
// futures path is untouched by construction. These tests pin both halves: the
// futures value that must never move, and the equity restore that must.
//
// F20 measured the weekend consequence: Saturday's and Sunday's runs both
// finalize Friday, so Monday loads Friday's row already holding a mark move and
// seeds it (META 2026-07-31..08-04: Tue 120.590763 = 73.351951 Friday mark +
// 31.311175 Mon trade + 15.927637 Tue trade). Restoring from the loaded row makes
// the Friday row a fixed point across all three finalizations.

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Pre-load std headers before flipping private->public (same device as
// test_finalize_unrealized_policy.cpp), or the macro leaks into libc++.
#include <algorithm>
#include <map>
#include <mutex>
#include <optional>
#include <ranges>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/instruments/futures.hpp"
#define private public
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/live/live_pnl_manager.hpp"
#undef private
#include "trade_ngin/live/live_daily_cycle.hpp"

using namespace trade_ngin;

namespace {

using Book = std::unordered_map<std::string, Position>;
using Closes = std::unordered_map<std::string, double>;

// Real MNQ.v.0 numbers from 2025-10-10, the E2-F3 fixture.
constexpr double kFutQty = 3.0;
constexpr double kFutT2 = 25335.0;
constexpr double kFutT1 = 24188.0;
constexpr double kFutPv = 2.0;
constexpr double kFutMove = kFutQty * (kFutT1 - kFutT2) * kFutPv;  // -6882.00

// Real TMUS numbers from 2026-04-14/15: the basis and the exit realized.
// The symbol is chosen because its name matches no futures root in the
// point-value fallback (AAPL matches "PL", MSFT matches "MSF").
constexpr const char* kEq = "TMUS";
constexpr double kEqQty = 37.515436;
constexpr double kEqBasis = 200.733033;
constexpr double kEqRealized = -402.654413;
constexpr double kEqT2 = 195.0;
constexpr double kEqT1 = 190.0;
constexpr double kEqMove = kEqQty * (kEqT1 - kEqT2);  // -187.577180, != kEqRealized

std::shared_ptr<FuturesInstrument> make_mnq_futures() {
    FuturesSpec spec;
    spec.root_symbol = "MNQ";
    spec.exchange = "CME";
    spec.currency = "USD";
    spec.multiplier = kFutPv;
    spec.tick_size = 0.25;
    spec.commission_per_contract = 0.5;
    spec.initial_margin = 2000.0;
    spec.maintenance_margin = 1800.0;
    spec.weight = 1.0;
    return std::make_shared<FuturesInstrument>("MNQ.v.0", spec);
}

Position make_row(const std::string& symbol, double qty, double basis, double realized) {
    Position p;
    p.symbol = symbol;
    p.quantity = Quantity(qty);
    p.average_price = Decimal(basis);
    p.realized_pnl = Decimal(realized);
    p.unrealized_pnl = Decimal(0.0);
    p.last_update = std::chrono::system_clock::now();
    return p;
}

class FinalizeRealizedPolicyTest : public ::testing::Test {
protected:
    void SetUp() override {
        InstrumentRegistry::instance().instruments_["MNQ.v.0"] = make_mnq_futures();
    }
    void TearDown() override {
        InstrumentRegistry::instance().instruments_.erase("MNQ.v.0");
    }

    LivePnLManager::FinalizationResult finalize(const Book& loaded, const Closes& t1,
                                                const Closes& t2,
                                                LivePnLManager::UnrealizedPolicy policy) {
        LivePnLManager mgr(100000.0, InstrumentRegistry::instance());
        std::vector<Position> prev;
        for (const auto& [_, p] : loaded) prev.push_back(p);
        auto res = mgr.finalize_previous_day(prev, t1, t2, 100000.0, 0.0, policy);
        EXPECT_TRUE(res.is_ok());
        return res.value();
    }

    // The equity runner's T-1 path: finalize, then put the loaded realized back.
    LivePnLManager::FinalizationResult finalize_and_restore(const Book& loaded, const Closes& t1,
                                                            const Closes& t2) {
        auto r = finalize(loaded, t1, t2, LivePnLManager::UnrealizedPolicy::MARK_TO_MARKET);
        LiveDailyCycle::restore_loaded_realized(r.finalized_positions, loaded);
        return r;
    }

    static const Position& only(const LivePnLManager::FinalizationResult& r) {
        EXPECT_EQ(r.finalized_positions.size(), 1u);
        return r.finalized_positions.front();
    }

    static const Position* find(const LivePnLManager::FinalizationResult& r,
                                const std::string& symbol) {
        for (const auto& p : r.finalized_positions) {
            if (p.symbol == symbol) return &p;
        }
        return nullptr;
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// T-1: the futures pin. Must pass before and after; fails loudly if anyone makes
// the equity behaviour unconditional or routes futures through the restore.
// ---------------------------------------------------------------------------

TEST_F(FinalizeRealizedPolicyTest, SettledFinalizationWritesTheSettlementMoveAsRealized) {
    Book loaded{{"MNQ.v.0", make_row("MNQ.v.0", kFutQty, kFutT2, /*realized*/ 123.45)}};
    auto r = finalize(loaded, {{"MNQ.v.0", kFutT1}}, {{"MNQ.v.0", kFutT2}},
                      LivePnLManager::UnrealizedPolicy::SETTLED);

    EXPECT_DOUBLE_EQ(only(r).realized_pnl.as_double(), kFutMove)
        << "Under SETTLED the settlement move IS the day's realized. An incoming "
           "realized on the input row is discarded, exactly as today.";
    EXPECT_DOUBLE_EQ(r.finalized_daily_pnl, kFutMove);
    EXPECT_DOUBLE_EQ(r.position_realized_pnl.at("MNQ.v.0"), kFutMove);
    EXPECT_DOUBLE_EQ(only(r).unrealized_pnl.as_double(), 0.0);
}

// The default policy is SETTLED, so a five-argument caller (both futures runners)
// gets the same row.
TEST_F(FinalizeRealizedPolicyTest, DefaultPolicyMatchesSettled) {
    LivePnLManager mgr(100000.0, InstrumentRegistry::instance());
    std::vector<Position> prev{make_row("MNQ.v.0", kFutQty, kFutT2, 123.45)};
    auto res = mgr.finalize_previous_day(prev, {{"MNQ.v.0", kFutT1}}, {{"MNQ.v.0", kFutT2}},
                                         100000.0, 0.0);
    ASSERT_TRUE(res.is_ok());
    EXPECT_DOUBLE_EQ(res.value().finalized_positions.front().realized_pnl.as_double(), kFutMove);
}

// ---------------------------------------------------------------------------
// T-2: the equity restore. The row keeps the trade realized the day's own run
// wrote; the mark goes to unrealized; the aggregate still reports the move.
// ---------------------------------------------------------------------------

TEST_F(FinalizeRealizedPolicyTest, EquityRowKeepsTheLoadedTradeRealized) {
    Book loaded{{kEq, make_row(kEq, kEqQty, kEqBasis, kEqRealized)}};
    auto r = finalize_and_restore(loaded, {{kEq, kEqT1}}, {{kEq, kEqT2}});

    ASSERT_NE(std::abs(kEqMove - kEqRealized), 0.0)
        << "fixture precondition: the mark move must differ from the trade realized "
           "or the restore is indistinguishable from the finalizer";
    EXPECT_NEAR(only(r).realized_pnl.as_double(), kEqRealized, 1e-6)
        << "the T-1 row must carry the day's trade realized, not the mark move "
           "(E2-F19 route 2 / E2-F20)";
    EXPECT_NEAR(only(r).unrealized_pnl.as_double(), kEqQty * (kEqT1 - kEqBasis), 1e-6)
        << "the mark belongs in daily_unrealized_pnl, measured against the cost basis";
}

// The restore is a row-only change: the aggregate the equity curve is built from
// still reports the settlement move. This is what keeps yesterday_total_pnl and
// total_unrealized_pnl byte-identical, and it is also the proof that the fix did
// not go into the finalizer.
TEST_F(FinalizeRealizedPolicyTest, RestoreDoesNotTouchTheAggregate) {
    Book loaded{{kEq, make_row(kEq, kEqQty, kEqBasis, kEqRealized)}};
    auto r = finalize_and_restore(loaded, {{kEq, kEqT1}}, {{kEq, kEqT2}});

    EXPECT_NEAR(r.finalized_daily_pnl, kEqMove, 1e-6)
        << "finalized_daily_pnl must still be the mark move (also guards against a "
           "stray point-value multiplier for the equity symbol)";
    EXPECT_NEAR(r.position_realized_pnl.at(kEq), kEqMove, 1e-6);
    EXPECT_NEAR(r.finalized_unrealized_pnl, kEqQty * (kEqT1 - kEqBasis), 1e-6);
}

// ---------------------------------------------------------------------------
// T-4: the degenerate branches -- no T-1 close, then no T-2 close. A single thin
// name reaches these at 852 symbols, and a naive fix would silently zero a real
// figure here.
// ---------------------------------------------------------------------------

TEST_F(FinalizeRealizedPolicyTest, MissingT1CloseStillRestoresRealized) {
    Book loaded{{kEq, make_row(kEq, kEqQty, kEqBasis, kEqRealized)}};
    loaded[kEq].unrealized_pnl = Decimal(146.98);  // the last known mark
    auto r = finalize_and_restore(loaded, /*t1*/ {}, {{kEq, kEqT2}});

    EXPECT_NEAR(only(r).realized_pnl.as_double(), kEqRealized, 1e-6)
        << "a missing T-1 close must not erase the day's trade realized";
    // R-2: the last known mark is carried, on the row AND in the aggregate. A halted or
    // unprinted name is still worth its last price; zero made the level vanish for a day
    // and reappear as a phantom jump.
    EXPECT_NEAR(only(r).unrealized_pnl.as_double(), 146.98, 1e-6);
    EXPECT_NEAR(r.finalized_unrealized_pnl, 146.98, 1e-6);
}

TEST_F(FinalizeRealizedPolicyTest, MissingT2CloseStillRestoresRealized) {
    Book loaded{{kEq, make_row(kEq, kEqQty, kEqBasis, kEqRealized)}};
    auto r = finalize_and_restore(loaded, {{kEq, kEqT1}}, /*t2*/ {{"OTHER", 1.0}});

    EXPECT_NEAR(only(r).realized_pnl.as_double(), kEqRealized, 1e-6);
    // R-2: a T-1 close exists, so the mark against the cost basis is computable; no
    // settlement move is reported (T-2 missing), but the level is not zeroed.
    EXPECT_NEAR(only(r).unrealized_pnl.as_double(), kEqQty * (kEqT1 - kEqBasis), 1e-6);
    EXPECT_NEAR(r.finalized_unrealized_pnl, kEqQty * (kEqT1 - kEqBasis), 1e-6);
    EXPECT_DOUBLE_EQ(r.finalized_daily_pnl, 0.0);
}

// Under SETTLED the degenerate branches keep writing 0 -- the futures pin for T-4.
TEST_F(FinalizeRealizedPolicyTest, SettledDegenerateBranchesWriteZero) {
    Book loaded{{"MNQ.v.0", make_row("MNQ.v.0", kFutQty, kFutT2, 123.45)}};
    auto r = finalize(loaded, {}, {{"MNQ.v.0", kFutT2}},
                      LivePnLManager::UnrealizedPolicy::SETTLED);
    EXPECT_DOUBLE_EQ(only(r).realized_pnl.as_double(), 0.0);
    EXPECT_DOUBLE_EQ(r.finalized_daily_pnl, 0.0);
}

// ---------------------------------------------------------------------------
// T-5 / T-5b: a closed row (qty 0, realized != 0) carried into the T-1 write set
// keeps its realized, contributes nothing to any aggregate, and is not dead.
// ---------------------------------------------------------------------------

TEST_F(FinalizeRealizedPolicyTest, ClosedRowSurvivesFinalizationWithItsRealized) {
    const double kExit = -327.342;
    Book open_only{{"META", make_row("META", 4.148866, 539.03, 0.0)}};
    Book with_closed = open_only;
    with_closed["AAPLX"] = make_row("AAPLX", 0.0, 0.0, kExit);  // closed yesterday

    Closes t1{{"META", 556.71}, {"AAPLX", 275.15}};
    Closes t2{{"META", 539.03}, {"AAPLX", 293.08}};

    auto base = finalize_and_restore(open_only, t1, t2);
    auto r = finalize_and_restore(with_closed, t1, t2);

    const Position* closed = find(r, "AAPLX");
    ASSERT_NE(closed, nullptr) << "the closed row must come back out of finalization";
    EXPECT_NEAR(closed->realized_pnl.as_double(), kExit, 1e-6)
        << "the exit's realized must survive the T-1 rewrite";
    EXPECT_DOUBLE_EQ(closed->unrealized_pnl.as_double(), 0.0);
    EXPECT_FALSE(LiveDailyCycle::is_dead_row(*closed))
        << "the T-1 write site must keep this row (route 3 leaked through this site)";

    EXPECT_DOUBLE_EQ(r.finalized_daily_pnl, base.finalized_daily_pnl)
        << "a zero-quantity row must not move the finalized aggregate";
    EXPECT_DOUBLE_EQ(r.finalized_unrealized_pnl, base.finalized_unrealized_pnl)
        << "total_unrealized_pnl is summed from these rows; a closed row adds 0";
}

// ---------------------------------------------------------------------------
// T-13: the Sat/Sun/Mon triple. Each finalization of the same loaded rows must
// leave the row equal to what was LOADED -- not merely equal to the previous
// pass, which the finalizer already guarantees because it is a fixed point of
// quantity and closes (F20 §5). The claim under test is stability against the
// day's own written value.
// ---------------------------------------------------------------------------

TEST_F(FinalizeRealizedPolicyTest, RepeatedFinalizationIsAFixedPointOfTheLoadedRow) {
    Book loaded{
        {kEq, make_row(kEq, kEqQty, kEqBasis, kEqRealized)},
        {"META", make_row("META", 4.148866, 539.03, 31.311175)},
    };
    Closes t1{{kEq, kEqT1}, {"META", 556.71}};
    Closes t2{{kEq, kEqT2}, {"META", 539.03}};

    for (int pass = 1; pass <= 3; ++pass) {
        auto r = finalize_and_restore(loaded, t1, t2);
        for (const auto& [symbol, original] : loaded) {
            const Position* fp = find(r, symbol);
            ASSERT_NE(fp, nullptr);
            EXPECT_DOUBLE_EQ(fp->realized_pnl.as_double(), original.realized_pnl.as_double())
                << "pass " << pass << ": " << symbol
                << " realized drifted from the loaded value (F20 weekend contamination)";
            EXPECT_DOUBLE_EQ(fp->quantity.as_double(), original.quantity.as_double());
            EXPECT_DOUBLE_EQ(fp->average_price.as_double(),
                             original.average_price.as_double());
        }
    }
}

// A finalized row with no loaded counterpart cannot carry a loaded realized; it
// gets 0 rather than the mark move.
TEST_F(FinalizeRealizedPolicyTest, RowWithoutALoadedCounterpartGetsZeroRealized) {
    std::vector<Position> finalized{make_row("META", 4.148866, 539.03, /*mark*/ 73.351951)};
    Book loaded;  // nothing loaded for META
    LiveDailyCycle::restore_loaded_realized(finalized, loaded);
    EXPECT_DOUBLE_EQ(finalized.front().realized_pnl.as_double(), 0.0);
}

// ---------------------------------------------------------------------------
// R-1: the finalizer itself no longer writes the mark move into realized under
// MARK_TO_MARKET -- without the runner's restore step. A new caller cannot fall
// into the trap. The aggregate still carries the move.
// ---------------------------------------------------------------------------
TEST_F(FinalizeRealizedPolicyTest, FinalizerAloneKeepsTradeRealizedUnderMarkToMarket) {
    Book loaded{{kEq, make_row(kEq, kEqQty, kEqBasis, kEqRealized)}};
    auto r = finalize(loaded, {{kEq, kEqT1}}, {{kEq, kEqT2}},
                      LivePnLManager::UnrealizedPolicy::MARK_TO_MARKET);
    EXPECT_NEAR(only(r).realized_pnl.as_double(), kEqRealized, 1e-6)
        << "no restore was applied: the finalizer must not write the mark move as realized "
           "on a cash book";
    EXPECT_NEAR(r.finalized_daily_pnl, kEqMove, 1e-6) << "the aggregate still carries the move";
}
