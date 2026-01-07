// include/trade_ngin/storage/results_manager_base.hpp
// Base class for unified storage architecture
#pragma once

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/postgres_database.hpp"

namespace trade_ngin {

// Forward declarations
class Position;
class ExecutionReport;

/**
 * @brief Base class for all results storage managers
 *
 * Provides unified interface for storing trading results to database.
 * Handles both backtest and live trading storage with a single control flag.
 */
class ResultsManagerBase {
protected:
    std::shared_ptr<PostgresDatabase> db_;
    bool store_enabled_;  // Single control flag (replaces save_positions, save_signals, etc.)
    std::string schema_;  // "backtest" or "trading"
    std::string strategy_id_;
    std::string portfolio_id_;
    std::string component_id_;

    // Common validation methods
    Result<void> validate_database_connection() const;
    Result<void> validate_storage_enabled() const;

public:
    ResultsManagerBase(std::shared_ptr<PostgresDatabase> db, bool store_enabled,
                       const std::string& schema, const std::string& strategy_id,
                       const std::string& portfolio_id = "BASE_PORTFOLIO");

    virtual ~ResultsManagerBase() = default;

    // Pure virtual methods that must be implemented by derived classes
    virtual Result<void> save_all_results(const std::string& run_id, const Timestamp& date) = 0;

    // Common getters/setters
    void set_storage_enabled(bool enabled) {
        store_enabled_ = enabled;
    }
    bool is_storage_enabled() const {
        return store_enabled_;
    }
    std::string get_schema() const {
        return schema_;
    }
    std::string get_strategy_id() const {
        return strategy_id_;
    }
    std::string get_portfolio_id() const {
        return portfolio_id_;
    }

    // Common storage operations (can be overridden if needed)
    virtual Result<void> save_positions(const std::vector<Position>& positions,
                                        const std::string& run_id, const Timestamp& date);

    virtual Result<void> save_executions(const std::vector<ExecutionReport>& executions,
                                         const std::string& run_id, const Timestamp& date);

    virtual Result<void> save_signals(const std::unordered_map<std::string, double>& signals,
                                      const std::string& run_id, const Timestamp& date);
};

}  // namespace trade_ngin