// What the Day T-1 finalization writes into unrealized_pnl, per settlement model.
//
// LivePnLManager::finalize_previous_day writes the Day T-1 row. On futures it writes
// `unrealized_pnl = 0`. A mark-to-market expression there, on the premise that
// "average_price resets to close after daily settlement, so this is ~0", is off by
// exactly one day.
//
// Under the futures T-1 lag model the
// runner enters a position at close(T-1) (the futures runners' STEP 2 sets
// average_price = yesterday_close) and the NEXT day's run settles it at close(T). So on a
// futures row average_price IS the prior settlement close -- precisely the day_t2_close the
// finalizer already uses to compute realized. A mark-to-market expression therefore reduces to
//
//     qty * (t1_close - average_price) * pt  ==  qty * (t1_close - t2_close) * pt  ==  realized
//
// i.e. it records the same settlement move in a second column.
//
// On a 10-day futures replay (2025-10-06..15) that expression left 65 of 77 rows non-zero;
// on 2025-10-10 six of nine rows had daily_unrealized_pnl EXACTLY equal
// to daily_realized_pnl (-315.00, -2295.00, -3558.75, -6882.00, -590.00, +718.75), and the
// per-row sum reached -12,586.69 while live_results correctly reported 0.00 -- a violation
// of the row-sums-to-aggregate invariant.
//
// Both directions are pinned. The SETTLED case is the futures invariant that must not move;
// the MARK_TO_MARKET case is the equity behaviour and must keep.

#include <gtest/gtest.h>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Pre-load std headers before flipping private->public, or the macro leaks into libc++.
#include <algorithm>
#include <map>
#include <mutex>
#include <optional>
#include <ranges>

#include "trade_ngin/instruments/futures.hpp"
#define private public
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/live/live_pnl_manager.hpp"
#undef private

using namespace trade_ngin;

namespace {

// Real numbers from the MNQ.v.0 row on 2025-10-10, so the fixture cannot drift from what
// production actually produced.
constexpr double kQty        = 3.0;
constexpr double kT2Close    = 25335.0;   // close 2025-10-09 -- what average_price holds
constexpr double kT1Close    = 24188.0;   // close 2025-10-10
constexpr double kPointValue = 2.0;       // MNQ contract size
// (24188 - 25335) * 3 * 2
constexpr double kOneDayMove = (kT1Close - kT2Close) * kQty * kPointValue;  // -6882.00

// Registered so get_point_value("MNQ.v.0") resolves deterministically rather than
// falling back to a default, which would make the exact-value assertions meaningless.
std::shared_ptr<FuturesInstrument> make_mnq_futures() {
    FuturesSpec spec;
    spec.root_symbol = "MNQ";
    spec.exchange = "CME";
    spec.currency = "USD";
    spec.multiplier = kPointValue;
    spec.tick_size = 0.25;
    spec.commission_per_contract = 0.5;
    spec.initial_margin = 2000.0;
    spec.maintenance_margin = 1800.0;
    spec.weight = 1.0;
    return std::make_shared<FuturesInstrument>("MNQ.v.0", spec);
}

Position futures_position() {
    Position p;
    p.symbol = "MNQ.v.0";
    p.quantity = Decimal(kQty);
    // The T-1 lag model: the run on day T-1 wrote the prior settlement close here.
    p.average_price = Decimal(kT2Close);
    p.realized_pnl = Decimal(0.0);
    p.unrealized_pnl = Decimal(0.0);
    p.last_update = std::chrono::system_clock::now();
    return p;
}

class FinalizeUnrealizedPolicyTest : public ::testing::Test {
protected:
    void SetUp() override {
        InstrumentRegistry::instance().instruments_["MNQ.v.0"] = make_mnq_futures();
        // The P&L manager looks the contract up by its metadata key, the symbol without the
        // ".v.0" suffix; with no fallback table the row must be there.
        InstrumentRegistry::instance().instruments_["MNQ"] = make_mnq_futures();
    }
    void TearDown() override {
        InstrumentRegistry::instance().instruments_.erase("MNQ.v.0");
        InstrumentRegistry::instance().instruments_.erase("MNQ");
    }

    // Returns the unrealized value the finalizer wrote onto the Day T-1 row.
    double finalized_unrealized(LivePnLManager::UnrealizedPolicy policy, bool pass_policy) {
        auto& registry = InstrumentRegistry::instance();
        LivePnLManager mgr(500000.0, registry);

        std::vector<Position> prev{futures_position()};
        std::unordered_map<std::string, double> t1{{"MNQ.v.0", kT1Close}};
        std::unordered_map<std::string, double> t2{{"MNQ.v.0", kT2Close}};

        auto res = pass_policy
                       ? mgr.finalize_previous_day(prev, t1, t2, 500000.0, 0.0, policy)
                       : mgr.finalize_previous_day(prev, t1, t2, 500000.0, 0.0);
        EXPECT_TRUE(res.is_ok());
        if (!res.is_ok() || res.value().finalized_positions.empty()) return NAN;
        return res.value().finalized_positions.front().unrealized_pnl.as_double();
    }
};

}  // namespace

// THE FUTURES INVARIANT. A position whose average_price is the prior settlement close --
// which under the T-1 lag model is every futures position -- must finalize with zero
// unrealized, because that move is already booked as realized.
TEST_F(FinalizeUnrealizedPolicyTest, SettledPolicyWritesZeroEvenWhenPriceMoved) {
    const double u = finalized_unrealized(LivePnLManager::UnrealizedPolicy::SETTLED, true);

    EXPECT_DOUBLE_EQ(u, 0.0)
        << "Futures finalization wrote a non-zero unrealized P&L. average_price is the "
           "prior settlement close, so this value is the settlement move already recorded "
           "in realized_pnl -- the same day counted twice.";
}

// The default must BE the futures behaviour, so a caller that says nothing is safe.
// Both futures runners call finalize_previous_day with no policy argument.
TEST_F(FinalizeUnrealizedPolicyTest, DefaultPolicyIsSettledSoFuturesCallersNeedNoArgument) {
    const double u = finalized_unrealized(LivePnLManager::UnrealizedPolicy::SETTLED, false);

    EXPECT_DOUBLE_EQ(u, 0.0)
        << "Omitting the policy argument did not yield the futures behaviour. The two "
           "futures runners pass no policy, so the default must be SETTLED.";
}

// The equity behaviour, which the futures guard must preserve.
TEST_F(FinalizeUnrealizedPolicyTest, MarkToMarketPolicyMeasuresAgainstTheCostBasis) {
    const double u =
        finalized_unrealized(LivePnLManager::UnrealizedPolicy::MARK_TO_MARKET, true);

    EXPECT_DOUBLE_EQ(u, kOneDayMove)
        << "Mark-to-market finalization did not measure the position against its "
           "average_price. Equities depend on this: there average_price is a true "
           "weighted cost basis.";
}

// The double-count made concrete: under SETTLED the unrealized must NOT equal the realized
// move, which is exactly the equality observed in production on 6 of 9 rows.
TEST_F(FinalizeUnrealizedPolicyTest, SettledUnrealizedDoesNotDuplicateTheRealizedMove) {
    const double settled = finalized_unrealized(LivePnLManager::UnrealizedPolicy::SETTLED, true);

    EXPECT_NE(settled, kOneDayMove)
        << "Futures unrealized equals the one-day settlement move (" << kOneDayMove
        << "), which is what realized_pnl already holds. This is the exact double-count signature: "
           "on 2025-10-10 six of nine rows had unrealized == realized to the cent.";
}

// Guards against the policy being accepted but ignored -- both assertions above could pass
// for reasons unrelated to the flag if the branch were never taken.
TEST_F(FinalizeUnrealizedPolicyTest, ThePolicyIsWhatDecidesTheOutcome) {
    const double settled = finalized_unrealized(LivePnLManager::UnrealizedPolicy::SETTLED, true);
    const double marked =
        finalized_unrealized(LivePnLManager::UnrealizedPolicy::MARK_TO_MARKET, true);

    EXPECT_NE(settled, marked)
        << "UnrealizedPolicy changed nothing. If both paths agree the parameter is not "
           "wired into the finalizer, and this suite would not catch a regression.";
}

// ===========================================================================
// FinalizationResult::finalized_unrealized_pnl -- the aggregate counterpart of the
// per-row unrealized written above.
//
// live_results.total_unrealized_pnl is summed from the same finalized rows that get
// persisted, priced at close(T-1). A DAY-T snapshot instead (the book priced at close(T-2),
// computed at insert and never revisited by the T-1 finalization) would differ from the
// rows it summarises by exactly that day's P&L. Summed from the rows, the
// row-sums-to-aggregate invariant holds by construction rather than by two code paths
// happening to agree (on a 12-day replay, 2026-07-24..08-04, its residual is 0.0000 every
// day, weekends included).
//
// Futures safety is structural: the accumulation lives INSIDE the MARK_TO_MARKET branch, so
// SETTLED cannot reach it. Both futures runners pass no policy argument and therefore get
// SETTLED and an aggregate of exactly 0.0.
// ===========================================================================

// THE FUTURES PIN. Must be exactly 0.0 with no policy argument at all -- that is how both
// futures runners call it.
TEST_F(FinalizeUnrealizedPolicyTest, FinalizedUnrealizedAggregateIsZeroForFuturesByDefault) {
    auto& registry = InstrumentRegistry::instance();
    LivePnLManager mgr(500000.0, registry);

    std::vector<Position> prev{futures_position()};
    std::unordered_map<std::string, double> t1{{"MNQ.v.0", kT1Close}};
    std::unordered_map<std::string, double> t2{{"MNQ.v.0", kT2Close}};

    auto res = mgr.finalize_previous_day(prev, t1, t2, 500000.0, 0.0);
    ASSERT_TRUE(res.is_ok());

    EXPECT_DOUBLE_EQ(res.value().finalized_unrealized_pnl, 0.0)
        << "The default (futures) path accumulated a non-zero unrealized aggregate. Under "
           "SETTLED every finalized row's unrealized is 0 by the settlement identity, so the "
           "sum must be 0 too -- by construction, not by measurement. A non-zero value here "
           "means the accumulation escaped the MARK_TO_MARKET branch.";
}

// And explicitly under SETTLED, for a caller that names the policy.
TEST_F(FinalizeUnrealizedPolicyTest, FinalizedUnrealizedAggregateIsZeroUnderSettled) {
    auto& registry = InstrumentRegistry::instance();
    LivePnLManager mgr(500000.0, registry);

    std::vector<Position> prev{futures_position()};
    std::unordered_map<std::string, double> t1{{"MNQ.v.0", kT1Close}};
    std::unordered_map<std::string, double> t2{{"MNQ.v.0", kT2Close}};

    auto res = mgr.finalize_previous_day(prev, t1, t2, 500000.0, 0.0,
                                         LivePnLManager::UnrealizedPolicy::SETTLED);
    ASSERT_TRUE(res.is_ok());
    EXPECT_DOUBLE_EQ(res.value().finalized_unrealized_pnl, 0.0);
}

// Equities: the aggregate must equal the sum of the rows it summarises. This is L5.
TEST_F(FinalizeUnrealizedPolicyTest, FinalizedUnrealizedAggregateEqualsSumOfFinalizedRows) {
    auto& registry = InstrumentRegistry::instance();
    LivePnLManager mgr(500000.0, registry);

    std::vector<Position> prev{futures_position()};
    std::unordered_map<std::string, double> t1{{"MNQ.v.0", kT1Close}};
    std::unordered_map<std::string, double> t2{{"MNQ.v.0", kT2Close}};

    auto res = mgr.finalize_previous_day(prev, t1, t2, 500000.0, 0.0,
                                         LivePnLManager::UnrealizedPolicy::MARK_TO_MARKET);
    ASSERT_TRUE(res.is_ok());

    double row_sum = 0.0;
    for (const auto& p : res.value().finalized_positions) {
        row_sum += p.unrealized_pnl.as_double();
    }

    EXPECT_DOUBLE_EQ(res.value().finalized_unrealized_pnl, row_sum)
        << "The aggregate diverged from the rows. live_results.total_unrealized_pnl is "
           "written from this figure and trading.positions.daily_unrealized_pnl from those "
           "rows, so any gap here reappears as an L5 violation in the database"
           ".";
    EXPECT_DOUBLE_EQ(res.value().finalized_unrealized_pnl, kOneDayMove)
        << "The mark-to-market aggregate should equal the one-day move for this fixture, "
           "whose average_price is the T-2 close.";
}
