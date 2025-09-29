// include/trade_ngin/data/postgres_database.hpp

#pragma once

#include <arrow/api.h>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <pqxx/pqxx>
#include <string>
#include <vector>
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/database_interface.hpp"

namespace trade_ngin {

/**
 * @brief Database interface for PostgreSQL
 */
class PostgresDatabase : public DatabaseInterface {
public:
    /**
     * @brief Constructor
     * @param connection_string Connection string for PostgreSQL
     */
    explicit PostgresDatabase(std::string connection_string);

    /**
     * @brief Destructor
     */
    ~PostgresDatabase() override;

    // Delete copy and move operations
    PostgresDatabase(const PostgresDatabase&) = delete;
    PostgresDatabase& operator=(const PostgresDatabase&) = delete;
    PostgresDatabase(PostgresDatabase&&) = delete;
    PostgresDatabase& operator=(PostgresDatabase&&) = delete;

    /**
     * @brief Connect to the database
     * @return Result indicating success or failure
     */
    Result<void> connect() override;

    /**
     * @brief Disconnect from the database
     */
    void disconnect() override;

    /**
     * @brief Check if the database connection is active
     * @return True if connected, false otherwise
     */
    bool is_connected() const override;

    /**
     * @brief Get market data for a list of symbols
     * @param symbols List of symbols to retrieve
     * @param start_date Start date for data retrieval
     * @param end_date End date for data retrieval
     * @param asset_class Asset class for the data
     * @param freq Data frequency
     * @param data_type Type of data to retrieve
     * @return Result containing the market data
     */
    Result<std::shared_ptr<arrow::Table>> get_market_data(
        const std::vector<std::string>& symbols, const Timestamp& start_date,
        const Timestamp& end_date, AssetClass asset_class,
        DataFrequency freq = DataFrequency::DAILY, const std::string& data_type = "ohlcv") override;

    /**
     * @brief Get latest market prices for symbols
     * @param symbols Vector of symbols to get prices for
     * @param asset_class Asset class of the symbols
     * @param freq Data frequency
     * @param data_type Type of data (ohlcv, etc.)
     * @return Result containing map of symbol to latest price
     */
    Result<std::unordered_map<std::string, double>> get_latest_prices(
        const std::vector<std::string>& symbols, AssetClass asset_class,
        DataFrequency freq = DataFrequency::DAILY, const std::string& data_type = "ohlcv") override;

    /**
     * @brief Load positions by date and strategy
     * @param strategy_id Strategy identifier
     * @param date Date to load positions for
     * @param table_name Name of the positions table
     * @return Result containing map of symbol to position
     */
    Result<std::unordered_map<std::string, Position>> load_positions_by_date(
        const std::string& strategy_id, const Timestamp& date,
        const std::string& table_name = "trading.positions") override;

    /**
     * @brief Store execution reports in the database
     * @param executions List of execution reports
     * @param table_name Name of the table to store data
     * @return Result indicating success or failure
     */
    Result<void> store_executions(const std::vector<ExecutionReport>& executions,
                                  const std::string& table_name) override;

    /**
     * @brief Store positions in the database
     * @param positions List of positions
     * @param table_name Name of the table to store data
     * @return Result indicating success or failure
     */
    Result<void> store_positions(const std::vector<Position>& positions,
                                 const std::string& strategy_id,
                                 const std::string& table_name) override;

    /**
     * @brief Store signals in the database
     * @param signals Map of signals by symbol
     * @param strategy_id ID of the strategy generating the signals
     * @param timestamp Timestamp for the signals
     * @param table_name Name of the table to store data
     * @return Result indicating success or failure
     */
    Result<void> store_signals(const std::unordered_map<std::string, double>& signals,
                               const std::string& strategy_id, const Timestamp& timestamp,
                               const std::string& table_name) override;

    /**
     * @brief Get a list of symbols from the database
     * @param asset_class Asset class to retrieve
     * @param freq Data frequency
     * @param data_type Type of data to retrieve
     * @return Result containing the list of symbols
     */
    Result<std::vector<std::string>> get_symbols(AssetClass asset_class,
                                                 DataFrequency freq = DataFrequency::DAILY,
                                                 const std::string& data_type = "ohlcv") override;

    /**
     * @brief Execute a query and return the result as an Arrow table
     * @param query SQL query to execute
     * @return Result containing the Arrow table
     */
    Result<std::shared_ptr<arrow::Table>> execute_query(const std::string& query) override;

    /**
     * @brief Execute a direct SQL query without Arrow table conversion
     * @param query SQL query to execute
     * @return Result indicating success or failure
     */
    Result<void> execute_direct_query(const std::string& query);

    // ============================================================================
    // BACKTEST DATA STORAGE METHODS
    // ============================================================================

    /**
     * @brief Store backtest execution data
     * @param executions Vector of execution reports
     * @param run_id Backtest run identifier
     * @param table_name Name of the table to insert into
     * @return Result indicating success or failure
     */
    Result<void> store_backtest_executions(const std::vector<ExecutionReport>& executions,
                                           const std::string& run_id,
                                           const std::string& table_name = "backtest.executions") override;

    /**
     * @brief Store backtest signals
     * @param signals Map of symbol to signal value
     * @param strategy_id ID of the strategy generating signals
     * @param run_id Backtest run identifier
     * @param timestamp Timestamp of signals
     * @param table_name Name of the table to insert into
     * @return Result indicating success or failure
     */
    Result<void> store_backtest_signals(const std::unordered_map<std::string, double>& signals,
                                        const std::string& strategy_id, const std::string& run_id,
                                        const Timestamp& timestamp,
                                        const std::string& table_name = "backtest.signals") override;

    /**
     * @brief Store backtest run metadata
     * @param run_id Backtest run identifier
     * @param name Run name
     * @param description Run description
     * @param start_date Start date
     * @param end_date End date
     * @param hyperparameters JSON configuration
     * @param table_name Name of the table to insert into
     * @return Result indicating success or failure
     */
    Result<void> store_backtest_metadata(const std::string& run_id, const std::string& name,
                                         const std::string& description, const Timestamp& start_date,
                                         const Timestamp& end_date, const nlohmann::json& hyperparameters,
                                         const std::string& table_name = "backtest.run_metadata") override;

    // ============================================================================
    // LIVE TRADING DATA STORAGE METHODS
    // ============================================================================

    /**
     * @brief Store live trading daily results
     * @param strategy_id Strategy identifier
     * @param date Trading date
     * @param total_return Total return for the day
     * @param sharpe_ratio Sharpe ratio
     * @param sortino_ratio Sortino ratio
     * @param max_drawdown Maximum drawdown
     * @param calmar_ratio Calmar ratio
     * @param volatility Volatility
     * @param total_trades Total number of trades
     * @param win_rate Win rate
     * @param profit_factor Profit factor
     * @param avg_win Average win
     * @param avg_loss Average loss
     * @param max_win Maximum win
     * @param max_loss Maximum loss
     * @param avg_holding_period Average holding period
     * @param var_95 Value at Risk (95%)
     * @param cvar_95 Conditional Value at Risk (95%)
     * @param beta Beta
     * @param correlation Correlation
     * @param downside_volatility Downside volatility
     * @param config Additional configuration JSON
     * @param table_name Name of the table to insert into
     * @return Result indicating success or failure
     */
    Result<void> store_trading_results(const std::string& strategy_id, const Timestamp& date,
                                       double total_return, double sharpe_ratio, double sortino_ratio,
                                       double max_drawdown, double calmar_ratio, double volatility,
                                       int total_trades, double win_rate, double profit_factor,
                                       double avg_win, double avg_loss, double max_win, double max_loss,
                                       double avg_holding_period, double var_95, double cvar_95,
                                       double beta, double correlation, double downside_volatility,
                                       const nlohmann::json& config,
                                       const std::string& table_name = "trading.results") override;

    /**
     * @brief Store live trading results with new schema
     * @param strategy_id Strategy identifier
     * @param date Trading date
     * @param total_return Total return for the day
     * @param volatility Portfolio volatility
     * @param total_pnl Total P&L
     * @param unrealized_pnl Unrealized P&L
     * @param realized_pnl Realized P&L
     * @param current_portfolio_value Current portfolio value
     * @param portfolio_var Portfolio VaR
     * @param gross_leverage Gross leverage
     * @param net_leverage Net leverage
     * @param portfolio_leverage Portfolio leverage
     * @param max_correlation Max correlation risk
     * @param jump_risk Jump risk (99th percentile)
     * @param risk_scale Risk scale factor
     * @param total_notional Total notional exposure
     * @param active_positions Number of active positions
     * @param config Strategy configuration JSON
     * @param table_name Name of the table to insert into
     * @return Result indicating success or failure
     */
    Result<void> store_live_results(const std::string& strategy_id, const Timestamp& date,
                                   double total_return, double volatility, double total_pnl,
                                   double unrealized_pnl, double realized_pnl, double current_portfolio_value,
                                   double daily_realized_pnl, double daily_unrealized_pnl,
                                   double portfolio_var, double gross_leverage, double net_leverage,
                                   double portfolio_leverage, double max_correlation, double jump_risk,
                                   double risk_scale, double gross_notional, double net_notional,
                                   int active_positions, double total_commissions,
                                   const nlohmann::json& config,
                                   const std::string& table_name = "trading.live_results") override;

    Result<std::tuple<double, double, double>> get_previous_live_aggregates(
        const std::string& strategy_id, const Timestamp& date,
        const std::string& table_name = "trading.live_results") override;

    /**
     * @brief Store live trading equity curve point
     * @param strategy_id Strategy identifier
     * @param timestamp Timestamp of the equity point
     * @param equity Equity value
     * @param table_name Name of the table to insert into
     * @return Result indicating success or failure
     */
    Result<void> store_trading_equity_curve(const std::string& strategy_id, const Timestamp& timestamp,
                                             double equity,
                                             const std::string& table_name = "trading.equity_curve") override;

    /**
     * @brief Store multiple live trading equity curve points
     * @param strategy_id Strategy identifier
     * @param equity_points Vector of timestamp-equity pairs
     * @param table_name Name of the table to insert into
     * @return Result indicating success or failure
     */
    Result<void> store_trading_equity_curve_batch(const std::string& strategy_id,
                                                  const std::vector<std::pair<Timestamp, double>>& equity_points,
                                                  const std::string& table_name = "trading.equity_curve") override;

    /**
     * @brief Get contract metadata for trading instruments
     * @return Result containing Arrow table with contract metadata
     */
    Result<std::shared_ptr<arrow::Table>> get_contract_metadata() const;

    /**
     * @brief Convert asset class to string for database queries
     * @param asset_class Asset class to convert
     * @return String representation for database queries
     */
    std::string asset_class_to_string(AssetClass asset_class) const;

    /**
     * @brief Get the connection string
     * @return Connection string
     */
    std::string get_connection_string() const {
        return connection_string_;
    }

    /**
     * @brief Get the component ID
     * @return Component ID
     */
    const std::string& get_component_id() const {
        return component_id_;
    }

    /**
     * @brief Validate table name for SQL injection prevention
     * @param table_name Table name to validate
     * @return Result indicating success or failure
     */
    Result<void> validate_table_name(const std::string& table_name) const;

private:
    std::string connection_string_;
    std::unique_ptr<pqxx::connection> connection_;
    std::mutex mutex_;
    std::string component_id_;

    /**
     * @brief Validate the database connection
     * @return Result indicating success or failure
     */
    Result<void> validate_connection() const;

    /**
     * @brief Format a timestamp as a string
     * @param ts Timestamp to format
     * @return Formatted string
     */
    std::string format_timestamp(const Timestamp& ts) const;

    /**
     * @brief Convert a Side enum to a string
     * @param side Side to convert
     * @return String representation of the side
     */
    std::string side_to_string(Side side) const;

    /**
     * @brief Execute market data query with proper parameterization
     * @param symbols List of symbols to retrieve
     * @param start_date Start date for data retrieval
     * @param end_date End date for data retrieval
     * @param asset_class Asset class for the data
     * @param freq Data frequency
     * @param data_type Type of data to retrieve
     * @param txn Database transaction
     * @return Result containing the query result
     */
    Result<pqxx::result> execute_market_data_query(const std::vector<std::string>& symbols,
                                                   const Timestamp& start_date,
                                                   const Timestamp& end_date,
                                                   AssetClass asset_class, DataFrequency freq,
                                                   const std::string& data_type,
                                                   pqxx::work& txn) const;

    /**
     * @brief Validate table name components to prevent injection
     * @param asset_class Asset class
     * @param data_type Data type
     * @param freq Data frequency
     * @return Result indicating success or failure
     */
    Result<void> validate_table_name_components(AssetClass asset_class,
                                                const std::string& data_type,
                                                DataFrequency freq) const;

    /**
     * @brief Validate symbol for SQL injection prevention
     * @param symbol Symbol to validate
     * @return Result indicating success or failure
     */
    Result<void> validate_symbol(const std::string& symbol) const;

    /**
     * @brief Validate symbols for SQL injection prevention
     * @param symbols Symbols to validate
     * @return Result indicating success or failure
     */
    Result<void> validate_symbols(const std::vector<std::string>& symbols) const;

    /**
     * @brief Validate strategy ID for SQL injection prevention
     * @param strategy_id Strategy ID to validate
     * @return Result indicating success or failure
     */
    Result<void> validate_strategy_id(const std::string& strategy_id) const;

    /**
     * @brief Validate execution report data
     * @param exec Execution report to validate
     * @return Result indicating success or failure
     */
    Result<void> validate_execution_report(const ExecutionReport& exec) const;

    /**
     * @brief Validate position data
     * @param pos Position to validate
     * @return Result indicating success or failure
     */
    Result<void> validate_position(const Position& pos) const;

    /**
     * @brief Validate signal data
     * @param symbol Symbol for signal
     * @param signal Signal value
     * @return Result indicating success or failure
     */
    Result<void> validate_signal_data(const std::string& symbol, double signal) const;

    /**
     * @brief Convert a pqxx result to an Arrow table
     * @param result pqxx result to convert
     * @return Result containing the Arrow table
     */
    Result<std::shared_ptr<arrow::Table>> convert_to_arrow_table(const pqxx::result& result) const;

    /**
     * @brief Convert contract metadata result to Arrow table
     * @param result pqxx result to convert
     * @return Result containing the Arrow table
     */
    Result<std::shared_ptr<arrow::Table>> convert_metadata_to_arrow(
        const pqxx::result& result) const;

    /**
     * @brief Get the latest data time for a given asset class and frequency
     * @param asset_class Asset class to retrieve
     * @param freq Data frequency
     * @param data_type Type of data to retrieve
     * @return Result containing the latest data time
     */
    Result<Timestamp> get_latest_data_time(AssetClass asset_class, DataFrequency freq,
                                           const std::string& data_type = "ohlcv") const;

    /**
     * @brief Get the time range for data in the database
     * @param asset_class Asset class to retrieve
     * @param freq Data frequency
     * @param data_type Type of data to retrieve
     * @return Result containing the time range
     */
    Result<std::pair<Timestamp, Timestamp>> get_data_time_range(
        AssetClass asset_class, DataFrequency freq, const std::string& data_type = "ohlcv") const;

    /**
     * @brief Get the number of data points in the database
     * @param asset_class Asset class to retrieve
     * @param freq Data frequency
     * @param symbol Symbol to retrieve
     * @param data_type Type of data to retrieve
     * @return Result containing the number of data points
     */
    Result<size_t> get_data_count(AssetClass asset_class, DataFrequency freq,
                                  const std::string& symbol,
                                  const std::string& data_type = "ohlcv") const;
};

}  // namespace trade_ngin