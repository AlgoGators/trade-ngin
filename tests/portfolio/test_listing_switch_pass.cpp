// Listing dates at the portfolio manager (data/listing_dates.hpp): the switch from a predecessor
// contract to its listed contract is made inside the backtest's one pass, once per pair, before
// anything reads the held book. These tests run the real pass on a stub sleeve and pin what the
// manager does: which fills are written (LC- the predecessor's exit, LO- the listed contract's
// entry), at what quantity under the default rule (open_at_target), when the switch waits, when
// there is no switch at all, and that nothing of it is reached with no contracts set or in live.
//
// The numbers. The predecessor TBIG is 1,000 x 100 = 100,000 of notional a contract (u = 0.2 on the
// 500,000 book), the listed contract TMIC a tenth of it (u = 0.02); the per-name cap of 2 is 10 and
// 100 contracts. The stub gives no window, so the optimiser's covariance is the guarded 0.01
// diagonal: one TBIG away from the target is a tracking error of 0.02, one TMIC 0.002, and the
// buffer is the larger of 0.01 and the largest held or wanted contract's own 0.02 or 0.002.

#include <gtest/gtest.h>

#include <chrono>
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
#include "trade_ngin/data/listing_dates.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

const std::string kBig = "TBIG.v.0";
const std::string kMicro = "TMIC.v.0";
constexpr int kBefore = 1;  // a signal day before the listing date
constexpr int kListed = 5;  // one_pass_day(5) = 2026-01-06, the listing date

std::vector<ListedContract> the_pair() { return {{"TMIC", "TBIG", "2026-01-06", 10.0}}; }

}  // namespace

class ListingSwitchPassTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ListingDates::instance().clear();
        ListingDates::instance().set_switch_rule(ListingSwitchRule::kOpenAtTarget);
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
        db_.reset();
        ListingDates::instance().clear();
        ListingDates::instance().set_switch_rule(ListingSwitchRule::kOpenAtTarget);
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        TestBase::TearDown();
    }

    void make_pm(bool backtest = true) {
        static int n = 0;
        pm_ = std::make_unique<PortfolioManager>(one_pass_config("A"), "PM_LS_" + std::to_string(++n));
        pm_->set_backtest_mode(backtest);
        a_ = make_overlay_stub("A", 500000.0, db_);
        ASSERT_TRUE(a_->initialize().is_ok());
        ASSERT_TRUE(a_->start().is_ok());
        ASSERT_TRUE(pm_->add_strategy(a_, 1.0, true).is_ok());
    }

    static OverlayStubStrategy::Row row(double multiplier, double optimal, double forecast, bool signalling) {
        OverlayStubStrategy::Row r;
        r.close = 100.0;
        r.multiplier = multiplier;
        r.optimal = optimal;
        r.forecast = forecast;
        r.signalling = signalling;
        return r;
    }

    /// The sleeve's answers before the listing date: the predecessor signals, the listed contract
    /// does not (the trend sleeve's tradeable window, stated here by hand).
    void before(double big_optimal, double big_forecast = 10.0) {
        a_->rows[kBig] = row(1000.0, big_optimal, big_forecast, true);
        a_->rows[kMicro] = row(100.0, 0.0, 0.0, false);
    }
    /// From the listing date: the listed contract signals, the predecessor no longer does.
    void listed(double micro_optimal, double micro_forecast = 10.0) {
        a_->rows[kBig] = row(1000.0, 0.0, 0.0, false);
        a_->rows[kMicro] = row(100.0, micro_optimal, micro_forecast, true);
    }

    /// One rebalance on signal day `d` (a bar of both contracts), stamped the next day.
    Result<void> rebalance(int d, bool warmup = false,
                           const std::unordered_set<std::string>* session = nullptr) {
        std::vector<Bar> bars = {one_pass_bar(kBig, d, 100.0), one_pass_bar(kMicro, d, 100.0)};
        for (const auto& b : bars) {
            pm_->update_cost_manager_market_data(b.symbol, b.volume, static_cast<double>(b.close),
                                                 static_cast<double>(b.close));
        }
        return pm_->process_market_data(bars, warmup, one_pass_day(d + 1), session);
    }

    double quantity(const std::string& symbol) {
        const auto books = pm_->get_strategy_positions();
        const auto& book = books.at("A");
        const auto it = book.find(symbol);
        return it == book.end() ? 0.0 : static_cast<double>(it->second.quantity);
    }

    std::vector<ExecutionReport> executions() {
        const auto all = pm_->get_strategy_executions();
        const auto it = all.find("A");
        return it == all.end() ? std::vector<ExecutionReport>{} : it->second;
    }

    /// "id symbol signed-quantity" of every execution so far whose id starts with `prefix`.
    std::vector<std::string> fills(const std::string& prefix) {
        std::vector<std::string> out;
        for (const auto& e : executions()) {
            if (e.exec_id.rfind(prefix, 0) != 0) continue;
            const double q = static_cast<double>(e.filled_quantity) * (e.side == Side::BUY ? 1.0 : -1.0);
            out.push_back(e.exec_id + " " + e.symbol + " " + std::to_string(static_cast<int>(q)));
        }
        return out;
    }
    std::vector<std::string> switch_fills() {
        std::vector<std::string> out = fills("LC-");
        const auto opens = fills("LO-");
        out.insert(out.end(), opens.begin(), opens.end());
        return out;
    }

    std::shared_ptr<MockPostgresDatabase> db_;
    std::unique_ptr<PortfolioManager> pm_;
    std::shared_ptr<OverlayStubStrategy> a_;
};

// The default rule: a held predecessor is exited and the listed contract entered at that day's
// rounded target, both as costed fills at the signal close; the pass then starts from that book and
// has nothing left to trade in the pair.
TEST_F(ListingSwitchPassTest, AHeldPredecessorIsExitedAndTheListedContractEnteredAtItsRoundedTarget) {
    ListingDates::instance().set(the_pair());
    make_pm();
    before(3.0);
    ASSERT_TRUE(rebalance(kBefore).is_ok());
    ASSERT_EQ(quantity(kBig), 2.0) << "0 -> 3 through the buffer stores 2";
    ASSERT_TRUE(switch_fills().empty()) << "nothing is switched before the listing date";

    listed(24.4);
    ASSERT_TRUE(rebalance(kListed).is_ok());
    EXPECT_EQ(switch_fills(), (std::vector<std::string>{"LC-A-0 TBIG.v.0 -2", "LO-A-0 TMIC.v.0 24"}));
    EXPECT_EQ(quantity(kBig), 0.0);
    EXPECT_EQ(quantity(kMicro), 24.0) << "the pass starts from the entered 24 and trades nothing more";
    EXPECT_EQ(fills("EX-").size(), 1u) << "the only ordinary fill is the first day's purchase";
    for (const auto& e : executions()) {
        if (e.exec_id.rfind("L", 0) != 0) continue;
        EXPECT_EQ(e.execution_type, ExecutionType::STRATEGY);
        EXPECT_EQ(static_cast<double>(e.fill_price), 100.0) << "the signal close";
        EXPECT_EQ(e.fill_time, one_pass_day(kListed + 1)) << "stamped on the cycle's own day";
        EXPECT_GT(static_cast<double>(e.commissions_fees), 0.0) << e.exec_id << " is costed";
        EXPECT_GT(static_cast<double>(e.total_transaction_costs), 0.0) << e.exec_id << " is costed";
    }

    // once per pair: the next day is an ordinary day
    ASSERT_TRUE(rebalance(kListed + 1).is_ok());
    EXPECT_EQ(switch_fills().size(), 2u);
}

// A pair that traded before the listing date and holds nothing on it is opened at its rounded
// target under the default rule; carry_to_target leaves it to the pass, which is inside its buffer.
TEST_F(ListingSwitchPassTest, APairThatHeldNothingIsOpenedAtItsTargetByTheDefaultRuleOnly) {
    for (const auto rule : {ListingSwitchRule::kOpenAtTarget, ListingSwitchRule::kCarryToTarget}) {
        ListingDates::instance().set(the_pair());
        ListingDates::instance().set_switch_rule(rule);
        make_pm();
        before(0.4);
        ASSERT_TRUE(rebalance(kBefore).is_ok());
        ASSERT_EQ(quantity(kBig), 0.0) << "0.4 of a contract is not worth one";
        listed(4.4);
        ASSERT_TRUE(rebalance(kListed).is_ok());
        if (rule == ListingSwitchRule::kOpenAtTarget) {
            EXPECT_EQ(switch_fills(), (std::vector<std::string>{"LO-A-0 TMIC.v.0 4"}));
            EXPECT_EQ(quantity(kMicro), 4.0);
        } else {
            EXPECT_TRUE(switch_fills().empty());
            EXPECT_EQ(quantity(kMicro), 0.0) << "4.4 micros is inside the book's buffer";
        }
        pm_.reset();
        a_.reset();
    }
}

// The forecast has turned at or beyond the deferral band on the switch pass: the predecessor is
// exited and the listed contract entered on the other side at its rounded target. The pass's
// forecast-sign close then has nothing to close.
TEST_F(ListingSwitchPassTest, ASignChangeOnTheSwitchPassEntersTheOtherSide) {
    ListingDates::instance().set(the_pair());
    make_pm();
    before(3.0);
    ASSERT_TRUE(rebalance(kBefore).is_ok());
    ASSERT_EQ(quantity(kBig), 2.0);
    listed(-34.4, -5.0);
    ASSERT_TRUE(rebalance(kListed).is_ok());
    EXPECT_EQ(switch_fills(), (std::vector<std::string>{"LC-A-0 TBIG.v.0 -2", "LO-A-0 TMIC.v.0 -34"}));
    EXPECT_EQ(quantity(kBig), 0.0);
    EXPECT_EQ(quantity(kMicro), -34.0);
    EXPECT_EQ(fills("EX-").size(), 1u) << "no sign close and no further fill";
}

// A holding against a forecast weaker than the band is one the pass HOLDS. The switch carries it
// (ratio x the held quantity) instead of trading it to a target the band says not to trade to yet;
// the pass holds the carried position, and closes it by the forecast-sign close when the forecast
// reaches the band.
TEST_F(ListingSwitchPassTest, AHoldingInsideTheDeferralBandIsCarriedAndHeldThenSignClosed) {
    ListingDates::instance().set(the_pair());
    make_pm();
    before(3.0);
    ASSERT_TRUE(rebalance(kBefore).is_ok());
    ASSERT_EQ(quantity(kBig), 2.0);
    listed(-3.4, -1.0);
    ASSERT_TRUE(rebalance(kListed).is_ok());
    EXPECT_EQ(switch_fills(), (std::vector<std::string>{"LC-A-0 TBIG.v.0 -2", "LO-A-0 TMIC.v.0 20"}));
    EXPECT_EQ(quantity(kMicro), 20.0) << "held by the band: no fill toward the short target";
    EXPECT_EQ(fills("EX-").size(), 1u);

    listed(-3.4, -2.0);  // at the band the symbol is free and the sign close applies
    ASSERT_TRUE(rebalance(kListed + 1).is_ok());
    EXPECT_EQ(quantity(kMicro), 0.0) << "one fill to flat; -3.4 micros is inside the buffer";
    EXPECT_EQ(switch_fills().size(), 2u);
    const auto ordinary = fills("EX-");
    ASSERT_EQ(ordinary.size(), 2u);
    EXPECT_NE(ordinary[1].find("TMIC.v.0 -20"), std::string::npos) << ordinary[1];
}

// A target beyond the per-name cap is entered at the cap, never above it.
TEST_F(ListingSwitchPassTest, TheEntryIsBoundedByThePerNameCap) {
    ListingDates::instance().set(the_pair());
    make_pm();
    before(3.0);
    ASSERT_TRUE(rebalance(kBefore).is_ok());
    listed(150.6);
    ASSERT_TRUE(rebalance(kListed).is_ok());
    EXPECT_EQ(switch_fills(), (std::vector<std::string>{"LC-A-0 TBIG.v.0 -2", "LO-A-0 TMIC.v.0 100"}));
    EXPECT_EQ(quantity(kMicro), 100.0);
}

// A hold on the listing date's pass (neither contract in the session set) delays the switch to the
// next pass on which both can trade; nothing is traded in the pair meanwhile.
TEST_F(ListingSwitchPassTest, AHoldDelaysTheSwitchToTheNextPassOnWhichBothCanTrade) {
    ListingDates::instance().set(the_pair());
    make_pm();
    before(3.0);
    ASSERT_TRUE(rebalance(kBefore).is_ok());
    listed(24.4);
    const std::unordered_set<std::string> nobody;
    ASSERT_TRUE(rebalance(kListed, false, &nobody).is_ok());
    EXPECT_TRUE(switch_fills().empty());
    EXPECT_EQ(quantity(kBig), 2.0) << "held";
    EXPECT_EQ(quantity(kMicro), 0.0);
    ASSERT_TRUE(rebalance(kListed + 1).is_ok());
    EXPECT_EQ(switch_fills(), (std::vector<std::string>{"LC-A-0 TBIG.v.0 -2", "LO-A-0 TMIC.v.0 24"}));
    for (const auto& e : executions()) {
        if (e.exec_id.rfind("L", 0) == 0) EXPECT_EQ(e.fill_time, one_pass_day(kListed + 2));
    }
}

// A run that starts trading after the listing date never traded the predecessor: there is no
// switch, and the listed contract opens through the ordinary pass as any symbol does.
TEST_F(ListingSwitchPassTest, ARunThatNeverTradedThePredecessorHasNoSwitch) {
    ListingDates::instance().set(the_pair());
    make_pm();
    before(3.0);
    ASSERT_TRUE(rebalance(kBefore, /*warmup=*/true).is_ok());
    listed(24.4);
    ASSERT_TRUE(rebalance(kListed).is_ok());
    EXPECT_TRUE(switch_fills().empty());
    EXPECT_EQ(quantity(kMicro), 19.0) << "0 -> 24 through the buffer stores 19";
    ASSERT_TRUE(rebalance(kListed + 1).is_ok());
    EXPECT_TRUE(switch_fills().empty()) << "and none later";
}

// No contracts set: the same two days are the engine's ordinary close-out and opening.
TEST_F(ListingSwitchPassTest, WithNoContractsSetNothingIsSwitched) {
    make_pm();
    before(3.0);
    ASSERT_TRUE(rebalance(kBefore).is_ok());
    listed(24.4);
    ASSERT_TRUE(rebalance(kListed).is_ok());
    EXPECT_TRUE(switch_fills().empty());
    EXPECT_EQ(quantity(kBig), 0.0) << "closed by the close-out";
    EXPECT_EQ(quantity(kMicro), 19.0) << "opened through the buffer";
    EXPECT_EQ(fills("EX-").size(), 3u);
}

// Rule close_reenter is the same two ordinary fills, with the contracts set.
TEST_F(ListingSwitchPassTest, CloseReenterLeavesTheSwitchToTheCloseOutAndThePass) {
    ListingDates::instance().set(the_pair());
    ListingDates::instance().set_switch_rule(ListingSwitchRule::kCloseReenter);
    make_pm();
    before(3.0);
    ASSERT_TRUE(rebalance(kBefore).is_ok());
    listed(24.4);
    ASSERT_TRUE(rebalance(kListed).is_ok());
    EXPECT_TRUE(switch_fills().empty());
    EXPECT_EQ(quantity(kBig), 0.0);
    EXPECT_EQ(quantity(kMicro), 19.0);
}

// The switch is the backtest's: a live manager writes no switch fill whatever is set.
TEST_F(ListingSwitchPassTest, ALiveManagerNeverSwitches) {
    ListingDates::instance().set(the_pair());
    make_pm(/*backtest=*/false);
    before(3.0);
    ASSERT_TRUE(rebalance(kBefore).is_ok());
    ASSERT_EQ(quantity(kBig), 2.0) << "the live pass holds the predecessor: a switch would have something to move";
    listed(24.4);
    ASSERT_TRUE(rebalance(kListed).is_ok());
    EXPECT_TRUE(switch_fills().empty());
}

// A wait caused by the listed contract alone (its bar is not a session bar) holds the predecessor
// too: it is never closed by the close-out while its switch waits.
TEST_F(ListingSwitchPassTest, AWaitOnTheListedContractAloneHoldsThePredecessor) {
    ListingDates::instance().set(the_pair());
    make_pm();
    before(3.0);
    ASSERT_TRUE(rebalance(kBefore).is_ok());
    listed(24.4);
    const std::unordered_set<std::string> only_big = {kBig};
    ASSERT_TRUE(rebalance(kListed, false, &only_big).is_ok());
    EXPECT_TRUE(switch_fills().empty());
    EXPECT_EQ(quantity(kBig), 2.0) << "held, not closed out";
    EXPECT_EQ(fills("EX-").size(), 1u);
    ASSERT_TRUE(rebalance(kListed + 1).is_ok());
    EXPECT_EQ(switch_fills(), (std::vector<std::string>{"LC-A-0 TBIG.v.0 -2", "LO-A-0 TMIC.v.0 24"}));
}

// A refused pass sends no order: the switch planned on it is undone (no fill, the predecessor still
// held) and is made on the next pass that is not refused.
TEST_F(ListingSwitchPassTest, ARefusedPassUndoesTheSwitch) {
    ListingDates::instance().set(the_pair());
    make_pm();
    before(3.0);
    ASSERT_TRUE(rebalance(kBefore).is_ok());
    listed(24.4);
    a_->rows["TOTH.v.0"] = row(1000.0, std::numeric_limits<double>::quiet_NaN(), 10.0, true);
    auto with_other = [&](int d) {
        std::vector<Bar> bars = {one_pass_bar(kBig, d, 100.0), one_pass_bar(kMicro, d, 100.0),
                                 one_pass_bar("TOTH.v.0", d, 100.0)};
        for (const auto& b : bars) {
            pm_->update_cost_manager_market_data(b.symbol, b.volume, static_cast<double>(b.close),
                                                 static_cast<double>(b.close));
        }
        return pm_->process_market_data(bars, false, one_pass_day(d + 1), nullptr);
    };
    (void)with_other(kListed);  // refused: a target that is not a number
    EXPECT_TRUE(switch_fills().empty()) << "no switch fill on a refused day";
    EXPECT_EQ(quantity(kBig), 2.0);
    EXPECT_EQ(quantity(kMicro), 0.0);
    EXPECT_EQ(fills("EX-").size(), 1u);
    a_->rows["TOTH.v.0"] = row(1000.0, 0.0, 10.0, true);
    (void)with_other(kListed + 1);
    EXPECT_EQ(switch_fills(), (std::vector<std::string>{"LC-A-0 TBIG.v.0 -2", "LO-A-0 TMIC.v.0 24"}));
    EXPECT_EQ(quantity(kBig), 0.0);
    EXPECT_EQ(quantity(kMicro), 24.0);
}

// Two pairs switch on one pass (four do on 2019-05-06): each pair's two fills share a number and no
// two pairs share one; a pair that waits a day takes the next number.
TEST_F(ListingSwitchPassTest, TwoPairsOnOnePassGetTheirOwnFillNumbers) {
    ListingDates::instance().set({{"TMIC", "TBIG", "2026-01-06", 10.0}, {"TMI2", "TBG2", "2026-01-06", 10.0}});
    make_pm();
    auto day = [&](int d, const std::unordered_set<std::string>* session = nullptr) {
        std::vector<Bar> bars;
        for (const char* s : {"TBIG.v.0", "TMIC.v.0", "TBG2.v.0", "TMI2.v.0"}) bars.push_back(one_pass_bar(s, d, 100.0));
        for (const auto& b : bars) {
            pm_->update_cost_manager_market_data(b.symbol, b.volume, static_cast<double>(b.close),
                                                 static_cast<double>(b.close));
        }
        return pm_->process_market_data(bars, false, one_pass_day(d + 1), session);
    };
    before(3.0);
    a_->rows["TBG2.v.0"] = row(1000.0, 3.0, 10.0, true);
    a_->rows["TMI2.v.0"] = row(100.0, 0.0, 0.0, false);
    ASSERT_TRUE(day(kBefore).is_ok());
    ASSERT_EQ(quantity(kBig), 2.0);
    ASSERT_EQ(quantity("TBG2.v.0"), 2.0);
    listed(24.4);
    a_->rows["TBG2.v.0"] = row(1000.0, 0.0, 0.0, false);
    a_->rows["TMI2.v.0"] = row(100.0, 12.4, 10.0, true);
    // the second pair's bars are not session bars on the listing date: it waits, the first switches
    const std::unordered_set<std::string> first_pair = {kBig, kMicro};
    ASSERT_TRUE(day(kListed, &first_pair).is_ok());
    EXPECT_EQ(switch_fills(), (std::vector<std::string>{"LC-A-0 TBIG.v.0 -2", "LO-A-0 TMIC.v.0 24"}));
    EXPECT_EQ(quantity("TBG2.v.0"), 2.0) << "held while it waits";
    ASSERT_TRUE(day(kListed + 1).is_ok());
    EXPECT_EQ(switch_fills(), (std::vector<std::string>{"LC-A-0 TBIG.v.0 -2", "LC-A-1 TBG2.v.0 -2",
                                                        "LO-A-0 TMIC.v.0 24", "LO-A-1 TMI2.v.0 12"}));
}

TEST_F(ListingSwitchPassTest, TwoPairsSwitchingTogetherAreNumberedApart) {
    ListingDates::instance().set({{"TMIC", "TBIG", "2026-01-06", 10.0}, {"TMI2", "TBG2", "2026-01-06", 10.0}});
    make_pm();
    auto day = [&](int d) {
        std::vector<Bar> bars;
        for (const char* s : {"TBIG.v.0", "TMIC.v.0", "TBG2.v.0", "TMI2.v.0"}) bars.push_back(one_pass_bar(s, d, 100.0));
        for (const auto& b : bars) {
            pm_->update_cost_manager_market_data(b.symbol, b.volume, static_cast<double>(b.close),
                                                 static_cast<double>(b.close));
        }
        return pm_->process_market_data(bars, false, one_pass_day(d + 1), nullptr);
    };
    before(3.0);
    a_->rows["TBG2.v.0"] = row(1000.0, 3.0, 10.0, true);
    a_->rows["TMI2.v.0"] = row(100.0, 0.0, 0.0, false);
    ASSERT_TRUE(day(kBefore).is_ok());
    listed(24.4);
    a_->rows["TBG2.v.0"] = row(1000.0, 0.0, 0.0, false);
    a_->rows["TMI2.v.0"] = row(100.0, 12.4, 10.0, true);
    ASSERT_TRUE(day(kListed).is_ok());
    EXPECT_EQ(switch_fills(), (std::vector<std::string>{"LC-A-0 TBIG.v.0 -2", "LC-A-1 TBG2.v.0 -2",
                                                        "LO-A-0 TMIC.v.0 24", "LO-A-1 TMI2.v.0 12"}));
    EXPECT_EQ(quantity(kMicro), 24.0);
    EXPECT_EQ(quantity("TMI2.v.0"), 12.0);
}

// The switch waits when the PREDECESSOR alone is on hold, and until the listed contract signals; the
// predecessor is held meanwhile.
TEST_F(ListingSwitchPassTest, AWaitOnThePredecessorAloneOrOnASilentListedContractHoldsToo) {
    ListingDates::instance().set(the_pair());
    make_pm();
    before(3.0);
    ASSERT_TRUE(rebalance(kBefore).is_ok());
    listed(24.4);
    const std::unordered_set<std::string> only_micro = {kMicro};
    ASSERT_TRUE(rebalance(kListed, false, &only_micro).is_ok());
    EXPECT_TRUE(switch_fills().empty());
    EXPECT_EQ(quantity(kBig), 2.0);
    EXPECT_EQ(quantity(kMicro), 0.0) << "the listed contract is not opened beside the held predecessor";
    // the next day the listed contract does not signal yet: still no switch, the predecessor held
    a_->rows[kMicro] = row(100.0, 0.0, 0.0, false);
    ASSERT_TRUE(rebalance(kListed + 1).is_ok());
    EXPECT_TRUE(switch_fills().empty());
    EXPECT_EQ(quantity(kBig), 2.0) << "not closed out while its switch waits";
    EXPECT_EQ(fills("EX-").size(), 1u);
    listed(24.4);
    ASSERT_TRUE(rebalance(kListed + 2).is_ok());
    EXPECT_EQ(switch_fills(), (std::vector<std::string>{"LC-A-0 TBIG.v.0 -2", "LO-A-0 TMIC.v.0 24"}));
}

// The band is the pass's own test: a weak forecast on the holding's side is no band, and at exactly
// the band the holding is free (traded to target, here to the other side).
TEST_F(ListingSwitchPassTest, TheBandIsStrictAndOnlyAgainstTheHolding) {
    ListingDates::instance().set(the_pair());
    make_pm();
    before(3.0);
    ASSERT_TRUE(rebalance(kBefore).is_ok());
    listed(24.4, 1.0);
    ASSERT_TRUE(rebalance(kListed).is_ok());
    EXPECT_EQ(switch_fills(), (std::vector<std::string>{"LC-A-0 TBIG.v.0 -2", "LO-A-0 TMIC.v.0 24"}));
    pm_.reset();
    a_.reset();
    make_pm();
    before(3.0);
    ASSERT_TRUE(rebalance(kBefore).is_ok());
    listed(-34.4, -2.0);
    ASSERT_TRUE(rebalance(kListed).is_ok());
    EXPECT_EQ(switch_fills(), (std::vector<std::string>{"LC-A-0 TBIG.v.0 -2", "LO-A-0 TMIC.v.0 -34"}));
}

// A short target beyond the cap is entered at the cap on the short side.
TEST_F(ListingSwitchPassTest, AShortEntryIsBoundedByTheCapToo) {
    ListingDates::instance().set(the_pair());
    make_pm();
    before(3.0);
    ASSERT_TRUE(rebalance(kBefore).is_ok());
    listed(-150.6, -5.0);
    ASSERT_TRUE(rebalance(kListed).is_ok());
    EXPECT_EQ(switch_fills(), (std::vector<std::string>{"LC-A-0 TBIG.v.0 -2", "LO-A-0 TMIC.v.0 -100"}));
}
