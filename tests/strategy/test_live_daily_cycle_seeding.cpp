#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <memory>
#include <unordered_map>
#include <vector>

#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
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
