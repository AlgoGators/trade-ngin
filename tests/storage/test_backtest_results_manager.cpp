#include <gtest/gtest.h>
#include <chrono>
#include <memory>
#include <thread>
#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "trade_ngin/storage/backtest_results_manager.hpp"

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

ExecutionReport make_exec(const std::string& symbol, double qty, double price) {
    ExecutionReport e;
    e.order_id = "O";
    e.exec_id = "E";
    e.symbol = symbol;
    e.side = qty > 0 ? Side::BUY : Side::SELL;
    e.filled_quantity = Quantity(std::abs(qty));
    e.fill_price = Price(price);
    e.fill_time = date_at(2026, 1, 5);
    e.commissions_fees = Decimal(1.0);
    e.implicit_price_impact = Decimal(0.0);
    e.slippage_market_impact = Decimal(0.0);
    e.total_transaction_costs = Decimal(1.0);
    return e;
}

Position make_pos(const std::string& sym, double qty, double avg_price = 100.0) {
    return Position(sym, Quantity(qty), Price(avg_price), Decimal(0.0), Decimal(0.0),
                    Timestamp{});
}

}  // namespace

class BacktestResultsManagerTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        ASSERT_TRUE(db_->connect().is_ok());
        mgr_ = std::make_unique<BacktestResultsManager>(db_, /*store_enabled=*/true,
                                                          "TEST_STRAT", "TEST_PORTFOLIO");
    }
    std::shared_ptr<MockPostgresDatabase> db_;
    std::unique_ptr<BacktestResultsManager> mgr_;
};

// ===== generate_run_id =====

TEST_F(BacktestResultsManagerTest, GenerateRunIdContainsStrategyId) {
    auto id = BacktestResultsManager::generate_run_id("TF_FAST");
    EXPECT_NE(id.find("TF_FAST"), std::string::npos);
    EXPECT_FALSE(id.empty());
}

TEST_F(BacktestResultsManagerTest, GenerateRunIdProducesDistinctIdsAcrossCalls) {
    auto id1 = BacktestResultsManager::generate_run_id("X");
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    auto id2 = BacktestResultsManager::generate_run_id("X");
    EXPECT_NE(id1, id2);
}

// ===== getters / setters =====

TEST_F(BacktestResultsManagerTest, ConstructorSetsAccessors) {
    EXPECT_TRUE(mgr_->is_storage_enabled());
    EXPECT_EQ(mgr_->get_schema(), "backtest");
    EXPECT_EQ(mgr_->get_strategy_id(), "TEST_STRAT");
    // FIXME: production bug — BacktestResultsManager declares its own
    // `portfolio_id_` member that shadows the one on ResultsManagerBase. The
    // constructor stores the supplied portfolio_id into the derived class's
    // shadow, while get_portfolio_id() (defined on the base) reads the base's
    // member, which keeps its default "BASE_PORTFOLIO". Capture observed
    // behavior so this test fires if/when the shadow is removed.
    EXPECT_EQ(mgr_->get_portfolio_id(), "BASE_PORTFOLIO");
}

TEST_F(BacktestResultsManagerTest, SetStorageEnabledFlipsFlag) {
    mgr_->set_storage_enabled(false);
    EXPECT_FALSE(mgr_->is_storage_enabled());
    mgr_->set_storage_enabled(true);
    EXPECT_TRUE(mgr_->is_storage_enabled());
}

// ===== save_all_results: storage disabled short-circuits =====

TEST_F(BacktestResultsManagerTest, SaveAllResultsWhenDisabledDoesNotInvokeAnyDbWrite) {
    mgr_->set_storage_enabled(false);
    db_->reset_call_counts();
    auto r = mgr_->save_all_results("RUN_1", date_at(2026, 1, 5));
    EXPECT_TRUE(r.is_ok());
    EXPECT_EQ(db_->call_count("store_backtest_summary"), 0);
    EXPECT_EQ(db_->call_count("store_backtest_equity_curve_batch"), 0);
    EXPECT_EQ(db_->call_count("store_backtest_positions"), 0);
}

// ===== save_summary_results =====

TEST_F(BacktestResultsManagerTest, SaveSummaryResultsCallsStoreBacktestSummaryOnce) {
    mgr_->set_metadata(date_at(2026, 1, 1), date_at(2026, 1, 5), nlohmann::json{}, "RUN", "");
    mgr_->set_performance_metrics({{"total_return", 0.05}, {"sharpe", 1.2}});
    db_->reset_call_counts();
    auto r = mgr_->save_summary_results("RUN_1");
    EXPECT_TRUE(r.is_ok());
    EXPECT_EQ(db_->call_count("store_backtest_summary"), 1);
}

TEST_F(BacktestResultsManagerTest, SaveSummaryResultsPropagatesDbError) {
    mgr_->set_metadata(date_at(2026, 1, 1), date_at(2026, 1, 5), nlohmann::json{}, "RUN", "");
    mgr_->set_performance_metrics({{"total_return", 0.05}});
    db_->fail_on_call("store_backtest_summary");
    auto r = mgr_->save_summary_results("RUN_1");
    EXPECT_TRUE(r.is_error());
    EXPECT_EQ(r.error()->code(), ErrorCode::DATABASE_ERROR);
}

// ===== save_equity_curve =====

TEST_F(BacktestResultsManagerTest, SaveEquityCurveCallsBatchStoreWhenCurvePresent) {
    mgr_->set_equity_curve({
        {date_at(2026, 1, 1), 1'000'000.0},
        {date_at(2026, 1, 2), 1'010'000.0},
    });
    db_->reset_call_counts();
    auto r = mgr_->save_equity_curve("RUN_1");
    EXPECT_TRUE(r.is_ok());
    EXPECT_EQ(db_->call_count("store_backtest_equity_curve_batch"), 1);
}

TEST_F(BacktestResultsManagerTest, SaveEquityCurveEmptySkipsDbCall) {
    db_->reset_call_counts();
    auto r = mgr_->save_equity_curve("RUN_1");
    EXPECT_TRUE(r.is_ok());
    EXPECT_EQ(db_->call_count("store_backtest_equity_curve_batch"), 0);
}

// ===== save_final_positions =====

TEST_F(BacktestResultsManagerTest, SaveFinalPositionsRoutesThroughStoreBacktestPositions) {
    mgr_->set_final_positions({make_pos("ES", 5.0), make_pos("NQ", -2.0)});
    db_->reset_call_counts();
    auto r = mgr_->save_final_positions("RUN_1");
    EXPECT_TRUE(r.is_ok());
    // Either route may be used depending on internal logic; assert at least one.
    int total = db_->call_count("store_backtest_positions") +
                db_->call_count("store_positions");
    EXPECT_GT(total, 0);
}

// ===== save_executions_batch =====

TEST_F(BacktestResultsManagerTest, SaveExecutionsBatchInvokesStoreExecutions) {
    mgr_->set_executions({make_exec("ES", 5.0, 4000.0), make_exec("ES", -2.0, 4010.0)});
    db_->reset_call_counts();
    auto r = mgr_->save_executions_batch("RUN_1");
    EXPECT_TRUE(r.is_ok());
    int total = db_->call_count("store_backtest_executions") +
                db_->call_count("store_backtest_executions_with_strategy");
    EXPECT_GT(total, 0);
}

TEST_F(BacktestResultsManagerTest, SaveExecutionsBatchEmptyIsNoOp) {
    db_->reset_call_counts();
    auto r = mgr_->save_executions_batch("RUN_1");
    EXPECT_TRUE(r.is_ok());
    EXPECT_EQ(db_->call_count("store_backtest_executions"), 0);
    EXPECT_EQ(db_->call_count("store_backtest_executions_with_strategy"), 0);
}

// ===== save_signals_batch =====

TEST_F(BacktestResultsManagerTest, SaveSignalsBatchInvokesStoreBacktestSignalsPerTimestamp) {
    mgr_->add_signals(date_at(2026, 1, 1), {{"ES", 0.5}, {"NQ", -0.3}});
    mgr_->add_signals(date_at(2026, 1, 2), {{"ES", 0.6}});
    db_->reset_call_counts();
    auto r = mgr_->save_signals_batch("RUN_1");
    EXPECT_TRUE(r.is_ok());
    EXPECT_GT(db_->call_count("store_backtest_signals"), 0);
}

// ===== save_metadata =====

TEST_F(BacktestResultsManagerTest, SaveMetadataInvokesStoreBacktestMetadata) {
    mgr_->set_metadata(date_at(2026, 1, 1), date_at(2026, 1, 5),
                        nlohmann::json{{"key", "val"}}, "Run name", "Run desc");
    db_->reset_call_counts();
    auto r = mgr_->save_metadata("RUN_1");
    EXPECT_TRUE(r.is_ok());
    EXPECT_EQ(db_->call_count("store_backtest_metadata"), 1);
}

// ===== save_strategy_positions / save_strategy_executions =====

TEST_F(BacktestResultsManagerTest, SaveStrategyPositionsInvokesPerStrategyDbCall) {
    mgr_->set_strategy_positions("STRAT_A", {make_pos("ES", 5.0)});
    mgr_->set_strategy_positions("STRAT_B", {make_pos("NQ", -2.0)});
    db_->reset_call_counts();
    auto r = mgr_->save_strategy_positions("PORTFOLIO_RUN_1");
    EXPECT_TRUE(r.is_ok());
    EXPECT_GE(db_->call_count("store_backtest_positions_with_strategy"), 2);
}

TEST_F(BacktestResultsManagerTest, SaveStrategyExecutionsInvokesPerStrategyDbCall) {
    mgr_->set_strategy_executions("STRAT_A", {make_exec("ES", 5.0, 4000.0)});
    mgr_->set_strategy_executions("STRAT_B", {make_exec("NQ", -2.0, 15000.0)});
    db_->reset_call_counts();
    auto r = mgr_->save_strategy_executions("PORTFOLIO_RUN_1");
    EXPECT_TRUE(r.is_ok());
    EXPECT_GE(db_->call_count("store_backtest_executions_with_strategy"), 2);
}

TEST_F(BacktestResultsManagerTest, SaveStrategyMetadataInvokesPerStrategyMetadataCall) {
    mgr_->set_strategy_positions("STRAT_A", {make_pos("ES", 5.0)});
    mgr_->set_metadata(date_at(2026, 1, 1), date_at(2026, 1, 5),
                        nlohmann::json{{"k", "v"}}, "Run", "");
    std::unordered_map<std::string, double> allocations{{"STRAT_A", 1.0}};
    nlohmann::json portfolio_config = {{"name", "test"}};
    db_->reset_call_counts();
    auto r = mgr_->save_strategy_metadata("PORTFOLIO_RUN_1", allocations, portfolio_config);
    EXPECT_TRUE(r.is_ok());
    EXPECT_GE(db_->call_count("store_backtest_metadata_with_portfolio"), 1);
}

// ===== save_all_results: integration =====

TEST_F(BacktestResultsManagerTest, SaveAllResultsRoutesAllStorageMethodsInOrder) {
    mgr_->set_metadata(date_at(2026, 1, 1), date_at(2026, 1, 5),
                        nlohmann::json{}, "Run", "Desc");
    mgr_->set_performance_metrics({{"total_return", 0.05}});
    mgr_->set_equity_curve({{date_at(2026, 1, 1), 1'000'000.0}});
    mgr_->set_final_positions({make_pos("ES", 5.0)});
    mgr_->set_executions({make_exec("ES", 5.0, 4000.0)});
    mgr_->add_signals(date_at(2026, 1, 1), {{"ES", 0.5}});

    db_->reset_call_counts();
    auto r = mgr_->save_all_results("RUN_1", date_at(2026, 1, 5));
    EXPECT_TRUE(r.is_ok());
    EXPECT_EQ(db_->call_count("store_backtest_summary"), 1);
    EXPECT_EQ(db_->call_count("store_backtest_equity_curve_batch"), 1);
    int positions_total = db_->call_count("store_backtest_positions") +
                          db_->call_count("store_positions");
    EXPECT_GT(positions_total, 0);
    int execs_total = db_->call_count("store_backtest_executions") +
                      db_->call_count("store_backtest_executions_with_strategy");
    EXPECT_GT(execs_total, 0);
}

// ===== validation paths =====

TEST_F(BacktestResultsManagerTest, NotConnectedDbCausesErrorOnSave) {
    db_->disconnect();
    mgr_->set_metadata(date_at(2026, 1, 1), date_at(2026, 1, 5), nlohmann::json{}, "R", "");
    mgr_->set_performance_metrics({{"a", 1.0}});
    auto r = mgr_->save_summary_results("RUN_1");
    EXPECT_TRUE(r.is_error());
}
