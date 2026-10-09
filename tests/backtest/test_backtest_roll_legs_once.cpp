// tests/backtest/test_backtest_roll_legs_once.cpp
//
// T-ROLLX commit 3 (LOOP_SPEC v6.1 section 6.5): the two ROLL legs of a confirmed roll are booked
// ONCE, on the cycle that consumes the confirming bar, whether or not the symbol has a bar on the
// cycles after it. The coordinator keeps each symbol's last roll status across groups without a
// bar of it (the hold of a pending change persists, L-04); the first cut carried the CONFIRM flag
// with it and booked the pair again on every cycle until the symbol's next bar (the probe P3 of
// 799e092a: ZC.v.0 three pairs for its 2025-07-03 roll, ZL.v.0 two for 2026-02-20).
//
// The fixture runs a whole futures backtest (run_portfolio) with one sleeve long 2 XA.v.0 from
// cycle 10, XA's contract id switching on day 30 (the change bar), kept on day 31 (the confirming
// bar), no XA bar on days 32 and 33, and XB.v.0 printing every day so the cycles go on.
//
// T-ROLLX-FIX: X-3, a leg without a usable close fails the run (an error out of run_portfolio, a
// non-zero exit of the runner), never a warning; X-4, the BORROW rows of an equity short reach the
// run's executions, so 018's transaction_costs is the sum of every stored row's cost.

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "../core/test_base.hpp"
#include "../risk/risk_module_test_helpers.hpp"
#include "cost_basis_test_helpers.hpp"
#include "trade_ngin/backtest/backtest_coordinator.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/data/market_data_bus.hpp"
#include "trade_ngin/instruments/equity.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/transaction_cost/transaction_cost_manager.hpp"

using namespace trade_ngin;
using namespace trade_ngin::backtest;
using namespace trade_ngin::testing;
using namespace trade_ngin::testing::cost_basis;

namespace {

const std::string kXA = "XA.v.0";  // rolls on day 30, confirmed on day 31, no bar on days 32 and 33
const std::string kXB = "XB.v.0";  // prints every day: the cycles go on without XA
constexpr int kLastDay = 40;
constexpr int kOpenSignal = 9;   // the sleeve's target of 2 applies from signal day 9: filled on cycle 10
constexpr int kChangeBar = 30;   // the first bar with the new id
constexpr int kConfirmBar = 31;  // the next consumed bar keeps it: a ROLL
constexpr int kLegCycle = 32;    // the cycle that consumes the confirming bar books the legs
const std::string kOldId = "A1";
const std::string kNewId = "A2";

double close_of(const std::string& symbol, int d) {
    const double phase = symbol == kXA ? 0.0 : 1.7;
    return 100.0 * (1.0 + 0.02 * std::sin(0.9 * d + phase) + 0.001 * d);
}

std::vector<Row> rows(double last_close_before_change = 0.0) {
    std::vector<Row> out;
    for (int d = 0; d <= kLastDay; ++d) {
        Row b{kXB, d, close_of(kXB, d), 200000.0};
        b.instrument_id = "B1";
        out.push_back(b);
        if (d == kConfirmBar + 1 || d == kConfirmBar + 2) continue;  // XA prints no bar
        const bool override_close = last_close_before_change != 0.0 && d == kChangeBar - 1;
        Row a{kXA, d, override_close ? last_close_before_change : close_of(kXA, d), 200000.0};
        a.instrument_id = d < kChangeBar ? kOldId : kNewId;
        out.push_back(a);
    }
    return out;
}

transaction_cost::ContractCostSpecSource spec() {
    return [](const std::string& symbol) -> std::optional<transaction_cost::ContractCostSpec> {
        if (symbol.rfind("XA", 0) != 0 && symbol.rfind("XB", 0) != 0) return std::nullopt;
        transaction_cost::ContractCostSpec s;
        s.point_value = 50.0;
        s.tick_size = 0.25;
        s.fee_per_contract = 1.50;
        return s;
    };
}

int leg_index(const ExecutionReport& r) {
    return std::stoi(r.exec_id.substr(r.exec_id.rfind('-') + 1));
}

}  // namespace

class BacktestRollLegsOnceTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        MarketDataBus::instance().set_publish_enabled(true);
        LoggerConfig lc;
        lc.destination = LogDestination::CONSOLE;
        lc.min_level = LogLevel::INFO;
        lc.include_timestamp = false;
        Logger::instance().initialize(lc);
        db_ = std::make_shared<ServingDb>("mock://testdb");
        ASSERT_TRUE(db_->connect().is_ok());
    }

    void TearDown() override {
        MarketDataBus::instance().set_publish_enabled(true);
        (void)MarketDataBus::instance().unsubscribe("PORTFOLIO_MANAGER");
        coord_.reset();
        a_.reset();
        pm_.reset();
        db_.reset();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        TestBase::TearDown();
    }

    void run() { ASSERT_TRUE(run_rows(rows(), nullptr)); }

    // The whole backtest on `bars`; false (and the error in *err) when run_portfolio fails.
    bool run_rows(const std::vector<Row>& bars, std::string* err) {
        db_->rows = bars;

        BacktestCoordinatorConfig cc;
        cc.initial_capital = 1'000'000.0;
        cc.store_results = false;
        cc.store_trade_details = true;
        cc.portfolio_id = "ROLL_LEGS_ONCE_TEST";
        cc.csv_output_path = temp_csv_dir("roll_legs_once");
        coord_ = std::make_unique<BacktestCoordinator>(db_, &InstrumentRegistry::instance(), cc);
        EXPECT_TRUE(coord_->initialize().is_ok());

        PortfolioConfig pc{1'000'000.0, 1.0, 0.0, /*optimization=*/false};
        pc.opt_config.capital = 1'000'000.0;
        pc.risk_config.capital = 1'000'000.0;
        pc.risk_modules = {test_none_module()};
        pm_ = std::make_shared<PortfolioManager>(pc, "PM_ROLL_LEGS_ONCE");

        StrategyConfig sc;
        sc.capital_allocation = 1'000'000.0;
        sc.max_leverage = 10.0;
        sc.asset_classes = {AssetClass::FUTURES};
        sc.frequencies = {DataFrequency::DAILY};
        for (const auto& s : {kXA, kXB}) {
            sc.trading_params[s] = 1.0;
            sc.position_limits[s] = 1.0e6;
        }
        a_ = std::make_shared<ScheduledStrategy>("ROLL_A", sc, db_);
        a_->targets = {{kXA, {{kOpenSignal, 2.0}}}};
        EXPECT_TRUE(a_->initialize().is_ok());
        EXPECT_TRUE(a_->start().is_ok());
        EXPECT_TRUE(pm_->add_strategy(a_, 1.0, false).is_ok());
        pm_->get_transaction_cost_manager().set_contract_spec_source(spec());
        coord_->get_execution_manager()->get_transaction_cost_manager().set_contract_spec_source(
            spec());

        ::testing::internal::CaptureStdout();
        auto result = coord_->run_portfolio(pm_, {kXA, kXB}, trading_day(0), trading_day(kLastDay),
                                            AssetClass::FUTURES, DataFrequency::DAILY);
        log_ = ::testing::internal::GetCapturedStdout();
        if (result.is_error()) {
            if (err) *err = result.error()->what();
            else ADD_FAILURE() << result.error()->what();
            return false;
        }
        results_ = result.value();
        return true;
    }

    std::vector<ExecutionReport> rows_of(ExecutionType type) const {
        std::vector<ExecutionReport> out;
        for (const auto& r : results_.executions) {
            if (r.symbol == kXA && r.execution_type == type) out.push_back(r);
        }
        return out;
    }

    size_t count_lines(const std::string& needle) const {
        size_t n = 0;
        for (size_t at = log_.find(needle); at != std::string::npos; at = log_.find(needle, at + 1)) ++n;
        return n;
    }

    std::shared_ptr<ServingDb> db_;
    std::unique_ptr<BacktestCoordinator> coord_;
    std::shared_ptr<PortfolioManager> pm_;
    std::shared_ptr<ScheduledStrategy> a_;
    BacktestResults results_;
    std::string log_;
};

// One confirmed roll of a held symbol books exactly one pair, on the cycle that consumes the
// confirming bar, and the cycles without a bar of the symbol book nothing more.
TEST_F(BacktestRollLegsOnceTest, AConfirmedRollBooksOnePairEvenWhenTheSymbolThenPrintsNoBar) {
    run();
    const auto legs = rows_of(ExecutionType::ROLL);
    ASSERT_EQ(legs.size(), 2u) << "two ROLL legs for the one confirmed roll of XA, not " << legs.size();
    for (const auto& leg : legs) {
        EXPECT_EQ(leg.fill_time, trading_day(kLegCycle)) << leg.exec_id;
        EXPECT_DOUBLE_EQ(static_cast<double>(leg.filled_quantity), 2.0);
        EXPECT_EQ(static_cast<double>(leg.netting_adjustment), 0.0);
    }
    const ExecutionReport& closing = leg_index(legs[0]) < leg_index(legs[1]) ? legs[0] : legs[1];
    const ExecutionReport& opening = leg_index(legs[0]) < leg_index(legs[1]) ? legs[1] : legs[0];
    EXPECT_EQ(closing.side, Side::SELL);  // against the long
    EXPECT_EQ(opening.side, Side::BUY);
    EXPECT_NEAR(static_cast<double>(closing.fill_price), close_of(kXA, kChangeBar - 1), 1e-7);  // Decimal: 8 places
    EXPECT_NEAR(static_cast<double>(opening.fill_price), close_of(kXA, kChangeBar), 1e-7);
    EXPECT_EQ(closing.instrument_id, kOldId);
    EXPECT_EQ(opening.instrument_id, kNewId);
    EXPECT_EQ(count_lines("ROLL_CONFIRMED " + kXA), 1u);
    EXPECT_EQ(count_lines("ROLL_LEG ROLL_A " + kXA), 2u);
    // Each ROLL_LEG line carries the cost model's inputs (a reader recomputes the leg's cost from them).
    const size_t at = log_.find("ROLL_LEG ROLL_A " + kXA);
    ASSERT_NE(at, std::string::npos);
    const std::string line = log_.substr(at, log_.find('\n', at) - at);
    EXPECT_NE(line.find(" adv="), std::string::npos) << line;
    EXPECT_NE(line.find(" vol_mult="), std::string::npos) << line;
    EXPECT_LT(line.find(" vol_mult="), line.find(" date=")) << line;
}

// The sleeve's own fills are untouched by the roll: the one STRATEGY fill is the open on cycle 10,
// nothing is traded on the change bar's cycle (the hold) and the position is still 2 at the end.
TEST_F(BacktestRollLegsOnceTest, TheStrategyFillsAreTheOpenAloneAndThePositionIsHeldThrough) {
    run();
    const auto fills = rows_of(ExecutionType::STRATEGY);
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].fill_time, trading_day(kOpenSignal + 1));
    EXPECT_EQ(fills[0].side, Side::BUY);
    EXPECT_DOUBLE_EQ(static_cast<double>(fills[0].filled_quantity), 2.0);
    EXPECT_GE(count_lines("CHANGE_BAR_HOLD " + kXA), 1u);
    const auto book = pm_->get_strategy_positions();
    ASSERT_TRUE(book.count("ROLL_A"));
    ASSERT_TRUE(book.at("ROLL_A").count(kXA));
    EXPECT_DOUBLE_EQ(static_cast<double>(book.at("ROLL_A").at(kXA).quantity), 2.0);
}

// X-3: the last close before the change bar is not usable (negative), so the closing leg has no
// price. Section 6.5: the run fails with ROLL_LEG STOP (run_portfolio returns the error; the runner
// exits non-zero on it), never a warning and a run that goes on.
TEST_F(BacktestRollLegsOnceTest, ALegWithoutAUsableCloseFailsTheRun) {
    std::string err;
    EXPECT_FALSE(run_rows(rows(/*last_close_before_change=*/-1.0), &err))
        << "the run went on with a leg on an unusable close";
    EXPECT_NE(err.find("ROLL_LEG STOP " + kXA), std::string::npos) << err;
}

// X-4: an equity short accrues a BORROW row each cycle it is open. The row is typed BORROW and
// reaches the run's executions, so 018's transaction_costs is the sum of every stored row's cost
// (the sum the equity curve charged), BORROW included.
class BacktestBorrowRowsTest : public BacktestRollLegsOnceTest {};

TEST_F(BacktestBorrowRowsTest, BorrowRowsReachTheRunsExecutionsAndTheCostTotal) {
    const std::string sym = "SHRTEQ";
    EquitySpec es;
    es.exchange = "NASDAQ";
    es.currency = "USD";
    es.tick_size = 0.01;
    es.account_mode = EquityAccountMode::REG_T;
    es.short_selling_allowed = true;
    es.borrow_rate_override = 0.50;
    InstrumentRegistry::instance().register_instrument(sym, std::make_shared<EquityInstrument>(sym, es));
    std::vector<Row> bars;
    for (int d = 0; d <= 30; ++d) bars.push_back({sym, d, 50.0 + 0.1 * d, 3'000'000.0});
    db_->rows = bars;
    BacktestCoordinatorConfig cc;
    cc.initial_capital = 1'000'000.0;
    cc.store_results = false;
    cc.store_trade_details = true;
    cc.portfolio_id = "BORROW_ROWS_TEST";
    cc.csv_output_path = temp_csv_dir("borrow_rows");
    coord_ = std::make_unique<BacktestCoordinator>(db_, &InstrumentRegistry::instance(), cc);
    ASSERT_TRUE(coord_->initialize().is_ok());
    PortfolioConfig pc{1'000'000.0, 1.0, 0.0, /*optimization=*/false};
    pc.opt_config.capital = 1'000'000.0;
    pc.risk_config.capital = 1'000'000.0;
    pc.risk_modules = {test_none_module()};
    pm_ = std::make_shared<PortfolioManager>(pc, "PM_BORROW_ROWS");
    StrategyConfig sc;
    sc.capital_allocation = 1'000'000.0;
    sc.max_leverage = 10.0;
    sc.asset_classes = {AssetClass::EQUITIES};
    sc.frequencies = {DataFrequency::DAILY};
    sc.trading_params[sym] = 1.0;
    sc.position_limits[sym] = 1.0e6;
    a_ = std::make_shared<ScheduledStrategy>("BORROW_S", sc, db_);
    a_->targets = {{sym, {{5, -100.0}}}};
    ASSERT_TRUE(a_->initialize().is_ok());
    ASSERT_TRUE(a_->start().is_ok());
    ASSERT_TRUE(pm_->add_strategy(a_, 1.0, false).is_ok());
    ::testing::internal::CaptureStdout();
    auto result = coord_->run_portfolio(pm_, {sym}, trading_day(0), trading_day(30),
                                        AssetClass::EQUITIES, DataFrequency::DAILY);
    log_ = ::testing::internal::GetCapturedStdout();
    ASSERT_TRUE(result.is_ok()) << result.error()->what();
    results_ = result.value();

    double stored_cost = 0.0;  // the rows backtest.executions stores (the PortfolioManager's)
    int stored_borrow = 0;
    for (const auto& [sid, execs] : pm_->get_strategy_executions()) {
        for (const auto& e : execs) {
            stored_cost += static_cast<double>(e.total_transaction_costs);
            if (e.execution_type == ExecutionType::BORROW) ++stored_borrow;
        }
    }
    int run_borrow = 0;
    double run_borrow_cost = 0.0;
    for (const auto& e : results_.executions) {
        if (e.exec_id.rfind("BORROW_", 0) != 0) continue;
        ++run_borrow;
        run_borrow_cost += static_cast<double>(e.total_transaction_costs);
        EXPECT_EQ(e.execution_type, ExecutionType::BORROW) << e.exec_id;
    }
    ASSERT_GE(stored_borrow, 1) << "the fixture must hold an open short that accrues borrow";
    EXPECT_EQ(run_borrow, stored_borrow) << "every stored BORROW row is one of the run's executions";
    EXPECT_GT(run_borrow_cost, 0.0);
    EXPECT_NEAR(results_.transaction_costs, stored_cost, 1e-6)
        << "018 transaction_costs " << results_.transaction_costs << " vs the stored rows' sum "
        << stored_cost << " (BORROW " << run_borrow_cost << ")";
}
