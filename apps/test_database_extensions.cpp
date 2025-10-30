// Test program for database extensions
// This verifies the new database methods work correctly

#include <iostream>
#include <chrono>
#include <unordered_map>
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/data/database_pooling.hpp"
#include "trade_ngin/core/types.hpp"

using namespace trade_ngin;

int main() {
    std::cout << "Testing Database Extensions...\n" << std::endl;

    // Initialize database pool with connection string
    std::string conn_string = "host=3.140.200.228 port=5432 dbname=algo_data user=postgres password=algogators";

    // Initialize the singleton database pool
    auto pool_result = DatabasePool::instance().initialize(conn_string, 2);
    if (pool_result.is_error()) {
        std::cerr << "Failed to initialize database pool: "
                  << pool_result.error()->what() << std::endl;
        return 1;
    }

    // Get a connection from the pool
    auto db_guard = DatabasePool::instance().acquire_connection();
    auto db = db_guard.get();

    // Test 1: Test delete_stale_executions
    std::cout << "Test 1: delete_stale_executions..." << std::endl;
    std::vector<std::string> order_ids = {"TEST_ORDER_1", "TEST_ORDER_2", "TEST_ORDER_3"};
    auto now = std::chrono::system_clock::now();

    auto delete_result = db->delete_stale_executions(order_ids, now, "trading.executions");
    if (delete_result.is_error()) {
        std::cout << "  Warning (expected if no matching records): "
                  << delete_result.error()->what() << std::endl;
    } else {
        std::cout << "  SUCCESS: delete_stale_executions completed" << std::endl;
    }

    // Test 2: Test store_backtest_summary
    std::cout << "\nTest 2: store_backtest_summary..." << std::endl;
    std::unordered_map<std::string, double> metrics = {
        {"total_return", 0.15},
        {"sharpe_ratio", 1.25},
        {"sortino_ratio", 1.45},
        {"max_drawdown", -0.08},
        {"calmar_ratio", 1.87},
        {"volatility", 0.12},
        {"total_trades", 150},
        {"win_rate", 0.55},
        {"profit_factor", 1.8}
    };

    std::string test_run_id = "TEST_RUN_" + std::to_string(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    auto summary_result = db->store_backtest_summary(
        test_run_id, now - std::chrono::hours(24*30), now, metrics, "backtest.results");

    if (summary_result.is_error()) {
        std::cout << "  ERROR: " << summary_result.error()->what() << std::endl;
    } else {
        std::cout << "  SUCCESS: store_backtest_summary completed" << std::endl;
    }

    // Test 3: Test store_backtest_equity_curve_batch
    std::cout << "\nTest 3: store_backtest_equity_curve_batch..." << std::endl;
    std::vector<std::pair<Timestamp, double>> equity_points;
    double equity = 1000000.0;
    for (int i = 0; i < 10; i++) {
        auto timestamp = now - std::chrono::hours(24 * (10-i));
        equity *= (1.0 + (rand() % 100 - 50) / 10000.0);  // Random small changes
        equity_points.push_back({timestamp, equity});
    }

    auto equity_result = db->store_backtest_equity_curve_batch(
        test_run_id, equity_points, "backtest.equity_curve");

    if (equity_result.is_error()) {
        std::cout << "  ERROR: " << equity_result.error()->what() << std::endl;
    } else {
        std::cout << "  SUCCESS: store_backtest_equity_curve_batch completed" << std::endl;
    }

    // Test 4: Test update_live_results
    std::cout << "\nTest 4: update_live_results..." << std::endl;
    std::unordered_map<std::string, double> updates = {
        {"daily_pnl", 5000.0},
        {"total_pnl", 50000.0},
        {"current_portfolio_value", 1050000.0}
    };

    auto update_result = db->update_live_results(
        "LIVE_TREND_FOLLOWING", now, updates, "trading.live_results");

    if (update_result.is_error()) {
        std::cout << "  Warning (expected if no matching record): "
                  << update_result.error()->what() << std::endl;
    } else {
        std::cout << "  SUCCESS: update_live_results completed" << std::endl;
    }

    // Test 5: Test store_backtest_positions
    std::cout << "\nTest 5: store_backtest_positions..." << std::endl;
    std::vector<Position> positions;

    // Create positions with the proper constructor
    Position pos1;
    pos1.symbol = "ES.v.0";
    pos1.quantity = Decimal(10.0);
    pos1.average_price = Decimal(4500.0);
    pos1.unrealized_pnl = Decimal(0.0);
    pos1.realized_pnl = Decimal(0.0);
    pos1.last_update = now;
    positions.push_back(pos1);

    Position pos2;
    pos2.symbol = "NQ.v.0";
    pos2.quantity = Decimal(-5.0);
    pos2.average_price = Decimal(15000.0);
    pos2.unrealized_pnl = Decimal(0.0);
    pos2.realized_pnl = Decimal(0.0);
    pos2.last_update = now;
    positions.push_back(pos2);

    auto positions_result = db->store_backtest_positions(
        positions, test_run_id, "backtest.final_positions");

    if (positions_result.is_error()) {
        std::cout << "  ERROR: " << positions_result.error()->what() << std::endl;
    } else {
        std::cout << "  SUCCESS: store_backtest_positions completed" << std::endl;
    }

    std::cout << "\n========================================" << std::endl;
    std::cout << "Database Extensions Testing Complete!" << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}