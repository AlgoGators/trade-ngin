#pragma once

#include <functional>
#include <vector>
#include <string>
#include <unordered_map>
#include <map>
#include "trade_ngin/core/types.hpp"

namespace trade_ngin {

// Forward declaration
namespace backtest {
struct BacktestResults;
}

/// The sessions a year every annualised figure of this calculator applies TODAY: volatility,
/// Sharpe, Sortino, downside volatility and the annualised return multiply by 252 or sqrt(252),
/// on every book, whatever the run's own grid is. A futures run has about 312 rows a year and its
/// ruled factor is 311.0574 (T-8D R2); aligning the calculator to it is T-8b's commit (1). Until
/// then this is the factor a backtest records as the one its figures use
/// (backtest.run_metadata.portfolio_config.statistics_K), with the ruled factor beside it.
inline constexpr double kBacktestAnnualisationApplied = 252.0;

/**
 * @brief Pure stateless calculation component for backtest metrics
 *
 * This class extracts the metrics calculation logic from BacktestEngine::calculate_metrics()
 * (lines 2110-2516). All methods are const and have no side effects.
 *
 * Key responsibilities:
 * - Return calculations (total, annualized, daily)
 * - Risk-adjusted metrics (Sharpe, Sortino, Calmar)
 * - Drawdown calculations
 * - Trade statistics (win rate, profit factor, etc.)
 * - Per-symbol P&L breakdown
 * - Monthly returns aggregation
 *
 * Design principles:
 * - All methods are const (no state mutation)
 * - No database dependencies
 * - No logging (caller is responsible for logging)
 * - Pure mathematical functions
 */
class BacktestMetricsCalculator {
public:
    BacktestMetricsCalculator() = default;
    ~BacktestMetricsCalculator() = default;

    // ========== Return Calculations ==========

    /**
     * @brief Calculate total return from equity curve
     * @param start_value Starting portfolio value
     * @param end_value Ending portfolio value
     * @return Total return as decimal (0.10 = 10%)
     */
    double calculate_total_return(double start_value, double end_value) const;

    /**
     * @brief Calculate annualized return
     * @param total_return Total return as decimal
     * @param trading_days Number of trading days
     * @return Annualized return as decimal
     */
    double calculate_annualized_return(double total_return, int trading_days) const;

    /**
     * @brief Calculate daily returns from equity curve
     * @param equity_curve Vector of (timestamp, portfolio_value) pairs
     * @return Vector of daily returns
     */
    std::vector<double> calculate_returns_from_equity(
        const std::vector<std::pair<Timestamp, double>>& equity_curve) const;

    // ========== Risk-Adjusted Return Metrics ==========

    /**
     * @brief Calculate Sharpe ratio
     * @param returns Vector of daily returns
     * @param trading_days Number of trading days (for annualization)
     * @param risk_free_rate Annual risk-free rate (default 0)
     * @return Sharpe ratio
     */
    double calculate_sharpe_ratio(
        const std::vector<double>& returns,
        int trading_days,
        double risk_free_rate = 0.0) const;

    /**
     * @brief Calculate Sortino ratio
     * @param returns Vector of daily returns
     * @param trading_days Number of trading days (for annualization)
     * @param minimum_acceptable_return Minimum acceptable return (default 0)
     * @return Sortino ratio
     */
    double calculate_sortino_ratio(
        const std::vector<double>& returns,
        int trading_days,
        double minimum_acceptable_return = 0.0) const;

    /**
     * @brief Calculate Calmar ratio (annualized return / max drawdown)
     * @param annualized_return Annualized return as decimal
     * @param max_drawdown Maximum drawdown as decimal
     * @return Calmar ratio
     */
    double calculate_calmar_ratio(double annualized_return, double max_drawdown) const;

    // ========== Volatility Metrics ==========

    /**
     * @brief Calculate annualized volatility
     * @param returns Vector of daily returns
     * @return Annualized volatility (using sqrt(252))
     */
    double calculate_volatility(const std::vector<double>& returns) const;

    /**
     * @brief Calculate downside volatility (for Sortino)
     * @param returns Vector of daily returns
     * @param target Target return (typically 0)
     * @return Annualized downside volatility
     */
    double calculate_downside_volatility(
        const std::vector<double>& returns,
        double target = 0.0) const;

    // ========== Drawdown Metrics ==========

    /**
     * @brief Calculate drawdown curve from equity curve
     * @param equity_curve Vector of (timestamp, portfolio_value) pairs
     * @return Vector of (timestamp, drawdown) pairs where drawdown is as decimal (0.10 = 10%)
     */
    std::vector<std::pair<Timestamp, double>> calculate_drawdowns(
        const std::vector<std::pair<Timestamp, double>>& equity_curve) const;

    /**
     * @brief Calculate maximum drawdown from equity curve
     * @param equity_curve Vector of (timestamp, portfolio_value) pairs
     * @return Maximum drawdown as decimal
     */
    double calculate_max_drawdown(
        const std::vector<std::pair<Timestamp, double>>& equity_curve) const;

    // ========== Risk Metrics ==========

    /**
     * @brief Calculate Value at Risk at 95% confidence
     * @param returns Vector of daily returns
     * @return VaR as positive decimal (loss amount)
     */
    double calculate_var_95(const std::vector<double>& returns) const;

    /**
     * @brief Calculate Conditional VaR (Expected Shortfall) at 95%
     * @param returns Vector of daily returns
     * @return CVaR as positive decimal
     */
    double calculate_cvar_95(const std::vector<double>& returns) const;

    /**
     * @brief Calculate all risk metrics at once
     * @param returns Vector of daily returns
     * @param trading_days Number of trading days
     * @return Map of metric name to value
     */
    std::unordered_map<std::string, double> calculate_risk_metrics(
        const std::vector<double>& returns,
        int trading_days) const;

    // ========== Trade Statistics ==========

    /**
     * @brief Trade statistics result structure
     */
    struct TradeStatistics {
        int total_trades = 0;
        int winning_trades = 0;
        double win_rate = 0.0;
        double profit_factor = 0.0;
        double total_profit = 0.0;
        double total_loss = 0.0;
        double avg_win = 0.0;
        double avg_loss = 0.0;
        double max_win = 0.0;
        double max_loss = 0.0;
        double avg_holding_period = 0.0;
        std::vector<ExecutionReport> actual_trades;  // Position-closing trades only
        /// The account's STRATEGY fills (account_fills): a contract-day the sleeves cross in full
        /// is none, a partial offset or two sleeves the same side is one.
        int strategy_fills{0};
        int roll_fills{0};          ///< T-ROLLX-FIX: ROLL legs seen (not trades)
        double roll_costs{0.0};     ///< T-ROLLX-FIX: their cost (never inside a trade's P&L)
    };

    /**
     * @brief The dollars of one price point of one contract (or share) of a symbol
     *
     * The run's coordinator passes BacktestPnLManager::get_point_value, the source the equity
     * curve reads (the metadata), so a trade's dollars and the booked dollars are one figure.
     */
    using PointValueSource = std::function<double(const std::string&)>;

    /**
     * @brief The book's fills: the executions netted between sleeves, in a stated order
     *
     * The STRATEGY rows of one contract on one bar (a backtest's bar is a day; the rows of a bar
     * share its fill time) are the account's one order for that contract: their signed quantities
     * are summed. A full cross (sum 0) is no fill; a partial offset is one fill of the net size on
     * the net side; rows on one side are one fill of the summed size. The fill's cost is the sum
     * of the rows' own costs and of their netting adjustments, so transaction_cost::net_cost of
     * it is the sum of the rows' costs after netting. Its price is the rows' one price; when the
     * rows' prices differ, the quantity-weighted price of the rows on the net side (a full cross
     * at different prices keeps its cost on a row of quantity 0, which is not a fill). A
     * contract-bar with one STRATEGY row is that row, unchanged. ROLL and BORROW rows are never
     * netted and are returned as they are.
     *
     * Order: by fill time; within a bar the ROLL legs, then the STRATEGY fills, then the BORROW
     * rows; then by symbol; then by exec id. The result does not depend on the order of the input.
     *
     * The one definition of "the account's fills" and of the trades paired from them: the trade
     * statistics, the per-symbol P&L and any count of STRATEGY fills read this list.
     */
    static std::vector<ExecutionReport> account_fills(const std::vector<ExecutionReport>& executions);

    /**
     * @brief Calculate trade statistics from executions
     *
     * The statistics are the BOOK's: the executions are first netted into the account's fills
     * (account_fills). A trade is an account fill that reduces, closes or flips the account's
     * net position in a contract, scored in DOLLARS: closed quantity x (fill price - entry) x
     * the position's side x the symbol's point value, less that fill's cost after netting. One
     * tracker per contract; a ROLL leg carries the open entry by the leg gap and a BORROW row is
     * a cost, neither is a trade. A fill through zero opens the remainder at its own price; a
     * position under 1e-9 after a fill is flat.
     *
     * @param executions Vector of execution reports (any order)
     * @param point_value Dollars per price point of each symbol
     * @return TradeStatistics structure
     */
    TradeStatistics calculate_trade_statistics(
        const std::vector<ExecutionReport>& executions,
        const PointValueSource& point_value) const;

    /**
     * @brief The three counts of a book's executions, each its own total (LOOP_SPEC section 10)
     *
     * The counts calculate_trade_statistics reaches on the same rows, by the same netting
     * (account_fills) and the same pairing, without the dollars: round trips (an account fill
     * that reduces, closes or flips a non-zero net position: total_trades), the account's
     * STRATEGY fills (a full cross is none, a partial offset or two sleeves on one side is one:
     * strategy_fills) and the ROLL legs (roll_fills). A live reader passes the stored
     * executions with each row's fill time set to its stored date, the day the sleeves' rows
     * are netted on. A count reads no entry price, so no leg gap is carried: ROLL legs that
     * the dollar statistics cannot pair are still counted.
     */
    struct FillCounts {
        int round_trips{0};
        int strategy_fills{0};
        int roll_fills{0};
    };
    static FillCounts account_fill_counts(const std::vector<ExecutionReport>& executions);

    // ========== Per-Symbol Analysis ==========

    /**
     * @brief Calculate P&L breakdown by symbol
     *
     * The pairing of calculate_trade_statistics (the same walk over the account's fills), in
     * dollars, summed per symbol, with every row's cost after netting charged.
     *
     * @param executions Vector of execution reports
     * @param point_value Dollars per price point of each symbol
     * @return Map of symbol to realized P&L
     */
    std::map<std::string, double> calculate_symbol_pnl(
        const std::vector<ExecutionReport>& executions,
        const PointValueSource& point_value) const;

    /**
     * @brief Calculate monthly returns
     * @param equity_curve Vector of (timestamp, portfolio_value) pairs
     * @return Map of "YYYY-MM" to monthly return
     */
    std::unordered_map<std::string, double> calculate_monthly_returns(
        const std::vector<std::pair<Timestamp, double>>& equity_curve) const;

    // ========== Beta and Correlation (Autocorrelation-Based) ==========

    /**
     * @brief Calculate beta and correlation from lag-1 autocorrelation
     *
     * NOTE: These are AUTOCORRELATION metrics, NOT market beta/correlation.
     * - beta: regression slope of today's return on yesterday's return
     * - correlation: lag-1 autocorrelation (today vs yesterday)
     * For true market beta/correlation vs a benchmark (e.g. S&P 500),
     * benchmark returns would need to be supplied.
     *
     * @param returns Vector of daily returns
     * @return Pair of (beta, correlation)
     */
    std::pair<double, double> calculate_beta_correlation(
        const std::vector<double>& returns) const;

    // ========== Composite Calculation ==========

    /**
     * @brief Calculate all metrics and populate BacktestResults
     *
     * This is the main entry point that computes all metrics at once. A UTC date the curve
     * carries more than once counts once, by its last row, before the warmup is cut.
     *
     * @param equity_curve Full equity curve including warmup period
     * @param executions All execution reports
     * @param warmup_days Number of days to exclude from metric calculations
     * @param point_value Dollars per price point of each symbol (the trade statistics)
     * @return Populated BacktestResults structure
     */
    backtest::BacktestResults calculate_all_metrics(
        const std::vector<std::pair<Timestamp, double>>& equity_curve,
        const std::vector<ExecutionReport>& executions,
        int warmup_days,
        const PointValueSource& point_value) const;

private:
    // ========== Helper Methods ==========

    /**
     * @brief Calculate mean of a vector
     */
    double calculate_mean(const std::vector<double>& values) const;

    /**
     * @brief Calculate standard deviation
     */
    double calculate_std_dev(const std::vector<double>& values, double mean) const;

    /**
     * @brief Filter equity curve to exclude warmup period
     */
    std::vector<std::pair<Timestamp, double>> filter_warmup_period(
        const std::vector<std::pair<Timestamp, double>>& equity_curve,
        int warmup_days) const;
};

} // namespace trade_ngin
