// T-7b-2 C9a (T-VOL C4): the DELIVERED cut beside the requested one.
//
// RISK_SCALE_REPORT prints the gate's request (applied_cumulative, the product of the factors the
// loop multiplied the book by). What reaches the stored book is not that: lap 2's buffer can
// refuse a cut contract back and the rounding moves whole contracts (T-VOL section 3.2: 0.747 of
// the lap-1 book in notional on cut days against a 0.870 request). RISK_DELIVERED puts, beside
// the request, the stored book's gross notional over the lap-1 optimizer book's, both valued at
// one notional per contract per symbol. These tests pin: the line's text and its `na` cases
// (pure functions), the PortfolioManager's measurement on real rebalances (a cut, a refusal that
// ships yesterday's book, an empty lap-1 book, the reset per call), and the valuation source.

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <deque>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "../risk/risk_module_test_helpers.hpp"
#include "trade_ngin/instruments/futures.hpp"
#include "trade_ngin/strategy/base_strategy.hpp"

// delivered_notional_per_contract, update_historical_returns, the registry's map and the trend
// strategy's instrument data are private; reach them directly (test_trend_following.cpp and
// test_portfolio_manager_internals.cpp use the same pattern).
#define private public
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#undef private

#include "trade_ngin/risk/basic_risk_modules.hpp"
#include "trade_ngin/risk/risk_scale_report.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

using Book = std::unordered_map<std::string, Position>;

class ScriptedStrategy : public BaseStrategy {
public:
    ScriptedStrategy(std::string id, StrategyConfig config, std::shared_ptr<DatabaseInterface> db,
                     std::vector<Book> script)
        : BaseStrategy(std::move(id), std::move(config),
                       std::static_pointer_cast<trade_ngin::PostgresDatabase>(db)),
          script_(std::move(script)) {
        metadata_.name = "Scripted Strategy";
    }
    Result<void> on_data(const std::vector<Bar>& data) override {
        (void)data;
        ++calls_;
        return Result<void>();
    }
    std::unordered_map<std::string, Position> get_target_positions() const override {
        if (calls_ == 0) return {};
        return script_[std::min(calls_, script_.size()) - 1];
    }

private:
    std::vector<Book> script_;
    size_t calls_{0};
};

Timestamp day(int d) {
    return Timestamp(std::chrono::seconds(1767225600LL + 86400LL * d));
}

Bar make_bar(const std::string& symbol, int d, double close) {
    Bar b;
    b.symbol = symbol;
    b.timestamp = day(d);
    b.open = b.high = b.low = b.close = Decimal(close);
    b.volume = 1000.0;
    return b;
}

Position make_pos(const std::string& symbol, double qty, double price) {
    Position p;
    p.symbol = symbol;
    p.quantity = Decimal(qty);
    p.average_price = Decimal(price);
    p.last_update = day(0);
    return p;
}

// ZZA closes 100, 102, 99: the manager's latest close is 99. No registry: multiplier 1.
std::vector<Bar> three_days() {
    return {make_bar("ZZA", 1, 100.0), make_bar("ZZA", 2, 102.0), make_bar("ZZA", 3, 99.0)};
}

PortfolioConfig pm_config() {
    PortfolioConfig pc{1000.0, 1.0, 0.0, /*optimization=*/false};
    pc.risk_config.capital = 1000.0;
    pc.risk_modules = {test_none_module()};
    return pc;
}

}  // namespace

// ---- the line (pure functions) --------------------------------------------------------------

// A cut: the lap-1 book is 10 lots worth 990 and the stored book 5 worth 495.
TEST(RiskDeliveredFormatTest, TheRatioIsTheStoredGrossOverTheLapOneGross) {
    RiskScaleSummary s;
    s.applied_cumulative = 0.87;
    const std::map<std::string, double> lap1{{"ZZA", 10.0}, {"ZZB", -4.0}};
    const std::map<std::string, double> final_book{{"ZZA", 5.0}, {"ZZB", -4.0}};
    const std::map<std::string, double> npc{{"ZZA", 99.0}, {"ZZB", 25.0}};
    const DeliveredCut d = measure_delivered_cut(&lap1, final_book, npc);
    EXPECT_TRUE(d.has_lap1);
    EXPECT_TRUE(d.has_final);
    EXPECT_EQ(d.lap1_gross, 10.0 * 99.0 + 4.0 * 25.0);  // 1090: a short counts by magnitude
    EXPECT_EQ(d.final_gross, 5.0 * 99.0 + 4.0 * 25.0);  // 595
    EXPECT_EQ(d.unpriced, 0);
    EXPECT_EQ(format_risk_delivered(s, d),
              "RISK_DELIVERED requested=0.87 delivered=0.54587155963302747 final_gross=595 "
              "lap1_gross=1090 unpriced=0");
}

// An EMPTY lap-1 book (no symbol, or every quantity 0) has nothing to cut: no ratio, and both
// grosses are printed (the final book may still hold yesterday's positions after a refusal).
TEST(RiskDeliveredFormatTest, AnEmptyLapOneBookPrintsNoRatio) {
    RiskScaleSummary s;
    const std::map<std::string, double> lap1{{"ZZA", 0.0}};
    const std::map<std::string, double> final_book{{"ZZA", 1.0}};
    const std::map<std::string, double> npc{{"ZZA", 99.0}};
    EXPECT_EQ(format_risk_delivered(s, measure_delivered_cut(&lap1, final_book, npc)),
              "RISK_DELIVERED requested=1 delivered=na final_gross=99 lap1_gross=0 unpriced=0");
    const std::map<std::string, double> none;
    EXPECT_EQ(format_risk_delivered(s, measure_delivered_cut(&none, none, npc)),
              "RISK_DELIVERED requested=1 delivered=na final_gross=0 lap1_gross=0 unpriced=0");
}

// No measurement (the backtest's all-JUNK cycle, a call that returned an error): every figure na.
// The backtest form carries the cycle's date.
TEST(RiskDeliveredFormatTest, NoMeasurementIsNaAndTheBacktestFormCarriesTheDate) {
    RiskScaleSummary s;
    EXPECT_EQ(format_risk_delivered(s, DeliveredCut{}, "2025-07-04"),
              "RISK_DELIVERED requested=1 delivered=na final_gross=na lap1_gross=na unpriced=0 "
              "date=2025-07-04");
}

// A held symbol with no notional per contract is left out of BOTH grosses and counted once.
TEST(RiskDeliveredFormatTest, AnUnpricedSymbolIsLeftOutOfBothAndCountedOnce) {
    const std::map<std::string, double> lap1{{"ZZA", 2.0}, {"NOPX", 3.0}};
    const std::map<std::string, double> final_book{{"ZZA", 1.0}, {"NOPX", 3.0}};
    const std::map<std::string, double> npc{{"ZZA", 50.0}, {"NOPX", 0.0}};
    const DeliveredCut d = measure_delivered_cut(&lap1, final_book, npc);
    EXPECT_EQ(d.lap1_gross, 100.0);
    EXPECT_EQ(d.final_gross, 50.0);
    EXPECT_EQ(d.unpriced, 1);
}

// The requested figure is RISK_SCALE_REPORT's applied_cumulative, the same double, same digits.
TEST(RiskDeliveredFormatTest, RequestedIsTheReportsAppliedCumulative) {
    RiskScaleSummary s;
    s.applied_cumulative = 0.62722324982945854;
    const std::string report = format_risk_scale_report(std::string("na"), s, "2025-07-04");
    const std::string delivered = format_risk_delivered(s, DeliveredCut{}, "2025-07-04");
    EXPECT_NE(report.find(" applied_cumulative=0.62722324982945854 "), std::string::npos);
    EXPECT_EQ(delivered.rfind("RISK_DELIVERED requested=0.62722324982945854 ", 0), 0u);
}

// ---- the PortfolioManager's measurement --------------------------------------------------------

class DeliveredCutPmTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        db_ = std::make_shared<MockPostgresDatabase>("mock://delivered");
        ASSERT_TRUE(db_->connect().is_ok());
    }
    void TearDown() override {
        pm_.reset();
        db_.reset();
        auto& registry = InstrumentRegistry::instance();
        registry.instruments_.erase("ZZB");
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        TestBase::TearDown();
    }

    void make_pm(std::vector<Book> script, std::vector<RiskModulePtr> modules) {
        static int n = 0;
        pm_ = std::make_unique<PortfolioManager>(pm_config(), "PM_DC_" + std::to_string(++n));
        StrategyConfig sc;
        sc.capital_allocation = 1000.0;
        sc.max_leverage = 10.0;
        sc.asset_classes = {AssetClass::FUTURES};
        sc.frequencies = {DataFrequency::DAILY};
        strategy_ = std::make_shared<ScriptedStrategy>("DC_S", sc, db_, std::move(script));
        ASSERT_TRUE(strategy_->initialize().is_ok());
        ASSERT_TRUE(strategy_->start().is_ok());
        ASSERT_TRUE(pm_->add_strategy(strategy_, 1.0, false).is_ok());
        ASSERT_TRUE(pm_->set_risk_modules(std::move(modules)).is_ok());
    }

    std::shared_ptr<MockPostgresDatabase> db_;
    std::unique_ptr<PortfolioManager> pm_;
    std::shared_ptr<ScriptedStrategy> strategy_;
};

// Lap 1 cuts 10 lots by 0.5 to 5, whole, so the loop converges there: the request is 0.5 and the
// stored book is exactly half the lap-1 book. Valued at the manager's latest close (99) x 1.
TEST_F(DeliveredCutPmTest, ALapOneCutThatShipsWholeDeliversTheRequest) {
    make_pm({{{"ZZA", make_pos("ZZA", 10.0, 100.0)}}},
            {std::make_shared<ConstantScaleRiskModule>("cut", 0.5)});
    ASSERT_TRUE(pm_->process_market_data(three_days()).is_ok());
    const DeliveredCut d = pm_->last_delivered_cut();
    EXPECT_TRUE(d.has_lap1);
    EXPECT_TRUE(d.has_final);
    EXPECT_EQ(d.lap1_gross, 990.0);
    EXPECT_EQ(d.final_gross, 495.0);
    EXPECT_EQ(format_risk_delivered(summarize_applied_risk(pm_->last_risk_decisions()), d),
              "RISK_DELIVERED requested=0.5 delivered=0.5 final_gross=495 lap1_gross=990 "
              "unpriced=0");
}

// The request is not what ships: lap 1 cuts 5 lots by 0.85 to 4.25 (fractional), lap 2 is
// refused, and the book ships at yesterday's 1 lot. requested = 0.85 (lap 2's SCALE lost to the
// REFUSE), delivered = 99 / 495 = 0.2.
TEST_F(DeliveredCutPmTest, ARefusalDeliversYesterdaysBookNotTheRequest) {
    make_pm({{{"ZZA", make_pos("ZZA", 5.0, 100.0)}}},
            {std::make_shared<ConstantScaleRiskModule>("cut", 0.85, /*every_lap=*/true),
             std::make_shared<RefuseOnConditionRiskModule>(
                 "stop", RiskCondition{RiskCondition::Kind::ALWAYS, 0.0}, "always")});
    ASSERT_TRUE(pm_->update_strategy_position("DC_S", "ZZA", make_pos("ZZA", 1.0, 100.0)).is_ok());
    ASSERT_TRUE(pm_->process_market_data(three_days()).is_ok());
    const DeliveredCut d = pm_->last_delivered_cut();
    EXPECT_EQ(d.lap1_gross, 495.0) << "the book the risk step read is the 5 lots before it";
    EXPECT_EQ(d.final_gross, 99.0) << "the stored book is yesterday's 1 lot";
    // The refusal won over the scale requested in the same step, so nothing was multiplied.
    EXPECT_EQ(format_risk_delivered(summarize_applied_risk(pm_->last_risk_decisions()), d),
              "RISK_DELIVERED requested=1 delivered=0.20000000000000001 "
              "final_gross=99 lap1_gross=495 unpriced=0");
}

// A strategy with no target: the lap-1 book is empty, so there is no ratio.
TEST_F(DeliveredCutPmTest, AnEmptyLapOneBookHasNoRatio) {
    make_pm({Book{}}, {std::make_shared<ConstantScaleRiskModule>("cut", 0.5)});
    ASSERT_TRUE(pm_->process_market_data(three_days()).is_ok());
    EXPECT_EQ(format_risk_delivered(summarize_applied_risk(pm_->last_risk_decisions()),
                                    pm_->last_delivered_cut()),
              "RISK_DELIVERED requested=1 delivered=na final_gross=0 lap1_gross=0 unpriced=0");
}

// Each call measures its own rebalance: the second call's books replace the first's.
TEST_F(DeliveredCutPmTest, EachCallReportsItsOwnRebalance) {
    make_pm({{{"ZZA", make_pos("ZZA", 10.0, 100.0)}}, {{"ZZA", make_pos("ZZA", 2.0, 100.0)}}},
            {std::make_shared<ConstantScaleRiskModule>("cut", 0.5)});
    ASSERT_TRUE(pm_->process_market_data(three_days()).is_ok());
    EXPECT_EQ(pm_->last_delivered_cut().lap1_gross, 990.0);
    ASSERT_TRUE(pm_->process_market_data({make_bar("ZZA", 4, 100.0)}).is_ok());
    const DeliveredCut d = pm_->last_delivered_cut();
    EXPECT_EQ(d.lap1_gross, 200.0) << "2 lots at the new close 100";
    EXPECT_EQ(d.final_gross, 100.0);
}

// The notional per contract: a TrendFollowingStrategy's own figure (contract_size x its latest
// price, what the optimizer uses) wins over the manager's close; otherwise the manager's latest
// close x the registry multiplier (1 when the symbol is not registered); a symbol with neither
// is absent (unpriced).
TEST_F(DeliveredCutPmTest, TheNotionalIsTheTrendStrategysOwnElseTheManagersClose) {
    auto& registry = InstrumentRegistry::instance();
    FuturesSpec spec;
    spec.root_symbol = "ZZB";
    spec.exchange = "CME";
    spec.currency = "USD";
    spec.multiplier = 50.0;
    spec.tick_size = 0.25;
    spec.commission_per_contract = 1.0;
    spec.initial_margin = 1000.0;
    spec.maintenance_margin = 900.0;
    registry.instruments_["ZZB"] = std::make_shared<FuturesInstrument>("ZZB", spec);
    auto registry_ptr = std::shared_ptr<InstrumentRegistry>(&registry, [](InstrumentRegistry*) {});

    PortfolioManager pm(pm_config(), "PM_DC_VALUE", registry_ptr);
    StrategyConfig sc;
    sc.capital_allocation = 1000.0;
    sc.asset_classes = {AssetClass::FUTURES};
    sc.frequencies = {DataFrequency::DAILY};
    auto tf = std::make_shared<TrendFollowingStrategy>("DC_TF", sc, TrendFollowingConfig{}, db_);
    auto& zza = tf->instrument_data_["ZZA"];
    zza.contract_size = 5.0;
    zza.price_history = {100.0, 120.0};  // latest 120: 5 x 120 = 600
    tf->instrument_data_["ZZE"].contract_size = 5.0;  // no price yet: falls to the close
    ASSERT_TRUE(pm.add_strategy(tf, 1.0, false).is_ok());

    pm.update_historical_returns({make_bar("ZZA", 1, 99.0), make_bar("ZZB", 1, 38.0),
                                  make_bar("ZZB", 2, 40.0), make_bar("ZZC", 1, 7.0),
                                  make_bar("ZZE", 1, 3.0)});
    const auto npc = pm.delivered_notional_per_contract({"ZZA", "ZZB", "ZZC", "ZZD", "ZZE"});
    EXPECT_EQ(npc.at("ZZA"), 600.0) << "the trend strategy's contract_size x latest price";
    EXPECT_EQ(npc.at("ZZB"), 40.0 * 50.0) << "the latest close x the registry multiplier";
    EXPECT_EQ(npc.at("ZZC"), 7.0) << "not registered: multiplier 1";
    EXPECT_EQ(npc.at("ZZE"), 3.0) << "trend data without a price falls to the close";
    EXPECT_EQ(npc.count("ZZD"), 0u) << "no price anywhere: unpriced";
}

// ---- T-7b-2 C9a3: final_gross measures the book the runner STORES (after its BOOK_GATE hold) -------
//
// The live futures runners hold a symbol whose T-1 verdict is not SESSION at its stored T-1 quantity AFTER
// process_market_data returns (hold_non_session_symbols), so the PM's own final book is not what they store on
// a feed-hole day (futchain 2026-04-24: the PM ships MYM 2, the runner stores MYM 1). The runners now pass the
// account book they store; the lap-1 book and the notionals stay the PM's measurement of the same rebalance.

// The account book of per-strategy books: per symbol, the sum of every sleeve's contracts.
TEST(RiskDeliveredFormatTest, TheAccountBookSumsTheSleeves) {
    std::unordered_map<std::string, std::unordered_map<std::string, Position>> books;
    books["TREND"]["MYM"] = make_pos("MYM", 2.0, 1.0);
    books["TREND"]["ZZA"] = make_pos("ZZA", 0.0, 1.0);
    books["FAST"]["MYM"] = make_pos("MYM", -1.0, 1.0);
    const auto account = account_book_of(books);
    ASSERT_EQ(account.size(), 2u);
    EXPECT_EQ(account.at("MYM"), 1.0);
    EXPECT_EQ(account.at("ZZA"), 0.0);
}

// The PM cuts 10 lots to 5 (lap-1 gross 990, its final 495). The runner then holds the symbol at yesterday's 7:
// the stored book is 7 lots, 693, so delivered is 0.7, not the PM's 0.5. A symbol the hold re-inserted that no
// PM book carried (ZZB, closes 20 in the manager's history) is priced from the manager's close.
TEST_F(DeliveredCutPmTest, FinalGrossMeasuresTheStoredBookAfterTheRunnersHold) {
    make_pm({{{"ZZA", make_pos("ZZA", 10.0, 100.0)}}},
            {std::make_shared<ConstantScaleRiskModule>("cut", 0.5)});
    ASSERT_TRUE(pm_->process_market_data({make_bar("ZZA", 1, 100.0), make_bar("ZZA", 2, 102.0),
                                          make_bar("ZZA", 3, 99.0), make_bar("ZZB", 3, 20.0)})
                    .is_ok());
    EXPECT_EQ(pm_->last_delivered_cut().final_gross, 495.0) << "the PM's own final book, before any hold";

    const DeliveredCut held = pm_->delivered_cut_for_book({{"ZZA", 7.0}});
    EXPECT_EQ(held.lap1_gross, 990.0);
    EXPECT_EQ(held.final_gross, 693.0) << "the held contracts are in the stored book's gross";
    EXPECT_EQ(format_risk_delivered(summarize_applied_risk(pm_->last_risk_decisions()), held),
              "RISK_DELIVERED requested=0.5 delivered=0.69999999999999996 final_gross=693 lap1_gross=990 "
              "unpriced=0");

    const DeliveredCut reinserted = pm_->delivered_cut_for_book({{"ZZA", 5.0}, {"ZZB", 2.0}});
    EXPECT_EQ(reinserted.final_gross, 495.0 + 40.0) << "ZZB priced at the manager's close 20 x 1";
    EXPECT_EQ(reinserted.unpriced, 0);

    // With no hold the stored book is the PM's final book: the same figures as last_delivered_cut().
    const DeliveredCut same = pm_->delivered_cut_for_book({{"ZZA", 5.0}});
    EXPECT_EQ(same.final_gross, pm_->last_delivered_cut().final_gross);
    EXPECT_EQ(same.lap1_gross, pm_->last_delivered_cut().lap1_gross);
}

// No measurement (no process_market_data call reached its end): the stored book changes nothing, every figure na.
TEST_F(DeliveredCutPmTest, NoMeasurementStaysNaWhateverTheStoredBook) {
    make_pm({{{"ZZA", make_pos("ZZA", 10.0, 100.0)}}},
            {std::make_shared<ConstantScaleRiskModule>("cut", 0.5)});
    const DeliveredCut d = pm_->delivered_cut_for_book({{"ZZA", 3.0}});
    EXPECT_EQ(format_risk_delivered(RiskScaleSummary{}, d),
              "RISK_DELIVERED requested=1 delivered=na final_gross=na lap1_gross=na unpriced=0");
}
