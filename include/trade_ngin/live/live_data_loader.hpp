// include/trade_ngin/live/live_data_loader.hpp
// Data loading component for live trading - encapsulates all SELECT queries

#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "trade_ngin/core/error.hpp"  // Contains Result<T>
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/postgres_database.hpp"

namespace trade_ngin {

// Forward declarations
struct Position;

/**
 * @brief Structure representing a row from trading.live_results table
 */
struct LiveResultsRow {
    double daily_pnl = 0.0;
    double total_pnl = 0.0;
    double daily_realized_pnl = 0.0;
    double daily_unrealized_pnl = 0.0;
    double daily_return = 0.0;
    double total_cumulative_return = 0.0;  // Total return since inception (non-annualized)
    double total_annualized_return = 0.0;  // Annualized return
    double current_portfolio_value = 0.0;
    double portfolio_leverage = 0.0;
    double equity_to_margin_ratio = 0.0;
    double gross_notional = 0.0;
    double margin_posted = 0.0;
    double cash_available = 0.0;
    double daily_commissions = 0.0;
    Timestamp date;
    std::string strategy_id;

    // Additional metrics
    double sharpe_ratio = 0.0;
    double sortino_ratio = 0.0;
    double max_drawdown = 0.0;
    double volatility = 0.0;
    int active_positions = 0;
    int winning_trades = 0;
    int losing_trades = 0;
};

/**
 * @brief Structure for margin-related metrics
 */
struct MarginMetrics {
    double portfolio_leverage = 0.0;
    double equity_to_margin_ratio = 0.0;
    double gross_notional = 0.0;
    double margin_posted = 0.0;
    double margin_cushion = 0.0;
    bool valid = false;  // Indicates if data was found
};

/**
 * @brief Structure for previous day's data
 */
struct PreviousDayData {
    double portfolio_value = 0.0;
    double total_pnl = 0.0;
    double daily_pnl = 0.0;
    double daily_commissions = 0.0;
    Timestamp date;
    bool exists = false;  // false if no previous day found
};

/**
 * @brief LiveDataLoader - Encapsulates all data retrieval operations for live trading
 *
 * This class replaces raw SQL SELECT queries with type-safe methods.
 * All methods return Result<T> for proper error handling.
 * Designed to be reusable across different live trading strategies.
 */
class LiveDataLoader {
private:
    std::shared_ptr<PostgresDatabase> db_;
    std::string schema_;  // "trading" or "backtest"

    // Helper method to check database connection
    Result<void> validate_connection() const;

public:
    /**
     * @brief Construct a new LiveDataLoader
     * @param db Shared database connection
     * @param schema Database schema to use (default: "trading")
     */
    LiveDataLoader(std::shared_ptr<PostgresDatabase> db, const std::string& schema = "trading");

    ~LiveDataLoader() = default;

    // ========== Portfolio Value Methods ==========

    /**
     * @brief Load the previous trading day's portfolio value
     * @param strategy_id Strategy identifier
     * @param date Current date (will find previous trading day)
     * @return Previous portfolio value or error
     */
    Result<double> load_previous_portfolio_value(const std::string& strategy_id,
                                                 const std::string& portfolio_id,
                                                 const Timestamp& date);

    /**
     * @brief Load portfolio value for a specific date
     * @param strategy_id Strategy identifier
     * @param date Target date
     * @return Portfolio value or error
     */
    Result<double> load_portfolio_value(const std::string& strategy_id,
                                        const std::string& portfolio_id, const Timestamp& date);

    // ========== Live Results Methods ==========

    /**
     * @brief Load complete live results row for a date
     * @param strategy_id Strategy identifier
     * @param date Target date
     * @return LiveResultsRow or error
     */
    Result<LiveResultsRow> load_live_results(const std::string& strategy_id,
                                             const std::string& portfolio_id,
                                             const Timestamp& date);

    /**
     * @brief Load previous day's complete data
     * @param strategy_id Strategy identifier
     * @param date Current date (will find previous trading day)
     * @return PreviousDayData or error
     */
    Result<PreviousDayData> load_previous_day_data(const std::string& strategy_id,
                                                   const std::string& portfolio_id,
                                                   const Timestamp& date);

    /**
     * @brief Check if live results exist for a date
     * @param strategy_id Strategy identifier
     * @param date Target date
     * @return true if results exist, false otherwise
     */
    Result<bool> has_live_results(const std::string& strategy_id, const std::string& portfolio_id,
                                  const Timestamp& date);

    /**
     * @brief Get count of live results rows for strategy
     * @param strategy_id Strategy identifier
     * @return Row count or error
     */
    Result<int> get_live_results_count(const std::string& strategy_id,
                                       const std::string& portfolio_id = "BASE_PORTFOLIO");

    // ========== Position Methods ==========

    /**
     * @brief Load positions for a specific date
     * @param strategy_id Strategy identifier
     * @param date Target date
     * @return Vector of positions or error
     */
    Result<std::vector<Position>> load_positions(const std::string& strategy_id,
                                                 const std::string& portfolio_id,
                                                 const Timestamp& date);

    /**
     * @brief Load positions for CSV export (with specific fields)
     * @param strategy_id Strategy identifier
     * @param date Target date
     * @return Positions suitable for CSV export
     */
    Result<std::vector<Position>> load_positions_for_export(const std::string& strategy_id,
                                                            const std::string& portfolio_id,
                                                            const Timestamp& date);

    // ========== Commission Methods ==========

    /**
     * @brief Load commissions grouped by symbol for a date
     * @param date Target date
     * @return Map of symbol to total commission or error
     */
    Result<std::unordered_map<std::string, double>> load_commissions_by_symbol(
        const std::string& portfolio_id, const Timestamp& date);

    /**
     * @brief Load total daily commissions for a strategy
     * @param strategy_id Strategy identifier
     * @param date Target date
     * @return Total commissions or error
     */
    Result<double> load_daily_commissions(const std::string& strategy_id,
                                          const std::string& portfolio_id, const Timestamp& date);

    // ========== Margin and Risk Methods ==========

    /**
     * @brief Load margin-related metrics
     * @param strategy_id Strategy identifier
     * @param date Target date
     * @return MarginMetrics or error
     */
    Result<MarginMetrics> load_margin_metrics(const std::string& strategy_id,
                                              const std::string& portfolio_id,
                                              const Timestamp& date);

    // ========== Email/Reporting Methods ==========

    /**
     * @brief Load daily metrics formatted for email
     * @param strategy_id Strategy identifier
     * @param date Target date
     * @return Map of metric name to value
     */
    Result<std::unordered_map<std::string, double>> load_daily_metrics_for_email(
        const std::string& strategy_id, const std::string& portfolio_id, const Timestamp& date);

    // ========== Utility Methods ==========

    /**
     * @brief Get the schema being used
     * @return Current schema name
     */
    const std::string& get_schema() const {
        return schema_;
    }

    /**
     * @brief Check if database connection is valid
     * @return true if connected, false otherwise
     */
    bool is_connected() const;
};

}  // namespace trade_ngin