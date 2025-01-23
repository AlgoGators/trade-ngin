// include/trade_ngin/strategy/database_handler.hpp
#pragma once

#include "trade_ngin/data/database_interface.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/core/error.hpp"
#include <memory>

namespace trade_ngin {

/**
 * @brief Handles database operations for strategies
 */
class StrategyDatabaseHandler {
public:
    explicit StrategyDatabaseHandler(std::shared_ptr<DatabaseInterface> db)
        : db_(std::move(db)) {}

    /**
     * @brief Store execution report
     */
    Result<void> store_execution(const ExecutionReport& exec) {
        return db_->store_executions({exec}, "trading.executions");
    }

    /**
     * @brief Store multiple execution reports in batch
     */
    Result<void> store_executions(const std::vector<ExecutionReport>& execs) {
        return db_->store_executions(execs, "trading.executions");
    }

    /**
     * @brief Update positions for a strategy
     */
    Result<void> update_positions(const std::unordered_map<std::string, Position>& positions) {
        std::vector<Position> pos_vec;
        pos_vec.reserve(positions.size());
        for (const auto& [symbol, pos] : positions) {
            pos_vec.push_back(pos);
        }
        return db_->store_positions(pos_vec, "trading.positions");
    }

    /**
     * @brief Store strategy signals
     */
    Result<void> store_signals(
        const std::string& strategy_id,
        const std::unordered_map<std::string, double>& signals,
        const Timestamp& timestamp) {
        return db_->store_signals(signals, strategy_id, timestamp, "trading.signals");
    }

    /**
     * @brief Get historical positions for a strategy
     */
    Result<std::vector<Position>> get_historical_positions(
        const std::string& strategy_id,
        const Timestamp& start_date,
        const Timestamp& end_date) {
        
        std::string query = 
            "SELECT symbol, quantity, average_price, unrealized_pnl, "
            "realized_pnl, last_update "
            "FROM trading.positions "
            "WHERE last_update BETWEEN $1 AND $2 "
            "ORDER BY last_update";

        auto result = db_->execute_query(query);
        if (result.is_error()) return make_error<std::vector<Position>>(
            result.error()->code(),
            result.error()->what()
        );

        // Convert Arrow table to positions
        std::vector<Position> positions;
        auto table = result.value();
        
        // Implementation of conversion...
        
        return Result<std::vector<Position>>(positions);
    }

    /**
     * @brief Get historical signals for a strategy
     */
    Result<std::vector<std::pair<std::string, double>>> get_historical_signals(
        const std::string& strategy_id,
        const std::string& symbol,
        const Timestamp& start_date,
        const Timestamp& end_date) {
        
        std::string query = 
            "SELECT timestamp, signal_value "
            "FROM trading.signals "
            "WHERE strategy_id = $1 "
            "AND symbol = $2 "
            "AND timestamp BETWEEN $3 AND $4 "
            "ORDER BY timestamp";

        auto result = db_->execute_query(query);
        if (result.is_error()) return make_error<std::vector<std::pair<std::string, double>>>(
            result.error()->code(),
            result.error()->what()
        );

        // Convert Arrow table to signal pairs
        std::vector<std::pair<std::string, double>> signals;
        auto table = result.value();
        
        // Implementation of conversion...
        
        return Result<std::vector<std::pair<std::string, double>>>(signals);
    }

private:
    std::shared_ptr<DatabaseInterface> db_;
};

} // namespace trade_ngin