#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <memory>
#include <unordered_map>
#include <vector>

#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/live/execution_manager.hpp"
#include "trade_ngin/live/live_daily_cycle.hpp"
#include "trade_ngin/live/live_pnl_manager.hpp"
#include "trade_ngin/strategy/mean_reversion.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

// Wave 2 regression tests.
//
// F-B: the live equity runner never seeded the strategy's positions_ from the
//      previous day's book, so MeanReversionStrategy::generate_signal() took its
//      flat-book ENTRY branch on every bar of every session. A held position was
//      liquidated the moment its z-score left the entry zone (|z| < 2.0) instead
//      of being held to the exit threshold (|z| < 0.5), and the stop-loss -- which
//      measures from Position::average_price -- could never fire at all.
//
// F-E: a held-but-untraded holding had its average_price overwritten with the
//      previous session's close every day, so the basis every PnL figure and the
//      stop-loss measure from drifted to the latest price and the position always
//      looked flat.
//
// The two are one defect in two places and are tested together: seeding is what
// puts a basis in front of the stop-loss, and retaining the basis is what makes
// that stop-loss measure from the truth.
class LiveDailyCycleSeedingTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();

        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        ASSERT_TRUE(db_->connect().is_ok());

        strategy_config_.capital_allocation = 100000.0;
        strategy_config_.max_leverage = 2.0;
        strategy_config_.asset_classes = {AssetClass::EQUITIES};
        strategy_config_.frequencies = {DataFrequency::DAILY};
        for (const auto& symbol : {"AAPL", "MSFT"}) {
            strategy_config_.trading_params[symbol] = 1.0;
            strategy_config_.position_limits[symbol] = 100000.0;
        }

        mr_config_.lookback_period = 20;
        mr_config_.vol_lookback = 20;
        mr_config_.entry_threshold = 2.0;
        mr_config_.exit_threshold = 0.5;
        mr_config_.risk_target = 0.15;
        mr_config_.position_size = 0.1;
        mr_config_.use_stop_loss = true;
        mr_config_.stop_loss_pct = 0.05;
        mr_config_.allow_fractional_shares = true;

        risk_limits_.max_position_size = 100000.0;
        risk_limits_.max_notional_value = 1e9;
        risk_limits_.max_drawdown = 0.9;
        risk_limits_.max_leverage = 10.0;
    }

    void TearDown() override {
        if (db_) {
            db_->disconnect();
            db_.reset();
        }
        TestBase::TearDown();
    }

    std::unique_ptr<MeanReversionStrategy> create_strategy() {
        static int test_id = 0;
        auto strategy = std::make_unique<MeanReversionStrategy>(
            "TEST_MR_SEED_" + std::to_string(++test_id),
            strategy_config_, mr_config_, db_);
        EXPECT_TRUE(strategy->initialize().is_ok());
        EXPECT_TRUE(strategy->update_risk_limits(risk_limits_).is_ok());
        EXPECT_TRUE(strategy->start().is_ok());
        return strategy;
    }

    // A deterministic series whose closing bar sits in the HOLD band: far enough
    // from the mean that a held position is not yet told to exit (|z| > 0.5), but
    // not far enough to open a new one (|z| < 2.0). That gap is precisely where
    // the entry branch and the exit branch disagree, so it is where an unseeded
    // strategy gives itself away.
    std::vector<Bar> hold_band_series(const std::string& symbol,
                                      double final_close = 98.5) const {
        std::vector<Bar> bars;
        auto now = std::chrono::system_clock::now();
        const int kPre = 30;
        for (int i = 0; i < kPre; ++i) {
            bars.push_back(make_bar(symbol, (i % 2 == 0) ? 99.0 : 101.0,
                                    now - std::chrono::hours(24 * (kPre + 1 - i))));
        }
        bars.push_back(make_bar(symbol, final_close, now));
        return bars;
    }

    Bar make_bar(const std::string& symbol, double close, Timestamp ts) const {
        Bar bar;
        bar.symbol = symbol;
        bar.timestamp = ts;
        bar.open = close;
        bar.high = close;
        bar.low = close;
        bar.close = close;
        bar.volume = 1000000.0;
        return bar;
    }

    Position held_long(const std::string& symbol, double quantity,
                       double average_price) const {
        Position pos;
        pos.symbol = symbol;
        pos.quantity = Quantity(quantity);
        pos.average_price = Decimal(average_price);
        pos.last_update = std::chrono::system_clock::now();
        return pos;
    }

    std::shared_ptr<MockPostgresDatabase> db_;
    StrategyConfig strategy_config_;
    RiskLimits risk_limits_;
    MeanReversionConfig mr_config_;
};

// ---------------------------------------------------------------------------
// F-B
// ---------------------------------------------------------------------------

// The bug in one assertion: same bars, same strategy, same day -- the only
// difference is whether the previous day's book was handed back. Without it the
// position is thrown away inside the hold band.
TEST_F(LiveDailyCycleSeedingTest, SeededLongIsHeldInsideTheHoldBand) {
    auto bars = hold_band_series("AAPL");

    auto flat = create_strategy();
    ASSERT_TRUE(LiveDailyCycle::prepare_strategy_for_signals(*flat, {}, bars).is_ok());

    const double z = flat->get_z_score("AAPL");
    ASSERT_LT(z, -mr_config_.exit_threshold)
        << "series precondition: z must be beyond the exit threshold";
    ASSERT_GT(z, -mr_config_.entry_threshold)
        << "series precondition: z must be inside the entry threshold (z=" << z << ")";

    // A genuinely flat book has nothing to hold, so it stays flat. Unchanged by
    // this fix, and the control the next assertion is measured against.
    EXPECT_DOUBLE_EQ(flat->get_position("AAPL"), 0.0);

    auto seeded = create_strategy();
    std::unordered_map<std::string, Position> previous{
        {"AAPL", held_long("AAPL", 100.0, 98.0)}};
    ASSERT_TRUE(
        LiveDailyCycle::prepare_strategy_for_signals(*seeded, previous, bars).is_ok());

    EXPECT_GT(seeded->get_position("AAPL"), 0.0)
        << "a held long inside the hold band must be kept, not liquidated at the "
           "entry threshold";
}

// The exit threshold has to be the thing that closes the position -- not the
// entry threshold wearing its clothes.
TEST_F(LiveDailyCycleSeedingTest, SeededLongExitsOnlyOnceInsideTheExitThreshold) {
    // 100.0 sits at the series mean, so |z| collapses below the exit threshold.
    auto bars = hold_band_series("AAPL", 100.0);

    auto seeded = create_strategy();
    std::unordered_map<std::string, Position> previous{
        {"AAPL", held_long("AAPL", 100.0, 98.0)}};
    ASSERT_TRUE(
        LiveDailyCycle::prepare_strategy_for_signals(*seeded, previous, bars).is_ok());

    ASSERT_LT(std::abs(seeded->get_z_score("AAPL")), mr_config_.exit_threshold)
        << "series precondition: z must be inside the exit threshold";
    EXPECT_DOUBLE_EQ(seeded->get_position("AAPL"), 0.0)
        << "mean reversion is complete -- the position must be closed";
}

// Unseeded, the stop-loss is unreachable: generate_signal() returns from the
// entry branch before it ever looks at average_price.
TEST_F(LiveDailyCycleSeedingTest, StopLossFiresFromTheSeededCostBasis) {
    auto bars = hold_band_series("AAPL");  // closes at 98.5, inside the hold band

    // Bought at 200, now marked 98.5: more than 50% underwater, far past the 5% stop.
    auto seeded = create_strategy();
    std::unordered_map<std::string, Position> previous{
        {"AAPL", held_long("AAPL", 100.0, 200.0)}};
    ASSERT_TRUE(
        LiveDailyCycle::prepare_strategy_for_signals(*seeded, previous, bars).is_ok());

    EXPECT_DOUBLE_EQ(seeded->get_position("AAPL"), 0.0)
        << "a position 50% underwater must be stopped out";

    // Same bars, same z-score, same hold band -- only the basis differs. This one
    // is barely underwater, so the stop must NOT fire. Together the two cases show
    // the stop is driven by the cost basis rather than by the z-score.
    auto healthy = create_strategy();
    std::unordered_map<std::string, Position> healthy_book{
        {"AAPL", held_long("AAPL", 100.0, 99.0)}};
    ASSERT_TRUE(
        LiveDailyCycle::prepare_strategy_for_signals(*healthy, healthy_book, bars).is_ok());

    EXPECT_GT(healthy->get_position("AAPL"), 0.0)
        << "a position 0.5% underwater is inside the 5% stop and must be held";
}

// ---------------------------------------------------------------------------
// F-B x F-E -- the reason the two fixes ship together
// ---------------------------------------------------------------------------

// If the runner re-anchors a held position's basis to the previous close (F-E),
// then seeding it (F-B) hands the stop-loss a basis that is always ~= the current
// price. The stop silently never fires, and the fix reads as if it worked.
TEST_F(LiveDailyCycleSeedingTest, StopLossMeasuresFromRetainedBasisNotPreviousClose) {
    const double kPreviousClose = 98.5;  // what the pre-Wave-2 runner wrote as basis
    const double kTrueBasis = 200.0;     // what the position actually cost
    auto bars = hold_band_series("AAPL", kPreviousClose);

    // The defect being guarded against: basis re-anchored to the previous close.
    auto reanchored = create_strategy();
    std::unordered_map<std::string, Position> reanchored_book{
        {"AAPL", held_long("AAPL", 100.0, kPreviousClose)}};
    ASSERT_TRUE(
        LiveDailyCycle::prepare_strategy_for_signals(*reanchored, reanchored_book, bars)
            .is_ok());
    ASSERT_GT(reanchored->get_position("AAPL"), 0.0)
        << "sanity: against a re-anchored basis the stop cannot fire -- this is the "
           "silent failure F-E would leave behind";

    // The same position with the basis it actually has.
    auto retained = create_strategy();
    std::unordered_map<std::string, Position> retained_book{
        {"AAPL", held_long("AAPL", 100.0, kTrueBasis)}};
    ASSERT_TRUE(
        LiveDailyCycle::prepare_strategy_for_signals(*retained, retained_book, bars)
            .is_ok());
    EXPECT_DOUBLE_EQ(retained->get_position("AAPL"), 0.0)
        << "the stop must measure from the retained cost basis";
}

// ---------------------------------------------------------------------------
// F-E -- the basis rule itself
// ---------------------------------------------------------------------------

TEST_F(LiveDailyCycleSeedingTest, UntradedHoldingRetainsItsCostBasisAcrossTheDay) {
    const double kEstablishedBasis = 150.0;
    const double kPreviousClose = 98.5;

    // A holding the strategy knows about (seeded, untraded) keeps its basis. The
    // previous close is not a candidate: it is a mark, not a cost.
    EXPECT_DOUBLE_EQ(
        LivePnLManager::resolve_day_t_cost_basis(kEstablishedBasis, kEstablishedBasis),
        kEstablishedBasis);
    EXPECT_NE(
        LivePnLManager::resolve_day_t_cost_basis(kEstablishedBasis, kEstablishedBasis),
        kPreviousClose);

    // Repeating the day must not move it -- the drift was the bug.
    double basis = kEstablishedBasis;
    for (int day = 0; day < 5; ++day) {
        basis = LivePnLManager::resolve_day_t_cost_basis(basis, basis);
    }
    EXPECT_DOUBLE_EQ(basis, kEstablishedBasis);
}

TEST_F(LiveDailyCycleSeedingTest, CarriedBasisSurvivesAStrategyThatHasNoRecord) {
    // Defence in depth: if seeding were ever removed or a symbol were missing from
    // the strategy, the basis still comes from the carried book rather than being
    // reinvented from the day's price.
    EXPECT_DOUBLE_EQ(LivePnLManager::resolve_day_t_cost_basis(0.0, 150.0), 150.0);
}

TEST_F(LiveDailyCycleSeedingTest, UnknownBasisIsZeroRatherThanAFabricatedPrice) {
    // A genuinely new position has no basis until its fill is processed. Reporting
    // 0.0 makes unrealized_from_cost_basis() report no PnL; substituting the day's
    // close would have booked the position as instantly flat at a price it never paid.
    EXPECT_DOUBLE_EQ(LivePnLManager::resolve_day_t_cost_basis(0.0, 0.0), 0.0);
    EXPECT_DOUBLE_EQ(
        LivePnLManager::unrealized_from_cost_basis(
            100.0, LivePnLManager::resolve_day_t_cost_basis(0.0, 0.0), 98.5),
        0.0);
}

TEST_F(LiveDailyCycleSeedingTest, StrategyBasisWinsOverTheCarriedBasisWhenBothExist) {
    // A symbol that traded today has a freshly weighted average from on_execution();
    // that is the current truth and must not be overwritten by yesterday's.
    EXPECT_DOUBLE_EQ(LivePnLManager::resolve_day_t_cost_basis(120.0, 150.0), 120.0);
}

// ---------------------------------------------------------------------------
// Blast radius
// ---------------------------------------------------------------------------

// Seeding an empty book must be indistinguishable from not seeding at all.
TEST_F(LiveDailyCycleSeedingTest, EmptyBookIsInert) {
    auto bars = hold_band_series("AAPL");

    auto unseeded = create_strategy();
    ASSERT_TRUE(unseeded->on_data(bars).is_ok());

    auto seeded_empty = create_strategy();
    ASSERT_TRUE(
        LiveDailyCycle::prepare_strategy_for_signals(*seeded_empty, {}, bars).is_ok());

    EXPECT_DOUBLE_EQ(seeded_empty->get_position("AAPL"), unseeded->get_position("AAPL"));
    EXPECT_DOUBLE_EQ(seeded_empty->get_z_score("AAPL"), unseeded->get_z_score("AAPL"));
}

// Seeding must not move a symbol that is genuinely flat: the entry branch is
// reached by exactly the symbols that reached it before.
TEST_F(LiveDailyCycleSeedingTest, FlatSymbolIsUnaffectedByAnotherSymbolsSeed) {
    std::vector<Bar> bars = hold_band_series("AAPL");
    for (auto& bar : hold_band_series("MSFT")) bars.push_back(bar);

    auto unseeded = create_strategy();
    ASSERT_TRUE(unseeded->on_data(bars).is_ok());

    auto seeded = create_strategy();
    std::unordered_map<std::string, Position> previous{
        {"AAPL", held_long("AAPL", 100.0, 98.0)}};
    ASSERT_TRUE(
        LiveDailyCycle::prepare_strategy_for_signals(*seeded, previous, bars).is_ok());

    EXPECT_DOUBLE_EQ(seeded->get_position("MSFT"), unseeded->get_position("MSFT"))
        << "MSFT was not seeded and must be unchanged";
    EXPECT_GT(seeded->get_position("AAPL"), 0.0)
        << "AAPL was seeded and must now be held";
}

// A position opened by an entry signal is still opened -- seeding restores held
// positions, it does not suppress new ones.
TEST_F(LiveDailyCycleSeedingTest, EntrySignalStillOpensANewPosition) {
    // 94.0 drives z past the entry threshold.
    auto bars = hold_band_series("AAPL", 94.0);

    auto seeded = create_strategy();
    ASSERT_TRUE(LiveDailyCycle::prepare_strategy_for_signals(*seeded, {}, bars).is_ok());

    ASSERT_LT(seeded->get_z_score("AAPL"), -mr_config_.entry_threshold)
        << "series precondition: z must be beyond the entry threshold";
    EXPECT_GT(seeded->get_position("AAPL"), 0.0)
        << "an oversold flat symbol must still be entered long";
}

// Backtest runs one process over the whole history, so positions_ accumulates
// through on_execution() and never needed seeding. That must keep working.
TEST_F(LiveDailyCycleSeedingTest, BacktestAccumulationPathIsUnchanged) {
    auto bars = hold_band_series("AAPL");
    auto strategy = create_strategy();

    ASSERT_TRUE(strategy->on_data(bars).is_ok());
    EXPECT_DOUBLE_EQ(strategy->get_position("AAPL"), 0.0)
        << "flat book, hold band: nothing to hold";

    // The backtest's own mechanism: a fill establishes the holding.
    ExecutionReport fill;
    fill.order_id = "BT_ORDER_1";
    fill.exec_id = "BT_EXEC_1";
    fill.symbol = "AAPL";
    fill.side = Side::BUY;
    fill.filled_quantity = Quantity(100.0);
    fill.fill_price = Price(98.0);
    fill.fill_time = std::chrono::system_clock::now();
    ASSERT_TRUE(strategy->on_execution(fill).is_ok());

    const auto& positions = strategy->get_positions();
    auto it = positions.find("AAPL");
    ASSERT_NE(it, positions.end()) << "on_execution must record the holding";
    EXPECT_DOUBLE_EQ(it->second.quantity.as_double(), 100.0);
    EXPECT_DOUBLE_EQ(it->second.average_price.as_double(), 98.0)
        << "on_execution remains the sole writer of the cost basis";

    // Re-running the day now takes the exit branch, with no seeding involved.
    ASSERT_TRUE(strategy->on_data(bars).is_ok());
    EXPECT_GT(strategy->get_position("AAPL"), 0.0)
        << "the accumulated holding is held inside the hold band";
}

// ---------------------------------------------------------------------------
// E2-F19 / E2-F20: the seeded book must not carry yesterday's realized P&L into
// today's row.
//
// seed_positions() is a wholesale copy, so positions_[sym].realized_pnl started
// each session at whatever the loaded T-1 row held, and on_execution() added
// today's fills on top. The persisted day-T row was therefore yesterday's value
// plus today's trades -- and on a Monday, yesterday's value was Friday's MARK MOVE,
// because the Saturday and Sunday runs had each finalized Friday (E2-F20).
//
// metrics_.realized_pnl is a separate accumulator that seeding never touches; it
// feeds live_results.daily_realized_pnl and has always been correct. These tests
// pin that asymmetry: the per-position figure becomes today's flow, the aggregate
// does not move.
// ---------------------------------------------------------------------------

namespace {

Position held_with_pnl(const std::string& symbol, double quantity, double average_price,
                       double realized, double unrealized) {
    Position pos;
    pos.symbol = symbol;
    pos.quantity = Quantity(quantity);
    pos.average_price = Decimal(average_price);
    pos.realized_pnl = Decimal(realized);
    pos.unrealized_pnl = Decimal(unrealized);
    pos.last_update = std::chrono::system_clock::now();
    return pos;
}

ExecutionReport fill(const std::string& symbol, Side side, double qty, double price) {
    ExecutionReport f;
    f.order_id = "T_" + symbol;
    f.exec_id = "T_" + symbol + "_X";
    f.symbol = symbol;
    f.side = side;
    f.filled_quantity = Quantity(qty);
    f.fill_price = Price(price);
    f.fill_time = std::chrono::system_clock::now();
    return f;
}

}  // namespace

// T-6. Seeding hands the strategy quantity, basis and mark; it does NOT hand it a
// realized figure. unrealized is asserted at the value on_data re-marks it to --
// (close - basis) x qty -- which is the stronger claim: the seeded basis reached
// the mark, so the seed did not zero the basis along with realized.
TEST_F(LiveDailyCycleSeedingTest, SeedZeroesRealizedAndKeepsBasisAndMark) {
    auto bars = hold_band_series("AAPL");  // closes at 98.5
    auto seeded = create_strategy();
    std::unordered_map<std::string, Position> previous{
        {"AAPL", held_with_pnl("AAPL", 100.0, 98.0, /*realized*/ -466.969196,
                               /*unrealized*/ 12.34)}};
    ASSERT_TRUE(
        LiveDailyCycle::prepare_strategy_for_signals(*seeded, previous, bars).is_ok());

    const auto& positions = seeded->get_positions();
    auto it = positions.find("AAPL");
    ASSERT_NE(it, positions.end());
    EXPECT_DOUBLE_EQ(it->second.quantity.as_double(), 100.0);
    EXPECT_DOUBLE_EQ(it->second.average_price.as_double(), 98.0)
        << "the seeded cost basis must survive: the stop-loss measures from it";
    EXPECT_NEAR(it->second.unrealized_pnl.as_double(), (98.5 - 98.0) * 100.0, 1e-6)
        << "unrealized is re-marked from the seeded basis; update_metrics() sums it "
           "into the drawdown gate, so it must be seeded, not zeroed";
    EXPECT_DOUBLE_EQ(it->second.realized_pnl.as_double(), 0.0)
        << "yesterday's realized must not be seeded: the row is a per-day flow "
           "(E2-F19 route 1)";
}

// T-7. The day's row is the day's trade, not the day's trade plus the chain.
// Real TMUS 2026-04-15 numbers: closing SELL 37.515436 @ 190 against a 200.733033
// basis realizes -402.654413. The map had -466.969196 seeded from the prior row,
// and stored -869.62 for the day; live_results stored -402.6544. This is the test
// that would have caught the second reverted attempt.
TEST_F(LiveDailyCycleSeedingTest, SeededRealizedDoesNotLeakIntoTheDaysFill) {
    mr_config_.use_stop_loss = false;  // basis 200.73 vs 98.5 closes would fire it
    strategy_config_.trading_params["TMUS"] = 1.0;
    strategy_config_.position_limits["TMUS"] = 100000.0;

    const double qty = 37.515436;
    const double basis = 200.733033;
    const double exit = 190.0;
    const double expected = (exit - basis) * qty;  // -402.654413

    auto bars = hold_band_series("TMUS");
    auto seeded = create_strategy();
    std::unordered_map<std::string, Position> previous{
        {"TMUS", held_with_pnl("TMUS", qty, basis, /*realized*/ -466.969196, 0.0)}};
    ASSERT_TRUE(
        LiveDailyCycle::prepare_strategy_for_signals(*seeded, previous, bars).is_ok());

    ASSERT_TRUE(seeded->on_execution(fill("TMUS", Side::SELL, qty, exit)).is_ok());

    const auto& positions = seeded->get_positions();
    auto it = positions.find("TMUS");
    ASSERT_NE(it, positions.end());
    EXPECT_NEAR(it->second.realized_pnl.as_double(), expected, 1e-6)
        << "the per-position realized must be today's fill alone, not the seeded "
           "chain plus today's fill";
    EXPECT_DOUBLE_EQ(it->second.quantity.as_double(), 0.0);

    // The control: the aggregate accumulator was never seeded and is unchanged
    // by this fix. It must equal the same figure in both the old and new world.
    EXPECT_NEAR(seeded->get_metrics().realized_pnl, expected, 1e-6)
        << "metrics_.realized_pnl feeds live_results and must not move";
}

// ---------------------------------------------------------------------------
// T-12. Three sessions -- open, hold, close -- with the T-1 finalization
// interposed between them the way the runner does it. Without the interposed
// finalize the old code also passes, because a day-1 row seeded with realized 0
// stays 0; the contamination needs the finalizer to have rewritten the row
// first (F20 Fact B). Every day: sum of per-position realized == the aggregate
// accumulator + the day's costs, and day 2's row is 0 while day 3's carries the
// exit.
// ---------------------------------------------------------------------------
TEST_F(LiveDailyCycleSeedingTest, ThreeDayFlowWithInterposedFinalization) {
    mr_config_.use_stop_loss = false;
    strategy_config_.trading_params["TMUS"] = 1.0;
    strategy_config_.position_limits["TMUS"] = 100000.0;

    ExecutionManager em;
    LivePnLManager pnl(100000.0, InstrumentRegistry::instance());
    const Timestamp now = std::chrono::system_clock::now();
    auto bars = hold_band_series("TMUS");

    // The "database": what each session loads is what the previous session's
    // T-1 finalization left behind, not what the previous session wrote.
    auto finalize_like_a_weekend = [&](const std::unordered_map<std::string, Position>& book,
                                       double t2, double t1) {
        std::vector<Position> prev;
        for (const auto& [_, p] : book) prev.push_back(p);
        auto r = pnl.finalize_previous_day(prev, {{"TMUS", t1}}, {{"TMUS", t2}}, 100000.0,
                                           0.0, LivePnLManager::UnrealizedPolicy::MARK_TO_MARKET);
        EXPECT_TRUE(r.is_ok());
        auto finalized = r.value().finalized_positions;
        LiveDailyCycle::restore_loaded_realized(finalized, book);
        std::unordered_map<std::string, Position> out;
        for (const auto& p : finalized) out[p.symbol] = p;
        return out;
    };

    auto run_day = [&](const std::unordered_map<std::string, Position>& loaded,
                       double target_qty, double t1_close) {
        auto strat = create_strategy();
        EXPECT_TRUE(
            LiveDailyCycle::prepare_strategy_for_signals(*strat, loaded, bars).is_ok());

        std::unordered_map<std::string, Position> positions;
        Position target;
        target.symbol = "TMUS";
        target.quantity = Quantity(target_qty);
        target.average_price = Decimal(t1_close);
        target.last_update = now;
        positions["TMUS"] = target;

        auto outcome = LiveDailyCycle::execute_day_t(em, positions, loaded,
                                                     {{"TMUS", t1_close}}, bars, now);
        EXPECT_TRUE(outcome.is_ok());
        double costs = 0.0;
        for (const auto& e : outcome.value().executions) {
            EXPECT_TRUE(strat->on_execution(e).is_ok());
            costs += e.total_transaction_costs.as_double();
        }
        LiveDailyCycle::resolve_and_apply_basis(positions, strat->get_positions(), loaded,
                                                outcome.value().execution_prices);

        double row_sum = 0.0;
        for (const auto& [_, p] : positions) row_sum += p.realized_pnl.as_double();
        EXPECT_NEAR(row_sum, strat->get_metrics().realized_pnl + costs, 1e-6)
            << "rows must sum to the aggregate on every day (protocol L5)";
        return positions;
    };

    // Day 1: open 10 @ 100.
    auto day1 = run_day({}, 10.0, 100.0);
    ASSERT_TRUE(day1.count("TMUS"));
    EXPECT_DOUBLE_EQ(day1.at("TMUS").realized_pnl.as_double(), 0.0);

    // Weekend: day 1's row is finalized (twice would be the same) before day 2 reads it.
    auto day1_as_loaded = finalize_like_a_weekend(day1, 100.0, 105.0);

    // Day 2: hold. Nothing traded, so the row must be 0 -- not the 50.0 mark move
    // the finalizer wrote onto day 1's row.
    auto day2 = run_day(day1_as_loaded, 10.0, 105.0);
    ASSERT_TRUE(day2.count("TMUS"));
    EXPECT_DOUBLE_EQ(day2.at("TMUS").realized_pnl.as_double(), 0.0)
        << "a held, untraded position realizes nothing today; a mark move leaked "
           "in through the seed (E2-F20)";

    auto day2_as_loaded = finalize_like_a_weekend(day2, 105.0, 110.0);

    // Day 3: close at 110. The exit realizes 10 x (110 - 100) = 100 -- and it must
    // be on a row, i.e. the qty-0 entry is not dead.
    auto day3 = run_day(day2_as_loaded, 0.0, 110.0);
    ASSERT_TRUE(day3.count("TMUS"));
    EXPECT_DOUBLE_EQ(day3.at("TMUS").quantity.as_double(), 0.0);
    EXPECT_NEAR(day3.at("TMUS").realized_pnl.as_double(), 100.0, 1e-6)
        << "the close-day row carries the exit's realized";
    EXPECT_FALSE(LiveDailyCycle::is_dead_row(day3.at("TMUS")))
        << "the day-T write must keep the close-day row (E2-F19 route 3)";
}

// ---------------------------------------------------------------------------
// G1 in both shapes. The runner's day-T book comes from the strategy's target
// map, which covers the configured universe. A held symbol IN the universe gets a
// qty-0 target and the close-out flows through the ordinary delta path (route 3:
// the row exists and must not be dropped). A held symbol NOT in the universe --
// contra-merged, renamed, or de-configured -- gets a close-out from
// generate_daily_executions' second loop and no row at all (G1: a row must be
// synthesized).
// ---------------------------------------------------------------------------
TEST_F(LiveDailyCycleSeedingTest, CloseOutOfAConfiguredSymbolKeepsItsRow) {
    ExecutionManager em;
    const Timestamp now = std::chrono::system_clock::now();
    auto bars = hold_band_series("MSFT");
    auto strat = create_strategy();
    std::unordered_map<std::string, Position> previous{
        {"MSFT", held_with_pnl("MSFT", 10.0, 100.0, 0.0, 0.0)}};
    ASSERT_TRUE(LiveDailyCycle::prepare_strategy_for_signals(*strat, previous, bars).is_ok());

    std::unordered_map<std::string, Position> positions;
    Position target;
    target.symbol = "MSFT";
    target.quantity = Quantity(0.0);
    target.last_update = now;
    positions["MSFT"] = target;

    auto outcome = LiveDailyCycle::execute_day_t(em, positions, previous, {{"MSFT", 110.0}},
                                                 bars, now);
    ASSERT_TRUE(outcome.is_ok());
    ASSERT_EQ(outcome.value().executions.size(), 1u);
    ASSERT_TRUE(strat->on_execution(outcome.value().executions.front()).is_ok());
    LiveDailyCycle::resolve_and_apply_basis(positions, strat->get_positions(), previous,
                                            outcome.value().execution_prices);

    ASSERT_TRUE(positions.count("MSFT"));
    EXPECT_NEAR(positions.at("MSFT").realized_pnl.as_double(), 100.0, 1e-6);
    EXPECT_FALSE(LiveDailyCycle::is_dead_row(positions.at("MSFT")));
}

TEST_F(LiveDailyCycleSeedingTest, CloseOutOfAnUnconfiguredSymbolGetsARow) {
    ExecutionManager em;
    const Timestamp now = std::chrono::system_clock::now();
    auto bars = hold_band_series("MSFT");
    auto strat = create_strategy();
    std::unordered_map<std::string, Position> previous{
        {"MSFT", held_with_pnl("MSFT", 10.0, 100.0, 0.0, 0.0)}};
    ASSERT_TRUE(LiveDailyCycle::prepare_strategy_for_signals(*strat, previous, bars).is_ok());

    // MSFT has left the universe: no target entry at all.
    std::unordered_map<std::string, Position> positions;

    auto outcome = LiveDailyCycle::execute_day_t(em, positions, previous, {{"MSFT", 110.0}},
                                                 bars, now);
    ASSERT_TRUE(outcome.is_ok());
    ASSERT_EQ(outcome.value().executions.size(), 1u)
        << "generate_daily_executions must close out a held symbol absent from targets";
    EXPECT_EQ(outcome.value().executions.front().side, Side::SELL);
    ASSERT_TRUE(strat->on_execution(outcome.value().executions.front()).is_ok());
    LiveDailyCycle::resolve_and_apply_basis(positions, strat->get_positions(), previous,
                                            outcome.value().execution_prices);
    EXPECT_FALSE(positions.count("MSFT")) << "precondition: no row exists yet for the exit";

    auto added = LiveDailyCycle::add_rowless_exits(positions, strat->get_positions(), now);

    ASSERT_EQ(added.size(), 1u);
    ASSERT_TRUE(positions.count("MSFT")) << "the exit's realized needs a row (G1)";
    EXPECT_NEAR(positions.at("MSFT").realized_pnl.as_double(), 100.0, 1e-6);
    EXPECT_DOUBLE_EQ(positions.at("MSFT").quantity.as_double(), 0.0);
    EXPECT_FALSE(LiveDailyCycle::is_dead_row(positions.at("MSFT")));
}
