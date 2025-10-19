// src/storage/results_manager_base.cpp
// Base implementation for storage managers

#include "trade_ngin/storage/results_manager_base.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/core/logger.hpp"

namespace trade_ngin {

ResultsManagerBase::ResultsManagerBase(std::shared_ptr<PostgresDatabase> db,
                                     bool store_enabled,
                                     const std::string& schema,
                                     const std::string& strategy_id)
    : db_(db),
      store_enabled_(store_enabled),
      schema_(schema),
      strategy_id_(strategy_id),
      component_id_("ResultsManager_" + schema) {

    INFO("Initialized " + component_id_ + " for strategy: " + strategy_id +
         ", storage " + (store_enabled ? "enabled" : "disabled"));
}

Result<void> ResultsManagerBase::validate_database_connection() const {
    if (!db_) {
        return make_error<void>(ErrorCode::DATABASE_ERROR,
                               "Database pointer is null", component_id_);
    }

    if (!db_->is_connected()) {
        return make_error<void>(ErrorCode::DATABASE_ERROR,
                               "Database is not connected", component_id_);
    }

    return Result<void>();
}

Result<void> ResultsManagerBase::validate_storage_enabled() const {
    if (!store_enabled_) {
        DEBUG("Storage is disabled for " + component_id_);
        return make_error<void>(ErrorCode::INVALID_DATA,
                               "Storage is disabled", component_id_);
    }
    return Result<void>();
}

Result<void> ResultsManagerBase::save_positions(const std::vector<Position>& positions,
                                               const std::string& run_id,
                                               const Timestamp& date) {
    // Validate preconditions
    auto validation = validate_storage_enabled();
    if (validation.is_error()) {
        return Result<void>();  // Storage disabled is not an error, just skip
    }

    validation = validate_database_connection();
    if (validation.is_error()) {
        return validation;
    }

    if (positions.empty()) {
        DEBUG("No positions to store for " + run_id);
        return Result<void>();
    }

    // Determine table based on schema
    std::string table_name = schema_ + ".positions";
    if (schema_ == "backtest") {
        table_name = schema_ + ".final_positions";
    }

    INFO("Storing " + std::to_string(positions.size()) +
         " positions to " + table_name + " for run_id: " + run_id);

    // Use appropriate storage method based on schema
    if (schema_ == "backtest") {
        return db_->store_backtest_positions(positions, run_id, table_name);
    } else {
        // For live trading, use regular store_positions
        return db_->store_positions(positions, strategy_id_, table_name);
    }
}

Result<void> ResultsManagerBase::save_executions(const std::vector<ExecutionReport>& executions,
                                                const std::string& run_id,
                                                const Timestamp& date) {
    // Validate preconditions
    auto validation = validate_storage_enabled();
    if (validation.is_error()) {
        return Result<void>();  // Storage disabled is not an error, just skip
    }

    validation = validate_database_connection();
    if (validation.is_error()) {
        return validation;
    }

    if (executions.empty()) {
        DEBUG("No executions to store for " + run_id);
        return Result<void>();
    }

    // Determine table based on schema
    std::string table_name = schema_ + ".executions";

    INFO("Storing " + std::to_string(executions.size()) +
         " executions to " + table_name);

    // Use appropriate storage method based on schema
    if (schema_ == "backtest") {
        return db_->store_backtest_executions(executions, run_id, table_name);
    } else {
        return db_->store_executions(executions, table_name);
    }
}

Result<void> ResultsManagerBase::save_signals(const std::unordered_map<std::string, double>& signals,
                                             const std::string& run_id,
                                             const Timestamp& date) {
    // Validate preconditions
    auto validation = validate_storage_enabled();
    if (validation.is_error()) {
        return Result<void>();  // Storage disabled is not an error, just skip
    }

    validation = validate_database_connection();
    if (validation.is_error()) {
        return validation;
    }

    if (signals.empty()) {
        DEBUG("No signals to store for " + run_id);
        return Result<void>();
    }

    // Determine table based on schema
    std::string table_name = schema_ + ".signals";

    INFO("Storing " + std::to_string(signals.size()) +
         " signals to " + table_name);

    // Use appropriate storage method based on schema
    if (schema_ == "backtest") {
        return db_->store_backtest_signals(signals, strategy_id_, run_id, date, table_name);
    } else {
        return db_->store_signals(signals, strategy_id_, date, table_name);
    }
}

} // namespace trade_ngin