// Parameter fan-out of the results managers: which database call each save
// makes, with which run id, portfolio id, strategy id, table and timestamp.
// The call-count tests in test_backtest_results_manager.cpp check that a call
// happened; these check what it carried. RecordingDb records the arguments and
// then defers to MockPostgresDatabase, so call counts and fail_on_call still work.

#include <gtest/gtest.h>
#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "trade_ngin/storage/backtest_results_manager.hpp"
#include "trade_ngin/storage/results_manager_base.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

Timestamp date_at(int year, int month, int day) {
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = 12;
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}

Position make_pos(const std::string& sym, double qty) {
    return Position(sym, Quantity(qty), Price(100.0), Decimal(0.0), Decimal(0.0), Timestamp{});
}

ExecutionReport make_exec(const std::string& symbol, double qty) {
    ExecutionReport e;
    e.order_id = "O_" + symbol;
    e.exec_id = "E_" + symbol;
    e.symbol = symbol;
    e.side = qty > 0 ? Side::BUY : Side::SELL;
    e.filled_quantity = Quantity(std::abs(qty));
    e.fill_price = Price(100.0);
    e.fill_time = date_at(2026, 1, 5);
    return e;
}

struct Call {
    std::string method;
    std::string run_id;
    std::string portfolio_id;
    std::string strategy_id;
    std::string table;
    Timestamp timestamp{};
    Timestamp start{};
    Timestamp end{};
    size_t items = 0;
    std::vector<Position> positions;
    double allocation = 0.0;
    std::string name;
    std::string description;
    nlohmann::json hyperparameters;
    std::vector<std::string> risk_detail;
    std::string portfolio_run_id;
};

class RecordingDb : public MockPostgresDatabase {
public:
    RecordingDb() : MockPostgresDatabase("mock://fanout") {}

    std::vector<Call> calls;

    std::vector<Call> calls_to(const std::string& method) const {
        std::vector<Call> out;
        for (const auto& c : calls)
            if (c.method == method) out.push_back(c);
        return out;
    }

    Result<void> store_backtest_summary(const std::string& run_id, const Timestamp& start_date,
                                        const Timestamp& end_date,
                                        const std::unordered_map<std::string, double>& metrics,
                                        const std::string& portfolio_id,
                                        const std::string& table_name) override {
        Call c{"store_backtest_summary", run_id, portfolio_id, "", table_name};
        c.start = start_date;
        c.end = end_date;
        c.items = metrics.size();
        calls.push_back(c);
        return MockPostgresDatabase::store_backtest_summary(run_id, start_date, end_date, metrics,
                                                            portfolio_id, table_name);
    }

    Result<void> store_backtest_equity_curve_batch(
        const std::string& run_id, const std::vector<std::pair<Timestamp, double>>& points,
        const std::string& portfolio_id, const std::string& table_name,
        const std::vector<std::string>& risk_detail) override {
        Call c{"store_backtest_equity_curve_batch", run_id, portfolio_id, "", table_name};
        c.items = points.size();
        c.risk_detail = risk_detail;
        calls.push_back(c);
        return MockPostgresDatabase::store_backtest_equity_curve_batch(run_id, points, portfolio_id,
                                                                       table_name, risk_detail);
    }

    Result<void> store_backtest_positions(const std::vector<Position>& positions,
                                          const std::string& run_id,
                                          const std::string& portfolio_id,
                                          const std::string& table_name,
                                          bool keep_closed_rows) override {
        Call c{"store_backtest_positions", run_id, portfolio_id, "", table_name};
        c.items = positions.size();
        c.positions = positions;
        calls.push_back(c);
        return MockPostgresDatabase::store_backtest_positions(positions, run_id, portfolio_id,
                                                              table_name, keep_closed_rows);
    }

    Result<void> store_backtest_executions(const std::vector<ExecutionReport>& executions,
                                           const std::string& run_id,
                                           const std::string& portfolio_id,
                                           const std::string& table_name) override {
        Call c{"store_backtest_executions", run_id, portfolio_id, "", table_name};
        c.items = executions.size();
        calls.push_back(c);
        return MockPostgresDatabase::store_backtest_executions(executions, run_id, portfolio_id,
                                                               table_name);
    }

    Result<void> store_backtest_signals(const std::unordered_map<std::string, double>& signals,
                                        const std::string& strategy_id, const std::string& run_id,
                                        const Timestamp& timestamp,
                                        const std::string& portfolio_id,
                                        const std::string& table_name) override {
        Call c{"store_backtest_signals", run_id, portfolio_id, strategy_id, table_name};
        c.timestamp = timestamp;
        c.items = signals.size();
        calls.push_back(c);
        return MockPostgresDatabase::store_backtest_signals(signals, strategy_id, run_id,
                                                            timestamp, portfolio_id, table_name);
    }

    Result<void> store_backtest_positions_with_strategy(const std::vector<Position>& positions,
                                                        const std::string& run_id,
                                                        const std::string& strategy_id,
                                                        const std::string& portfolio_id,
                                                        const std::string& table_name) override {
        Call c{"store_backtest_positions_with_strategy", run_id, portfolio_id, strategy_id,
               table_name};
        c.items = positions.size();
        calls.push_back(c);
        return MockPostgresDatabase::store_backtest_positions_with_strategy(
            positions, run_id, strategy_id, portfolio_id, table_name);
    }

    Result<void> store_backtest_executions_with_strategy(
        const std::vector<ExecutionReport>& executions, const std::string& run_id,
        const std::string& strategy_id, const std::string& portfolio_id,
        const std::string& table_name) override {
        Call c{"store_backtest_executions_with_strategy", run_id, portfolio_id, strategy_id,
               table_name};
        c.items = executions.size();
        calls.push_back(c);
        return MockPostgresDatabase::store_backtest_executions_with_strategy(
            executions, run_id, strategy_id, portfolio_id, table_name);
    }

    Result<void> store_backtest_metadata_with_portfolio(
        const std::string& run_id, const std::string& portfolio_run_id,
        const std::string& strategy_id, double strategy_allocation,
        const nlohmann::json& portfolio_config, const std::string& name,
        const std::string& description, const Timestamp& start_date, const Timestamp& end_date,
        const nlohmann::json& hyperparameters, const std::string& portfolio_id,
        const std::string& table_name) override {
        Call c{"store_backtest_metadata_with_portfolio", run_id, portfolio_id, strategy_id,
               table_name};
        c.allocation = strategy_allocation;
        c.name = name;
        c.description = description;
        c.start = start_date;
        c.end = end_date;
        c.hyperparameters = hyperparameters;
        c.portfolio_run_id = portfolio_run_id;
        calls.push_back(c);
        return MockPostgresDatabase::store_backtest_metadata_with_portfolio(
            run_id, portfolio_run_id, strategy_id, strategy_allocation, portfolio_config, name,
            description, start_date, end_date, hyperparameters, portfolio_id, table_name);
    }

    Result<void> store_signals(const std::unordered_map<std::string, double>& signals,
                               const std::string& strategy_id, const std::string& strategy_name,
                               const std::string& portfolio_id, const Timestamp& timestamp,
                               const std::string& table_name) override {
        Call c{"store_signals", "", portfolio_id, strategy_id, table_name};
        c.name = strategy_name;
        c.timestamp = timestamp;
        c.items = signals.size();
        calls.push_back(c);
        return MockPostgresDatabase::store_signals(signals, strategy_id, strategy_name,
                                                   portfolio_id, timestamp, table_name);
    }
};

// The base class's own save_positions / save_executions / save_signals, reached
// through a minimal concrete subclass.
class PlainResultsManager : public ResultsManagerBase {
public:
    using ResultsManagerBase::ResultsManagerBase;
    Result<void> save_all_results(const std::string&, const Timestamp&) override {
        return Result<void>();
    }
};

}  // namespace

// ============================================================================
// ResultsManagerBase
// ============================================================================

class ResultsManagerBaseFanoutTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        db_ = std::make_shared<RecordingDb>();
        ASSERT_TRUE(db_->connect().is_ok());
    }
    std::shared_ptr<RecordingDb> db_;
};

TEST_F(ResultsManagerBaseFanoutTest, BacktestPositionsGoToFinalPositionsStampedWithTheDate) {
    PlainResultsManager mgr(db_, true, "backtest", "STRAT", "PORT_A");
    const auto day = date_at(2026, 2, 3);
    ASSERT_TRUE(mgr.save_positions({make_pos("ES", 2.0), make_pos("NQ", -1.0)}, "RUN_7", day)
                    .is_ok());

    auto calls = db_->calls_to("store_backtest_positions");
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].run_id, "RUN_7");
    EXPECT_EQ(calls[0].table, "backtest.final_positions");
    ASSERT_EQ(calls[0].positions.size(), 2u);
    // Every position is re-stamped with the day being stored.
    EXPECT_EQ(calls[0].positions[0].last_update, day);
    EXPECT_EQ(calls[0].positions[1].last_update, day);
    // FIXME: the base class passes the literal "BASE_PORTFOLIO" for the backtest schema
    // instead of portfolio_id_ ("PORT_A" here). No code in src/ or apps/ calls the base
    // save_positions / save_executions / save_signals (BacktestResultsManager has its own
    // batch methods that do pass the portfolio), so this moves no stored number today;
    // the right value is portfolio_id_. Same for executions and signals below.
    EXPECT_EQ(calls[0].portfolio_id, "BASE_PORTFOLIO");
}

TEST_F(ResultsManagerBaseFanoutTest, TradingPositionsUseThePositionsTableAndTheManagersKey) {
    PlainResultsManager mgr(db_, true, "trading", "STRAT_ID", "PORT_A", "STRAT_NAME");
    ASSERT_TRUE(mgr.set_book("qt").is_ok());
    ASSERT_TRUE(mgr.save_positions({make_pos("ES", 2.0)}, "RUN", date_at(2026, 2, 3)).is_ok());

    EXPECT_EQ(db_->call_count("store_positions"), 1);  // the mock refuses any other table
    auto key = db_->last_key("store_positions");
    EXPECT_EQ(key.strategy_id, "STRAT_ID");
    EXPECT_EQ(key.strategy_name, "STRAT_NAME");
    EXPECT_EQ(key.portfolio_id, "PORT_A");
    EXPECT_EQ(db_->last_book("store_positions"), "qt");
    EXPECT_EQ(db_->call_count("store_backtest_positions"), 0);
}

TEST_F(ResultsManagerBaseFanoutTest, ExecutionsRouteBySchema) {
    PlainResultsManager bt(db_, true, "backtest", "STRAT", "PORT_A");
    ASSERT_TRUE(bt.save_executions({make_exec("ES", 1.0)}, "RUN_B", date_at(2026, 2, 3)).is_ok());
    auto calls = db_->calls_to("store_backtest_executions");
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].run_id, "RUN_B");
    EXPECT_EQ(calls[0].table, "backtest.executions");
    EXPECT_EQ(calls[0].items, 1u);
    EXPECT_EQ(calls[0].portfolio_id, "BASE_PORTFOLIO");  // FIXME: see the positions test

    PlainResultsManager live(db_, true, "trading", "STRAT", "PORT_A");
    ASSERT_TRUE(live.save_executions({make_exec("ES", 1.0)}, "RUN_L", date_at(2026, 2, 3))
                    .is_ok());
    EXPECT_EQ(db_->call_count("store_executions"), 1);  // table "trading.executions"
    auto key = db_->last_key("store_executions");
    EXPECT_EQ(key.strategy_id, "STRAT");
    EXPECT_EQ(key.strategy_name, "STRAT");  // empty name falls back to the id
    EXPECT_EQ(key.portfolio_id, "PORT_A");
    EXPECT_EQ(db_->last_book("store_executions"), kDefaultBook);
}

TEST_F(ResultsManagerBaseFanoutTest, SignalsRouteBySchemaWithTheirDate) {
    const auto day = date_at(2026, 2, 4);
    PlainResultsManager bt(db_, true, "backtest", "STRAT", "PORT_A");
    ASSERT_TRUE(bt.save_signals({{"ES", 0.5}, {"NQ", -0.25}}, "RUN_B", day).is_ok());
    auto bcalls = db_->calls_to("store_backtest_signals");
    ASSERT_EQ(bcalls.size(), 1u);
    EXPECT_EQ(bcalls[0].strategy_id, "STRAT");
    EXPECT_EQ(bcalls[0].run_id, "RUN_B");
    EXPECT_EQ(bcalls[0].timestamp, day);
    EXPECT_EQ(bcalls[0].table, "backtest.signals");
    EXPECT_EQ(bcalls[0].items, 2u);
    EXPECT_EQ(bcalls[0].portfolio_id, "BASE_PORTFOLIO");  // FIXME: see the positions test

    PlainResultsManager live(db_, true, "trading", "STRAT", "PORT_A", "NAME");
    ASSERT_TRUE(live.save_signals({{"ES", 0.5}}, "RUN_L", day).is_ok());
    auto lcalls = db_->calls_to("store_signals");
    ASSERT_EQ(lcalls.size(), 1u);
    EXPECT_EQ(lcalls[0].strategy_id, "STRAT");
    EXPECT_EQ(lcalls[0].name, "NAME");
    EXPECT_EQ(lcalls[0].portfolio_id, "PORT_A");
    EXPECT_EQ(lcalls[0].timestamp, day);
    EXPECT_EQ(lcalls[0].table, "trading.signals");
}

TEST_F(ResultsManagerBaseFanoutTest, DisabledStorageSkipsEveryWriteAndIsNotAnError) {
    PlainResultsManager mgr(db_, false, "trading", "STRAT", "PORT_A");
    const auto day = date_at(2026, 2, 3);
    EXPECT_TRUE(mgr.save_positions({make_pos("ES", 1.0)}, "R", day).is_ok());
    EXPECT_TRUE(mgr.save_executions({make_exec("ES", 1.0)}, "R", day).is_ok());
    EXPECT_TRUE(mgr.save_signals({{"ES", 1.0}}, "R", day).is_ok());
    EXPECT_TRUE(db_->calls.empty());
    EXPECT_EQ(db_->call_count("store_positions"), 0);
    EXPECT_EQ(db_->call_count("store_executions"), 0);
}

TEST_F(ResultsManagerBaseFanoutTest, EmptyInputsMakeNoCall) {
    PlainResultsManager mgr(db_, true, "backtest", "STRAT", "PORT_A");
    const auto day = date_at(2026, 2, 3);
    EXPECT_TRUE(mgr.save_positions({}, "R", day).is_ok());
    EXPECT_TRUE(mgr.save_executions({}, "R", day).is_ok());
    EXPECT_TRUE(mgr.save_signals({}, "R", day).is_ok());
    EXPECT_TRUE(db_->calls.empty());
}

TEST_F(ResultsManagerBaseFanoutTest, NullOrDisconnectedDatabaseIsADatabaseError) {
    const auto day = date_at(2026, 2, 3);
    PlainResultsManager null_db(nullptr, true, "trading", "STRAT");
    auto r = null_db.save_positions({make_pos("ES", 1.0)}, "R", day);
    ASSERT_TRUE(r.is_error());
    EXPECT_EQ(r.error()->code(), ErrorCode::DATABASE_ERROR);
    EXPECT_TRUE(null_db.save_executions({make_exec("ES", 1.0)}, "R", day).is_error());
    EXPECT_TRUE(null_db.save_signals({{"ES", 1.0}}, "R", day).is_error());

    db_->disconnect();
    PlainResultsManager mgr(db_, true, "backtest", "STRAT");
    auto r2 = mgr.save_signals({{"ES", 1.0}}, "R", day);
    ASSERT_TRUE(r2.is_error());
    EXPECT_EQ(r2.error()->code(), ErrorCode::DATABASE_ERROR);
    EXPECT_TRUE(db_->calls.empty());  // refused before reaching the database
}

TEST_F(ResultsManagerBaseFanoutTest, StrategyNameDefaultsToTheIdAndComponentNamesTheSchema) {
    PlainResultsManager mgr(db_, true, "trading", "LIVE_TF");
    EXPECT_EQ(mgr.get_strategy_name(), "LIVE_TF");
    EXPECT_EQ(mgr.get_portfolio_id(), "BASE_PORTFOLIO");
    EXPECT_EQ(mgr.get_schema(), "trading");
    EXPECT_EQ(mgr.get_book(), kDefaultBook);
    auto bad = mgr.set_book("hedge");
    ASSERT_TRUE(bad.is_error());
    EXPECT_NE(std::string(bad.error()->what()).find("hedge"), std::string::npos);
    EXPECT_EQ(mgr.get_book(), kDefaultBook);
}

// ============================================================================
// BacktestResultsManager
// ============================================================================

class BacktestResultsFanoutTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        db_ = std::make_shared<RecordingDb>();
        ASSERT_TRUE(db_->connect().is_ok());
        mgr_ = std::make_unique<BacktestResultsManager>(db_, true, "TF_SLOW", "PORT_CONS");
        mgr_->set_metadata(start_, end_, nlohmann::json{{"lookback", 20}}, "Run name", "Run desc");
        mgr_->set_performance_metrics({{"sharpe", 1.1}, {"total_return", 0.2}});
        mgr_->set_equity_curve({{start_, 1e6}, {end_, 1.1e6}});
        mgr_->set_equity_risk_detail({"{\"a\":1}", ""});
        mgr_->set_final_positions({make_pos("ES", 3.0)});
        mgr_->set_executions({make_exec("ES", 3.0), make_exec("NQ", -1.0)});
        mgr_->add_signals(date_at(2026, 1, 6), {{"ES", 0.1}});
        mgr_->add_signals(date_at(2026, 1, 2), {{"ES", 0.2}, {"NQ", 0.3}});
    }

    const Timestamp start_ = date_at(2026, 1, 2);
    const Timestamp end_ = date_at(2026, 1, 30);
    std::shared_ptr<RecordingDb> db_;
    std::unique_ptr<BacktestResultsManager> mgr_;
};

TEST_F(BacktestResultsFanoutTest, EveryWriteCarriesTheRunIdAndTheConstructorPortfolio) {
    ASSERT_TRUE(mgr_->save_all_results("RUN_42", end_).is_ok());

    // summary, equity curve, final positions, executions, two signal timestamps
    ASSERT_EQ(db_->calls.size(), 6u);
    for (const auto& c : db_->calls) {
        EXPECT_EQ(c.run_id, "RUN_42") << c.method;
        // The writes use the derived class's portfolio_id_ even though
        // get_portfolio_id() (base member) still reads "BASE_PORTFOLIO": see the
        // FIXME in test_backtest_results_manager.cpp ConstructorSetsAccessors.
        EXPECT_EQ(c.portfolio_id, "PORT_CONS") << c.method;
    }
    EXPECT_EQ(mgr_->get_portfolio_id(), "BASE_PORTFOLIO");
}

TEST_F(BacktestResultsFanoutTest, SaveAllWritesInForeignKeyOrderToTheRightTables) {
    ASSERT_TRUE(mgr_->save_all_results("RUN_42", end_).is_ok());
    std::vector<std::string> methods, tables;
    for (const auto& c : db_->calls) {
        methods.push_back(c.method);
        tables.push_back(c.table);
    }
    EXPECT_EQ(methods, (std::vector<std::string>{
                           "store_backtest_summary", "store_backtest_equity_curve_batch",
                           "store_backtest_positions", "store_backtest_executions",
                           "store_backtest_signals", "store_backtest_signals"}));
    EXPECT_EQ(tables, (std::vector<std::string>{"backtest.results", "backtest.equity_curve",
                                                "backtest.final_positions", "backtest.executions",
                                                "backtest.signals", "backtest.signals"}));
}

TEST_F(BacktestResultsFanoutTest, SummaryAndCurveCarryTheirPayload) {
    ASSERT_TRUE(mgr_->save_summary_results("R").is_ok());
    ASSERT_TRUE(mgr_->save_equity_curve("R").is_ok());
    auto summary = db_->calls_to("store_backtest_summary");
    ASSERT_EQ(summary.size(), 1u);
    EXPECT_EQ(summary[0].start, start_);
    EXPECT_EQ(summary[0].end, end_);
    EXPECT_EQ(summary[0].items, 2u);
    auto curve = db_->calls_to("store_backtest_equity_curve_batch");
    ASSERT_EQ(curve.size(), 1u);
    EXPECT_EQ(curve[0].items, 2u);
    EXPECT_EQ(curve[0].risk_detail, (std::vector<std::string>{"{\"a\":1}", ""}));
}

TEST_F(BacktestResultsFanoutTest, SignalsAreOneCallPerTimestampInTimeOrder) {
    ASSERT_TRUE(mgr_->save_signals_batch("R").is_ok());
    auto calls = db_->calls_to("store_backtest_signals");
    ASSERT_EQ(calls.size(), 2u);
    // Added out of order; stored in timestamp order.
    EXPECT_EQ(calls[0].timestamp, date_at(2026, 1, 2));
    EXPECT_EQ(calls[0].items, 2u);
    EXPECT_EQ(calls[1].timestamp, date_at(2026, 1, 6));
    EXPECT_EQ(calls[1].items, 1u);
    for (const auto& c : calls) EXPECT_EQ(c.strategy_id, "TF_SLOW");
}

TEST_F(BacktestResultsFanoutTest, SummaryFailureStopsTheRunBeforeAnyOtherWrite) {
    db_->fail_on_call("store_backtest_summary");
    auto r = mgr_->save_all_results("R", end_);
    ASSERT_TRUE(r.is_error());
    EXPECT_EQ(r.error()->code(), ErrorCode::DATABASE_ERROR);
    ASSERT_EQ(db_->calls.size(), 1u);
    EXPECT_EQ(db_->calls[0].method, "store_backtest_summary");
}

TEST_F(BacktestResultsFanoutTest, LaterFailuresAreLoggedAndTheRunStillReportsSuccess) {
    // Today's behaviour: only the summary is fatal. A failed equity curve, position,
    // execution or signal write is logged and save_all_results still returns ok, and
    // the remaining tables are still attempted.
    for (const char* method :
         {"store_backtest_equity_curve_batch", "store_backtest_positions",
          "store_backtest_executions", "store_backtest_signals"}) {
        db_->calls.clear();
        db_->fail_on_call(method);
        EXPECT_TRUE(mgr_->save_all_results("R", end_).is_ok()) << method;
        EXPECT_EQ(db_->calls.size(), 6u) << method;
        db_->clear_failure();
    }
}

TEST_F(BacktestResultsFanoutTest, NoPortfolioLevelExecutionsMeansNoExecutionsCall) {
    mgr_->set_executions({});
    ASSERT_TRUE(mgr_->save_all_results("R", end_).is_ok());
    EXPECT_TRUE(db_->calls_to("store_backtest_executions").empty());
    EXPECT_EQ(db_->calls.size(), 5u);
}

TEST_F(BacktestResultsFanoutTest, EmptyPortfolioIdFallsBackToBasePortfolio) {
    BacktestResultsManager mgr(db_, true, "TF", "");
    mgr.set_final_positions({make_pos("ES", 1.0)});
    ASSERT_TRUE(mgr.save_final_positions("R").is_ok());
    auto calls = db_->calls_to("store_backtest_positions");
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].portfolio_id, "BASE_PORTFOLIO");
}

TEST_F(BacktestResultsFanoutTest, PerStrategyRowsCarryTheirStrategyAndSkipEmptyOnes) {
    mgr_->set_strategy_positions("TF_FAST", {make_pos("ES", 1.0), make_pos("NQ", 2.0)});
    mgr_->set_strategy_positions("TF_EMPTY", {});
    mgr_->set_strategy_executions("TF_FAST", {make_exec("ES", 1.0)});
    mgr_->set_strategy_executions("TF_EMPTY", {});
    ASSERT_TRUE(mgr_->save_strategy_positions("PRUN").is_ok());
    ASSERT_TRUE(mgr_->save_strategy_executions("PRUN").is_ok());

    auto pos = db_->calls_to("store_backtest_positions_with_strategy");
    ASSERT_EQ(pos.size(), 1u);
    EXPECT_EQ(pos[0].strategy_id, "TF_FAST");
    EXPECT_EQ(pos[0].run_id, "PRUN");
    EXPECT_EQ(pos[0].portfolio_id, "PORT_CONS");
    EXPECT_EQ(pos[0].table, "backtest.final_positions");
    EXPECT_EQ(pos[0].items, 2u);

    auto exe = db_->calls_to("store_backtest_executions_with_strategy");
    ASSERT_EQ(exe.size(), 1u);
    EXPECT_EQ(exe[0].strategy_id, "TF_FAST");
    EXPECT_EQ(exe[0].table, "backtest.executions");
    EXPECT_EQ(exe[0].items, 1u);
}

TEST_F(BacktestResultsFanoutTest, PerStrategyFailureDoesNotStopTheOtherStrategies) {
    mgr_->set_strategy_positions("A", {make_pos("ES", 1.0)});
    mgr_->set_strategy_positions("B", {make_pos("NQ", 1.0)});
    db_->fail_on_call("store_backtest_positions_with_strategy");
    EXPECT_TRUE(mgr_->save_strategy_positions("PRUN").is_ok());
    EXPECT_EQ(db_->calls_to("store_backtest_positions_with_strategy").size(), 2u);
}

TEST_F(BacktestResultsFanoutTest, StrategyMetadataRowPerAllocationWithTheRunMetadata) {
    ASSERT_TRUE(mgr_->save_strategy_metadata("PRUN", {{"TF_SLOW", 0.6}, {"TF_FAST", 0.4}},
                                             nlohmann::json{{"capital", 1e6}})
                    .is_ok());
    auto calls = db_->calls_to("store_backtest_metadata_with_portfolio");
    ASSERT_EQ(calls.size(), 2u);
    double total_alloc = 0.0;
    for (const auto& c : calls) {
        EXPECT_EQ(c.run_id, "PRUN");
        EXPECT_EQ(c.portfolio_run_id, "PRUN");
        EXPECT_EQ(c.portfolio_id, "PORT_CONS");
        EXPECT_EQ(c.table, "backtest.run_metadata");
        EXPECT_EQ(c.name, "Run name");
        EXPECT_EQ(c.description, "Run desc");
        EXPECT_EQ(c.start, start_);
        EXPECT_EQ(c.end, end_);
        EXPECT_EQ(c.hyperparameters, (nlohmann::json{{"lookback", 20}}));
        if (c.strategy_id == "TF_SLOW") EXPECT_DOUBLE_EQ(c.allocation, 0.6);
        if (c.strategy_id == "TF_FAST") EXPECT_DOUBLE_EQ(c.allocation, 0.4);
        total_alloc += c.allocation;
    }
    EXPECT_DOUBLE_EQ(total_alloc, 1.0);
}

TEST_F(BacktestResultsFanoutTest, DisabledStorageMakesEveryIndividualSaveANoOp) {
    mgr_->set_storage_enabled(false);
    mgr_->set_strategy_positions("A", {make_pos("ES", 1.0)});
    mgr_->set_strategy_executions("A", {make_exec("ES", 1.0)});
    EXPECT_TRUE(mgr_->save_summary_results("R").is_ok());
    EXPECT_TRUE(mgr_->save_equity_curve("R").is_ok());
    EXPECT_TRUE(mgr_->save_final_positions("R").is_ok());
    EXPECT_TRUE(mgr_->save_executions_batch("R").is_ok());
    EXPECT_TRUE(mgr_->save_signals_batch("R").is_ok());
    EXPECT_TRUE(mgr_->save_strategy_positions("R").is_ok());
    EXPECT_TRUE(mgr_->save_strategy_executions("R").is_ok());
    EXPECT_TRUE(mgr_->save_strategy_metadata("R", {{"A", 1.0}}, {}).is_ok());
    EXPECT_TRUE(db_->calls.empty());
}

TEST_F(BacktestResultsFanoutTest, NoMetricsMeansNoSummaryRow) {
    mgr_->set_performance_metrics({});
    ASSERT_TRUE(mgr_->save_summary_results("R").is_ok());
    EXPECT_TRUE(db_->calls.empty());
}

TEST(BacktestResultsRunId, RunIdIsStrategyUtcStampAndMilliseconds) {
    const auto id = BacktestResultsManager::generate_run_id("TF");
    // TF_YYYYMMDD_HHMMSS_mmm
    ASSERT_EQ(id.size(), std::string("TF_20260101_120000_000").size()) << id;
    EXPECT_EQ(id.substr(0, 3), "TF_");
    EXPECT_EQ(id[11], '_');
    EXPECT_EQ(id[18], '_');
    for (size_t i : {3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 12u, 13u, 14u, 15u, 16u, 17u, 19u, 20u, 21u})
        EXPECT_TRUE(std::isdigit(static_cast<unsigned char>(id[i]))) << id;
}
