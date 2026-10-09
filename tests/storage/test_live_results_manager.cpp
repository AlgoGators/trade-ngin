#include <gtest/gtest.h>
#include <chrono>
#include <memory>
#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "trade_ngin/storage/live_results_manager.hpp"

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

ExecutionReport make_exec(const std::string& symbol, double qty, double price) {
    ExecutionReport e;
    e.order_id = "O";
    e.exec_id = "E";
    e.symbol = symbol;
    e.side = qty > 0 ? Side::BUY : Side::SELL;
    e.filled_quantity = Quantity(std::abs(qty));
    e.fill_price = Price(price);
    e.fill_time = Timestamp{};
    e.commissions_fees = Decimal(1.0);
    e.implicit_price_impact = Decimal(0.0);
    e.slippage_market_impact = Decimal(0.0);
    e.total_transaction_costs = Decimal(1.0);
    return e;
}

}  // namespace

class LiveResultsManagerTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        ASSERT_TRUE(db_->connect().is_ok());
        mgr_ = std::make_unique<LiveResultsManager>(db_, /*store_enabled=*/true,
                                                      "STRAT_X", "PORT_Y");
    }
    std::shared_ptr<MockPostgresDatabase> db_;
    std::unique_ptr<LiveResultsManager> mgr_;
};

TEST_F(LiveResultsManagerTest, ConstructorSetsBaseAccessors) {
    EXPECT_TRUE(mgr_->is_storage_enabled());
    EXPECT_EQ(mgr_->get_schema(), "trading");
    EXPECT_EQ(mgr_->get_strategy_id(), "STRAT_X");
}

TEST_F(LiveResultsManagerTest, GenerateRunIdEncodesStrategyAndDate) {
    auto id = LiveResultsManager::generate_run_id("STRAT_X", date_at(2026, 3, 15));
    EXPECT_NE(id.find("STRAT_X"), std::string::npos);
    EXPECT_FALSE(id.empty());
}

TEST_F(LiveResultsManagerTest, NeedsFinalizationDetectsDateChange) {
    EXPECT_TRUE(mgr_->needs_finalization(date_at(2026, 3, 15), date_at(2026, 3, 14)));
    EXPECT_FALSE(mgr_->needs_finalization(date_at(2026, 3, 15), date_at(2026, 3, 15)));
}

TEST_F(LiveResultsManagerTest, DeleteStaleDataInvokesAllThreeDeletes) {
    db_->reset_call_counts();
    auto r = mgr_->delete_stale_data(date_at(2026, 3, 15));
    EXPECT_TRUE(r.is_ok());
    EXPECT_EQ(db_->call_count("delete_live_results"), 1);
    EXPECT_EQ(db_->call_count("delete_live_equity_curve"), 1);
    // delete_stale_executions only called when there are order_ids — 0 here.
}

TEST_F(LiveResultsManagerTest, SavePositionsSnapshotInvokesStorePositions) {
    mgr_->set_positions({make_pos("ES", 5.0), make_pos("NQ", -2.0)});
    db_->reset_call_counts();
    auto r = mgr_->save_positions_snapshot(date_at(2026, 3, 15));
    EXPECT_TRUE(r.is_ok());
    int total = db_->call_count("store_positions");
    EXPECT_GT(total, 0);
}

TEST_F(LiveResultsManagerTest, SavePositionsSnapshotEmptyIsNoOp) {
    db_->reset_call_counts();
    auto r = mgr_->save_positions_snapshot(date_at(2026, 3, 15));
    EXPECT_TRUE(r.is_ok());
    EXPECT_EQ(db_->call_count("store_positions"), 0);
}

TEST_F(LiveResultsManagerTest, SaveExecutionsBatchEmptyIsNoOp) {
    db_->reset_call_counts();
    auto r = mgr_->save_executions_batch(date_at(2026, 3, 15));
    EXPECT_TRUE(r.is_ok());
    EXPECT_EQ(db_->call_count("store_executions"), 0);
}

TEST_F(LiveResultsManagerTest, SaveSignalsSnapshotEmptyIsNoOp) {
    db_->reset_call_counts();
    auto r = mgr_->save_signals_snapshot(date_at(2026, 3, 15));
    EXPECT_TRUE(r.is_ok());
    EXPECT_EQ(db_->call_count("store_signals"), 0);
}

TEST_F(LiveResultsManagerTest, SaveLiveResultsInvokesStoreLiveResultsComplete) {
    mgr_->set_metrics({{"total_return", 0.05}, {"sharpe", 1.2}}, {{"trades", 10}});
    mgr_->set_config(nlohmann::json{{"k", "v"}});
    db_->reset_call_counts();
    auto r = mgr_->save_live_results(date_at(2026, 3, 15));
    EXPECT_TRUE(r.is_ok());
    EXPECT_EQ(db_->call_count("store_live_results_complete"), 1);
}

TEST_F(LiveResultsManagerTest, SaveEquityCurveWhenSetInvokesStoreTradingEquityCurve) {
    mgr_->set_equity(1'050'000.0);
    db_->reset_call_counts();
    auto r = mgr_->save_equity_curve(date_at(2026, 3, 15));
    EXPECT_TRUE(r.is_ok());
    EXPECT_EQ(db_->call_count("store_trading_equity_curve"), 1);
}

TEST_F(LiveResultsManagerTest, UpdateLiveResultsInvokesUpdate) {
    db_->reset_call_counts();
    auto r = mgr_->update_live_results(date_at(2026, 3, 15),
                                        {{"realized_pnl", 1234.5}});
    EXPECT_TRUE(r.is_ok());
    EXPECT_EQ(db_->call_count("update_live_results"), 1);
}

TEST_F(LiveResultsManagerTest, UpdateEquityCurveInvokesUpdateLiveEquityCurve) {
    db_->reset_call_counts();
    auto r = mgr_->update_equity_curve(date_at(2026, 3, 15), 1'050'000.0);
    EXPECT_TRUE(r.is_ok());
    EXPECT_EQ(db_->call_count("update_live_equity_curve"), 1);
}

TEST_F(LiveResultsManagerTest, SaveAllResultsRoutesAllStorageMethods) {
    mgr_->set_positions({make_pos("ES", 5.0)});
    mgr_->set_executions({make_exec("ES", 5.0, 4000.0)});
    mgr_->set_signals({{"ES", 0.5}});
    mgr_->set_metrics({{"total_return", 0.05}}, {{"trades", 1}});
    mgr_->set_config(nlohmann::json{});
    mgr_->set_equity(1'050'000.0);
    db_->reset_call_counts();
    auto r = mgr_->save_all_results("RUN_1", date_at(2026, 3, 15));
    EXPECT_TRUE(r.is_ok());
    // Every observable storage entry point invoked at least once.
    EXPECT_GT(db_->call_count("delete_live_results"), 0);
    EXPECT_GT(db_->call_count("store_live_results_complete"), 0);
    EXPECT_GT(db_->call_count("store_trading_equity_curve"), 0);
}

TEST_F(LiveResultsManagerTest, StorageDisabledShortCircuitsSaveAllResults) {
    mgr_->set_storage_enabled(false);
    db_->reset_call_counts();
    auto r = mgr_->save_all_results("RUN_1", date_at(2026, 3, 15));
    EXPECT_TRUE(r.is_ok());
    EXPECT_EQ(db_->call_count("store_live_results_complete"), 0);
    EXPECT_EQ(db_->call_count("store_trading_equity_curve"), 0);
}

TEST_F(LiveResultsManagerTest, DbErrorPropagatesFromSaveLiveResults) {
    mgr_->set_metrics({{"total_return", 0.05}}, {});
    mgr_->set_config(nlohmann::json{});
    db_->fail_on_call("store_live_results_complete");
    auto r = mgr_->save_live_results(date_at(2026, 3, 15));
    EXPECT_TRUE(r.is_error());
    EXPECT_EQ(r.error()->code(), ErrorCode::DATABASE_ERROR);
}

TEST_F(LiveResultsManagerTest, NotConnectedDbCausesError) {
    mgr_->set_positions({make_pos("ES", 5.0)});  // non-empty so the call reaches DB validation
    db_->disconnect();
    auto r = mgr_->save_positions_snapshot(date_at(2026, 3, 15));
    EXPECT_TRUE(r.is_error());
}

// ===================== FIX-0: storage key threading =====================
//
// Live rows are keyed (strategy_id, strategy_name, portfolio_id). Before FIX-0
// ResultsManagerBase passed strategy_id_ for BOTH id and name, and the equity runner
// never set portfolio_id, so the coordinator's BASE_PORTFOLIO default (the futures
// book) went on the row. Two of three key columns differed from what every read used,
// which is why run N+1 loaded an empty book. These tests pin all three columns at the
// DB-call boundary in both shapes: the equity shape (distinct name) and the futures
// shape (empty name, which must still write id-as-name byte-for-byte).

class LiveResultsManagerKeyTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        ASSERT_TRUE(db_->connect().is_ok());
    }
    std::unique_ptr<LiveResultsManager> make_mgr(const std::string& strategy_id,
                                                 const std::string& portfolio_id,
                                                 const std::string& strategy_name) {
        return std::make_unique<LiveResultsManager>(db_, /*store_enabled=*/true, strategy_id,
                                                    portfolio_id, strategy_name);
    }
    std::shared_ptr<MockPostgresDatabase> db_;
};

TEST_F(LiveResultsManagerKeyTest, PositionsWriteUnderDistinctStrategyName) {
    auto mgr = make_mgr("LIVE_EQUITY_MEAN_REVERSION", "EQUITY_PORTFOLIO",
                        "EQUITY_MEAN_REVERSION");
    mgr->set_positions({make_pos("AAPL", 5.0)});
    ASSERT_TRUE(mgr->save_positions_snapshot(date_at(2026, 3, 15)).is_ok());

    auto key = db_->last_key("store_positions");
    EXPECT_EQ(key.strategy_id, "LIVE_EQUITY_MEAN_REVERSION");
    // Pre-FIX-0 this was "LIVE_EQUITY_MEAN_REVERSION" — strategy_id_ was passed twice.
    EXPECT_EQ(key.strategy_name, "EQUITY_MEAN_REVERSION");
    EXPECT_EQ(key.portfolio_id, "EQUITY_PORTFOLIO");
}

TEST_F(LiveResultsManagerKeyTest, ExecutionsAndSignalsShareThePositionsKey) {
    auto mgr = make_mgr("LIVE_EQUITY_MEAN_REVERSION", "EQUITY_PORTFOLIO",
                        "EQUITY_MEAN_REVERSION");
    mgr->set_positions({make_pos("AAPL", 5.0)});
    mgr->set_executions({make_exec("AAPL", 5.0, 190.0)});
    mgr->set_signals({{"AAPL", 0.5}});
    ASSERT_TRUE(mgr->save_positions_snapshot(date_at(2026, 3, 15)).is_ok());
    ASSERT_TRUE(mgr->save_executions_batch(date_at(2026, 3, 15)).is_ok());
    ASSERT_TRUE(mgr->save_signals_snapshot(date_at(2026, 3, 15)).is_ok());

    auto pos = db_->last_key("store_positions");
    auto exe = db_->last_key("store_executions");
    auto sig = db_->last_key("store_signals");
    // A row written under a key its siblings do not share is a row no read reassembles.
    EXPECT_EQ(exe.strategy_id, pos.strategy_id);
    EXPECT_EQ(exe.strategy_name, pos.strategy_name);
    EXPECT_EQ(exe.portfolio_id, pos.portfolio_id);
    EXPECT_EQ(sig.strategy_id, pos.strategy_id);
    EXPECT_EQ(sig.strategy_name, pos.strategy_name);
    EXPECT_EQ(sig.portfolio_id, pos.portfolio_id);
}

TEST_F(LiveResultsManagerKeyTest, EmptyStrategyNameFallsBackToStrategyIdForFutures) {
    // Inverse pin: the futures runners never set strategy_name, and their existing rows
    // are keyed (strategy_id, strategy_id, portfolio_id). FIX-0 must not move them.
    auto mgr = make_mgr("LIVE_TREND_FOLLOWING", "BASE_PORTFOLIO", "");
    mgr->set_positions({make_pos("ES", 5.0)});
    ASSERT_TRUE(mgr->save_positions_snapshot(date_at(2026, 3, 15)).is_ok());

    auto key = db_->last_key("store_positions");
    EXPECT_EQ(key.strategy_id, "LIVE_TREND_FOLLOWING");
    EXPECT_EQ(key.strategy_name, "LIVE_TREND_FOLLOWING");
    EXPECT_EQ(key.portfolio_id, "BASE_PORTFOLIO");
    EXPECT_EQ(mgr->get_strategy_name(), "LIVE_TREND_FOLLOWING");
}

TEST_F(LiveResultsManagerKeyTest, DefaultedStrategyNameArgPreservesFuturesKey) {
    // Same pin via the 4-arg call shape the futures code actually compiles against.
    LiveResultsManager mgr(db_, /*store_enabled=*/true, "LIVE_TREND_FOLLOWING",
                           "BASE_PORTFOLIO");
    EXPECT_EQ(mgr.get_strategy_name(), "LIVE_TREND_FOLLOWING");
}

// ===================== F-J: save_all_results failure propagation =====================

TEST_F(LiveResultsManagerKeyTest, SaveAllResultsReportsAFailedTableInsteadOfExitingClean) {
    auto mgr = make_mgr("LIVE_EQUITY_MEAN_REVERSION", "EQUITY_PORTFOLIO",
                        "EQUITY_MEAN_REVERSION");
    mgr->set_positions({make_pos("AAPL", 5.0)});
    mgr->set_metrics({{"total_return", 0.05}}, {{"trades", 1}});
    mgr->set_config(nlohmann::json{});
    db_->fail_on_call("store_live_results_complete");

    auto r = mgr->save_all_results("RUN_1", date_at(2026, 3, 15));
    // Pre-F-J every per-table error was logged and swallowed, so this returned ok and the
    // runner exited 0 with trading.live_results missing.
    ASSERT_TRUE(r.is_error());
    EXPECT_EQ(r.error()->code(), ErrorCode::DATABASE_ERROR);
    EXPECT_NE(std::string(r.error()->what()).find("live_results"), std::string::npos);
}

TEST_F(LiveResultsManagerKeyTest, SaveAllResultsStillAttemptsLaterTablesAfterAFailure) {
    auto mgr = make_mgr("LIVE_EQUITY_MEAN_REVERSION", "EQUITY_PORTFOLIO",
                        "EQUITY_MEAN_REVERSION");
    mgr->set_positions({make_pos("AAPL", 5.0)});
    mgr->set_metrics({{"total_return", 0.05}}, {});
    mgr->set_config(nlohmann::json{});
    mgr->set_equity(1'050'000.0);
    db_->fail_on_call("store_live_results_complete");
    db_->reset_call_counts();

    auto r = mgr->save_all_results("RUN_1", date_at(2026, 3, 15));
    EXPECT_TRUE(r.is_error());
    // Failing table 5 must not cost us table 6: reporting the failure is not aborting.
    EXPECT_EQ(db_->call_count("store_trading_equity_curve"), 1);
    EXPECT_GT(db_->call_count("store_positions"), 0);
}

TEST_F(LiveResultsManagerKeyTest, SaveAllResultsSucceedsWhenEveryTableWrites) {
    auto mgr = make_mgr("LIVE_EQUITY_MEAN_REVERSION", "EQUITY_PORTFOLIO",
                        "EQUITY_MEAN_REVERSION");
    mgr->set_positions({make_pos("AAPL", 5.0)});
    mgr->set_executions({make_exec("AAPL", 5.0, 190.0)});
    mgr->set_signals({{"AAPL", 0.5}});
    mgr->set_metrics({{"total_return", 0.05}}, {});
    mgr->set_config(nlohmann::json{});
    mgr->set_equity(1'050'000.0);

    EXPECT_TRUE(mgr->save_all_results("RUN_1", date_at(2026, 3, 15)).is_ok());
}

// QT E1: with no book set the manager names system on every write and delete (byte-identical to
// before, where the book was implicit); set to qt it names qt on every one of them, the stale-data
// deletes included, so a desk save never clears or overwrites the system book.
TEST_F(LiveResultsManagerTest, EveryWriteAndDeleteNamesTheManagersBook) {
    const char* ops[] = {"store_positions", "store_executions", "store_live_results_complete",
                         "delete_live_results", "delete_live_equity_curve",
                         "delete_stale_executions", "update_live_results",
                         "update_live_equity_curve"};
    auto run = [&](LiveResultsManager& m) {
        m.set_positions({make_pos("ES.v.0", 2.0)});
        m.set_executions({make_exec("ES.v.0", 2.0, 100.0)});
        m.set_metrics({{"total_pnl", 1.0}});
        m.set_equity(500000.0);
        ASSERT_TRUE(m.save_all_results("run", date_at(2026, 4, 24)).is_ok());
        ASSERT_TRUE(m.update_live_results(date_at(2026, 4, 24), {{"total_pnl", 2.0}}).is_ok());
        ASSERT_TRUE(m.update_equity_curve(date_at(2026, 4, 24), 500001.0).is_ok());
    };

    EXPECT_EQ(mgr_->get_book(), "system");
    run(*mgr_);
    for (const char* op : ops) EXPECT_EQ(db_->last_book(op), "system") << op;
    EXPECT_EQ(db_->last_equity_curve_stream(), "system");

    LiveResultsManager qt(db_, true, "STRAT_X", "PORT_Y");
    ASSERT_TRUE(qt.set_book("qt").is_ok());
    run(qt);
    for (const char* op : ops) EXPECT_EQ(db_->last_book(op), "qt") << op;
    EXPECT_EQ(db_->last_equity_curve_stream(), "qt");
}

TEST_F(LiveResultsManagerTest, SetBookRefusesAnythingButTheThreeBooks) {
    for (const char* bad : {"", "desk", "QT", "qt "}) {
        EXPECT_TRUE(mgr_->set_book(bad).is_error()) << bad;
        EXPECT_EQ(mgr_->get_book(), "system") << "a refused book must leave the book unchanged";
    }
    for (const char* ok : {"qt_proposal", "qt", "system"}) {
        EXPECT_TRUE(mgr_->set_book(ok).is_ok()) << ok;
        EXPECT_EQ(mgr_->get_book(), ok);
    }
}
