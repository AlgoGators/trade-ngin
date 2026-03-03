#include "trade_ngin/live/live_historical_metrics.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace trade_ngin {

namespace {
constexpr double TRADING_DAYS_PER_YEAR = 252.0;
}

double LiveHistoricalMetricsCalculator::calculate_mean(const std::vector<double>& values) {
    if (values.empty()) {
        return 0.0;
    }
    double sum = std::accumulate(values.begin(), values.end(), 0.0);
    return sum / static_cast<double>(values.size());
}

double LiveHistoricalMetricsCalculator::calculate_annualized_volatility(
    const std::vector<double>& returns_pct) {
    if (returns_pct.size() < 2) {
        return 0.0;
    }

    double mean = calculate_mean(returns_pct);
    double sq_sum = 0.0;
    for (double r : returns_pct) {
        double diff = r - mean;
        sq_sum += diff * diff;
    }

    double variance = sq_sum / static_cast<double>(returns_pct.size());
    double daily_std = std::sqrt(variance);
    return daily_std * std::sqrt(TRADING_DAYS_PER_YEAR);
}

double LiveHistoricalMetricsCalculator::calculate_annualized_downside_deviation(
    const std::vector<double>& returns_pct, double target) {
    std::vector<double> negatives;
    negatives.reserve(returns_pct.size());

    for (double r : returns_pct) {
        if (r < target) {
            double diff = r - target;
            negatives.push_back(diff);
        }
    }

    if (negatives.size() < 2) {
        return 0.0;
    }

    double sq_sum = 0.0;
    for (double d : negatives) {
        sq_sum += d * d;
    }

    double variance = sq_sum / static_cast<double>(negatives.size());
    double daily_downside_std = std::sqrt(variance);
    return daily_downside_std * std::sqrt(TRADING_DAYS_PER_YEAR);
}

double LiveHistoricalMetricsCalculator::calculate_max_drawdown_from_equity(
    const std::vector<double>& equity_values) {
    if (equity_values.empty()) {
        return 0.0;
    }

    double peak = equity_values.front();
    double max_dd_pct = 0.0;

    for (double equity : equity_values) {
        if (equity > peak) {
            peak = equity;
        }
        if (peak > 0.0 && equity < peak) {
            double dd = (peak - equity) / peak * 100.0;
            if (dd > max_dd_pct) {
                max_dd_pct = dd;
            }
        }
    }

    return max_dd_pct;
}

HistoricalMetrics LiveHistoricalMetricsCalculator::calculate(
    const std::vector<double>& daily_returns_pct,
    const std::vector<double>& daily_pnl_dollars,
    const std::vector<double>& equity_values,
    double total_annualized_return_pct,
    int total_trades_executions) const {
    HistoricalMetrics metrics;

    // Basic counts
    metrics.total_days = static_cast<int>(daily_returns_pct.size());
    metrics.total_trades = total_trades_executions;

    // Early exit if no returns history – keep everything at 0
    if (daily_returns_pct.empty()) {
        return metrics;
    }

    // Volatility and downside deviation (annualized, % units)
    metrics.volatility = calculate_annualized_volatility(daily_returns_pct);
    metrics.downside_deviation = calculate_annualized_downside_deviation(daily_returns_pct, 0.0);

    // Sharpe and Sortino (using user formulas)
    if (metrics.volatility > 0.0) {
        metrics.sharpe_ratio = total_annualized_return_pct / metrics.volatility;
    }
    if (metrics.downside_deviation > 0.0) {
        metrics.sortino_ratio = total_annualized_return_pct / metrics.downside_deviation;
    }

    // Max drawdown from equity curve (if available)
    if (!equity_values.empty()) {
        metrics.max_drawdown = calculate_max_drawdown_from_equity(equity_values);
    }

    // Day-level win/loss stats based on daily returns
    double sum_wins = 0.0;
    double sum_losses_abs = 0.0;
    metrics.best_day = daily_returns_pct.front();
    metrics.worst_day = daily_returns_pct.front();

    for (double r : daily_returns_pct) {
        if (r > 0.0) {
            metrics.winning_days += 1;
            sum_wins += r;
        } else if (r < 0.0) {
            metrics.losing_days += 1;
            sum_losses_abs += std::abs(r);
        }

        if (r > metrics.best_day) {
            metrics.best_day = r;
        }
        if (r < metrics.worst_day) {
            metrics.worst_day = r;
        }
    }

    if (metrics.total_days > 0) {
        metrics.win_rate =
            static_cast<double>(metrics.winning_days) / static_cast<double>(metrics.total_days) *
            100.0;
    }
    if (metrics.winning_days > 0) {
        metrics.avg_win = sum_wins / static_cast<double>(metrics.winning_days);
    }
    if (metrics.losing_days > 0) {
        metrics.avg_loss = sum_losses_abs / static_cast<double>(metrics.losing_days);
    }

    // Profit factor based on daily PnL
    if (!daily_pnl_dollars.empty()) {
        for (double pnl : daily_pnl_dollars) {
            if (pnl > 0.0) {
                metrics.gross_profit += pnl;
            } else if (pnl < 0.0) {
                metrics.gross_loss += std::abs(pnl);
            }
        }

        if (metrics.gross_loss > 0.0) {
            metrics.profit_factor = metrics.gross_profit / metrics.gross_loss;
        } else if (metrics.gross_profit > 0.0) {
            // Convention: very large profit factor if there are no losses
            metrics.profit_factor = 999.99;
        }
    }

    return metrics;
}

}  // namespace trade_ngin

