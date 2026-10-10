#include "trade_ngin/live/live_historical_metrics.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <numeric>

#include "trade_ngin/core/time_utils.hpp"

namespace trade_ngin {

double LiveHistoricalMetricsCalculator::calculate_mean(const std::vector<double>& values) {
    if (values.empty()) {
        return 0.0;
    }
    double sum = std::accumulate(values.begin(), values.end(), 0.0);
    return sum / static_cast<double>(values.size());
}

double LiveHistoricalMetricsCalculator::calculate_annualized_volatility(
    const std::vector<double>& returns_pct, double sessions_per_year) {
    if (returns_pct.size() < 2 || sessions_per_year <= 0.0) {
        return 0.0;
    }

    double mean = calculate_mean(returns_pct);
    double sq_sum = 0.0;
    for (double r : returns_pct) {
        double diff = r - mean;
        sq_sum += diff * diff;
    }

    // The sample variance (n - 1): the returns are a sample of the book's sessions.
    double variance = sq_sum / static_cast<double>(returns_pct.size() - 1);
    double daily_std = std::sqrt(variance);
    return daily_std * std::sqrt(sessions_per_year);
}

double LiveHistoricalMetricsCalculator::calculate_annualized_downside_deviation(
    const std::vector<double>& returns_pct, double sessions_per_year, double target) {
    if (returns_pct.size() < 2 || sessions_per_year <= 0.0) {
        return 0.0;
    }

    // The squared shortfalls below target, averaged over EVERY day of the series: a day at
    // or above target counts as a zero, not as a missing observation.
    double sq_sum = 0.0;
    for (double r : returns_pct) {
        if (r < target) {
            double diff = r - target;
            sq_sum += diff * diff;
        }
    }

    double variance = sq_sum / static_cast<double>(returns_pct.size());
    double daily_downside_std = std::sqrt(variance);
    return daily_downside_std * std::sqrt(sessions_per_year);
}

double LiveHistoricalMetricsCalculator::calculate_max_drawdown_from_equity(
    const std::vector<double>& equity_values, double peak_seed) {
    if (equity_values.empty()) {
        return 0.0;
    }

    double peak = peak_seed;
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

std::vector<std::string> statistics_session_dates(
    const std::string& from, const std::string& through,
    const std::function<bool(const std::string&, int)>& is_session) {
    std::vector<std::string> dates;
    std::chrono::system_clock::time_point day;
    std::chrono::system_clock::time_point last;
    if (!core::parse_utc_date(from, day) || !core::parse_utc_date(through, last)) {
        return dates;
    }
    for (; day <= last; day += std::chrono::hours(24)) {
        const std::time_t t = std::chrono::system_clock::to_time_t(day);
        std::tm tm{};
        if (core::safe_gmtime(&t, &tm) == nullptr) {
            break;
        }
        const std::string date = core::format_utc_date(day);
        if (is_session(date, tm.tm_wday)) {
            dates.push_back(date);
        }
    }
    return dates;
}

StatisticsSeries build_statistics_series(const std::vector<DatedLevel>& stored_levels,
                                         const std::vector<std::string>& grid_dates,
                                         const std::string& book_start, double initial_capital,
                                         const std::string& through_date) {
    StatisticsSeries series;
    series.base = initial_capital;

    // The stored rows in date order; "YYYY-MM-DD" text orders as the dates do.
    std::vector<DatedLevel> stored = stored_levels;
    std::stable_sort(stored.begin(), stored.end(),
                     [](const DatedLevel& a, const DatedLevel& b) { return a.date < b.date; });

    // The anchor: the book's start, or the first stored row of a book that has none recorded.
    const std::string anchor =
        !book_start.empty() ? book_start : (stored.empty() ? std::string() : stored.front().date);
    if (anchor.empty() || initial_capital <= 0.0) {
        return series;
    }

    std::vector<std::string> grid = grid_dates;
    std::sort(grid.begin(), grid.end());
    grid.erase(std::unique(grid.begin(), grid.end()), grid.end());

    size_t next = 0;             // the first stored row not yet passed
    double level = series.base;  // the last stored level on or before the grid date (R6)
    bool on_row = false;         // that level is a row of the grid date itself
    double previous = series.base;
    for (const auto& date : grid) {
        if (date < anchor) {
            continue;
        }
        if (date > through_date) {
            break;
        }
        on_row = false;
        while (next < stored.size() && stored[next].date <= date) {
            if (stored[next].date >= anchor) {
                level = stored[next].level;
                on_row = stored[next].date == date;
            }
            ++next;
        }
        // R5 as amended by R77 (c): the start date is the base, not a return row, when its
        // level is the initial capital; a start row that carries P&L is a return row.
        if (date == anchor && level == series.base) {
            continue;
        }
        if (!on_row) {
            series.carried.push_back(date);
        }
        series.dates.push_back(date);
        series.levels.push_back(level);
        series.returns_pct.push_back((level / previous - 1.0) * 100.0);
        series.pnl.push_back(level - previous);
        previous = level;
    }
    return series;
}

double grid_annualized_return_pct(const StatisticsSeries& series, double sessions_per_year) {
    const int n = series.size();
    if (n <= 0 || series.base <= 0.0 || sessions_per_year <= 0.0) {
        return 0.0;
    }
    const double growth = series.levels.back() / series.base;  // 1 + R
    if (growth <= 0.0) {
        return -100.0;
    }
    return (std::pow(growth, sessions_per_year / static_cast<double>(n)) - 1.0) * 100.0;
}

std::string statistics_days_warning(const HistoricalMetrics& m, const StatisticsSeries& series) {
    std::string text;
    const int counted = m.winning_days + m.losing_days + m.flat_days;
    if (counted != m.total_days) {
        text = "winning_days + losing_days + flat days = " + std::to_string(m.winning_days) +
               " + " + std::to_string(m.losing_days) + " + " + std::to_string(m.flat_days) +
               " = " + std::to_string(counted) + " differs from total_days = " +
               std::to_string(m.total_days);
    }
    if (!series.carried.empty()) {
        if (!text.empty()) {
            text += "; ";
        }
        text += std::to_string(series.carried.size()) +
                " session(s) of the statistics grid have no stored row and carry the last "
                "stored level (first " + series.carried.front() + ", last " +
                series.carried.back() + ")";
    }
    return text;
}

HistoricalMetrics LiveHistoricalMetricsCalculator::calculate(const StatisticsSeries& grid,
                                                             double sessions_per_year,
                                                             int total_trades_executions) const {
    HistoricalMetrics metrics;

    // Returns and days, on the statistics grid
    metrics.total_days = grid.size();
    metrics.total_annualized_return = grid_annualized_return_pct(grid, sessions_per_year);
    metrics.total_trades = total_trades_executions;

    // An empty series: everything stays at 0
    if (grid.returns_pct.empty()) {
        return metrics;
    }

    // Volatility, downside deviation, Sharpe and Sortino on the grid returns (% units), every
    // one annualised with the grid's own sessions a year: sample sd x sqrt(K); mean / sd x
    // sqrt(K), rf 0; sqrt(sum of min(r, 0)^2 over every return / n) x sqrt(K); mean x K over it.
    metrics.volatility = calculate_annualized_volatility(grid.returns_pct, sessions_per_year);
    metrics.downside_deviation =
        calculate_annualized_downside_deviation(grid.returns_pct, sessions_per_year, 0.0);
    const double grid_mean = calculate_mean(grid.returns_pct);
    if (metrics.volatility > 0.0) {
        metrics.sharpe_ratio = grid_mean * sessions_per_year / metrics.volatility;
    }
    if (metrics.downside_deviation > 0.0) {
        metrics.sortino_ratio = grid_mean * sessions_per_year / metrics.downside_deviation;
    }

    // Max drawdown over every grid level from the book's start, the peak seeded at the base
    metrics.max_drawdown = calculate_max_drawdown_from_equity(grid.levels, grid.base);

    // Winning, losing and flat days over the grid returns, with the average win and loss and
    // the best and worst day; win_rate = W / (W + L) in percent, so a flat session is in
    // neither count and in no denominator (B5A D7, T-8D R10).
    double sum_wins = 0.0;
    double sum_losses_abs = 0.0;
    metrics.best_day = grid.returns_pct.front();
    metrics.worst_day = grid.returns_pct.front();
    for (double r : grid.returns_pct) {
        if (r > 0.0) {
            metrics.winning_days += 1;
            sum_wins += r;
        } else if (r < 0.0) {
            metrics.losing_days += 1;
            sum_losses_abs += std::abs(r);
        } else {
            metrics.flat_days += 1;
        }

        if (r > metrics.best_day) {
            metrics.best_day = r;
        }
        if (r < metrics.worst_day) {
            metrics.worst_day = r;
        }
    }
    if (metrics.winning_days + metrics.losing_days > 0) {
        metrics.win_rate = static_cast<double>(metrics.winning_days) /
                           static_cast<double>(metrics.winning_days + metrics.losing_days) * 100.0;
    }
    if (metrics.winning_days > 0) {
        metrics.avg_win = sum_wins / static_cast<double>(metrics.winning_days);
    }
    if (metrics.losing_days > 0) {
        metrics.avg_loss = sum_losses_abs / static_cast<double>(metrics.losing_days);
    }

    // Gross profit and loss on the P&L of the grid returns: each is the difference of two grid
    // levels, so it holds the P&L of every stored row folded into that return.
    for (double pnl : grid.pnl) {
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

    return metrics;
}

std::unordered_map<std::string, double> historical_metrics_double_columns(
    const HistoricalMetrics& m) {
    return {
        {"volatility", m.volatility},
        {"sharpe_ratio", m.sharpe_ratio},
        {"sortino_ratio", m.sortino_ratio},
        {"max_drawdown", m.max_drawdown},
        {"downside_deviation", m.downside_deviation},
        {"win_rate", m.win_rate},
        {"avg_win", m.avg_win},
        {"avg_loss", m.avg_loss},
        {"profit_factor", m.profit_factor},
        {"best_day", m.best_day},
        {"worst_day", m.worst_day},
        {"gross_profit", m.gross_profit},
        {"gross_loss", m.gross_loss},
    };
}

std::unordered_map<std::string, int> historical_metrics_int_columns(const HistoricalMetrics& m) {
    return {
        {"winning_days", m.winning_days},
        {"losing_days", m.losing_days},
        {"total_days", m.total_days},
    };
}

std::unordered_map<std::string, double> historical_metrics_update_columns(
    const HistoricalMetrics& m) {
    auto columns = historical_metrics_double_columns(m);
    for (const auto& [column, value] : historical_metrics_int_columns(m)) {
        columns[column] = static_cast<double>(value);
    }
    return columns;
}

}  // namespace trade_ngin

