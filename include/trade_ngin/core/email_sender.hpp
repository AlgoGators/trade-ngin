#pragma once

#include <string>
#include <vector>
#include <memory>
#include <map>
#include <unordered_map>
#include <optional>
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/credential_store.hpp"
#include "trade_ngin/data/database_interface.hpp"
#include "trade_ngin/core/holiday_checker.hpp"
#include "trade_ngin/risk/risk_manager.hpp"

namespace trade_ngin {

// Type alias for per-strategy positions map
using StrategyPositionsMap = std::unordered_map<std::string, std::unordered_map<std::string, Position>>;

// Type alias for per-strategy executions map
using StrategyExecutionsMap = std::unordered_map<std::string, std::vector<ExecutionReport>>;

/**
 * @brief Email configuration structure
 */
struct EmailConfig {
    std::string smtp_host;
    int smtp_port;
    std::string username;
    std::string password;
    std::string from_email;
    std::vector<std::string> to_emails;
    bool use_tls;
};

/**
 * @brief Email sender class for sending trading reports
 */
class EmailSender {
public:
    /**
     * @brief Constructor
     * @param credentials Shared pointer to credential store
     */
    explicit EmailSender(std::shared_ptr<CredentialStore> credentials);

    /**
     * @brief Initialize email configuration from credential store
     * @return Result indicating success or failure
     */
    Result<void> initialize();

    /**
     * @brief Send trading results email
     * @param subject Email subject
     * @param body Email body (HTML or plain text)
     * @param is_html Whether the body is HTML format
     * @param attachment_paths Optional vector of file paths to attach (e.g., CSV files)
     * @return Result indicating success or failure
     */
    Result<void> send_email(const std::string& subject, const std::string& body, bool is_html = true, const std::vector<std::string>& attachment_paths = {});

    /**
     * @brief Generate trading results email body
     * @param positions Current portfolio positions
     * @param risk_metrics Risk metrics from the pipeline
     * @param strategy_metrics Strategy performance metrics
     * @param date Trading date
     * @param is_daily_strategy Flag indicating if this is a daily strategy
     * @return HTML email body
     */
    std::string generate_trading_report_body(
        const std::unordered_map<std::string, Position>& positions,
        const std::optional<RiskResult>& risk_metrics,
        const std::map<std::string, double>& strategy_metrics,
        const std::vector<ExecutionReport>& executions,
        const std::string& date,
        bool is_daily_strategy = true,
        const std::unordered_map<std::string, double>& current_prices = {},
        std::shared_ptr<DatabaseInterface> db = nullptr,
        const std::unordered_map<std::string, Position>& yesterday_positions = {},
        const std::unordered_map<std::string, double>& yesterday_close_prices = {},
        const std::unordered_map<std::string, double>& two_days_ago_close_prices = {},
        const std::map<std::string, double>& yesterday_daily_metrics = {}
    );

    /**
     * @brief Generate trading results email body with per-strategy positions and executions tables
     * @param strategy_positions Per-strategy positions map for grouped tables
     * @param positions Current portfolio positions (for portfolio-level calculations)
     * @param risk_metrics Risk metrics from the pipeline
     * @param strategy_metrics Strategy performance metrics
     * @param strategy_executions Per-strategy executions map for grouped tables
     * @param date Trading date
     * @param portfolio_name Display name for the portfolio (shown in header)
     * @param is_daily_strategy Flag indicating if this is a daily strategy
     * @return HTML email body
     */
    std::string generate_trading_report_body(
        const StrategyPositionsMap& strategy_positions,
        const std::unordered_map<std::string, Position>& positions,
        const std::optional<RiskResult>& risk_metrics,
        const std::map<std::string, double>& strategy_metrics,
        const StrategyExecutionsMap& strategy_executions,
        const std::string& date,
        const std::string& portfolio_name,
        bool is_daily_strategy = true,
        const std::unordered_map<std::string, double>& current_prices = {},
        std::shared_ptr<DatabaseInterface> db = nullptr,
        const StrategyPositionsMap& yesterday_strategy_positions = {},
        const std::unordered_map<std::string, double>& yesterday_close_prices = {},
        const std::unordered_map<std::string, double>& two_days_ago_close_prices = {},
        const std::map<std::string, double>& yesterday_daily_metrics = {}
    );

    /**
     * @brief Generate trading results email body from database results
     * @param db Database interface to query trading.results
     * @param strategy_id Strategy identifier
     * @param date Trading date
     * @return HTML email body
     */
    std::string generate_trading_report_from_db(
        std::shared_ptr<DatabaseInterface> db,
        const std::string& strategy_id,
        const std::string& date
    );

private:
    std::shared_ptr<CredentialStore> credentials_;
    EmailConfig config_;
    bool initialized_;
    std::string chart_base64_;  // Store equity curve chart data for embedding in email
    std::string pnl_by_symbol_base64_;  // Store PnL by symbol chart data
    std::string daily_pnl_base64_;  // Store daily PnL chart data
    std::string total_transaction_costs_base64_; // Store cumulative transaction costs 
    std::string margin_posted_base64_; //Store total margin posted
    std::string portfolio_composition_base64_; //Store portfolio composition
    std::string cumulative_pnl_by_symbol_base64_; //Store cumalative pnl
    HolidayChecker holiday_checker_; //Holiday Checking

    /**
     * @brief Load email configuration from credential store
     * @return Result indicating success or failure
     */
    Result<void> load_config();

    /**
     * @brief Format position data for email
     * @param positions Portfolio positions
     * @param is_daily_strategy Flag indicating if this is a daily strategy
     * @param current_prices Current market prices
     * @param strategy_metrics Strategy metrics (to extract volatility)
     * @return Formatted position table HTML
     */
    std::string format_positions_table(const std::unordered_map<std::string, Position>& positions,
                                       bool is_daily_strategy = true,
                                       const std::unordered_map<std::string, double>& current_prices = {},
                                       const std::map<std::string, double>& strategy_metrics = {});

    /**
     * @brief Format risk metrics for email
     * @param risk_metrics Risk metrics
     * @return Formatted risk metrics HTML
     */
    std::string format_risk_metrics(const RiskResult& risk_metrics);

    /**
     * @brief Format strategy metrics for email
     * @param strategy_metrics Strategy metrics
     * @return Formatted strategy metrics HTML
     */
    std::string format_strategy_metrics(const std::map<std::string, double>& strategy_metrics);

    /**
     * @brief Format executions table for email
     * @param executions Daily execution reports
     * @return Formatted executions table HTML
     */
    std::string format_executions_table(const std::vector<ExecutionReport>& executions);

    /**
     * @brief Format symbols reference table for email
     * @param positions Current portfolio positions
     * @param db Database interface to query symbols
     * @param date Current date for front month calculation
     * @return Formatted symbols table HTML
     */
    std::string format_symbols_table_for_positions(const std::unordered_map<std::string, Position>& positions,std::shared_ptr<DatabaseInterface> db, const std::string& date);

    /**
     * @brief Format yesterday's finalized positions table with actual PnL
     * @param yesterday_positions Yesterday's positions
     * @param entry_prices Entry prices (Day T-2 close)
     * @param exit_prices Exit prices (Day T-1 close)
     * @param db Database interface to query contract multipliers
     * @param strategy_metrics Strategy metrics (to extract daily metrics)
     * @param yesterday_date Yesterday's date string
     * @return Formatted yesterday's finalized positions table HTML
     */
    std::string format_yesterday_finalized_positions_table(
        const std::unordered_map<std::string, Position>& yesterday_positions,
        const std::unordered_map<std::string, double>& entry_prices,
        const std::unordered_map<std::string, double>& exit_prices,
        std::shared_ptr<DatabaseInterface> db,
        const std::map<std::string, double>& strategy_metrics = {},
        const std::string& yesterday_date = ""
    );

    /**
     * @brief Format yesterday's finalized positions table with per-strategy breakdown
     * @param strategy_positions Per-strategy positions map
     * @param entry_prices Entry prices (Day T-2 close)
     * @param exit_prices Exit prices (Day T-1 close)
     * @param db Database interface to query contract multipliers
     * @param strategy_metrics Strategy metrics (to extract daily metrics)
     * @param yesterday_date Yesterday's date string
     * @return Formatted yesterday's finalized positions table HTML with per-strategy sections
     */
    std::string format_yesterday_finalized_positions_table(
        const StrategyPositionsMap& strategy_positions,
        const std::unordered_map<std::string, double>& entry_prices,
        const std::unordered_map<std::string, double>& exit_prices,
        std::shared_ptr<DatabaseInterface> db,
        const std::map<std::string, double>& strategy_metrics = {},
        const std::string& yesterday_date = ""
    );

    /**
     * @brief Format rollover warning for contracts approaching expiry
     * @param positions Current portfolio positions
     * @param date Current trading date (YYYY-MM-DD format)
     * @param db Database interface to query contract metadata
     * @param date_override_for_testing Optional date override for testing (YYYY-MM-DD format)
     *        If empty, uses the 'date' parameter. Use this to simulate different dates
     *        for testing rollover warnings without affecting actual data.
     * @return Formatted rollover warning HTML (empty if no contracts in rollover window)
     */
    std::string format_rollover_warning(
        const std::unordered_map<std::string, Position>& positions,
        const std::string& date,
        std::shared_ptr<DatabaseInterface> db,
        const std::string& date_override_for_testing = ""
    );

    /**
     * @brief Format all per-strategy position tables for email
     * @param strategy_positions Per-strategy positions map
     * @param current_prices Current market prices
     * @param strategy_metrics Strategy metrics (to extract volatility)
     * @return Formatted HTML with all strategy tables and portfolio summary
     */
    std::string format_strategy_positions_tables(
        const StrategyPositionsMap& strategy_positions,
        const std::unordered_map<std::string, double>& current_prices,
        const std::map<std::string, double>& strategy_metrics
    );

    /**
     * @brief Format a single strategy's position table
     * @param strategy_name Strategy identifier
     * @param positions Positions for this strategy
     * @param current_prices Current market prices
     * @return Formatted HTML table for this strategy
     */
    std::string format_single_strategy_table(
        const std::string& strategy_name,
        const std::unordered_map<std::string, Position>& positions,
        const std::unordered_map<std::string, double>& current_prices
    );

    /**
     * @brief Convert strategy ID to display name
     * @param strategy_id Strategy identifier (e.g., "TREND_FOLLOWING_FAST")
     * @return Display name (e.g., "Trend Following Fast")
     */
    std::string format_strategy_display_name(const std::string& strategy_id);

    /**
     * @brief Format all per-strategy execution tables for email
     * @param strategy_executions Per-strategy executions map
     * @return Formatted HTML with all strategy execution tables and portfolio summary
     */
    std::string format_strategy_executions_tables(
        const StrategyExecutionsMap& strategy_executions
    );

    /**
     * @brief Format a single strategy's execution table
     * @param strategy_name Strategy identifier
     * @param executions Executions for this strategy
     * @return Formatted HTML table for this strategy's executions
     */
    std::string format_single_strategy_executions_table(
        const std::string& strategy_name,
        const std::vector<ExecutionReport>& executions
    );

};

} // namespace trade_ngin
