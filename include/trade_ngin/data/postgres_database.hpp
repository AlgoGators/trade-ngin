// include/trade_ngin/data/postgres_database.hpp
//
// Timezone contract (Phase 5 §5c):
//   All `Timestamp` parameters and all `YYYY-MM-DD` keys produced by this
//   module are UTC. Provider date columns are interpreted as calendar dates
//   with no timezone shift -- their semantics are determined by the ingest
//   pipeline, not by this DB layer. If a strategy needs market-local
//   semantics, convert at the strategy boundary, not here.
//
//   Date-string keys MUST be produced via `trade_ngin::core::format_utc_date`
//   (a wrapper around `safe_gmtime + strftime`); direct `std::gmtime` use
//   is forbidden in this file (non-thread-safe and locale-dependent).
//
// SQL contract (Phase 5 §5b):
//   Value interpolation MUST go through `pqxx::params` / `exec_params`.
//   Identifier interpolation (table/column names) MUST go through
//   `validate_identifier` (private helper in postgres_database.cpp) before
//   string concatenation -- never inject untrusted input as a SQL identifier.

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

class PostgresDatabase;

/**
 * @brief RAII scope for composing several writes into one atomic unit.
 *
 * Every write method on PostgresDatabase historically opened and committed its
 * own transaction, so no caller -- equity or futures -- could make two writes
 * land together. That is not a style problem: the live equity path writes
 * corp-action-adjusted positions and then the dedup record that stops those
 * events being applied again. Split across two transactions, a failure between
 * them leaves positions adjusted with no dedup row, and the next run re-applies
 * the events: splits re-multiply quantity, dividends re-rescale cost basis.
 *
 * Hold one of these across the writes that must not be separated, then commit.
 * Destruction without commit rolls back, so an early return or a thrown
 * exception cannot leave a half-applied unit behind.
 *
 * pqxx is deliberately not exposed: callers pass this object back to the
 * PostgresDatabase overloads, which reach the underlying transaction as a
 * friend.
 */
class DbTransaction {
public:
    ~DbTransaction();

    DbTransaction(const DbTransaction&) = delete;
    DbTransaction& operator=(const DbTransaction&) = delete;
    DbTransaction(DbTransaction&&) noexcept;
    DbTransaction& operator=(DbTransaction&&) noexcept;

    /**
     * @brief Commit every write made in this scope. Idempotent-safe: a second
     *        call is an error rather than a double commit.
     */
    Result<void> commit();

    /// True once commit() has succeeded. False means the destructor will roll back.
    bool committed() const { return committed_; }

    /// True when the scope holds a live transaction (false after a move).
    bool valid() const { return txn_ != nullptr; }

private:
    friend class PostgresDatabase;

    explicit DbTransaction(pqxx::connection& conn);

    pqxx::work& work() { return *txn_; }

    std::unique_ptr<pqxx::work> txn_;
    bool committed_{false};
};

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
     * @param strategy_id Combined strategy identifier (e.g.,
     * "LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST")
     * @param strategy_name Individual strategy name (e.g., "TREND_FOLLOWING"). If empty, loads all
     *                      positions matching the strategy_id.
     * @param portfolio_id Portfolio identifier (e.g., BASE_PORTFOLIO, CONSERVATIVE_PORTFOLIO)
     * @param date Date to load positions for
     * @param table_name Name of the positions table
     * @return Result containing map of symbol to position
     */
    Result<std::unordered_map<std::string, Position>> load_positions_by_date(
        const std::string& strategy_id, const std::string& strategy_name,
        const std::string& portfolio_id, const Timestamp& date,
        const std::string& table_name = "trading.positions") override;

    /**
     * @brief Store execution reports in the database
     * @param executions List of execution reports
     * @param strategy_id Combined strategy identifier
     * @param strategy_name Individual strategy name
     * @param portfolio_id Portfolio identifier
     * @param table_name Name of the table to store data
     * @return Result indicating success or failure
     */
    Result<void> store_executions(const std::vector<ExecutionReport>& executions,
                                  const std::string& strategy_id, const std::string& strategy_name,
                                  const std::string& portfolio_id,
                                  const std::string& table_name) override;

    /**
     * @brief Store positions in the database
     * @param positions List of positions
     * @param strategy_id Combined strategy identifier
     * @param strategy_name Individual strategy name
     * @param portfolio_id Portfolio identifier
     * @param table_name Name of the table to store data
     * @return Result indicating success or failure
     */
    Result<void> store_positions(const std::vector<Position>& positions,
                                 const std::string& strategy_id, const std::string& strategy_name,
                                 const std::string& portfolio_id,
                                 const std::string& table_name) override;

    /**
     * @brief Open a scope in which several writes commit or roll back together.
     *
     * Pass the returned scope to the overloads that accept a DbTransaction, then
     * call commit(). Errors if the connection is unavailable.
     */
    Result<std::unique_ptr<DbTransaction>> begin_unit_of_work();

    /**
     * @brief Store positions inside a caller-owned unit of work.
     *
     * Same statements as the single-write overload; the caller commits. Use when
     * the positions must land together with another write.
     */
    Result<void> store_positions(DbTransaction& txn, const std::vector<Position>& positions,
                                 const std::string& strategy_id, const std::string& strategy_name,
                                 const std::string& portfolio_id, const std::string& table_name);

    /**
     * @brief Store signals in the database
     * @param signals Map of signals by symbol
     * @param strategy_id Combined strategy identifier
     * @param strategy_name Individual strategy name
     * @param portfolio_id Portfolio identifier
     * @param timestamp Timestamp for the signals
     * @param table_name Name of the table to store data
     * @return Result indicating success or failure
     */
    Result<void> store_signals(const std::unordered_map<std::string, double>& signals,
                               const std::string& strategy_id, const std::string& strategy_name,
                               const std::string& portfolio_id, const Timestamp& timestamp,
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
    Result<void> store_backtest_executions(
        const std::vector<ExecutionReport>& executions, const std::string& run_id,
        const std::string& portfolio_id = "BASE_PORTFOLIO",
        const std::string& table_name = "backtest.executions") override;

    // Multi-strategy version: store executions with strategy_id
    virtual Result<void> store_backtest_executions_with_strategy(
        const std::vector<ExecutionReport>& executions, const std::string& run_id,
        const std::string& strategy_id, const std::string& portfolio_id = "BASE_PORTFOLIO",
        const std::string& table_name = "backtest.executions");

    /**
     * @brief Store backtest signals
     * @param signals Map of symbol to signal value
     * @param strategy_id ID of the strategy generating signals
     * @param run_id Backtest run identifier
     * @param timestamp Timestamp of signals
     * @param table_name Name of the table to insert into
     * @return Result indicating success or failure
     */
    Result<void> store_backtest_signals(
        const std::unordered_map<std::string, double>& signals, const std::string& strategy_id,
        const std::string& run_id, const Timestamp& timestamp,
        const std::string& portfolio_id = "BASE_PORTFOLIO",
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
    Result<void> store_backtest_metadata(
        const std::string& run_id, const std::string& name, const std::string& description,
        const Timestamp& start_date, const Timestamp& end_date,
        const nlohmann::json& hyperparameters, const std::string& portfolio_id = "BASE_PORTFOLIO",
        const std::string& table_name = "backtest.run_metadata") override;

    // Multi-strategy version: store metadata with portfolio_run_id, strategy_allocation,
    // portfolio_config
    virtual Result<void> store_backtest_metadata_with_portfolio(
        const std::string& run_id, const std::string& portfolio_run_id,
        const std::string& strategy_id, double strategy_allocation,
        const nlohmann::json& portfolio_config, const std::string& name,
        const std::string& description, const Timestamp& start_date, const Timestamp& end_date,
        const nlohmann::json& hyperparameters, const std::string& portfolio_id = "BASE_PORTFOLIO",
        const std::string& table_name = "backtest.run_metadata");

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
    Result<void> store_trading_results(
        const std::string& strategy_id, const Timestamp& date, double total_return,
        double sharpe_ratio, double sortino_ratio, double max_drawdown, double calmar_ratio,
        double volatility, int total_trades, double win_rate, double profit_factor, double avg_win,
        double avg_loss, double max_win, double max_loss, double avg_holding_period, double var_95,
        double cvar_95, double beta, double correlation, double downside_volatility,
        const nlohmann::json& config, const std::string& table_name = "trading.results") override;

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
     * @param net_leverage Net leverage
     * @param gross_leverage Gross leverage
     * @param max_correlation Max correlation risk
     * @param jump_risk Jump risk (99th percentile)
     * @param risk_scale Risk scale factor
     * @param total_notional Total notional exposure
     * @param active_positions Number of active positions
     * @param config Strategy configuration JSON
     * @param table_name Name of the table to insert into
     * @return Result indicating success or failure
     */
    Result<void> store_live_results(
        const std::string& strategy_id, const Timestamp& date, double total_return,
        double volatility, double total_pnl, double unrealized_pnl, double realized_pnl,
        double current_portfolio_value, double daily_realized_pnl, double daily_unrealized_pnl,
        double portfolio_var, double net_leverage, double gross_leverage,
        double margin_leverage, double margin_cushion, double max_correlation, double jump_risk,
        double risk_scale, double gross_notional, double net_notional, int active_positions,
        double total_transaction_costs, double margin_posted, double cash_available,
        const nlohmann::json& config,
        const std::string& table_name = "trading.live_results") override;

    Result<std::tuple<double, double, double>> get_previous_live_aggregates(
        const std::string& strategy_id, const std::string& portfolio_id, const Timestamp& date,
        const std::string& table_name = "trading.live_results") override;

    /**
     * @brief Store live trading equity curve point
     * @param strategy_id Strategy identifier
     * @param timestamp Timestamp of the equity point
     * @param equity Equity value
     * @param portfolio_id Portfolio identifier
     * @param table_name Name of the table to insert into
     * @return Result indicating success or failure
     */
    Result<void> store_trading_equity_curve(
        const std::string& strategy_id, const Timestamp& timestamp, double equity,
        const std::string& portfolio_id,
        const std::string& table_name = "trading.equity_curve") override;

    /**
     * @brief Store multiple live trading equity curve points
     * @param strategy_id Strategy identifier
     * @param equity_points Vector of timestamp-equity pairs
     * @param portfolio_id Portfolio identifier
     * @param table_name Name of the table to insert into
     * @return Result indicating success or failure
     */
    Result<void> store_trading_equity_curve_batch(
        const std::string& strategy_id,
        const std::vector<std::pair<Timestamp, double>>& equity_points,
        const std::string& portfolio_id,
        const std::string& table_name = "trading.equity_curve") override;

    // ============================================================================
    // NEW METHODS TO REPLACE RAW SQL (Phase 0 Refactoring)
    // ============================================================================

    /**
     * @brief Delete stale executions for a given date and order IDs
     * @param order_ids List of order IDs to match
     * @param date Date to filter executions
     * @param table_name Name of the executions table
     * @return Result indicating success or failure
     */
    /**
     * @brief Delete this book's executions for a date so a re-run can re-insert them.
     *
     * E2-F4: portfolio_id is REQUIRED and deliberately has no default.
     *
     * The predicate used to be (date, strategy_name, order_id) with no portfolio at all,
     * while trading.executions is keyed (portfolio_id, strategy_id, strategy_name, date,
     * exec_id). order_id is portfolio-independent -- ExecutionManager builds it as
     * "DAILY_<symbol>_<date>" (execution_manager.cpp:147) -- and TREND_FOLLOWING is
     * enabled-live in BOTH the base and conservative books. So a conservative run could
     * delete BASE_PORTFOLIO's rows for the same symbol and date, and vice versa; the
     * survivor was whichever book ran last, and the delete leaves no trace of what it took.
     *
     * It never fired only because of a SECOND bug that happened to mask it: two call sites
     * passed table_name into the strategy_name slot, so the predicate matched nothing.
     * Fixing that alone would have ARMED this one -- the two must move together, which is
     * why portfolio_id is required rather than defaulted. A default would let a caller be
     * silently re-armed by omission.
     */
    virtual Result<void> delete_stale_executions(const std::vector<std::string>& order_ids,
                                                  const Timestamp& date,
                                                  const std::string& strategy_name,
                                                  const std::string& portfolio_id,
                                                  const std::string& table_name = "trading.executions");

    /**
     * @brief Store backtest summary results (replaces raw SQL INSERT)
     * @param run_id Backtest run identifier
     * @param start_date Start date of backtest
     * @param end_date End date of backtest
     * @param metrics Map of metric name to value
     * @param table_name Name of the results table
     * @return Result indicating success or failure
     */
    virtual Result<void> store_backtest_summary(
        const std::string& run_id, const Timestamp& start_date, const Timestamp& end_date,
        const std::unordered_map<std::string, double>& metrics,
        const std::string& portfolio_id = "BASE_PORTFOLIO",
        const std::string& table_name = "backtest.results");

    /**
     * @brief Store backtest equity curve batch (replaces raw SQL INSERT)
     * @param run_id Backtest run identifier
     * @param equity_points Vector of timestamp-equity pairs
     * @param table_name Name of the equity curve table
     * @return Result indicating success or failure
     */
    virtual Result<void> store_backtest_equity_curve_batch(
        const std::string& run_id, const std::vector<std::pair<Timestamp, double>>& equity_points,
        const std::string& portfolio_id = "BASE_PORTFOLIO",
        const std::string& table_name = "backtest.equity_curve");

    /**
     * @brief Store backtest final positions (replaces raw SQL INSERT)
     * @param positions Vector of positions
     * @param run_id Backtest run identifier
     * @param table_name Name of the positions table
     * @return Result indicating success or failure
     */
    virtual Result<void> store_backtest_positions(
        const std::vector<Position>& positions, const std::string& run_id,
        const std::string& portfolio_id = "BASE_PORTFOLIO",
        const std::string& table_name = "backtest.final_positions");

    // Multi-strategy version: store positions with strategy_id
    virtual Result<void> store_backtest_positions_with_strategy(
        const std::vector<Position>& positions, const std::string& run_id,
        const std::string& strategy_id, const std::string& portfolio_id = "BASE_PORTFOLIO",
        const std::string& table_name = "backtest.final_positions");

    /**
     * @brief Update live results for previous day finalization (replaces raw SQL UPDATE)
     * @param strategy_id Strategy identifier
     * @param date Date to update
     * @param updates Map of column name to new value
     * @param portfolio_id Portfolio identifier
     * @param table_name Name of the live results table
     * @return Result indicating success or failure
     */
    virtual Result<void> update_live_results(
        const std::string& strategy_id, const Timestamp& date,
        const std::unordered_map<std::string, double>& updates,
        const std::string& portfolio_id,
        const std::string& table_name = "trading.live_results");

    /**
     * @brief Update live equity curve (replaces raw SQL UPDATE)
     * @param strategy_id Strategy identifier
     * @param date Date to update
     * @param equity New equity value
     * @param portfolio_id Portfolio identifier
     * @param table_name Name of the equity curve table
     * @return Result indicating success or failure
     */
    virtual Result<void> update_live_equity_curve(
        const std::string& strategy_id, const Timestamp& date, double equity,
        const std::string& portfolio_id,
        const std::string& table_name = "trading.equity_curve");

    /**
     * @brief Delete existing live results for a date (replaces raw SQL DELETE)
     * @param strategy_id Strategy identifier
     * @param date Date to delete
     * @param portfolio_id Portfolio identifier
     * @param table_name Name of the live results table
     * @return Result indicating success or failure
     */
    virtual Result<void> delete_live_results(
        const std::string& strategy_id, const Timestamp& date,
        const std::string& portfolio_id,
        const std::string& table_name = "trading.live_results");

    /**
     * @brief Delete existing equity curve entry for a date (replaces raw SQL DELETE)
     * @param strategy_id Strategy identifier
     * @param date Date to delete
     * @param portfolio_id Portfolio identifier
     * @param table_name Name of the equity curve table
     * @return Result indicating success or failure
     */
    virtual Result<void> delete_live_equity_curve(
        const std::string& strategy_id, const Timestamp& date,
        const std::string& portfolio_id,
        const std::string& table_name = "trading.equity_curve");

    /**
     * @brief Store complete live results row with all metrics (replaces raw SQL INSERT)
     * @param strategy_id Strategy identifier
     * @param date Trading date
     * @param metrics Complete set of metrics as key-value pairs
     * @param table_name Name of the live results table
     * @return Result indicating success or failure
     */
    virtual Result<void> store_live_results_complete(
        const std::string& strategy_id, const Timestamp& date,
        const std::unordered_map<std::string, double>& metrics,
        const std::unordered_map<std::string, int>& int_metrics, const nlohmann::json& config,
        const std::string& portfolio_id = "BASE_PORTFOLIO",
        const std::string& table_name = "trading.live_results");

    /**
     * @brief Store live trading run metadata
     * @param date Trading date
     * @param strategy_id Combined strategy identifier
     * @param portfolio_id Portfolio identifier
     * @param strategy_allocations Map of strategy name to allocation (JSON)
     * @param portfolio_config Portfolio configuration (JSON)
     * @param strategy_configs Per-strategy configurations (JSON)
     * @param table_name Name of the table
     * @return Result indicating success or failure
     */
    Result<void> store_live_run_metadata(
        const Timestamp& date, const std::string& strategy_id, const std::string& portfolio_id,
        const nlohmann::json& strategy_allocations, const nlohmann::json& portfolio_config,
        const nlohmann::json& strategy_configs,
        const std::string& table_name = "trading.live_run_metadata");

    /**
     * @brief Get contract metadata for trading instruments
     * @return Result containing Arrow table with contract metadata
     */
    virtual Result<std::shared_ptr<arrow::Table>> get_contract_metadata() const;

    /**
     * @brief Corporate action row from equities_data.corporate_action.
     *
     * The `value` field is parsed from the source table's text column:
     *   - SPLIT / ADR_SPLIT: split factor (e.g. 4.0 for a 4-for-1)
     *   - DIVIDEND: cash amount per share in trading currency
     */
    struct CorpActionRow {
        std::string ticker;
        std::string date_str;  // YYYY-MM-DD
        std::string action;    // vendor label, e.g. "split" | "dividend" | "mergerto"
        double value;
        // Deal terms, populated only for TERMINATION-class rows that carry
        // them (contraticker/contraname are NULL for splits and dividends).
        std::string contra_ticker;
        std::string contra_name;
        std::string name;
    };

    /**
     * @brief Read corporate actions for a ticker list between two dates.
     *
     * Reads from equities_data.corporate_action (existing schema; no DDL).
     * That feed stopped receiving events on 2025-08-29, so this returns
     * nothing for recent windows. It remains the only source of TERMINATION
     * deal terms; PRICE_RESTATING events are now sourced from the live
     * per-bar columns via get_per_bar_corporate_actions() instead.
     *
     * @param actions      Vendor labels to filter on. Defaults to the
     *                     price-restating set for backward compatibility;
     *                     pass vendor_labels_for_class(TERMINATION) for the
     *                     deal-terms path.
     *
     * @param tickers      Symbols to query (typically the live portfolio's
     *                     equity universe).
     * @param start_date   Inclusive YYYY-MM-DD.
     * @param end_date     Inclusive YYYY-MM-DD.
     * @return Sorted by (date, ticker, action); empty result is not an error.
     */
    Result<std::vector<CorpActionRow>> get_corporate_actions(
        const std::vector<std::string>& tickers,
        const std::string& start_date,
        const std::string& end_date,
        const std::vector<std::string>& actions = {"split", "dividend", "adrratiosplit"});

    /**
     * @brief PRICE_RESTATING events sourced from the live per-bar columns.
     *
     * equities_data.ohlcv_1d carries div_cash and split_factor on the bar the
     * event goes ex. Unlike equities_data.corporate_action these are current,
     * so this is the production source for class-1 events. Splits (including
     * ADR-ratio changes and spin-offs, which the vendor also encodes in
     * split_factor) surface as action "split"; cash dividends as "dividend".
     *
     * @param tickers    Symbols to query; empty returns an empty result.
     * @param start_date Inclusive YYYY-MM-DD.
     * @param end_date   Inclusive YYYY-MM-DD.
     * @return Sorted by (date, ticker, action); empty result is not an error.
     */
    virtual Result<std::vector<CorpActionRow>> get_per_bar_corporate_actions(
        const std::vector<std::string>& tickers,
        const std::string& start_date,
        const std::string& end_date);

    /** @brief One equities_data.ticker_aliases row (SERIES_CONTINUITY source). */
    struct TickerAliasRow {
        std::string historical_ticker;
        std::string current_symbol;
        std::string effective_until;  // YYYY-MM-DD; empty when NULL
        std::string note;
    };

    /**
     * @brief Read the curated historical-ticker -> current-symbol map.
     *
     * A curated subset, not the full rename history: symbols absent from it
     * are simply left unmapped by the caller.
     */
    virtual Result<std::vector<TickerAliasRow>> get_ticker_aliases();

    /**
     * @brief Delisting dates for the given symbols (TERMINATION timing).
     *
     * From equities_data.ohlcv_1d.delisting_date, which is maintained
     * independently of the frozen corporate_action feed.
     *
     * @return symbol -> YYYY-MM-DD for symbols carrying a delisting date.
     */
    virtual Result<std::unordered_map<std::string, std::string>> get_delisting_dates(
        const std::vector<std::string>& tickers);

    /**
     * @brief Earliest date each symbol was held (non-zero) by this strategy.
     *
     * The corp-action window must reach back to when a position was
     * ESTABLISHED, not to when its row was last written. `last_update` cannot
     * serve that purpose: load_positions_by_date() selects
     * `WHERE DATE(last_update) = DATE($n)`, so every row it returns carries the
     * requested date by construction, and the table has zero rows where
     * last_update differs from date. Deriving a lookback from it always
     * collapses to "yesterday". This queries the position history instead.
     *
     * Erring wide is safe: re-fetched events are rejected by
     * trading.corp_action_applied, so the only cost of an over-wide window is
     * query time.
     *
     * @return symbol -> YYYY-MM-DD of the earliest non-zero holding. Symbols
     *         with no history are absent.
     */
    virtual Result<std::unordered_map<std::string, std::string>> get_position_inception_dates(
        const std::string& strategy_id,
        const std::string& strategy_name,
        const std::string& portfolio_id,
        const std::vector<std::string>& symbols,
        const std::string& table_name = "trading.positions");

    /**
     * @brief Raw closes for specific symbols over a date range.
     *
     * Targeted top-up for the corp-action path when a position predates the
     * bulk price load. The dividend denominator needs a close AT each ex-date,
     * not a contiguous series, so this is a plain indexed range read (~45 ms
     * for one symbol over ten years) rather than a re-run of the ~25 s
     * adjusted-series window function.
     *
     * Returns RAW closes, matching close_by_symbol_date's frame -- the
     * denominator is raw-dollar over the ex-date close (05-22 doc §B6).
     *
     * @return symbol -> (YYYY-MM-DD -> close).
     */
    virtual Result<std::unordered_map<std::string, std::map<std::string, double>>>
    get_historical_closes(const std::vector<std::string>& symbols,
                          const std::string& start_date,
                          const std::string& end_date);

    /**
     * @brief One corporate action already applied to a live position.
     *
     * Mirrors trading.corp_action_applied. Dividend fields are populated only
     * for DIVIDEND rows and are INFORMATIONAL -- the price series is
     * total-return adjusted, so total_cash must never be added to P&L.
     */
    struct AppliedCorpActionRow {
        std::string symbol;
        std::string action_type;
        std::string ex_date;  ///< YYYY-MM-DD
        double qty_held{0.0};
        double dividend_per_share{0.0};
        double total_cash{0.0};
    };

    /**
     * @brief Load every corp action already applied for one portfolio+strategy+name.
     *
     * Durable replacement for the applied_corp_actions.json state file, which
     * lived under a container path with no volume and so was lost on redeploy.
     * Loading everything is deliberate: the record is the strategy's lifetime
     * dedup set and is small (order thousands of rows even at full universe
     * scale), and cumulative dividend income is summed from it.
     *
     * strategy_name is part of the key, not decoration: one strategy_id can
     * carry several names (the live runners build a combined id, so
     * LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST holds both TREND_FOLLOWING and
     * TREND_FOLLOWING_FAST rows). Reading without it hands one strategy's
     * applied events to another, which then skips its own adjustment and
     * carries a permanently wrong cost basis, and sums dividend income across
     * every name under the id.
     */
    virtual Result<std::vector<AppliedCorpActionRow>> load_applied_corp_actions(
        const std::string& portfolio_id, const std::string& strategy_id,
        const std::string& strategy_name);

    /**
     * @brief Record corp actions as applied. Idempotent per natural key.
     *
     * ON CONFLICT DO NOTHING against
     * (portfolio_id, strategy_id, strategy_name, symbol, action_type, ex_date):
     * re-recording an event is a no-op rather than an error, so a partially
     * completed run is safe to repeat.
     */
    virtual Result<void> store_applied_corp_actions(
        const std::string& portfolio_id, const std::string& strategy_id,
        const std::string& strategy_name,
        const std::vector<AppliedCorpActionRow>& rows);

    /**
     * @brief Record applied corp actions inside a caller-owned unit of work.
     *
     * Composed with store_positions(DbTransaction&, ...) so an adjusted position
     * and the dedup row that protects it cannot be separated by a failure.
     */
    virtual Result<void> store_applied_corp_actions(
        DbTransaction& txn, const std::string& portfolio_id, const std::string& strategy_id,
        const std::string& strategy_name,
        const std::vector<AppliedCorpActionRow>& rows);

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

    /**
     * @brief Validate strategy ID for SQL injection prevention.
     *        Allowlist: alphanumeric + `_-`, 1-50 chars. Public for
     *        testability (Phase 5 §5b -- the SQL-injection chokepoint
     *        deserves a regression test).
     */
    Result<void> validate_strategy_id(const std::string& strategy_id) const;

    /**
     * @brief Validate a generic SQL identifier (table name fragment, column
     *        name) against a strict allowlist: `[A-Za-z_][A-Za-z0-9_.]*`.
     *        Phase 5 §5b -- use this when an identifier MUST be string-
     *        concatenated into a query (Postgres does not allow $-binding
     *        identifiers). For values, prefer `pqxx::params` / `exec_params`.
     */
    Result<void> validate_identifier(const std::string& identifier) const;

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
     * @brief Position-write statements, executed in a transaction that is NOT
     *        committed here. The single-write overload wraps this in its own
     *        transaction; the composing overload runs it in the caller's.
     */
    Result<void> store_positions_in(pqxx::work& txn, const std::vector<Position>& positions,
                                    const std::string& strategy_id,
                                    const std::string& strategy_name,
                                    const std::string& portfolio_id,
                                    const std::string& table_name);

    /**
     * @brief Dedup-write statements, executed in a transaction that is NOT
     *        committed here. Same wrapper/composition split as above.
     */
    Result<void> store_applied_corp_actions_in(pqxx::work& txn, const std::string& portfolio_id,
                                               const std::string& strategy_id,
                                               const std::string& strategy_name,
                                               const std::vector<AppliedCorpActionRow>& rows);

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
     * @brief Convert any pqxx result to a generic Arrow table
     * @param result pqxx result to convert
     * @return Result containing the Arrow table with columns dynamically determined
     */
    Result<std::shared_ptr<arrow::Table>> convert_generic_to_arrow(
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