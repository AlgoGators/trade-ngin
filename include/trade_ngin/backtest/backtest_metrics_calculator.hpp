#pragma once

#include <functional>
#include <vector>
#include <string>
#include <unordered_map>
#include <map>
#include "trade_ngin/backtest/backtest_statistics_columns.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/live/live_historical_metrics.hpp"

namespace trade_ngin {

// Forward declaration
namespace backtest {
struct BacktestResults;
}

/**
 * @brief The grid a run's statistics are taken on and its sessions a year (T-8D R1, R2, R3;
 *        T-8D-2 R76). The calculator holds no factor of its own: every annualised figure uses
 *        `sessions_per_year`, which must be the row frequency of this grid.
 *
 * A statistics construct only: every row of the equity curve stays. A futures run's grid is the
 * Sunday-to-Friday dates on which enough of its universe printed a bar
 * (futures_statistics_grid_dates), an equity run's its own rows (NYSE sessions). A curve row
 * dated off the grid (a Saturday point, a thin date) is no observation: returns are taken on
 * the grid's LEVELS, so its P&L lands in the next grid return and nothing is dropped.
 */
struct BacktestStatisticsGrid {
    double sessions_per_year{0.0};   ///< K of the grid
    bool own_rows{false};            ///< the grid is every row of the curve; `dates` is not read
    std::vector<std::string> dates;  ///< the grid dates "YYYY-MM-DD", any order

    /// The grid of a run whose every curve row is a session (an equity run).
    static BacktestStatisticsGrid of_own_rows(double sessions_per_year) {
        return {sessions_per_year, true, {}};
    }
    /// The grid of a run with `dates` as its sessions (a futures run).
    static BacktestStatisticsGrid of_dates(double sessions_per_year, std::vector<std::string> dates) {
        return {sessions_per_year, false, std::move(dates)};
    }
};

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
     * @brief Calculate annualized return: the CAGR, (1 + R)^(K / n) - 1 (grid_annualized_return)
     * @param total_return R, from the base to the last grid level, as decimal
     * @param trading_days n, the number of returns of the statistics grid
     * @param sessions_per_year K, the grid's sessions a year
     * @return Annualized return as decimal; 0 without a return or a positive K, -1 when the
     *         book lost everything
     */
    double calculate_annualized_return(double total_return, int trading_days,
                                       double sessions_per_year) const;

    /**
     * @brief Calculate daily returns from equity curve
     * @param equity_curve Vector of (timestamp, portfolio_value) pairs
     * @return Vector of daily returns
     */
    std::vector<double> calculate_returns_from_equity(
        const std::vector<std::pair<Timestamp, double>>& equity_curve) const;

    // ========== Risk-Adjusted Return Metrics ==========

    /**
     * @brief Calculate Sharpe ratio: mean x K over the sample standard deviation x sqrt(K)
     * @param returns Vector of grid returns
     * @param sessions_per_year K, the sessions a year of the grid the returns are taken on
     * @param risk_free_rate Annual risk-free rate (default 0)
     * @return Sharpe ratio; 0 for an empty series or without a positive K
     */
    double calculate_sharpe_ratio(
        const std::vector<double>& returns,
        double sessions_per_year,
        double risk_free_rate = 0.0) const;

    /**
     * @brief Calculate Sortino ratio: mean x K over the downside volatility
     * @param returns Vector of grid returns
     * @param sessions_per_year K, the sessions a year of the grid the returns are taken on
     * @param minimum_acceptable_return Minimum acceptable return (default 0)
     * @return Sortino ratio; 0 for an empty series or without a positive K
     */
    double calculate_sortino_ratio(
        const std::vector<double>& returns,
        double sessions_per_year,
        double minimum_acceptable_return = 0.0) const;

    /**
     * @brief Calculate Calmar ratio (annualized return / max drawdown)
     * @param annualized_return Annualized return as decimal (calculate_annualized_return)
     * @param max_drawdown Maximum drawdown as decimal
     * @return Calmar ratio
     */
    double calculate_calmar_ratio(double annualized_return, double max_drawdown) const;

    // ========== Volatility Metrics ==========

    /**
     * @brief Calculate annualized volatility: the SAMPLE standard deviation (n - 1) x sqrt(K),
     *        the live statistics' own function (LiveHistoricalMetricsCalculator)
     * @param returns Vector of grid returns
     * @param sessions_per_year K, the sessions a year of the grid the returns are taken on
     * @return Annualized volatility; 0 under two returns or without a positive K
     */
    double calculate_volatility(const std::vector<double>& returns,
                                double sessions_per_year) const;

    /**
     * @brief Calculate downside volatility (for Sortino): sqrt(sum of the squared shortfalls
     *        below target over EVERY return / n) x sqrt(K), the live statistics' own function
     * @param returns Vector of grid returns
     * @param sessions_per_year K, the sessions a year of the grid the returns are taken on
     * @param target Target return (typically 0)
     * @return Annualized downside volatility; 0 under two returns or without a positive K
     */
    double calculate_downside_volatility(
        const std::vector<double>& returns,
        double sessions_per_year,
        double target) const;

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
     * @brief Calculate Conditional VaR (Expected Shortfall) at 95%: the mean of the worst
     *        floor(0.05 n) + 1 returns, the VaR observation included
     * @param returns Vector of daily returns
     * @return CVaR as positive decimal
     */
    double calculate_cvar_95(const std::vector<double>& returns) const;

    /**
     * @brief Calculate all risk metrics at once
     * @param returns Vector of grid returns
     * @param sessions_per_year K, the sessions a year of the grid the returns are taken on
     * @return Map of metric name to value
     */
    std::unordered_map<std::string, double> calculate_risk_metrics(
        const std::vector<double>& returns,
        double sessions_per_year) const;

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

    // ========== The Statistics Grid ==========

    /**
     * @brief The levels the statistics are taken on: the curve folded onto the grid
     *
     * `curve` is the run's rows after the warm-up, one per UTC date, ascending; its first row
     * is the base (the level the first return is taken on) and never a return row. Every later
     * grid date through the curve's last date is one point, at the last curve level on or before
     * it (build_statistics_series, the live statistics' own fold): the P&L of a row dated off
     * the grid lands in the next grid point, a grid date with no row of its own repeats the
     * level before it, and a curve whose last rows are off the grid ends at its last grid date.
     * With `grid.own_rows` the curve is returned as it is.
     *
     * @return (timestamp, level) points, the base first; a point's timestamp is its curve row's,
     *         or midnight UTC of a grid date that has no row
     */
    std::vector<std::pair<Timestamp, double>> statistics_grid_curve(
        const std::vector<std::pair<Timestamp, double>>& curve,
        const BacktestStatisticsGrid& grid) const;

    /**
     * @brief The statistics series of a run: the grid returns statistics_grid_curve's levels
     *        give, with their dates and P&L (build_statistics_series, the live statistics' own
     *        fold)
     *
     * `curve` as for statistics_grid_curve: its first row is the base. With `grid.own_rows`
     * every later row of the curve is a return row. The series the statistics of migration 031
     * are taken on (backtest_statistics_columns). Empty for an empty curve.
     */
    StatisticsSeries statistics_series(const std::vector<std::pair<Timestamp, double>>& curve,
                                       const BacktestStatisticsGrid& grid) const;

    // ========== Composite Calculation ==========

    /**
     * @brief Calculate all metrics and populate BacktestResults
     *
     * This is the main entry point that computes all metrics at once. A UTC date the curve
     * carries more than once counts once, by its last row, before the warmup is cut.
     *
     * total_return is the level of the last row over the first row after the warm-up, every row
     * counted. Volatility, Sharpe, Sortino, the downside volatility, the Calmar ratio (the
     * grid's CAGR over the maximum drawdown), the maximum drawdown, VaR, CVaR, beta and
     * correlation are taken on the statistics grid (statistics_grid_curve) and annualised with
     * its sessions a year, which the result records (statistics_sessions_per_year,
     * statistics_days, annualized_return). The trade statistics read the executions,
     * the drawdown curve and the monthly returns every row. The statistics of migration 031
     * (statistics_columns) are taken on the same grid series (statistics_series) with the same
     * sessions a year, and total_strategy_fills is the trade statistics' own count of the
     * account's fills.
     *
     * @param equity_curve Full equity curve including warmup period
     * @param executions All execution reports
     * @param warmup_days Number of days to exclude from metric calculations
     * @param point_value Dollars per price point of each symbol (the trade statistics)
     * @param grid The statistics grid of the run and its sessions a year
     * @param statistics_inputs The run's stored per-symbol P&L and sizing capitals (the worst
     *        day's symbol and the three sizing-capital statistics; NULL without them)
     * @return Populated BacktestResults structure
     */
    backtest::BacktestResults calculate_all_metrics(
        const std::vector<std::pair<Timestamp, double>>& equity_curve,
        const std::vector<ExecutionReport>& executions,
        int warmup_days,
        const PointValueSource& point_value,
        const BacktestStatisticsGrid& grid,
        const BacktestStatisticsInputs& statistics_inputs = BacktestStatisticsInputs()) const;

private:
    // ========== Helper Methods ==========

    /**
     * @brief Calculate mean of a vector
     */
    double calculate_mean(const std::vector<double>& values) const;

    /**
     * @brief Filter equity curve to exclude warmup period
     */
    std::vector<std::pair<Timestamp, double>> filter_warmup_period(
        const std::vector<std::pair<Timestamp, double>>& equity_curve,
        int warmup_days) const;
};

} // namespace trade_ngin
