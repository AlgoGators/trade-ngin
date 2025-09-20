// include/trade_ngin/data/database_interface.hpp

#pragma once

#include <arrow/api.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"

namespace trade_ngin {

/**
 * @brief Abstract interface for database operations
 * Defines the contract that any database implementation must fulfill
 */
class DatabaseInterface {
public:
    virtual ~DatabaseInterface() = default;

    /**
     * @brief Connect to the database
     * @return Result indicating success or failure
     */
    virtual Result<void> connect() = 0;

    /**
     * @brief Disconnect from the database
     */
    virtual void disconnect() = 0;

    /**
     * @brief Check if connected to the database
     * @return True if connected
     */
    virtual bool is_connected() const = 0;

    /**
     * @brief Get market data for specified symbols and date range
     * @param symbols List of symbols to fetch
     * @param start_date Start date for data
     * @param end_date End date for data
     * @param table_name Name of the table to query
     * @return Result containing Arrow table with market data
     */
    virtual Result<std::shared_ptr<arrow::Table>> get_market_data(
        const std::vector<std::string>& symbols, const Timestamp& start_date,
        const Timestamp& end_date, AssetClass asset_class,
        DataFrequency freq = DataFrequency::DAILY, const std::string& table_name = "ohlcv") = 0;

    /**
     * @brief Store trade execution data
     * @param executions Vector of execution reports
     * @param table_name Name of the table to insert into
     * @return Result indicating success or failure
     */
    virtual Result<void> store_executions(const std::vector<ExecutionReport>& executions,
                                          const std::string& table_name = "trading.executions") = 0;

    /**
     * @brief Store position data
     * @param positions Vector of positions
     * @param table_name Name of the table to insert into
     * @return Result indicating success or failure
     */
    virtual Result<void> store_positions(const std::vector<Position>& positions,
                                         const std::string& strategy_id,
                                         const std::string& table_name = "trading.positions") = 0;

    /**
     * @brief Get latest market prices for symbols
     * @param symbols Vector of symbols to get prices for
     * @param asset_class Asset class of the symbols
     * @param freq Data frequency
     * @param data_type Type of data (ohlcv, etc.)
     * @return Result containing map of symbol to latest price
     */
    virtual Result<std::unordered_map<std::string, double>> get_latest_prices(
        const std::vector<std::string>& symbols, AssetClass asset_class,
        DataFrequency freq = DataFrequency::DAILY, const std::string& data_type = "ohlcv") = 0;

    /**
     * @brief Load positions by date and strategy
     * @param strategy_id Strategy identifier
     * @param date Date to load positions for
     * @param table_name Name of the positions table
     * @return Result containing map of symbol to position
     */
    virtual Result<std::unordered_map<std::string, Position>> load_positions_by_date(
        const std::string& strategy_id, const Timestamp& date,
        const std::string& table_name = "trading.positions") = 0;

    /**
     * @brief Store strategy signals
     * @param signals Map of symbol to signal value
     * @param strategy_id ID of the strategy generating signals
     * @param timestamp Timestamp of signals
     * @param table_name Name of the table to insert into
     * @return Result indicating success or failure
     */
    virtual Result<void> store_signals(const std::unordered_map<std::string, double>& signals,
                                       const std::string& strategy_id, const Timestamp& timestamp,
                                       const std::string& table_name = "trading.signals") = 0;

    /**
     * @brief Get list of available symbols
     * @param table_name Name of the table to query
     * @return Result containing vector of symbols
     */
    virtual Result<std::vector<std::string>> get_symbols(
        AssetClass asset_class, DataFrequency freq = DataFrequency::DAILY,
        const std::string& table_name = "ohlcv") = 0;

    /**
     * @brief Execute a custom SQL query
     * @param query SQL query to execute
     * @return Result containing Arrow table with query results
     */
    virtual Result<std::shared_ptr<arrow::Table>> execute_query(const std::string& query) = 0;

    // ============================================================================
    // NEW METHODS FOR BACKTEST DATA STORAGE
    // ============================================================================

    /**
     * @brief Store backtest execution data
     * @param executions Vector of execution reports
     * @param run_id Backtest run identifier
     * @param table_name Name of the table to insert into
     * @return Result indicating success or failure
     */
    virtual Result<void> store_backtest_executions(
        const std::vector<ExecutionReport>& executions, const std::string& run_id,
        const std::string& table_name = "backtest.executions") = 0;

    /**
     * @brief Store backtest signals
     * @param signals Map of symbol to signal value
     * @param strategy_id ID of the strategy generating signals
     * @param run_id Backtest run identifier
     * @param timestamp Timestamp of signals
     * @param table_name Name of the table to insert into
     * @return Result indicating success or failure
     */
    virtual Result<void> store_backtest_signals(
        const std::unordered_map<std::string, double>& signals, const std::string& strategy_id,
        const std::string& run_id, const Timestamp& timestamp,
        const std::string& table_name = "backtest.signals") = 0;

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
    virtual Result<void> store_backtest_metadata(
        const std::string& run_id, const std::string& name, const std::string& description,
        const Timestamp& start_date, const Timestamp& end_date,
        const nlohmann::json& hyperparameters,
        const std::string& table_name = "backtest.run_metadata") = 0;

    // ============================================================================
    // NEW METHODS FOR LIVE TRADING DATA STORAGE
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
    virtual Result<void> store_trading_results(
        const std::string& strategy_id, const Timestamp& date, double total_return,
        double sharpe_ratio, double sortino_ratio, double max_drawdown, double calmar_ratio,
        double volatility, int total_trades, double win_rate, double profit_factor, double avg_win,
        double avg_loss, double max_win, double max_loss, double avg_holding_period, double var_95,
        double cvar_95, double beta, double correlation, double downside_volatility,
        const nlohmann::json& config, const std::string& table_name = "trading.results") = 0;

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
    virtual Result<void> store_live_results(
        const std::string& strategy_id, const Timestamp& date, double total_return,
        double volatility, double total_pnl, double unrealized_pnl, double realized_pnl,
        double current_portfolio_value, double portfolio_var, double gross_leverage,
        double net_leverage, double portfolio_leverage, double max_correlation, double jump_risk,
        double risk_scale, double total_notional, int active_positions,
        const nlohmann::json& config, const std::string& table_name = "trading.live_results") = 0;

    /**
     * @brief Store live trading equity curve point
     * @param strategy_id Strategy identifier
     * @param timestamp Timestamp of the equity point
     * @param equity Equity value
     * @param table_name Name of the table to insert into
     * @return Result indicating success or failure
     */
    virtual Result<void> store_trading_equity_curve(
        const std::string& strategy_id, const Timestamp& timestamp, double equity,
        const std::string& table_name = "trading.equity_curve") = 0;

    /**
     * @brief Store multiple live trading equity curve points
     * @param strategy_id Strategy identifier
     * @param equity_points Vector of timestamp-equity pairs
     * @param table_name Name of the table to insert into
     * @return Result indicating success or failure
     */
    virtual Result<void> store_trading_equity_curve_batch(
        const std::string& strategy_id,
        const std::vector<std::pair<Timestamp, double>>& equity_points,
        const std::string& table_name = "trading.equity_curve") = 0;

protected:
    /**
     * @brief Helper method to validate date range
     * @param start_date Start date
     * @param end_date End date
     * @return Result indicating if date range is valid
     */
    virtual Result<void> validate_date_range(const Timestamp& start_date,
                                             const Timestamp& end_date) const {
        if (start_date >= end_date) {
            return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                    "Start date must be before end date", "DatabaseInterface");
        }
        return Result<void>();
    }
};

}  // namespace trade_ngin
