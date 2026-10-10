#include "trade_ngin/live/live_statistics_columns.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <stdexcept>

#include "trade_ngin/backtest/backtest_metrics_calculator.hpp"
#include "trade_ngin/risk/overlay.hpp"

namespace trade_ngin {

namespace {

int days_in_month(int year, int month) {
    static const int days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return month == 2 && leap ? 29 : days[month - 1];
}

/// "YYYY-MM" -> its last calendar day, "YYYY-MM-DD".
std::string last_day_of_month(const std::string& month) {
    const int y = std::stoi(month.substr(0, 4));
    const int m = std::stoi(month.substr(5, 2));
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m, days_in_month(y, m));
    return buf;
}

/// The index of the first smallest value; the values are not empty.
size_t first_smallest(const std::vector<double>& values) {
    size_t at = 0;
    for (size_t i = 1; i < values.size(); ++i) {
        if (values[i] < values[at]) at = i;
    }
    return at;
}

/// The symbol of the rows folded into the series' return `index`.
std::optional<std::string> symbol_of_return(const StatisticsSeries& series, size_t index,
                                            const std::vector<SymbolDayPnl>& symbol_pnl) {
    return most_negative_symbol(symbol_pnl, index > 0 ? series.dates[index - 1] : std::string(),
                                series.dates[index]);
}

}  // namespace

std::optional<std::string> most_negative_symbol(const std::vector<SymbolDayPnl>& symbol_pnl,
                                                const std::string& after,
                                                const std::string& through) {
    // The stored dates the unrealised level is read on: the last on or before each end.
    std::string level_from, level_to;
    for (const auto& row : symbol_pnl) {
        if (!after.empty() && row.date <= after && row.date > level_from) level_from = row.date;
        if (row.date <= through && row.date > level_to) level_to = row.date;
    }
    std::map<std::string, double> pnl;  // text order, so equal losses name the first symbol
    for (const auto& row : symbol_pnl) {
        if (row.date > through) continue;
        if (after.empty() || row.date > after) pnl[row.symbol] += row.realized;
        if (level_to != level_from) {
            if (row.date == level_to) pnl[row.symbol] += row.unrealized_level;
            if (row.date == level_from) pnl[row.symbol] -= row.unrealized_level;
        }
    }
    std::optional<std::string> worst;
    double worst_pnl = 0.0;
    for (const auto& [symbol, value] : pnl) {
        if (value < worst_pnl) {
            worst_pnl = value;
            worst = symbol;
        }
    }
    return worst;
}

SizingReturns sizing_capital_returns(const StatisticsSeries& series,
                                     const std::vector<DatedCapital>& capitals) {
    SizingReturns out;
    std::vector<DatedCapital> stored = capitals;
    std::stable_sort(stored.begin(), stored.end(),
                     [](const DatedCapital& a, const DatedCapital& b) { return a.date < b.date; });
    size_t next = 0;
    double capital = 0.0;
    for (size_t k = 0; k < series.dates.size(); ++k) {
        while (next < stored.size() && stored[next].date <= series.dates[k]) {
            capital = stored[next].capital;
            ++next;
        }
        if (!(capital > 0.0)) continue;  // before the first row that carries a sizing capital
        out.dates.push_back(series.dates[k]);
        out.returns_pct.push_back(series.pnl[k] / capital * 100.0);
        out.grid_index.push_back(k);
    }
    return out;
}

std::vector<MonthlyReturn> complete_month_returns(const StatisticsSeries& series,
                                                  const std::string& book_start,
                                                  const std::string& through) {
    std::vector<MonthlyReturn> months;
    const std::string start =
        !book_start.empty() ? book_start
                            : (series.dates.empty() ? std::string() : series.dates.front());
    for (size_t k = 0; k < series.dates.size(); ++k) {
        const std::string month = series.dates[k].substr(0, 7);
        if (!(month + "-01" > start) || last_day_of_month(month) > through) continue;
        if (months.empty() || months.back().month != month) {
            months.push_back({month, 1.0});
        }
        months.back().return_pct *= 1.0 + series.returns_pct[k] / 100.0;
    }
    for (auto& month : months) month.return_pct = (month.return_pct - 1.0) * 100.0;
    return months;
}

std::optional<double> sample_skewness(const std::vector<double>& values) {
    const size_t count = values.size();
    if (count < 3) return std::nullopt;
    const double n = static_cast<double>(count);
    double mean = 0.0;
    for (double v : values) mean += v;
    mean /= n;
    double squares = 0.0;
    for (double v : values) squares += (v - mean) * (v - mean);
    const double sd = std::sqrt(squares / (n - 1.0));
    if (!(sd > 0.0)) return std::nullopt;
    double cubes = 0.0;
    for (double v : values) {
        const double z = (v - mean) / sd;
        cubes += z * z * z;
    }
    return n / ((n - 1.0) * (n - 2.0)) * cubes;
}

std::optional<double> tail_ratio(const std::vector<double>& values) {
    if (values.empty()) return std::nullopt;
    const double upper = overlay::percentile(values, 95.0);
    const double lower = overlay::percentile(values, 5.0);
    double upper_sum = 0.0, lower_sum = 0.0;
    int upper_count = 0, lower_count = 0;
    for (double v : values) {
        if (v >= upper) {
            upper_sum += v;
            ++upper_count;
        }
        if (v <= lower) {
            lower_sum += v;
            ++lower_count;
        }
    }
    if (upper_count == 0 || lower_count == 0) return std::nullopt;
    const double lower_mean = std::abs(lower_sum / lower_count);
    if (!(lower_mean > 0.0)) return std::nullopt;
    return (upper_sum / upper_count) / lower_mean;
}

nlohmann::json calendar_year_returns(const StatisticsSeries& series, const std::string& book_start,
                                     const std::string& through, int* losing_years) {
    nlohmann::json years = nlohmann::json::object();
    if (losing_years) *losing_years = 0;
    const std::string start =
        !book_start.empty() ? book_start
                            : (series.dates.empty() ? std::string() : series.dates.front());
    std::map<std::string, double> growth;  // year -> the product of (1 + r)
    for (size_t k = 0; k < series.dates.size(); ++k) {
        auto [it, added] = growth.emplace(series.dates[k].substr(0, 4), 1.0);
        (void)added;
        it->second *= 1.0 + series.returns_pct[k] / 100.0;
    }
    for (const auto& [year, product] : growth) {
        const double year_return = (product - 1.0) * 100.0;
        const bool full = year + "-01-01" > start && !(year + "-12-31" > through);
        years[year] = {{"return", year_return}, {"partial", !full}};
        if (full && year_return < 0.0 && losing_years) ++*losing_years;
    }
    return years;
}

LiveStatisticsColumns live_statistics_columns(const StatisticsSeries& series,
                                              double sessions_per_year,
                                              const std::string& book_start,
                                              const std::string& through,
                                              const std::vector<SymbolDayPnl>& symbol_pnl,
                                              const std::vector<DatedCapital>& capitals) {
    LiveStatisticsColumns out;

    // The worst day on the account: the first smallest grid return, and the symbol that lost the
    // most over the stored rows folded into it.
    if (!series.returns_pct.empty()) {
        const size_t worst = first_smallest(series.returns_pct);
        out.worst_day_date = series.dates[worst];
        out.worst_day_symbol = symbol_of_return(series, worst, symbol_pnl);
    }

    // On the sizing capital: r^s = the grid return's P&L / E_t, not compounded.
    const SizingReturns sizing = sizing_capital_returns(series, capitals);
    out.sizing_returns = static_cast<int>(sizing.returns_pct.size());
    if (sizing.returns_pct.size() >= 2 && sessions_per_year > 0.0) {
        const double n = static_cast<double>(sizing.returns_pct.size());
        double mean = 0.0;
        for (double r : sizing.returns_pct) mean += r;
        mean /= n;
        double squares = 0.0;
        for (double r : sizing.returns_pct) squares += (r - mean) * (r - mean);
        out.volatility_sizing = std::sqrt(squares / (n - 1.0)) * std::sqrt(sessions_per_year);

        // The largest fall of the running sum from its running peak; the sum starts at 0.
        double sum = 0.0, peak = 0.0, drawdown = 0.0;
        for (double r : sizing.returns_pct) {
            sum += r;
            peak = std::max(peak, sum);
            drawdown = std::max(drawdown, peak - sum);
        }
        out.max_drawdown_sizing = drawdown;

        const size_t worst = first_smallest(sizing.returns_pct);
        out.worst_day_sizing = sizing.returns_pct[worst];
        out.worst_day_sizing_date = sizing.dates[worst];
        out.worst_day_sizing_symbol =
            symbol_of_return(series, sizing.grid_index[worst], symbol_pnl);
    }

    // The complete calendar months.
    const std::vector<MonthlyReturn> months = complete_month_returns(series, book_start, through);
    out.complete_months = static_cast<int>(months.size());
    if (out.complete_months >= kMonthlyStatisticsMinMonths) {
        std::vector<double> monthly;
        for (const auto& month : months) monthly.push_back(month.return_pct);
        out.monthly_skew = sample_skewness(monthly);
        out.monthly_tail_ratio = tail_ratio(monthly);
    }

    // The calendar years.
    int losing_years = 0;
    out.calendar_year_returns = calendar_year_returns(series, book_start, through, &losing_years);
    out.losing_years = losing_years;
    return out;
}

std::string set_fill_counts(LiveStatisticsColumns& columns,
                            const std::vector<ExecutionReport>& executions) {
    try {
        const auto counts = BacktestMetricsCalculator::account_fill_counts(executions);
        columns.total_trades = counts.round_trips;
        columns.total_strategy_fills = counts.strategy_fills;
        columns.total_roll_fills = counts.roll_fills;
        return std::string();
    } catch (const std::exception& e) {
        columns.total_trades.reset();
        columns.total_strategy_fills.reset();
        columns.total_roll_fills.reset();
        return e.what();
    }
}

std::string live_statistics_log_line(const LiveStatisticsColumns& c) {
    auto number = [](const std::optional<double>& v) {
        const auto text = v ? live_results_number(*v) : std::optional<std::string>();
        return text ? *text : std::string("null");
    };
    auto whole = [](const std::optional<int>& v) {
        return v ? std::to_string(*v) : std::string("null");
    };
    auto text = [](const std::optional<std::string>& v) { return v ? *v : std::string("null"); };
    return "worst_day_date=" + text(c.worst_day_date) + " worst_day_symbol=" +
           text(c.worst_day_symbol) + " sizing_returns=" + std::to_string(c.sizing_returns) +
           " max_drawdown_sizing=" + number(c.max_drawdown_sizing) + " volatility_sizing=" +
           number(c.volatility_sizing) + " worst_day_sizing=" + number(c.worst_day_sizing) +
           " worst_day_sizing_date=" + text(c.worst_day_sizing_date) +
           " worst_day_sizing_symbol=" + text(c.worst_day_sizing_symbol) + " complete_months=" +
           std::to_string(c.complete_months) + " monthly_skew=" + number(c.monthly_skew) +
           " monthly_tail_ratio=" + number(c.monthly_tail_ratio) + " calendar_year_returns=" +
           (c.calendar_year_returns ? c.calendar_year_returns->dump() : std::string("null")) +
           " losing_years=" + whole(c.losing_years) + " total_trades=" + whole(c.total_trades) +
           " total_strategy_fills=" + whole(c.total_strategy_fills) + " total_roll_fills=" +
           whole(c.total_roll_fills);
}

std::optional<std::string> live_results_number(double value) {
    if (!std::isfinite(value)) return std::nullopt;
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.10g", value);
    return std::string(buf);
}

std::vector<LiveResultsCell> live_statistics_cells(const LiveStatisticsColumns& c) {
    auto number = [](const std::optional<double>& v) {
        return v ? live_results_number(*v) : std::optional<std::string>();
    };
    auto whole = [](const std::optional<int>& v) {
        return v ? std::optional<std::string>(std::to_string(*v)) : std::optional<std::string>();
    };
    return {
        {"worst_day_date", "date", c.worst_day_date},
        {"worst_day_symbol", "text", c.worst_day_symbol},
        {"max_drawdown_sizing", "numeric", number(c.max_drawdown_sizing)},
        {"volatility_sizing", "numeric", number(c.volatility_sizing)},
        {"worst_day_sizing", "numeric", number(c.worst_day_sizing)},
        {"worst_day_sizing_date", "date", c.worst_day_sizing_date},
        {"worst_day_sizing_symbol", "text", c.worst_day_sizing_symbol},
        {"monthly_skew", "numeric", number(c.monthly_skew)},
        {"monthly_tail_ratio", "numeric", number(c.monthly_tail_ratio)},
        {"calendar_year_returns", "jsonb",
         c.calendar_year_returns ? std::optional<std::string>(c.calendar_year_returns->dump())
                                 : std::optional<std::string>()},
        {"losing_years", "integer", whole(c.losing_years)},
        {"total_trades", "integer", whole(c.total_trades)},
        {"total_strategy_fills", "integer", whole(c.total_strategy_fills)},
        {"total_roll_fills", "integer", whole(c.total_roll_fills)},
    };
}

}  // namespace trade_ngin
