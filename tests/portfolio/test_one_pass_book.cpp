// LOOP_SPEC sections 4 to 6 at the portfolio manager: a book that names an overlay sleeve is
// rebalanced by ONE pass (the overlay once on the capped target, the forecast-sign close, one
// search from the held book, the buffer and one rounding, the cap, the trim, the split back to the
// sleeves, the fills). The arithmetic itself is pinned against the frozen oracle in
// tests/optimization/test_one_pass.cpp; these tests pin what the manager does with it: where the
// held book comes from, the hold set, the fills and the ledger, the sleeves' split, the record the
// runners store, the refusal contract, and that no second lap exists.
//
// The numbers. One contract of a test symbol is 1,000 x 100 = 100,000 of notional, a weight of
// u = 0.2 on the 500,000 book, so the per-name cap of 2 is 10 contracts. The stub sleeve gives no
// window, so the overlay is BLIND (m = 1 unless a leverage limit binds) and the optimiser's
// covariance is the guarded 0.01 diagonal: one contract away from the target is a tracking error
// of 0.2 x 0.1 = 0.02, far above the cost of a contract, and B_sigma is that same 0.02. From a
// held h to a search answer y the buffer therefore moves a = (|y - h| - 1) / |y - h| of the way:
// 0 -> 5 stores 4, a gap of one contract is no trade.

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "one_pass_test_fixture.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/live/run_metadata_marks.hpp"
#include "trade_ngin/risk/risk_detail.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

size_t count_of(const std::string& text, const std::string& needle) {
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
    return n;
}

}  // namespace

class OnePassBookTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        ASSERT_TRUE(db_->connect().is_ok());
        LoggerConfig lc;
        lc.destination = LogDestination::CONSOLE;
        lc.min_level = LogLevel::INFO;
        lc.include_timestamp = false;
        Logger::instance().initialize(lc);
    }
    void TearDown() override {
        pm_.reset();
        a_.reset();
        b_.reset();
        db_.reset();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        TestBase::TearDown();
    }

    /// One sleeve "A" holding the whole book, in backtest mode unless told otherwise.
    void make_pm(bool backtest = true, PortfolioConfig pc = one_pass_config("A")) {
        static int n = 0;
        pm_ = std::make_unique<PortfolioManager>(pc, "PM_OP_" + std::to_string(++n));
        pm_->set_backtest_mode(backtest);
        a_ = make_overlay_stub("A", 500000.0, db_);
        ASSERT_TRUE(a_->initialize().is_ok());
        ASSERT_TRUE(a_->start().is_ok());
        ASSERT_TRUE(pm_->add_strategy(a_, 1.0, true).is_ok());
    }

    /// Two sleeves "A" (the overlay sleeve) and "B", half the book each.
    void make_two_sleeves() {
        static int n = 0;
        pm_ = std::make_unique<PortfolioManager>(one_pass_config("A"), "PM_OP2_" + std::to_string(++n));
        pm_->set_backtest_mode(true);
        a_ = make_overlay_stub("A", 250000.0, db_);
        b_ = make_overlay_stub("B", 250000.0, db_);
        for (auto& s : {a_, b_}) {
            ASSERT_TRUE(s->initialize().is_ok());
            ASSERT_TRUE(s->start().is_ok());
            ASSERT_TRUE(pm_->add_strategy(s, 0.5, true).is_ok());
        }
    }

    static OverlayStubStrategy::Row row(double optimal, double forecast = 10.0) {
        OverlayStubStrategy::Row r;
        r.close = 100.0;
        r.multiplier = 1000.0;
        r.optimal = optimal;
        r.forecast = forecast;
        return r;
    }

    /// One rebalance on day `d`: a bar of every symbol in `symbols`, stamped the next day.
    Result<void> rebalance(int d, const std::vector<std::string>& symbols, bool warmup = false,
                           const std::unordered_set<std::string>* session = nullptr) {
        std::vector<Bar> bars;
        for (const auto& s : symbols) bars.push_back(one_pass_bar(s, d, 100.0));
        return pm_->process_market_data(bars, warmup, one_pass_day(d + 1), session);
    }

    double quantity(const std::string& sleeve, const std::string& symbol) {
        const auto books = pm_->get_strategy_positions();
        const auto& book = books.at(sleeve);
        const auto it = book.find(symbol);
        return it == book.end() ? 0.0 : static_cast<double>(it->second.quantity);
    }

    /// The signed fills of `symbol` a sleeve has been given so far, in order.
    std::vector<double> fills(const std::string& sleeve, const std::string& symbol) {
        std::vector<double> out;
        const auto all = pm_->get_strategy_executions();
        const auto it = all.find(sleeve);
        if (it == all.end()) return out;
        for (const auto& e : it->second) {
            if (e.symbol != symbol) continue;
            out.push_back(static_cast<double>(e.filled_quantity) * (e.side == Side::BUY ? 1.0 : -1.0));
        }
        return out;
    }

    std::shared_ptr<MockPostgresDatabase> db_;
    std::unique_ptr<PortfolioManager> pm_;
    std::shared_ptr<OverlayStubStrategy> a_;
    std::shared_ptr<OverlayStubStrategy> b_;
};

// One search from the HELD book, one buffer, one rounding: 0 -> 5 stores 4 with one fill at the
// signal close, and the next day's gap of one contract is no trade. The held book of a backtest is
// the filled ledger, so day 2 starts from day 1's stored 4.
TEST_F(OnePassBookTest, TheBookIsOnePassFromTheHeldBookAndTheLedgerIsTheStoredBook) {
    make_pm();
    a_->rows["AAA"] = row(5.0);

    ::testing::internal::CaptureStdout();
    ASSERT_TRUE(rebalance(1, {"AAA"}).is_ok());
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(quantity("A", "AAA"), 4.0);
    EXPECT_EQ(fills("A", "AAA"), (std::vector<double>{4.0}));
    const auto execs = pm_->get_strategy_executions().at("A");
    ASSERT_EQ(execs.size(), 1u);
    EXPECT_EQ(static_cast<double>(execs[0].fill_price), 100.0) << "filled at the signal close";
    EXPECT_EQ(execs[0].fill_time, one_pass_day(2)) << "stamped on the cycle's own day";
    EXPECT_GT(static_cast<double>(execs[0].total_transaction_costs), 0.0);
    EXPECT_EQ(execs[0].exec_id, "EX-A-0");

    // One OVERLAY, one OPTIMISER and one BOOK line; none of the lap loop's lines.
    EXPECT_EQ(count_of(out, "OVERLAY m=1 binding=none"), 1u) << out;
    EXPECT_EQ(count_of(out, "OPTIMISER searched=1"), 1u) << out;
    EXPECT_EQ(count_of(out, "BOOK raw_target_gross="), 1u) << out;
    for (const char* retired : {"Iteration 1 of dynamic optimization", "Iteration 2", "RISK_APPLIED",
                                "RISK_CUT_ONCE", "RISK_CUT_BOOK", "Max iterations reached",
                                "Final forced rounding", "RISK_CONVERGED_SNAP", "BOOK_GATE backtest"}) {
        EXPECT_EQ(count_of(out, retired), 0u) << retired << "\n" << out;
    }

    // The record the runners store: no cut, blind (the stub gives no window), the delivered scale
    // 4 / 5, and one decision row of the overlay's module at lap 1.
    const OnePassDay day = pm_->last_one_pass();
    EXPECT_TRUE(day.ran);
    EXPECT_TRUE(day.sized);
    EXPECT_FALSE(day.refused);
    EXPECT_TRUE(day.stores_detail());
    EXPECT_DOUBLE_EQ(day.risk_requested, 1.0);
    EXPECT_EQ(day.binding_term, "none");
    EXPECT_TRUE(day.overlay_blind);
    EXPECT_DOUBLE_EQ(day.sizing_capital, 500000.0);
    EXPECT_NEAR(day.risk_scale, 0.8, 1e-12);
    EXPECT_NEAR(day.capped_target_gross, 500000.0, 1e-6);
    EXPECT_NEAR(day.stored_gross, 400000.0, 1e-6);
    EXPECT_TRUE(day.sign_closes.empty());
    const auto rows = pm_->last_risk_decisions();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].module_id, "carver");
    EXPECT_EQ(rows[0].phase, RiskPhase::LAP);
    EXPECT_EQ(rows[0].lap, 1);
    EXPECT_EQ(rows[0].applied_action, RiskAction::NONE);

    // Day 2, the same target: the search answers 5, one contract from the held 4, inside the
    // buffer. No trade, no second fill.
    ASSERT_TRUE(rebalance(2, {"AAA"}).is_ok());
    EXPECT_EQ(quantity("A", "AAA"), 4.0);
    EXPECT_EQ(fills("A", "AAA"), (std::vector<double>{4.0}));
    EXPECT_NEAR(pm_->last_one_pass().risk_scale, 0.8, 1e-12);

    // Day 3, the target at 9: 4 -> 9 is five contracts, the buffer moves four of them.
    a_->rows["AAA"].optimal = 9.0;
    ASSERT_TRUE(rebalance(3, {"AAA"}).is_ok());
    EXPECT_EQ(quantity("A", "AAA"), 8.0);
    EXPECT_EQ(fills("A", "AAA"), (std::vector<double>{4.0, 4.0}));
}

// Section 4 and 5.3 (D14, F1): the overlay reads the CAPPED target and the stored book never holds
// a free row beyond the cap. A target of 30 contracts is a weight of 6; the cap of 2 is 10.
TEST_F(OnePassBookTest, TheTargetIsCappedBeforeTheOverlayAndTheStoredRowStaysInsideTheCap) {
    make_pm();
    a_->rows["AAA"] = row(30.0);
    ASSERT_TRUE(rebalance(1, {"AAA"}).is_ok());
    EXPECT_EQ(quantity("A", "AAA"), 9.0) << "0 -> 10 (the capped target), nine through the buffer";
    const OnePassDay day = pm_->last_one_pass();
    EXPECT_NEAR(day.capped_target_gross, 1'000'000.0, 1e-6) << "the capped 10 contracts, never the 30";
    EXPECT_NEAR(day.risk_scale, 0.9, 1e-12);
    for (int d = 2; d <= 6; ++d) {
        ASSERT_TRUE(rebalance(d, {"AAA"}).is_ok());
        EXPECT_LE(quantity("A", "AAA"), 10.0) << "day " << d;
    }
}

// Section 4: m is applied ONCE to the capped target, and the request is recorded. Three longs and
// two shorts at the cap are a gross leverage of 10; against a limit of 8.1 that is m = 0.81,
// binding L_g, each row scaled to 8.1 contracts and stored at 8 (a gross of 8.0, inside the limit:
// no trim). The decision row is one SCALE at lap 1.
TEST_F(OnePassBookTest, TheOverlayCutsOnceAndTheRequestIsRecorded) {
    PortfolioConfig pc = one_pass_config("A");
    std::get<CarverModuleConfig>(pc.risk_modules[0].params).max_gross_leverage = 8.1;
    make_pm(true, pc);
    for (const char* s : {"AAA", "BBB", "CCC"}) a_->rows[s] = row(30.0);
    for (const char* s : {"DDD", "EEE"}) a_->rows[s] = row(-30.0, -10.0);
    ::testing::internal::CaptureStdout();
    ASSERT_TRUE(rebalance(1, {"AAA", "BBB", "CCC", "DDD", "EEE"}).is_ok());
    const std::string out = ::testing::internal::GetCapturedStdout();
    const OnePassDay day = pm_->last_one_pass();
    EXPECT_NEAR(day.risk_requested, 0.81, 1e-12);
    EXPECT_EQ(day.binding_term, "L_g");
    for (const char* s : {"AAA", "BBB", "CCC"}) EXPECT_EQ(quantity("A", s), 8.0) << s;
    for (const char* s : {"DDD", "EEE"}) EXPECT_EQ(quantity("A", s), -8.0) << s;
    EXPECT_NEAR(day.risk_scale, 0.8, 1e-12) << "the stored gross 8.0 over the capped target's 10.0";
    EXPECT_EQ(day.over_limit_after_rounding_terms, "") << "the stored book is inside the limit";
    EXPECT_EQ(count_of(out, "OVERLAY m=0.81 binding=L_g"), 1u) << out;
    EXPECT_EQ(count_of(out, "RISK_TRIM"), 0u) << out;
    const auto rows = pm_->last_risk_decisions();
    ASSERT_EQ(rows.size(), 1u) << "one row: the overlay answers once per rebalance";
    EXPECT_EQ(rows[0].applied_action, RiskAction::SCALE);
    EXPECT_EQ(rows[0].lap, 1);
    EXPECT_NEAR(rows[0].requested.scale, 0.81, 1e-12);
    EXPECT_EQ(rows[0].applied_factor.raw_value(), Decimal(0.81).raw_value());

    // Section 6.4: with the limit at 7.9 the scaled target is 7.9 a row, the rounding stores 8,
    // the stored gross 8.0 is over, and the trim removes ONE contract (the stored gross 7.8).
    make_pm(true, [] {
        PortfolioConfig tight = one_pass_config("A");
        std::get<CarverModuleConfig>(tight.risk_modules[0].params).max_gross_leverage = 7.9;
        return tight;
    }());
    for (const char* s : {"AAA", "BBB", "CCC"}) a_->rows[s] = row(30.0);
    for (const char* s : {"DDD", "EEE"}) a_->rows[s] = row(-30.0, -10.0);
    ASSERT_TRUE(rebalance(1, {"AAA", "BBB", "CCC", "DDD", "EEE"}).is_ok());
    double gross = 0.0;
    for (const char* s : {"AAA", "BBB", "CCC", "DDD", "EEE"}) gross += std::abs(quantity("A", s));
    EXPECT_EQ(gross, 39.0) << "five rows of 8 less the one contract the trim removed";
    EXPECT_EQ(pm_->last_one_pass().over_limit_after_rounding_terms, "");
}

// Section 6.1: a symbol outside the backtest's session set is HELD on every rebalance: its row is
// fixed at the held quantity (never opened at a stale price when flat, never filled when held),
// and it re-enters the search on its next session bar.
TEST_F(OnePassBookTest, ASymbolOutsideTheSessionSetIsHeldAndFillsOnItsNextSession) {
    make_pm();
    a_->rows["AAA"] = row(5.0);
    a_->rows["BBB"] = row(5.0);
    const std::unordered_set<std::string> only_a = {"AAA"};
    const std::unordered_set<std::string> both = {"AAA", "BBB"};

    ASSERT_TRUE(rebalance(1, {"AAA", "BBB"}, false, &only_a).is_ok());
    EXPECT_EQ(quantity("A", "AAA"), 4.0);
    EXPECT_EQ(quantity("A", "BBB"), 0.0) << "flat and held: never opened";
    EXPECT_TRUE(fills("A", "BBB").empty());

    ASSERT_TRUE(rebalance(2, {"AAA", "BBB"}, false, &both).is_ok());
    EXPECT_EQ(quantity("A", "BBB"), 4.0);
    EXPECT_EQ(fills("A", "BBB"), (std::vector<double>{4.0}));

    // Held at a quantity: a target of 9 would trade four more, the hold trades none, with or
    // without a bar of the symbol in the group.
    a_->rows["BBB"].optimal = 9.0;
    ASSERT_TRUE(rebalance(3, {"AAA", "BBB"}, false, &only_a).is_ok());
    EXPECT_EQ(quantity("A", "BBB"), 4.0);
    ASSERT_TRUE(rebalance(4, {"AAA"}, false, &only_a).is_ok());
    EXPECT_EQ(quantity("A", "BBB"), 4.0);
    EXPECT_EQ(fills("A", "BBB"), (std::vector<double>{4.0}));
    ASSERT_TRUE(rebalance(5, {"AAA", "BBB"}, false, &both).is_ok());
    EXPECT_EQ(quantity("A", "BBB"), 8.0);
    EXPECT_EQ(fills("A", "BBB"), (std::vector<double>{4.0, 4.0}));
}

// Section 6.1, the live runners' half: set_hold_set gives the caller's hold set for ONE rebalance,
// and the held book of a run that is not a backtest is the seeded book the call started with.
TEST_F(OnePassBookTest, TheCallersHoldSetHoldsTheSeededBookForOneRebalance) {
    make_pm(/*backtest=*/false);
    a_->rows["AAA"] = row(9.0);
    a_->rows["BBB"] = row(9.0);
    Position seed;
    seed.symbol = "AAA";
    seed.quantity = Decimal(4.0);
    seed.average_price = Decimal(100.0);
    ASSERT_TRUE(pm_->update_strategy_position("A", "AAA", seed).is_ok());
    seed.symbol = "BBB";
    ASSERT_TRUE(pm_->update_strategy_position("A", "BBB", seed).is_ok());

    pm_->set_hold_set({"BBB"});
    ASSERT_TRUE(pm_->process_market_data({one_pass_bar("AAA", 1, 100.0), one_pass_bar("BBB", 1, 100.0)})
                    .is_ok());
    EXPECT_EQ(quantity("A", "AAA"), 8.0) << "from the seeded 4, not from flat (which would store 8 too "
                                            "only by 0 -> 9; the fill below tells them apart)";
    EXPECT_EQ(fills("A", "AAA"), (std::vector<double>{4.0})) << "the trade is 4 -> 8";
    EXPECT_EQ(quantity("A", "BBB"), 4.0) << "held at the seeded quantity";
    EXPECT_TRUE(fills("A", "BBB").empty());

    // The set was this call's only.
    ASSERT_TRUE(pm_->process_market_data({one_pass_bar("AAA", 2, 100.0), one_pass_bar("BBB", 2, 100.0)})
                    .is_ok());
    EXPECT_EQ(quantity("A", "BBB"), 8.0);
}

// Section 5.2: a free symbol held on the other side of its target is closed with ONE fill to flat
// before the search, and the move from flat is a second fill. The closes are in the record, per
// sleeve, for the live runners to book.
TEST_F(OnePassBookTest, AForecastSignCloseIsItsOwnFillBeforeTheSearch) {
    make_pm();
    a_->rows["AAA"] = row(5.0);
    ASSERT_TRUE(rebalance(1, {"AAA"}).is_ok());
    ASSERT_EQ(quantity("A", "AAA"), 4.0);

    a_->rows["AAA"] = row(-5.0, -10.0);
    ::testing::internal::CaptureStdout();
    ASSERT_TRUE(rebalance(2, {"AAA"}).is_ok());
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(quantity("A", "AAA"), -4.0);
    EXPECT_EQ(fills("A", "AAA"), (std::vector<double>{4.0, -4.0, -4.0}))
        << "the close to flat, then the move from flat: two fills, not one of eight";
    const OnePassDay day = pm_->last_one_pass();
    ASSERT_EQ(day.sign_closes.count("A"), 1u);
    EXPECT_EQ(day.sign_closes.at("A").at("AAA"), -4.0);
    EXPECT_EQ(count_of(out, "sign_closes=[AAA]"), 1u) << out;
}

// Section 5.2 (D39), the deferral band: held against the first sleeve's forecast while |F| < 2,
// the symbol is HELD; at |F| = 2 exactly it is free and the sign close applies. A zero forecast
// has no sign and is never in the band.
TEST_F(OnePassBookTest, TheDeferralBandHoldsUntilTheForecastReachesTwo) {
    make_pm();
    a_->rows["AAA"] = row(5.0);
    ASSERT_TRUE(rebalance(1, {"AAA"}).is_ok());
    ASSERT_EQ(quantity("A", "AAA"), 4.0);

    a_->rows["AAA"] = row(-0.4, -1.99);
    ::testing::internal::CaptureStdout();
    ASSERT_TRUE(rebalance(2, {"AAA"}).is_ok());
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(quantity("A", "AAA"), 4.0) << "inside the band: held";
    EXPECT_EQ(fills("A", "AAA"), (std::vector<double>{4.0}));
    EXPECT_EQ(count_of(out, "band_holds=[AAA]"), 1u) << out;

    a_->rows["AAA"] = row(0.0, 0.0);
    ASSERT_TRUE(rebalance(3, {"AAA"}).is_ok());
    EXPECT_LT(quantity("A", "AAA"), 4.0) << "a zero forecast is not in the band: the book moves "
                                            "toward its zero target";
    const double after_zero = quantity("A", "AAA");
    EXPECT_GE(after_zero, 0.0);

    // Back to +4 for the strict edge.
    a_->rows["AAA"] = row(9.0, 10.0);
    for (int d = 4; d <= 8; ++d) ASSERT_TRUE(rebalance(d, {"AAA"}).is_ok());
    const double held = quantity("A", "AAA");
    ASSERT_GT(held, 0.0);
    const size_t before = fills("A", "AAA").size();
    a_->rows["AAA"] = row(-0.4, -2.0);
    ASSERT_TRUE(rebalance(9, {"AAA"}).is_ok());
    EXPECT_EQ(quantity("A", "AAA"), 0.0) << "at |F| = 2 the symbol is free: closed to flat";
    ASSERT_EQ(fills("A", "AAA").size(), before + 1);
    EXPECT_EQ(fills("A", "AAA").back(), -held) << "ONE fill, the whole held quantity";
}

// Section 6.2: a symbol that signalled before and no longer signals is closed with one fill to
// flat on a session bar, not walked down through the buffer.
TEST_F(OnePassBookTest, ASymbolThatStopsSignallingIsClosedWithOneFill) {
    make_pm();
    a_->rows["AAA"] = row(9.0);
    ASSERT_TRUE(rebalance(1, {"AAA"}).is_ok());
    ASSERT_EQ(quantity("A", "AAA"), 8.0);
    a_->rows["AAA"].signalling = false;
    ASSERT_TRUE(rebalance(2, {"AAA"}).is_ok());
    EXPECT_EQ(quantity("A", "AAA"), 0.0);
    EXPECT_EQ(fills("A", "AAA"), (std::vector<double>{8.0, -8.0}));
}

// Section 5.4: one search on the summed book, split back in proportion to the sleeves' unrounded
// contributions by largest remainder. A symbol no sleeve targets any more is split in proportion
// to what each sleeve holds, so the book the buffer keeps is not handed to nobody.
TEST_F(OnePassBookTest, TheBookIsSplitBackToTheSleevesByLargestRemainder) {
    make_two_sleeves();
    a_->rows["AAA"] = row(3.0);
    b_->rows["AAA"] = row(1.0);
    ASSERT_TRUE(rebalance(1, {"AAA"}).is_ok());
    // The summed target 4 from flat stores 3; the quotas are 2.25 and 0.75.
    EXPECT_EQ(quantity("A", "AAA"), 2.0);
    EXPECT_EQ(quantity("B", "AAA"), 1.0);
    EXPECT_EQ(fills("A", "AAA"), (std::vector<double>{2.0}));
    EXPECT_EQ(fills("B", "AAA"), (std::vector<double>{1.0}));

    // Both sleeves now target zero. The search walks to 0, the buffer keeps 1 of the held 3; the
    // split is by the held 2 and 1 (quotas 2/3 and 1/3).
    a_->rows["AAA"] = row(0.0, 0.0);
    b_->rows["AAA"] = row(0.0, 0.0);
    ASSERT_TRUE(rebalance(2, {"AAA"}).is_ok());
    EXPECT_EQ(quantity("A", "AAA") + quantity("B", "AAA"), 1.0);
    EXPECT_EQ(quantity("A", "AAA"), 1.0);
    EXPECT_EQ(quantity("B", "AAA"), 0.0);
    EXPECT_EQ(fills("A", "AAA"), (std::vector<double>{2.0, -1.0}));
    EXPECT_EQ(fills("B", "AAA"), (std::vector<double>{1.0, -1.0}));
}

// Section 6.1: the hold applies on sized days only. A warm-up cycle stores nothing: its book
// follows the search, no fill is generated, the ledger stays flat, and the first sized day starts
// from that flat ledger.
TEST_F(OnePassBookTest, AWarmUpCycleFillsNothingAndTheFirstSizedDayStartsFlat) {
    make_pm();
    a_->rows["AAA"] = row(5.0);
    const std::unordered_set<std::string> nobody;
    ::testing::internal::CaptureStdout();
    ASSERT_TRUE(rebalance(1, {"AAA"}, /*warmup=*/true, &nobody).is_ok());
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(quantity("A", "AAA"), 4.0) << "the warm-up book follows the search; no hold applies";
    EXPECT_TRUE(fills("A", "AAA").empty());
    EXPECT_FALSE(pm_->last_one_pass().sized);
    EXPECT_FALSE(pm_->last_one_pass().stores_detail());
    EXPECT_EQ(count_of(out, "RISK_TRIM"), 0u);
    {
        const auto ledger = pm_->get_filled_strategy_positions();
        EXPECT_TRUE(ledger.count("A") == 0 || ledger.at("A").empty()) << "nothing was filled";
    }

    const std::unordered_set<std::string> session = {"AAA"};
    ASSERT_TRUE(rebalance(2, {"AAA"}, false, &session).is_ok());
    EXPECT_EQ(fills("A", "AAA"), (std::vector<double>{4.0})) << "from the flat ledger, not from the "
                                                                "warm-up book";
}

// Section 4, the REFUSE contract: an overlay that cannot answer refuses the scope. No search, no
// trim, no fill; the held book is stored; the decision row is a REFUSE at phase "lap", lap 1; the
// record stores no risk_detail; one OVERLAY line and no OPTIMISER line.
TEST_F(OnePassBookTest, AnOverlayThatCannotAnswerRefusesAndTheHeldBookIsStored) {
    make_pm();
    a_->rows["AAA"] = row(5.0);
    ASSERT_TRUE(rebalance(1, {"AAA"}).is_ok());
    ASSERT_EQ(quantity("A", "AAA"), 4.0);

    a_->rows["AAA"].optimal = std::numeric_limits<double>::quiet_NaN();
    ::testing::internal::CaptureStdout();
    const auto refused = rebalance(2, {"AAA"});
    const std::string out = ::testing::internal::GetCapturedStdout();
    ASSERT_TRUE(refused.is_ok()) << "a seeded scope holds its book; the run goes on";
    EXPECT_EQ(quantity("A", "AAA"), 4.0);
    EXPECT_EQ(fills("A", "AAA"), (std::vector<double>{4.0}));
    const OnePassDay day = pm_->last_one_pass();
    EXPECT_TRUE(day.ran);
    EXPECT_TRUE(day.refused);
    EXPECT_FALSE(day.stores_detail());
    EXPECT_EQ(count_of(out, "OVERLAY refused"), 1u) << out;
    EXPECT_EQ(count_of(out, "OPTIMISER"), 0u) << out;
    const auto mark = portfolio_risk_refusal(pm_->last_risk_decisions());
    ASSERT_TRUE(mark.has_value());
    EXPECT_EQ(mark->value("action", ""), "REFUSE");
    EXPECT_EQ(mark->value("module", ""), "carver");
    EXPECT_EQ(mark->value("phase", ""), "lap");
    EXPECT_EQ(mark->value("lap", 0), 1);
    EXPECT_FALSE(mark->value("reason", "").empty());

    // The overlay answers again: the book trades from the held 4.
    a_->rows["AAA"].optimal = 9.0;
    ASSERT_TRUE(rebalance(3, {"AAA"}).is_ok());
    EXPECT_EQ(quantity("A", "AAA"), 8.0);
}

// The same refusal on a live scope whose previous book was never seeded refuses the RUN: storing a
// flat book would read as "liquidate everything".
TEST_F(OnePassBookTest, ARefusalOnAnUnseededLiveScopeRefusesTheRun) {
    make_pm(/*backtest=*/false);
    a_->rows["AAA"] = row(std::numeric_limits<double>::quiet_NaN());
    const auto r = pm_->process_market_data({one_pass_bar("AAA", 1, 100.0)});
    ASSERT_TRUE(r.is_error());
    EXPECT_EQ(r.error()->code(), ErrorCode::RISK_LIMIT_EXCEEDED);
    EXPECT_NE(std::string(r.error()->what()).find("never seeded"), std::string::npos);
    ASSERT_TRUE(portfolio_risk_refusal(pm_->last_risk_decisions()).has_value());

    // Seeded, the same refusal holds the seeded book and the call succeeds.
    Position seed;
    seed.symbol = "AAA";
    seed.quantity = Decimal(3.0);
    seed.average_price = Decimal(100.0);
    ASSERT_TRUE(pm_->update_strategy_position("A", "AAA", seed).is_ok());
    ASSERT_TRUE(pm_->process_market_data({one_pass_bar("AAA", 2, 100.0)}).is_ok());
    EXPECT_EQ(quantity("A", "AAA"), 3.0);
    EXPECT_TRUE(pm_->last_one_pass().refused);
}

// A symbol the first sleeve has no series for is held at each sleeve's held quantity and named:
// no silent default for its price, multiplier or cost (section 5.1).
TEST_F(OnePassBookTest, ASymbolWithoutASeriesIsHeldAndNamed) {
    make_two_sleeves();
    a_->rows["AAA"] = row(5.0);
    b_->rows["AAA"] = row(0.0, 0.0);
    b_->rows["ZZZ"] = row(5.0);  // the overlay sleeve A knows nothing of ZZZ
    ::testing::internal::CaptureStdout();
    ASSERT_TRUE(rebalance(1, {"AAA", "ZZZ"}).is_ok());
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(quantity("B", "ZZZ"), 0.0);
    EXPECT_TRUE(fills("B", "ZZZ").empty());
    EXPECT_EQ(count_of(out, "BOOK_UNPRICED ZZZ"), 1u) << out;
    EXPECT_EQ(quantity("A", "AAA"), 4.0) << "the rest of the book trades";
}

// The sizing capital the pass reads is the one the runner set today (section 3.1): the same
// target is fewer contracts' worth of weight on a larger capital, and the record carries E_t.
TEST_F(OnePassBookTest, ThePassSizesOnTheCapitalSetForTheRebalance) {
    make_pm();
    a_->rows["AAA"] = row(30.0);
    ASSERT_TRUE(pm_->set_sizing_capital(1'000'000.0).is_ok());
    ASSERT_TRUE(rebalance(1, {"AAA"}).is_ok());
    // u is 0.1 on 1,000,000: the cap of 2 is 20 contracts, and 0 -> 20 stores 19.
    EXPECT_EQ(quantity("A", "AAA"), 19.0);
    EXPECT_DOUBLE_EQ(pm_->last_one_pass().sizing_capital, 1'000'000.0);
}

// A book that names an overlay sleeve runs the overlay and nothing else at portfolio scope: a
// module list that is not exactly the one carver module carrying the limits is an error, never a
// book that quietly runs something else.
TEST_F(OnePassBookTest, AnOverlayBookWithoutItsLimitsIsAnError) {
    static int n = 0;
    PortfolioConfig pc = one_pass_config("A");
    std::get<CarverModuleConfig>(pc.risk_modules[0].params).r_max = 0.0;  // no overlay limits
    pm_ = std::make_unique<PortfolioManager>(pc, "PM_OP_BAD_" + std::to_string(++n));
    pm_->set_backtest_mode(true);
    a_ = make_overlay_stub("A", 500000.0, db_);
    ASSERT_TRUE(a_->initialize().is_ok());
    ASSERT_TRUE(a_->start().is_ok());
    ASSERT_TRUE(pm_->add_strategy(a_, 1.0, true).is_ok());
    a_->rows["AAA"] = row(5.0);
    const auto r = rebalance(1, {"AAA"});
    ASSERT_TRUE(r.is_error());
    EXPECT_NE(std::string(r.error()->what()).find("overlay"), std::string::npos);
    EXPECT_TRUE(fills("A", "AAA").empty());
}

// Section 7.3: risk_detail's nine keys, flat, in the table's order, with null where nothing is
// marked. Pinned here: a key added, removed or renamed fails.
TEST(RiskDetailJson, TheNineKeysArePinned) {
    OnePassDay day;
    day.ran = day.sized = true;
    day.risk_requested = 0.8;
    day.binding_term = "L_g";
    day.overlay_blind = false;
    day.sizing_capital = 487250.5;
    const nlohmann::json plain = risk_detail_json(day, 512300.25);
    std::vector<std::string> keys;
    for (const auto& item : plain.items()) keys.push_back(item.key());
    std::vector<std::string> want(kRiskDetailKeys.begin(), kRiskDetailKeys.end());
    std::sort(want.begin(), want.end());
    EXPECT_EQ(keys, want);
    EXPECT_EQ(plain.size(), 9u);
    EXPECT_DOUBLE_EQ(plain.at("risk_requested").get<double>(), 0.8);
    EXPECT_EQ(plain.at("binding_term").get<std::string>(), "L_g");
    EXPECT_TRUE(plain.at("over_limit_after_rounding_terms").is_null());
    EXPECT_TRUE(plain.at("over_limit_after_rounding_excess").is_null());
    EXPECT_TRUE(plain.at("over_limit_by_hold_terms").is_null());
    EXPECT_TRUE(plain.at("over_limit_by_hold_symbols").is_null());
    EXPECT_EQ(plain.at("overlay_blind").get<bool>(), false);
    EXPECT_DOUBLE_EQ(plain.at("sizing_capital").get<double>(), 487250.5);
    EXPECT_DOUBLE_EQ(plain.at("account_value").get<double>(), 512300.25);

    day.over_limit_after_rounding_terms = "R;L_g";
    day.over_limit_after_rounding_excess = 0.25;
    day.over_limit_by_hold_terms = "CAP";
    day.over_limit_by_hold_symbols = "ZN.v.0 ZT.v.0";
    const nlohmann::json marked = risk_detail_json(day, 512300.25);
    EXPECT_EQ(marked.size(), 9u);
    EXPECT_EQ(marked.at("over_limit_after_rounding_terms").get<std::string>(), "R;L_g");
    EXPECT_DOUBLE_EQ(marked.at("over_limit_after_rounding_excess").get<double>(), 0.25);
    EXPECT_EQ(marked.at("over_limit_by_hold_terms").get<std::string>(), "CAP");
    EXPECT_EQ(marked.at("over_limit_by_hold_symbols").get<std::string>(), "ZN.v.0 ZT.v.0");

    // A refused rebalance, a warm-up cycle and a book with no overlay store none.
    OnePassDay refused = day;
    refused.refused = true;
    EXPECT_FALSE(refused.stores_detail());
    OnePassDay warm = day;
    warm.sized = false;
    EXPECT_FALSE(warm.stores_detail());
    EXPECT_FALSE(OnePassDay{}.stores_detail());
    EXPECT_TRUE(day.stores_detail());
}
