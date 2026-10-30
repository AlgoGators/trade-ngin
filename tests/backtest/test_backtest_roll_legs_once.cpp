// tests/backtest/test_backtest_roll_legs_once.cpp
//
// The two ROLL legs of a confirmed roll are booked ONCE, on the cycle that consumes the confirming
// bar, whether or not the symbol has a bar on the cycles after it. The coordinator keeps each
// symbol's last roll status across groups without a bar of it (the hold of a pending change
// persists), but not the CONFIRM flag: carried along, it would book the pair again on every cycle
// until the symbol's next bar.
//
// The fixture runs a whole futures backtest (run_portfolio) with one sleeve long 2 XA.v.0 from
// cycle 10, XA's contract id switching on day 30 (the change bar), kept on day 31 (the confirming
// bar), no XA bar on days 32 and 33, and XB.v.0 printing every day so the cycles go on.
//
// A leg without a usable close fails the run (an error out of run_portfolio, a non-zero exit of
// the runner), never a warning; the BORROW rows of an equity short reach the
// run's executions, so 018's transaction_costs is the sum of every stored row's cost.

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/instruments/futures.hpp"
// The results-row test registers XA (the P&L's point value) and puts the registry back as it was.
#define private public
#include "trade_ngin/instruments/instrument_registry.hpp"
#undef private

#include "../core/test_base.hpp"
#include "../risk/risk_module_test_helpers.hpp"
#include "cost_basis_test_helpers.hpp"
#include "trade_ngin/backtest/backtest_coordinator.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/data/market_data_bus.hpp"
#include "trade_ngin/data/per_contract_source.hpp"
#include "trade_ngin/instruments/equity.hpp"
#include "trade_ngin/instruments/futures.hpp"
#include "trade_ngin/live/live_statistics_columns.hpp"
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
        cc.statistics_sessions_per_year = 252.0;  // a run is refused without one
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

// Migration 040: the run's two split totals are set only with
// per_contract_rolls on (the source enabled): NULL in backtest.results with it off. XA's roll is on the
// vendor's series (no roll field: the micros' case), so no row carries a split and the totals are 0.
TEST_F(BacktestRollLegsOnceTest, TheRunsSplitTotalsAreNotSetWithTheSwitchOff) {
    run();
    EXPECT_FALSE(results_.roll_bar_moves.has_value()) << "switch off: no total";
    EXPECT_FALSE(results_.roll_gap_values.has_value());
    EXPECT_EQ(count_lines("ROLL_SPLIT"), 0u);
}

TEST_F(BacktestRollLegsOnceTest, TheRunsSplitTotalsAreSetWithTheSwitchOn) {
    contract_rolls::PerContractSource::instance().enable(contract_rolls::FuturesCalendar{});
    const bool ok = run_rows(rows(), nullptr);
    contract_rolls::PerContractSource::instance().disable();
    ASSERT_TRUE(ok);
    ASSERT_TRUE(results_.roll_bar_moves.has_value()) << "switch on: a total, 0 with no split";
    ASSERT_TRUE(results_.roll_gap_values.has_value());
    EXPECT_EQ(*results_.roll_bar_moves, 0.0);
    EXPECT_EQ(*results_.roll_gap_values, 0.0);
    EXPECT_EQ(rows_of(ExecutionType::ROLL).size(), 2u) << "the vendor-series roll is still legged";
    for (const auto& leg : rows_of(ExecutionType::ROLL)) EXPECT_FALSE(leg.roll_bar_move.has_value());
    EXPECT_EQ(count_lines("ROLL_SPLIT_TOTALS rolls=0 roll_bar_moves=0.000000 roll_gap_values=0.000000"), 1u);
}

// The results row carries the run's two split totals. The bar source hands
// XA's change bar out with the per-contract roll field (a roll known on its bar, the old contract's
// close 3 points under the new one's), so the run books a roll with a split; the cells the
// coordinator hands the results row must be the run's sums over its rows, as stored.
namespace {
constexpr double kGapPoints = 3.0;

class RollFieldDb : public ServingDb {
public:
    using ServingDb::ServingDb;
    /// The kind of the change bar's field (SOURCE_CHANGE for a micro's change of price source).
    static inline RollFromKind kind_on_change = RollFromKind::KNOWN;

    Result<std::shared_ptr<arrow::Table>> get_market_data(
        const std::vector<std::string>& symbols, const Timestamp& start_date,
        const Timestamp& end_date, AssetClass asset_class, DataFrequency freq = DataFrequency::DAILY,
        const std::string& data_type = "ohlcv") override {
        auto served =
            ServingDb::get_market_data(symbols, start_date, end_date, asset_class, freq, data_type);
        if (served.is_error()) return served;
        auto table = served.value();
        auto symbol = std::static_pointer_cast<arrow::StringArray>(
            table->GetColumnByName("symbol")->chunk(0));
        auto time = std::static_pointer_cast<arrow::TimestampArray>(
            table->GetColumnByName("time")->chunk(0));
        const auto change_bar = std::chrono::duration_cast<std::chrono::seconds>(
                                    trading_day(kChangeBar).time_since_epoch())
                                    .count();
        arrow::DoubleBuilder close;
        arrow::StringBuilder id, date;
        arrow::Int8Builder kind;
        for (int64_t i = 0; i < table->num_rows(); ++i) {
            if (symbol->GetString(i) == kXA && time->Value(i) == change_bar) {
                ARROW_CHECK_OK(close.Append(close_of(kXA, kChangeBar) - kGapPoints));
                ARROW_CHECK_OK(id.Append(kOldId));
                ARROW_CHECK_OK(kind.Append(static_cast<int8_t>(kind_on_change)));
                ARROW_CHECK_OK(date.Append(ymd(kChangeBar)));
            } else {
                ARROW_CHECK_OK(close.AppendNull());
                ARROW_CHECK_OK(id.AppendNull());
                ARROW_CHECK_OK(kind.AppendNull());
                ARROW_CHECK_OK(date.AppendNull());
            }
        }
        std::shared_ptr<arrow::Array> c, i, k, d;
        ARROW_CHECK_OK(close.Finish(&c));
        ARROW_CHECK_OK(id.Finish(&i));
        ARROW_CHECK_OK(kind.Finish(&k));
        ARROW_CHECK_OK(date.Finish(&d));
        auto add = [&table](const std::string& name, const std::shared_ptr<arrow::DataType>& type,
                            const std::shared_ptr<arrow::Array>& array) {
            table = table
                        ->AddColumn(table->num_columns(), arrow::field(name, type),
                                    std::make_shared<arrow::ChunkedArray>(array))
                        .ValueOrDie();
        };
        add("roll_from_close", arrow::float64(), c);
        add("roll_from_id", arrow::utf8(), i);
        add("roll_from_kind", arrow::int8(), k);
        add("roll_from_date", arrow::utf8(), d);
        return Result<std::shared_ptr<arrow::Table>>(table);
    }
};
}  // namespace

class BacktestResultsRowSplitTest : public BacktestRollLegsOnceTest {
protected:
    void SetUp() override {
        BacktestRollLegsOnceTest::SetUp();
        db_ = std::make_shared<RollFieldDb>("mock://testdb");
        ASSERT_TRUE(db_->connect().is_ok());
        // The P&L's point value (the split's multiplier) comes from the instrument: XA, 50 a point.
        FuturesSpec spec;
        spec.root_symbol = "XA";
        spec.exchange = "CME";
        spec.currency = "USD";
        spec.multiplier = 50.0;
        spec.tick_size = 0.25;
        spec.initial_margin = 1.0;
        spec.maintenance_margin = 1.0;
        spec.weight = 1.0;
        spec.trading_hours = "09:30-16:00";
        auto& registry = InstrumentRegistry::instance();
        saved_instruments_ = registry.instruments_;
        saved_initialized_ = registry.initialized_;
        registry.instruments_["XA"] = std::make_shared<FuturesInstrument>("XA", spec);
        registry.initialized_ = true;
    }
    void TearDown() override {
        BacktestRollLegsOnceTest::TearDown();
        auto& registry = InstrumentRegistry::instance();
        registry.instruments_ = saved_instruments_;
        registry.initialized_ = saved_initialized_;
    }

    std::unordered_map<std::string, std::shared_ptr<Instrument>> saved_instruments_;
    bool saved_initialized_ = false;
};

TEST_F(BacktestResultsRowSplitTest, TheResultsRowCarriesTheRunsTwoSplitTotals) {
    contract_rolls::PerContractSource::instance().enable(contract_rolls::FuturesCalendar{});
    const bool ok = run_rows(rows(), nullptr);
    contract_rolls::PerContractSource::instance().disable();
    ASSERT_TRUE(ok);
    const auto legs = rows_of(ExecutionType::ROLL);
    ASSERT_EQ(legs.size(), 2u);
    double moves = 0.0, gaps = 0.0;
    int with_split = 0;
    for (const auto& leg : legs) {
        if (!leg.roll_bar_move) continue;
        ++with_split;
        moves += *leg.roll_bar_move;
        gaps += *leg.roll_gap_value;
        EXPECT_NEAR(*leg.roll_gap_points, kGapPoints, 1e-9);
    }
    ASSERT_EQ(with_split, 1) << "one roll, its split on the closing leg";
    // Long 2 x 50: the old contract's move from day 29 to its own close on the change bar, and the gap.
    EXPECT_NEAR(moves, 2.0 * 50.0 * (close_of(kXA, kChangeBar) - kGapPoints - close_of(kXA, kChangeBar - 1)),
                1e-6);
    EXPECT_NEAR(gaps, 2.0 * 50.0 * kGapPoints, 1e-9);
    ASSERT_NE(moves, 0.0);

    auto saved = coord_->save_portfolio_results_to_db(results_, {"ROLL_A"}, {{"ROLL_A", 1.0}}, pm_,
                                                      nlohmann::json::object());
    ASSERT_TRUE(saved.is_ok()) << saved.error()->what();
    ASSERT_EQ(db_->call_count("store_backtest_summary"), 1);
    std::map<std::string, std::optional<std::string>> cell;
    for (const auto& stored : db_->last_backtest_summary_cells) cell[stored.column] = stored.value;
    ASSERT_EQ(cell.count("roll_bar_moves"), 1u) << "the results row names the run's roll_bar_moves";
    ASSERT_EQ(cell.count("roll_gap_values"), 1u);
    ASSERT_TRUE(cell.at("roll_bar_moves"));
    ASSERT_TRUE(cell.at("roll_gap_values"));
    EXPECT_EQ(*cell.at("roll_bar_moves"), live_results_number(moves).value_or(""));
    EXPECT_EQ(*cell.at("roll_gap_values"), live_results_number(gaps).value_or(""));
}

// The same run with the change bar's field a change of price source (the micros'
// 2026-02-23 change): the series books exactly what the known roll books (the old contract's move to its
// close, the gap excluded), but no execution is stored for it, no cost, no roll in the totals; the run logs
// one PRICE_SOURCE_CHANGE line with its gap.
TEST_F(BacktestResultsRowSplitTest, AChangeOfPriceSourceBooksTheMoveAndNoLegsAndNoCost) {
    contract_rolls::PerContractSource::instance().enable(contract_rolls::FuturesCalendar{});
    RollFieldDb::kind_on_change = RollFromKind::SOURCE_CHANGE;
    const bool ok = run_rows(rows(), nullptr);
    RollFieldDb::kind_on_change = RollFromKind::KNOWN;
    contract_rolls::PerContractSource::instance().disable();
    ASSERT_TRUE(ok);
    EXPECT_TRUE(rows_of(ExecutionType::ROLL).empty()) << "no legs for a change of price source";
    EXPECT_EQ(results_.roll_costs, 0.0);
    ASSERT_TRUE(results_.roll_bar_moves.has_value());
    EXPECT_EQ(*results_.roll_bar_moves, 0.0);
    EXPECT_EQ(*results_.roll_gap_values, 0.0);
    double fill_costs = 0.0;
    for (const auto& f : rows_of(ExecutionType::STRATEGY)) fill_costs += static_cast<double>(f.total_transaction_costs);
    EXPECT_NEAR(results_.transaction_costs, fill_costs, 1e-9) << "every cost is a STRATEGY fill's";
    EXPECT_EQ(count_lines("PRICE_SOURCE_CHANGE ROLL_A " + kXA), 1u);
    const size_t at = log_.find("PRICE_SOURCE_CHANGE ROLL_A " + kXA);
    const std::string line = at == std::string::npos ? std::string() : log_.substr(at, log_.find('\n', at) - at);
    EXPECT_NE(line.find(" qty=2 gap_points=3.0"), std::string::npos) << line;
    EXPECT_NE(line.find(" gap_value=300.0"), std::string::npos)
        << "long 2 x 50 x the 3-point gap, on record: " << line;
    EXPECT_EQ(count_lines("ROLL_LEG ROLL_A " + kXA), 0u);
    EXPECT_EQ(count_lines("ROLL_CONFIRMED " + kXA), 0u);
    // The position is still held, now in the new contract.
    const auto book = pm_->get_filled_strategy_positions();
    EXPECT_DOUBLE_EQ(static_cast<double>(book.at("ROLL_A").at(kXA).quantity), 2.0);
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

// The last close before the change bar is not usable (negative), so the closing leg has no
// price: the run fails with ROLL_LEG STOP (run_portfolio returns the error; the runner
// exits non-zero on it), never a warning and a run that goes on.
TEST_F(BacktestRollLegsOnceTest, ALegWithoutAUsableCloseFailsTheRun) {
    std::string err;
    EXPECT_FALSE(run_rows(rows(/*last_close_before_change=*/-1.0), &err))
        << "the run went on with a leg on an unusable close";
    EXPECT_NE(err.find("ROLL_LEG STOP " + kXA), std::string::npos) << err;
}

// An equity short accrues a BORROW row each cycle it is open. The row is typed BORROW and
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
    cc.statistics_sessions_per_year = 252.0;  // a run is refused without one
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
