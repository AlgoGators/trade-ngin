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
#include <filesystem>
#include <fstream>
#include <sstream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "../risk/risk_module_test_helpers.hpp"

// the two-sleeve cases set each sleeve's filled ledger directly and read the manager's cost model
#define private public
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#undef private
#include "trade_ngin/instruments/futures.hpp"

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

// With no contracts declared the pass makes no copy of the held book for a switch and asks for no
// conversion: the switch's code is behind the same "contracts are declared" test as the rest. The
// source is read (the house pattern for a guard that has no value to observe).
TEST(ListingSwitchWiring, TheSwitchIsBehindTheContractsAreDeclaredTest) {
    namespace fs = std::filesystem;
    const fs::path file = fs::path(__FILE__).parent_path().parent_path().parent_path() /
                          "src/portfolio/portfolio_manager.cpp";
    std::ifstream in(file);
    ASSERT_TRUE(in.good()) << file;
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string src = ss.str();
    const size_t guard = src.find("ListingDates::instance().enabled() &&\n"
                                  "            ListingDates::instance().switch_rule() != ListingSwitchRule::kCloseReenter) {");
    ASSERT_NE(guard, std::string::npos) << "the switch's guard does not test enabled()";
    for (const char* inside : {"held_before_switch = in.held;", "sleeve_held_before_switch = sleeve_held;",
                               "ListingDates::instance().conversions_due(data)"}) {
        const size_t at = src.find(inside);
        ASSERT_NE(at, std::string::npos) << inside;
        EXPECT_GT(at, guard) << inside << " runs outside the guard";
        EXPECT_EQ(src.find(inside, at + 1), std::string::npos) << inside << " appears twice";
    }
}

// On a day the overlay cuts the book the listed contract is entered at the SCALED target (the
// capped target times the overlay's scalar), so the switch lands on the cut target and the pass has
// nothing left to trade in the pair. Three other symbols at the cap (a weight of 2 each) and the
// listed contract's target of 100 (a weight of 2) make a net leverage of 8 against the limit of 6:
// m = 0.75, and the entry is 75, not 100.
TEST_F(ListingSwitchPassTest, OnACutDayTheEntryIsTheScaledTarget) {
    ListingDates::instance().set(the_pair());
    make_pm();
    before(3.0);
    ASSERT_TRUE(rebalance(kBefore).is_ok());
    ASSERT_EQ(quantity(kBig), 2.0);
    listed(100.0);
    for (const char* other : {"TO1.v.0", "TO2.v.0", "TO3.v.0"}) a_->rows[other] = row(1000.0, 10.0, 10.0, true);
    std::vector<Bar> bars = {one_pass_bar(kBig, kListed, 100.0), one_pass_bar(kMicro, kListed, 100.0)};
    for (const char* other : {"TO1.v.0", "TO2.v.0", "TO3.v.0"}) bars.push_back(one_pass_bar(other, kListed, 100.0));
    for (const auto& b : bars) {
        pm_->update_cost_manager_market_data(b.symbol, b.volume, static_cast<double>(b.close),
                                             static_cast<double>(b.close));
    }
    ::testing::internal::CaptureStdout();
    ASSERT_TRUE(pm_->process_market_data(bars, false, one_pass_day(kListed + 1), nullptr).is_ok());
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_NE(out.find("OVERLAY m=0.75"), std::string::npos) << "the fixture's overlay does not cut to 0.75";
    EXPECT_EQ(switch_fills(), (std::vector<std::string>{"LC-A-0 TBIG.v.0 -2", "LO-A-0 TMIC.v.0 75"}));
    EXPECT_EQ(quantity(kMicro), 75.0) << "the pass has nothing left to trade in the pair";
    for (const auto& line : fills("EX-")) EXPECT_EQ(line.find("TMIC"), std::string::npos) << line;
}

// ---- a book of two sleeves: the switch follows the book's own rule (LOOP_SPEC section 5.4) ----
//
// The book's net predecessor holding is exited and the listed contract entered at the BOOK's summed
// scaled target; the book's whole number is split to the sleeves by the function the pass's own
// split uses; the rows are stored one per sleeve and netted as every bar's rows are, so the account
// sends ONE order per symbol and the net costs of a symbol's rows sum to the cost of that order.
// Sleeve A is the first sleeve (the overlay's, whose forecast the deferral band reads), B the other.
// The worked tables are in lead2/evidence/T-LISTING/fix_round/ITEM5_WORKED_CASES.md.
class ListingSwitchTwoSleeveTest : public ListingSwitchPassTest {
protected:
    void make_book(int sleeves) {
        static int n = 0;
        pm_ = std::make_unique<PortfolioManager>(one_pass_config("A"), "PM_LS2_" + std::to_string(++n));
        pm_->set_backtest_mode(true);
        a_ = make_overlay_stub("A", 500000.0 / sleeves, db_);
        ASSERT_TRUE(a_->initialize().is_ok());
        ASSERT_TRUE(a_->start().is_ok());
        ASSERT_TRUE(pm_->add_strategy(a_, 1.0 / sleeves, true).is_ok());
        if (sleeves == 2) {
            b_ = make_overlay_stub("B", 250000.0, db_);
            ASSERT_TRUE(b_->initialize().is_ok());
            ASSERT_TRUE(b_->start().is_ok());
            ASSERT_TRUE(pm_->add_strategy(b_, 0.5, true).is_ok());
        }
        // the predecessor traded on an earlier sized pass of this run
        pm_->listing_predecessor_traded_.insert(kBig);
        pm_->ever_signalled_.insert(kBig);
    }
    void TearDown() override {
        b_.reset();
        ListingSwitchPassTest::TearDown();
    }
    void hold(const std::string& sleeve, const std::string& symbol, double quantity) {
        pm_->filled_positions_[sleeve][symbol] = quantity;
    }
    /// The listing date's rows: the predecessor signals for nobody; each sleeve's N* and forecast
    /// for the listed contract (signalling false: the sleeve does not publish it).
    void listed_rows(OverlayStubStrategy& sleeve, double optimal, double forecast, bool signalling = true) {
        sleeve.rows[kBig] = row(1000.0, 0.0, 0.0, false);
        sleeve.rows[kMicro] = row(100.0, optimal, forecast, signalling);
    }
    double held(const std::string& sleeve, const std::string& symbol) {
        const auto books = pm_->get_strategy_positions();
        const auto it = books.at(sleeve).find(symbol);
        return it == books.at(sleeve).end() ? 0.0 : static_cast<double>(it->second.quantity);
    }
    /// Every LC- / LO- row of every sleeve: "id symbol signed-quantity", in sleeve then fill order.
    std::vector<std::string> rows() {
        std::vector<std::string> out;
        const auto all = pm_->get_strategy_executions();
        for (const auto& [sid, execs] : all) {
            for (const auto& e : execs) {
                if (e.exec_id.rfind("LC-", 0) != 0 && e.exec_id.rfind("LO-", 0) != 0) continue;
                const double q = static_cast<double>(e.filled_quantity) * (e.side == Side::BUY ? 1.0 : -1.0);
                out.push_back(e.exec_id + " " + e.symbol + " " + std::to_string(static_cast<int>(q)));
            }
        }
        std::sort(out.begin(), out.end());
        return out;
    }
    /// The account's order in `symbol` (the signed sum of the rows) and the rows' net cost (own
    /// cost less netting_adjustment), over every fill of the symbol so far.
    std::pair<double, double> account(const std::string& symbol) {
        double q = 0.0, net = 0.0;
        for (const auto& [sid, execs] : pm_->get_strategy_executions()) {
            for (const auto& e : execs) {
                if (e.symbol != symbol) continue;
                q += static_cast<double>(e.filled_quantity) * (e.side == Side::BUY ? 1.0 : -1.0);
                net += static_cast<double>(e.total_transaction_costs) - static_cast<double>(e.netting_adjustment);
            }
        }
        return {q, net};
    }
    double cost_of(const std::string& symbol, double q) {
        return q == 0.0 ? 0.0 : pm_->cost_manager_.calculate_costs(symbol, q, 100.0).total_transaction_costs;
    }
    /// The book and the account must be what ONE sleeve given the summed target and the net holding
    /// produces: the same stored quantities, the same account orders, the same net cost.
    void expect_equal_to_one_sleeve(double net_held_big, double summed_optimal, double first_forecast) {
        const double two_big = held("A", kBig) + held("B", kBig);
        const double two_micro = held("A", kMicro) + held("B", kMicro);
        const auto two_acct_big = account(kBig);
        const auto two_acct_micro = account(kMicro);
        pm_.reset();
        a_.reset();
        b_.reset();
        make_book(1);
        if (net_held_big != 0.0) hold("A", kBig, net_held_big);
        listed_rows(*a_, summed_optimal, first_forecast);
        ASSERT_TRUE(rebalance(kListed).is_ok());
        EXPECT_EQ(held("A", kBig), two_big);
        EXPECT_EQ(held("A", kMicro), two_micro);
        EXPECT_EQ(account(kBig).first, two_acct_big.first);
        EXPECT_EQ(account(kMicro).first, two_acct_micro.first);
        EXPECT_NEAR(account(kBig).second, two_acct_big.second, 1e-6);
        EXPECT_NEAR(account(kMicro).second, two_acct_micro.second, 1e-6);
    }
    std::shared_ptr<OverlayStubStrategy> b_;
};

// (i) Both sleeves long the predecessor: the account sells 2 and buys round(24.4) = 24, split 14 and
// 10 by largest remainder (quotas 14.16 and 9.84).
TEST_F(ListingSwitchTwoSleeveTest, BothSleevesLong) {
    ListingDates::instance().set(the_pair());
    make_book(2);
    hold("A", kBig, 1.0);
    hold("B", kBig, 1.0);
    listed_rows(*a_, 14.4, 10.0);
    listed_rows(*b_, 10.0, 10.0);
    ASSERT_TRUE(rebalance(kListed).is_ok());
    EXPECT_EQ(rows(), (std::vector<std::string>{"LC-A-0 TBIG.v.0 -1", "LC-B-0 TBIG.v.0 -1",
                                                "LO-A-0 TMIC.v.0 14", "LO-B-0 TMIC.v.0 10"}));
    EXPECT_EQ(held("A", kMicro), 14.0);
    EXPECT_EQ(held("B", kMicro), 10.0);
    EXPECT_EQ(held("A", kBig) + held("B", kBig), 0.0);
    EXPECT_EQ(account(kBig).first, -2.0);
    EXPECT_NEAR(account(kBig).second, cost_of(kBig, -2.0), 1e-6) << "the rows' net cost is the cost of ONE order of 2";
    EXPECT_EQ(account(kMicro).first, 24.0);
    EXPECT_NEAR(account(kMicro).second, cost_of(kMicro, 24.0), 1e-6);
    expect_equal_to_one_sleeve(2.0, 24.4, 10.0);
}

// (ii) The sleeves opposed and the book net long: the account sells the NET 1 and buys the net 14;
// A is credited 20 long and B 6 short (the opposed split of the deviation 14 - 14.4).
TEST_F(ListingSwitchTwoSleeveTest, OpposedSleevesNetLong) {
    ListingDates::instance().set(the_pair());
    make_book(2);
    hold("A", kBig, 2.0);
    hold("B", kBig, -1.0);
    listed_rows(*a_, 20.4, 10.0);
    listed_rows(*b_, -6.0, -10.0);
    ASSERT_TRUE(rebalance(kListed).is_ok());
    EXPECT_EQ(rows(), (std::vector<std::string>{"LC-A-0 TBIG.v.0 -2", "LC-B-0 TBIG.v.0 1",
                                                "LO-A-0 TMIC.v.0 20", "LO-B-0 TMIC.v.0 -6"}));
    EXPECT_EQ(held("A", kMicro), 20.0);
    EXPECT_EQ(held("B", kMicro), -6.0);
    EXPECT_EQ(account(kBig).first, -1.0) << "one order for the net, never two offsetting orders";
    EXPECT_NEAR(account(kBig).second, cost_of(kBig, -1.0), 1e-6);
    EXPECT_EQ(account(kMicro).first, 14.0);
    EXPECT_NEAR(account(kMicro).second, cost_of(kMicro, 14.0), 1e-6);
    expect_equal_to_one_sleeve(1.0, 14.4, 10.0);
}

// (iii) The sleeves opposed and the book net ZERO in the predecessor: no exit order reaches the
// market; each sleeve's own leg is closed against the other at a net cost of nothing (a full cross:
// each row's adjustment is its own cost). The listed contract is entered at the book's net 4.
TEST_F(ListingSwitchTwoSleeveTest, OpposedSleevesNetZeroCrossAtNoCost) {
    ListingDates::instance().set(the_pair());
    make_book(2);
    hold("A", kBig, 1.0);
    hold("B", kBig, -1.0);
    listed_rows(*a_, 13.0, 10.0);
    listed_rows(*b_, -9.0, -10.0);
    ASSERT_TRUE(rebalance(kListed).is_ok());
    EXPECT_EQ(rows(), (std::vector<std::string>{"LC-A-0 TBIG.v.0 -1", "LC-B-0 TBIG.v.0 1",
                                                "LO-A-0 TMIC.v.0 13", "LO-B-0 TMIC.v.0 -9"}));
    EXPECT_EQ(account(kBig).first, 0.0) << "no order in the predecessor";
    EXPECT_NEAR(account(kBig).second, 0.0, 1e-9) << "the crossed legs cost nothing";
    for (const auto& [sid, execs] : pm_->get_strategy_executions()) {
        for (const auto& e : execs) {
            if (e.symbol != kBig) continue;
            EXPECT_GT(static_cast<double>(e.total_transaction_costs), 0.0) << "the row keeps its own cost";
            EXPECT_EQ(e.netting_adjustment, e.total_transaction_costs) << sid << ": the whole of it is credited";
        }
    }
    EXPECT_EQ(held("A", kBig), 0.0);
    EXPECT_EQ(held("B", kBig), 0.0);
    EXPECT_EQ(held("A", kMicro), 13.0);
    EXPECT_EQ(held("B", kMicro), -9.0);
    EXPECT_EQ(account(kMicro).first, 4.0);
    EXPECT_NEAR(account(kMicro).second, cost_of(kMicro, 4.0), 1e-6);
    expect_equal_to_one_sleeve(0.0, 4.0, 10.0);
}

// (iv) One sleeve publishing, the other not (in warm-up, or stopped): a sleeve with no contribution
// has no share of the listed contract; a leg it still holds in the predecessor is exited with the
// book's.
TEST_F(ListingSwitchTwoSleeveTest, OneSleevePublishingTheOtherNot) {
    ListingDates::instance().set(the_pair());
    make_book(2);
    hold("A", kBig, 2.0);
    listed_rows(*a_, 24.4, 10.0);
    listed_rows(*b_, 0.0, 0.0, /*signalling=*/false);
    ASSERT_TRUE(rebalance(kListed).is_ok());
    EXPECT_EQ(rows(), (std::vector<std::string>{"LC-A-0 TBIG.v.0 -2", "LO-A-0 TMIC.v.0 24"}));
    EXPECT_EQ(held("B", kMicro), 0.0);
    for (const auto& [sid, execs] : pm_->get_strategy_executions()) {
        for (const auto& e : execs) EXPECT_EQ(e.netting_adjustment, Decimal()) << "one row a symbol: nothing to net";
    }

    pm_.reset();
    a_.reset();
    b_.reset();
    make_book(2);
    hold("A", kBig, 2.0);
    hold("B", kBig, 1.0);  // B no longer publishes and still holds a leg
    listed_rows(*a_, 24.4, 10.0);
    listed_rows(*b_, 0.0, 0.0, /*signalling=*/false);
    ASSERT_TRUE(rebalance(kListed).is_ok());
    EXPECT_EQ(rows(), (std::vector<std::string>{"LC-A-0 TBIG.v.0 -2", "LC-B-0 TBIG.v.0 -1", "LO-A-0 TMIC.v.0 24"}));
    EXPECT_EQ(account(kBig).first, -3.0);
    EXPECT_NEAR(account(kBig).second, cost_of(kBig, -3.0), 1e-6);
    EXPECT_EQ(held("A", kMicro), 24.0);
    EXPECT_EQ(held("B", kMicro), 0.0);
    EXPECT_EQ(held("B", kBig), 0.0);
}

// (v) The equity slow rule zeroes the first sleeve's forecast and not the other's: the first
// sleeve publishes nothing, the book's target is the other sleeve's short, and a zero forecast has
// no sign, so the band holds nothing. With a WEAK forecast of the first sleeve against the book's
// holding the band reads the first sleeve's forecast and the book's holding (section 5.2): the
// holding is carried ratio for one on the sleeve that holds it and the pass holds it.
TEST_F(ListingSwitchTwoSleeveTest, TheSlowRuleOnTheFirstSleeveOnlyAndTheBandOnTheBook) {
    ListingDates::instance().set(the_pair());
    make_book(2);
    hold("B", kBig, -1.0);
    listed_rows(*a_, 0.0, 0.0);
    a_->rows[kMicro].slow_rule_zeroed = true;
    listed_rows(*b_, -8.2, -10.0);
    ASSERT_TRUE(rebalance(kListed).is_ok());
    EXPECT_EQ(rows(), (std::vector<std::string>{"LC-B-0 TBIG.v.0 1", "LO-B-0 TMIC.v.0 -8"}));
    EXPECT_EQ(held("A", kMicro), 0.0);
    EXPECT_EQ(held("B", kMicro), -8.0);

    pm_.reset();
    a_.reset();
    b_.reset();
    make_book(2);
    hold("B", kBig, -1.0);
    listed_rows(*a_, 1.3, 1.0);  // weak, against the book's short: the band
    listed_rows(*b_, -8.2, -10.0);
    ASSERT_TRUE(rebalance(kListed).is_ok());
    EXPECT_EQ(rows(), (std::vector<std::string>{"LC-B-0 TBIG.v.0 1", "LO-B-0 TMIC.v.0 -10"}));
    EXPECT_EQ(held("B", kMicro), -10.0) << "carried ten for one and held by the pass";
    EXPECT_EQ(held("A", kMicro), 0.0);
    for (const auto& [sid, execs] : pm_->get_strategy_executions()) {
        for (const auto& e : execs) EXPECT_NE(e.exec_id.rfind("EX-", 0), 0u) << "no ordinary fill: " << e.exec_id;
    }
}

// Each switch fill is costed on ITS OWN contract's row: two registry rows of different fee and
// contract size, and each fill's commission is its own row's fee times its quantity (the exit on
// the predecessor's 2.25 a contract, the entry on the listed contract's 0.60), and each fill's
// spread and impact dollars scale with its own contract size.
TEST_F(ListingSwitchPassTest, EachSwitchFillIsCostedOnItsOwnContractsRow) {
    auto& registry = InstrumentRegistry::instance();
    const auto saved = registry.instruments_;
    const bool saved_init = registry.initialized_;
    registry.instruments_.clear();
    auto add = [&](const std::string& root, double multiplier, double fee) {
        FuturesSpec spec;
        spec.root_symbol = root;
        spec.exchange = "CME";
        spec.currency = "USD";
        spec.multiplier = multiplier;
        spec.tick_size = 0.25;
        spec.commission_per_contract = fee;
        spec.fee_per_contract = fee;
        registry.instruments_[root] = std::make_shared<FuturesInstrument>(root, spec);
    };
    add("TBIG", 1000.0, 2.25);
    add("TMIC", 100.0, 0.60);
    registry.initialized_ = true;

    ListingDates::instance().set(the_pair());
    make_pm();
    // the book holds 2 of the predecessor, which traded earlier in the run (set directly: with real
    // contract rows the fixture's one-contract steps are no longer worth their cost)
    pm_->filled_positions_["A"][kBig] = 2.0;
    pm_->listing_predecessor_traded_.insert(kBig);
    pm_->ever_signalled_.insert(kBig);
    listed(24.4);
    ASSERT_TRUE(rebalance(kListed).is_ok());
    ExecutionReport exit_fill, entry_fill;
    for (const auto& e : executions()) {
        if (e.exec_id == "LC-A-0") exit_fill = e;
        if (e.exec_id == "LO-A-0") entry_fill = e;
    }
    ASSERT_EQ(exit_fill.symbol, kBig);
    ASSERT_EQ(entry_fill.symbol, kMicro);
    EXPECT_NEAR(static_cast<double>(exit_fill.commissions_fees), 2.0 * 2.25, 1e-9) << "2 contracts on the predecessor's fee";
    EXPECT_NEAR(static_cast<double>(entry_fill.commissions_fees), 24.0 * 0.60, 1e-9) << "24 contracts on the listed contract's fee";
    // the same call the manager makes for any fill of that symbol and size
    const auto own_exit = pm_->cost_manager_.calculate_costs(kBig, -2.0, 100.0);
    const auto own_entry = pm_->cost_manager_.calculate_costs(kMicro, 24.0, 100.0);
    EXPECT_NEAR(static_cast<double>(exit_fill.total_transaction_costs), own_exit.total_transaction_costs, 1e-6);
    EXPECT_NEAR(static_cast<double>(entry_fill.total_transaction_costs), own_entry.total_transaction_costs, 1e-6);
    // priced on the other contract's row either would differ
    EXPECT_GT(std::abs(pm_->cost_manager_.calculate_costs(kMicro, -2.0, 100.0).total_transaction_costs -
                       own_exit.total_transaction_costs),
              1.0);

    registry.instruments_ = saved;
    registry.initialized_ = saved_init;
}
