#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif

// Map popen/pclose to _popen/_pclose on Windows
#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

#include "trade_ngin/core/timed_process.hpp"
#include "trade_ngin/core/chart_generator.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>  // for popen, pclose
#include <ctime>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <unordered_map>
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"

namespace trade_ngin {

namespace {
// How the chart asked for last on this thread ended (read through last_chart_status).
thread_local core::ChartStatus g_last_status = core::ChartStatus::NothingToPlot;
thread_local std::string g_last_reason;

void begin_chart() {
    g_last_status = core::ChartStatus::NothingToPlot;
    g_last_reason.clear();
}


// A failure keeps its first reason: the later lines of the same chart are its consequence.
void mark_chart_failed(const char* reason) {
    if (g_last_status == core::ChartStatus::Failed) return;
    g_last_status = core::ChartStatus::Failed;
    g_last_reason = reason;
}
}  // namespace

// ============================================================================
// ChartHelpers Implementation
// ============================================================================

std::string ChartHelpers::encode_to_base64(const std::vector<unsigned char>& data) {
    static const char base64_chars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";

    std::string result;
    int i = 0;
    int j = 0;
    unsigned char char_array_3[3];
    unsigned char char_array_4[4];

    for (size_t idx = 0; idx < data.size(); idx++) {
        char_array_3[i++] = data[idx];
        if (i == 3) {
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;

            for(i = 0; i < 4; i++)
                result += base64_chars[char_array_4[i]];
            i = 0;
        }
    }

    if (i) {
        for(j = i; j < 3; j++)
            char_array_3[j] = '\0';

        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);

        for (j = 0; j < i + 1; j++)
            result += base64_chars[char_array_4[j]];

        while(i++ < 3)
            result += '=';
    }

    return result;
}

std::string ChartHelpers::format_currency(double value, int precision) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << value;
    std::string s = oss.str();

    size_t dp = s.find('.');
    if (dp == std::string::npos) dp = s.size();
    int pos = static_cast<int>(dp) - 3;
    while (pos > 0) {
        s.insert(static_cast<size_t>(pos), ",");
        pos -= 3;
    }
    return s;
}

// ============================================================================
// ChartQueries: the text of each chart's query
// ============================================================================
//
// Every query reads one book: its strategy_id and its portfolio_id (T-8D-2 R55). The two tables
// that carry a portfolio_type (trading.positions and trading.equity_curve, migration 001) are
// read for the system book only. trading.live_results and trading.executions have no such column:
// their rows are the book's by the two ids.

std::string ChartQueries::equity_curve(const std::string& strategy_id,
                                       const std::string& portfolio_id, const std::string& date,
                                       int lookback_days) {
    // T-8D-2 R63: the rows BEFORE the run date. The run date's own row is not settled (its P&L
    // settles tomorrow), and a re-run of a past date must not chart the days after it.
    return "SELECT timestamp, equity "
           "FROM trading.equity_curve "
           "WHERE strategy_id = '" +
           strategy_id +
           "' "
           "AND portfolio_id = '" +
           portfolio_id +
           "' "
           "AND portfolio_type = 'system' "
           "AND DATE(timestamp) < DATE('" +
           date +
           "') "
           "ORDER BY timestamp DESC "
           "LIMIT " +
           std::to_string(lookback_days);
}

std::string ChartQueries::book_capital(const std::string& strategy_id,
                                       const std::string& portfolio_id, const std::string& date) {
    // The book's starting capital as its latest run on or before the run date recorded it.
    return "SELECT portfolio_config->>'total_capital' AS total_capital "
           "FROM trading.live_run_metadata "
           "WHERE strategy_id = '" +
           strategy_id +
           "' "
           "AND portfolio_id = '" +
           portfolio_id +
           "' "
           "AND DATE(date) <= DATE('" +
           date +
           "') "
           "ORDER BY date DESC "
           "LIMIT 1";
}

std::string ChartQueries::pnl_by_symbol(const std::string& strategy_id,
                                        const std::string& portfolio_id, const std::string& date) {
    // T-8D-2 R64: one row per symbol. A book of several sleeves stores a row per sleeve for a
    // symbol more than one of them holds; the chart shows the book's figure for the symbol.
    return "SELECT symbol, SUM(daily_realized_pnl) AS daily_realized_pnl "
           "FROM trading.positions "
           "WHERE strategy_id = '" +
           strategy_id +
           "' "
           "AND portfolio_id = '" +
           portfolio_id +
           "' "
           "AND portfolio_type = 'system' "
           // E2-F19 R-6: the previous SESSION, not the previous calendar day -- a Monday
           // report used to read Sunday's carried rows.
           "AND DATE(last_update) = (SELECT MAX(DATE(last_update)) FROM trading.positions "
           "                         WHERE strategy_id = '" +
           strategy_id +
           "' "
           "                           AND portfolio_id = '" +
           portfolio_id +
           "' "
           "                           AND portfolio_type = 'system' "
           "                           AND DATE(last_update) < DATE('" +
           date +
           "')) "
           "GROUP BY symbol "
           "ORDER BY symbol";
}

std::string ChartQueries::daily_pnl(const std::string& strategy_id, const std::string& portfolio_id,
                                    const std::string& date, int lookback_days) {
    return "SELECT date, daily_pnl "
           "FROM trading.live_results "
           "WHERE strategy_id = '" +
           strategy_id +
           "' "
           "AND portfolio_id = '" +
           portfolio_id +
           "' "
           "AND DATE(date) < DATE('" +
           date +
           "') "
           "ORDER BY date DESC "
           "LIMIT " +
           std::to_string(lookback_days);
}

std::string ChartQueries::cost_per_million(const std::string& strategy_id,
                                           const std::string& portfolio_id,
                                           const std::string& date) {
    // T-8D-2 R64: each stored day of the book with the fills the BOOK made that day (its own
    // trading.executions rows, by strategy_id and portfolio_id); a day without a fill is one row
    // whose fill columns are NULL. The day's cost is the stored day total, which the runner
    // charged after netting.
    return "SELECT lr.date, COALESCE(lr.daily_transaction_costs, 0) AS daily_cost, "
           "e.symbol, e.quantity, e.price "
           "FROM trading.live_results lr "
           "LEFT JOIN trading.executions e "
           "ON DATE(e.execution_time) = DATE(lr.date) "
           "AND e.strategy_id = lr.strategy_id "
           "AND e.portfolio_id = lr.portfolio_id "
           "WHERE lr.strategy_id = '" +
           strategy_id +
           "' "
           "AND lr.portfolio_id = '" +
           portfolio_id +
           "' "
           "AND DATE(lr.date) <= DATE('" +
           date +
           "') "
           "ORDER BY lr.date ASC, e.symbol ASC, e.strategy_name ASC, e.exec_id ASC";
}

std::string ChartQueries::margin_posted(const std::string& strategy_id,
                                        const std::string& portfolio_id, const std::string& date) {
    return "SELECT date, margin_posted "
           "FROM trading.live_results "
           "WHERE strategy_id = '" +
           strategy_id +
           "' "
           "AND portfolio_id = '" +
           portfolio_id +
           "' "
           "AND DATE(date) <= DATE('" +
           date +
           "') "
           "ORDER BY date ASC";
}

std::string ChartQueries::cumulative_pnl_by_symbol(const std::string& strategy_id,
                                                   const std::string& portfolio_id,
                                                   const std::string& date) {
    // The sum of the stored daily realized P&L of each symbol, gross of costs. On BASE it is not
    // the book's stored total_realized_pnl: BASE's early history rows do not reconcile (T-8D-2
    // D2-OUT-25: left as it is, documented).
    return "SELECT symbol, SUM(daily_realized_pnl) as cumulative_pnl "
           "FROM trading.positions "
           "WHERE strategy_id = '" +
           strategy_id +
           "' "
           "AND portfolio_id = '" +
           portfolio_id +
           "' "
           "AND portfolio_type = 'system' "
           "AND DATE(last_update) <= DATE('" +
           date +
           "') "
           "GROUP BY symbol "
           "HAVING SUM(daily_realized_pnl) IS NOT NULL "
           "AND SUM(daily_realized_pnl) != 0 "
           "ORDER BY cumulative_pnl DESC";
}

// ============================================================================
// ChartSeries: from a query's rows to the points a chart plots
// ============================================================================

ChartData ChartSeries::equity_curve(std::vector<ChartPoint> rows,
                                    std::optional<double> book_capital) {
    ChartData chart_data;

    std::vector<std::string> dates;
    std::vector<double> equity_values;
    for (const auto& [date, equity] : rows) {
        dates.push_back(date);
        equity_values.push_back(equity);
    }

    if (dates.empty() || equity_values.empty()) {
        ERROR("No valid data extracted");
        return chart_data;
    }

    INFO("Initial data extracted: " + std::to_string(dates.size()) + " points");

    // Sort data by date to find gaps
    std::vector<std::pair<std::string, double>> date_equity_pairs;
    for (size_t i = 0; i < dates.size(); i++) {
        date_equity_pairs.push_back({dates[i], equity_values[i]});
    }
    std::sort(date_equity_pairs.begin(), date_equity_pairs.end());

    // Find the most recent consecutive block of data (no gaps > 5 days)
    std::vector<std::pair<std::string, double>> recent_data;
    if (!date_equity_pairs.empty()) {
        recent_data.push_back(date_equity_pairs.back());

        // Work backwards to find consecutive data
        for (int i = static_cast<int>(date_equity_pairs.size()) - 2; i >= 0; i--) {
            std::tm tm1 = {}, tm2 = {};
            std::istringstream ss1(date_equity_pairs[i].first);
            std::istringstream ss2(recent_data.front().first);
            ss1 >> std::get_time(&tm1, "%Y-%m-%d");
            ss2 >> std::get_time(&tm2, "%Y-%m-%d");

            if (!ss1.fail() && !ss2.fail()) {
                auto t1 = std::chrono::system_clock::from_time_t(std::mktime(&tm1));
                auto t2 = std::chrono::system_clock::from_time_t(std::mktime(&tm2));
                auto days_diff =
                    std::chrono::duration_cast<std::chrono::hours>(t2 - t1).count() / 24;

                // If gap is more than 5 days, stop here
                if (days_diff > 5) {
                    INFO("Found data gap of " + std::to_string(days_diff) +
                         " days, using recent data only");
                    break;
                }
                recent_data.insert(recent_data.begin(), date_equity_pairs[i]);
            }
        }
    }

    // Clear and repopulate with recent data only
    dates.clear();
    equity_values.clear();
    for (const auto& [date, equity] : recent_data) {
        dates.push_back(date);
        equity_values.push_back(equity);
    }

    INFO("Using " + std::to_string(dates.size()) + " recent consecutive data points");

    if (dates.empty()) {
        ERROR("No valid consecutive data after filtering");
        return chart_data;
    }

    // T-8D-2 R63: the points are the stored rows and nothing else. The query stops before the run
    // date, so the last point is the last settled day; no starting point is made up in front of
    // the first.
    // Populate ChartData
    chart_data.labels = dates;
    chart_data.values = equity_values;
    chart_data.title = "Equity Curve through " + dates.back() + " (settled)";
    chart_data.x_label = "Date";
    chart_data.y_label = "Portfolio Value ($)";
    // The reference line is the book's own starting capital, or none when it is not on record.
    if (book_capital && std::isfinite(*book_capital) && *book_capital > 0.0) {
        chart_data.reference_line = *book_capital;
        chart_data.has_reference_line = true;
        // The curve of a book that sits wholly above or below its capital would leave the line
        // off the chart: the value axis is widened to hold it.
        chart_data.reference_line_on_axis = true;
    }

    return chart_data;
}

namespace {
// "YYYY-MM-DD" moved by a count of calendar days; empty when the text is not a date.
std::string shifted_day(const std::string& ymd, int days) {
    int year = 0, month = 0, day = 0;
    char tail = 0;
    if (ymd.size() != 10 ||
        std::sscanf(ymd.c_str(), "%4d-%2d-%2d%c", &year, &month, &day, &tail) != 3) {
        return {};
    }
    const std::chrono::year_month_day given{std::chrono::year{year},
                                            std::chrono::month{static_cast<unsigned>(month)},
                                            std::chrono::day{static_cast<unsigned>(day)}};
    if (!given.ok()) {
        return {};
    }
    const std::chrono::year_month_day moved{std::chrono::sys_days{given} + std::chrono::days{days}};
    char text[16];
    std::snprintf(text, sizeof(text), "%04d-%02u-%02u", static_cast<int>(moved.year()),
                  static_cast<unsigned>(moved.month()), static_cast<unsigned>(moved.day()));
    return text;
}
}  // namespace

std::optional<std::pair<double, double>> ChartSeries::line_chart_y_range(const ChartData& data) {
    if (!data.has_reference_line || !data.reference_line_on_axis || data.values.empty()) {
        return std::nullopt;  // the renderer's own scaling
    }
    double low = data.reference_line;
    double high = data.reference_line;
    for (double value : data.values) {
        low = std::min(low, value);
        high = std::max(high, value);
    }
    // a margin of a twentieth of the span either side, so neither the line nor a point sits on
    // the frame; a curve that is flat at its reference gets one percent of the level
    double margin = (high - low) * 0.05;
    if (margin <= 0.0) {
        margin = std::max(1.0, std::abs(high) * 0.01);
    }
    return std::make_pair(low - margin, high + margin);
}

std::pair<std::string, std::string> ChartSeries::line_chart_x_range(
    const std::vector<std::string>& labels) {
    if (labels.empty()) {
        return {};
    }
    if (labels.front() != labels.back()) {
        return {labels.front(), labels.back()};
    }
    // One day (a book with one stored row): an axis from that day to itself is empty and cannot
    // be drawn, so the axis runs from the day before to the day after and the point sits on it.
    const std::string before = shifted_day(labels.front(), -1);
    const std::string after = shifted_day(labels.front(), 1);
    if (before.empty() || after.empty()) {
        return {};
    }
    return {before, after};
}

ChartData ChartSeries::pnl_by_symbol(std::vector<ChartPoint> rows, const std::string& date,
                                     bool equities) {
    ChartData chart_data;

    // One bar per symbol (T-8D-2 R64): rows of the same symbol (one per sleeve that holds it) are
    // summed, in the order the symbols first appear.
    std::vector<ChartPoint> symbol_pnl_data;
    std::unordered_map<std::string, size_t> bar_of;
    for (const auto& [symbol, pnl] : rows) {
        const auto [at, is_new] = bar_of.try_emplace(symbol, symbol_pnl_data.size());
        if (is_new) {
            symbol_pnl_data.emplace_back(symbol, pnl);
        } else {
            symbol_pnl_data[at->second].second += pnl;
        }
    }

    // Calculate yesterday's date for the title
    std::string yesterday_date;
    std::tm tm = {};
    std::istringstream ss(date);
    ss >> std::get_time(&tm, "%Y-%m-%d");
    if (!ss.fail()) {
        auto timepoint = std::chrono::system_clock::from_time_t(std::mktime(&tm));
        auto yesterday = timepoint - std::chrono::hours(24);
        auto yesterday_time_t = std::chrono::system_clock::to_time_t(yesterday);
        std::tm* yesterday_tm = std::gmtime(&yesterday_time_t);
        std::ostringstream date_oss;
        date_oss << std::put_time(yesterday_tm, "%Y-%m-%d");
        yesterday_date = date_oss.str();
    }

    if (symbol_pnl_data.empty()) {
        WARN("No valid realized PnL data extracted");
        return chart_data;
    }

    // Sort by realized PnL (descending); symbols of equal PnL keep the order they came in
    std::stable_sort(symbol_pnl_data.begin(), symbol_pnl_data.end(),
                     [](const auto& a, const auto& b) { return a.second > b.second; });

    // Populate ChartData
    INFO("Populating chart data with " + std::to_string(symbol_pnl_data.size()) + " symbols");
    for (const auto& [sym, pnl] : symbol_pnl_data) {
        chart_data.labels.push_back(sym);
        chart_data.values.push_back(pnl);
    }

    // Debug sample
    if (!symbol_pnl_data.empty()) {
        INFO("Sample Realized PnL - First: " + symbol_pnl_data[0].first +
             " PnL: " + std::to_string(symbol_pnl_data[0].second));
    }

    // Add yesterday's date to title if calculated successfully
    if (!yesterday_date.empty()) {
        chart_data.title =
            ChartTitles::pnl_by_symbol_heading(equities) + " (" + yesterday_date + ")";
    }
    chart_data.x_label = "Symbol";
    chart_data.y_label = "Realized PnL ($)";
    chart_data.reference_line = 0.0;
    chart_data.has_reference_line = true;

    INFO("Chart data prepared with " + std::to_string(chart_data.labels.size()) + " labels and " +
         std::to_string(chart_data.values.size()) + " values");

    return chart_data;
}

ChartData ChartSeries::daily_pnl(std::vector<ChartPoint> rows) {
    ChartData chart_data;
    if (rows.empty()) {
        return chart_data;
    }

    // Reverse to get chronological order (DESC query returns newest first)
    std::reverse(rows.begin(), rows.end());
    for (const auto& [day, pnl] : rows) {
        chart_data.labels.push_back(day);
        chart_data.values.push_back(pnl);
    }

    chart_data.x_label = "Date";
    chart_data.y_label = "Daily PnL ($)";
    chart_data.reference_line = 0.0;
    chart_data.has_reference_line = true;

    return chart_data;
}

ChartData ChartSeries::cost_per_million(const std::vector<CostFillRow>& rows,
                                        const MultiplierLookup& multiplier_of) {
    ChartData chart_data;

    // Rows come grouped by day. T-8D-2 R64: a day's value is its cost over the notional the book
    // TRADED that day (each fill's quantity x price x contract multiplier), not over the notional
    // it held.
    size_t at = 0;
    while (at < rows.size()) {
        const std::string& day = rows[at].date;
        const double daily_cost = rows[at].daily_cost;
        double traded_notional = 0.0;
        bool every_fill_sized = true;
        for (; at < rows.size() && rows[at].date == day; ++at) {
            const CostFillRow& fill = rows[at];
            if (fill.symbol.empty()) {
                continue;  // the day's row with no fill
            }
            const std::optional<double> multiplier =
                multiplier_of ? multiplier_of(fill.symbol) : std::nullopt;
            if (!multiplier) {
                // Never counted at a multiplier of 1: the day's ratio would be wrong.
                WARN("Cost per $1M traded: " + fill.symbol + " on " + day +
                     " has no contract multiplier on record, the day has no point");
                every_fill_sized = false;
                continue;
            }
            traded_notional += std::abs(fill.quantity * fill.price * *multiplier);
        }

        // Formula: (daily_cost / traded_notional) * 1,000,000
        // A day the book did not trade has no point
        if (every_fill_sized && traded_notional > 0.0) {
            double cost_per_million = (daily_cost / traded_notional) * 1000000.0;
            chart_data.labels.push_back(day);
            chart_data.values.push_back(cost_per_million);

            INFO("Date: " + day + ", Cost: $" + std::to_string(daily_cost) +
                 ", Traded notional: $" + std::to_string(traded_notional) + ", Cost per $1M: $" +
                 std::to_string(cost_per_million));
        }
    }

    if (chart_data.labels.empty()) {
        WARN("No valid cost per $1M traded data points");
        return chart_data;
    }

    if (chart_data.labels.size() == 1) {
        // guard against single-point x-range (gnuplot needs width)
        std::tm tm{};
        std::istringstream ss(chart_data.labels[0]);
        ss >> std::get_time(&tm, "%Y-%m-%d");
        if (!ss.fail()) {
            auto t = std::chrono::system_clock::from_time_t(std::mktime(&tm));
            auto t_prev = t - std::chrono::hours(24);
            std::time_t tt = std::chrono::system_clock::to_time_t(t_prev);
            std::tm* g = std::gmtime(&tt);
            std::ostringstream os;
            os << std::put_time(g, "%Y-%m-%d");
            chart_data.labels.insert(chart_data.labels.begin(), os.str());
            chart_data.values.insert(chart_data.values.begin(), 0.0);
        }
    }

    chart_data.title = "Cost per $1M Traded (Efficiency Metric)";
    chart_data.x_label = "Date";
    chart_data.y_label = "Cost per $1M Traded ($)";
    chart_data.reference_line = 0.0;
    chart_data.has_reference_line = false;  // No reference line needed for this metric
    return chart_data;
}

ChartData ChartSeries::margin_posted(std::vector<ChartPoint> rows) {
    ChartData chart_data;
    for (const auto& [day, value] : rows) {
        chart_data.labels.push_back(day);
        chart_data.values.push_back(value);
    }

    if (chart_data.labels.empty()) {
        WARN("No valid margin posted points parsed");
        return chart_data;
    }

    chart_data.title = "Margin Posted";
    chart_data.x_label = "Date";
    chart_data.y_label = "Margin Posted ($)";
    chart_data.reference_line = 0.0;
    chart_data.has_reference_line = true;
    return chart_data;
}

ChartData ChartSeries::cumulative_pnl_by_symbol(std::vector<ChartPoint> symbol_pnl_data,
                                                const std::string& date) {
    ChartData chart_data;

    if (symbol_pnl_data.empty()) {
        WARN("No valid cumulative PnL data extracted");
        return chart_data;
    }

    // Already sorted by query (DESC), but let's ensure it
    std::sort(symbol_pnl_data.begin(), symbol_pnl_data.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    // Populate ChartData
    INFO("Populating cumulative PnL chart data with " + std::to_string(symbol_pnl_data.size()) +
         " symbols");
    for (const auto& [sym, pnl] : symbol_pnl_data) {
        chart_data.labels.push_back(sym);
        chart_data.values.push_back(pnl);
    }

    chart_data.title = ChartTitles::cumulative_pnl_title(date);
    chart_data.x_label = "Cumulative Realized PnL ($)";
    chart_data.y_label = "Symbol";
    chart_data.reference_line = 0.0;
    chart_data.has_reference_line = true;

    INFO("Chart data prepared with " + std::to_string(chart_data.labels.size()) + " labels and " +
         std::to_string(chart_data.values.size()) + " values");

    return chart_data;
}

// ============================================================================
// ChartGenerator Implementation - Data Fetchers
// ============================================================================

namespace {
// One cell of a query's answer as a number: the database layer hands most columns over as text.
std::optional<double> number_cell(const std::shared_ptr<arrow::ChunkedArray>& column, int64_t i) {
    if (!column || column->num_chunks() == 0) {
        return std::nullopt;
    }
    const auto chunk = column->chunk(0);
    if (i >= chunk->length() || chunk->IsNull(i)) {
        return std::nullopt;
    }
    try {
        switch (chunk->type_id()) {
            case arrow::Type::STRING:
                return std::stod(std::static_pointer_cast<arrow::StringArray>(chunk)->GetString(i));
            case arrow::Type::LARGE_STRING:
                return std::stod(
                    std::static_pointer_cast<arrow::LargeStringArray>(chunk)->GetString(i));
            case arrow::Type::DOUBLE:
                return std::static_pointer_cast<arrow::DoubleArray>(chunk)->Value(i);
            case arrow::Type::FLOAT:
                return static_cast<double>(
                    std::static_pointer_cast<arrow::FloatArray>(chunk)->Value(i));
            case arrow::Type::INT64:
                return static_cast<double>(
                    std::static_pointer_cast<arrow::Int64Array>(chunk)->Value(i));
            case arrow::Type::INT32:
                return static_cast<double>(
                    std::static_pointer_cast<arrow::Int32Array>(chunk)->Value(i));
            default:
                return std::nullopt;
        }
    } catch (const std::exception&) {
        return std::nullopt;  // text that is not a number
    }
}

// One cell as text; empty when it is NULL or not text.
std::string text_cell(const std::shared_ptr<arrow::ChunkedArray>& column, int64_t i) {
    if (!column || column->num_chunks() == 0) {
        return {};
    }
    const auto chunk = column->chunk(0);
    if (i >= chunk->length() || chunk->IsNull(i)) {
        return {};
    }
    if (chunk->type_id() == arrow::Type::STRING) {
        return std::static_pointer_cast<arrow::StringArray>(chunk)->GetString(i);
    }
    if (chunk->type_id() == arrow::Type::LARGE_STRING) {
        return std::static_pointer_cast<arrow::LargeStringArray>(chunk)->GetString(i);
    }
    return {};
}

// One cell of a date or timestamp column as "YYYY-MM-DD"; empty when it is NULL.
std::string day_cell(const std::shared_ptr<arrow::ChunkedArray>& column, int64_t i) {
    if (!column || column->num_chunks() == 0) {
        return {};
    }
    const auto chunk = column->chunk(0);
    if (i >= chunk->length() || chunk->IsNull(i)) {
        return {};
    }
    std::time_t seconds = 0;
    switch (chunk->type_id()) {
        case arrow::Type::STRING:
        case arrow::Type::LARGE_STRING:
            return text_cell(column, i).substr(0, 10);
        case arrow::Type::DATE32:
            seconds = static_cast<std::time_t>(
                          std::static_pointer_cast<arrow::Date32Array>(chunk)->Value(i)) *
                      86400;
            break;
        case arrow::Type::TIMESTAMP:
            seconds = std::static_pointer_cast<arrow::TimestampArray>(chunk)->Value(i) /
                      1000000;  // micro -> sec
            break;
        default:
            return {};
    }
    std::ostringstream oss;
    oss << std::put_time(std::gmtime(&seconds), "%Y-%m-%d");
    return oss.str();
}

// A symbol's contract multiplier from the instrument registry (a futures root's contract size, 1
// for a share); none when the registry does not hold the symbol.
std::optional<double> registry_multiplier(const std::string& symbol) {
    try {
        // Normalize variant-suffixed symbols for lookup
        std::string lookup_sym = symbol;
        auto dotpos = lookup_sym.find(".v.");
        if (dotpos != std::string::npos) {
            lookup_sym = lookup_sym.substr(0, dotpos);
        }
        dotpos = lookup_sym.find(".c.");
        if (dotpos != std::string::npos) {
            lookup_sym = lookup_sym.substr(0, dotpos);
        }
        auto instrument = InstrumentRegistry::instance().get_instrument(lookup_sym);
        if (!instrument) {
            return std::nullopt;
        }
        return instrument->get_multiplier();
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// The book's starting capital from its run metadata (T-8D-2 R63: the equity curve's reference
// line). No row, no key or a failed read: no value, and the chart is drawn without the line.
std::optional<double> read_book_capital(const std::shared_ptr<DatabaseInterface>& db,
                                        const std::string& strategy_id,
                                        const std::string& portfolio_id, const std::string& date) {
    auto result = db->execute_query(ChartQueries::book_capital(strategy_id, portfolio_id, date));
    if (result.is_error() || !result.value() || result.value()->num_rows() == 0 ||
        result.value()->num_columns() == 0) {
        INFO("Equity curve: no starting capital on record for " + portfolio_id +
             ", the chart has no reference line");
        return std::nullopt;
    }
    auto combined = result.value()->CombineChunks();
    if (!combined.ok()) {
        return std::nullopt;
    }
    const auto capital = number_cell(combined.ValueOrDie()->column(0), 0);
    if (!capital) {
        INFO("Equity curve: no starting capital on record for " + portfolio_id +
             ", the chart has no reference line");
    }
    return capital;
}
}  // namespace

ChartData ChartGenerator::fetch_equity_curve_data(std::shared_ptr<DatabaseInterface> db,
                                                  const std::string& strategy_id,
                                                  const std::string& portfolio_id,
                                                  const std::string& date, int lookback_days) {
    ChartData chart_data;

    if (!db) {
        WARN("Database interface is null, cannot fetch equity curve data");
        return chart_data;
    }

    try {
        // Query equity curve data
        std::string query =
            ChartQueries::equity_curve(strategy_id, portfolio_id, date, lookback_days);

        INFO("Querying equity curve with: " + query);
        auto result = db->execute_query(query);

        if (result.is_error()) {
            ERROR("Failed to query equity curve: " + std::string(result.error()->what()));
            mark_chart_failed("the data could not be read");
            return chart_data;
        }

        auto table = result.value();
        if (!table || table->num_rows() == 0) {
            WARN("No equity curve data available");
            return chart_data;
        }

        INFO("Retrieved " + std::to_string(table->num_rows()) + " rows for chart");

        // Extract and process data
        std::vector<std::string> dates;
        std::vector<double> equity_values;

        auto combined_result = table->CombineChunks();
        if (!combined_result.ok()) {
            ERROR("Failed to combine chunks");
            mark_chart_failed("the data could not be read");
            return chart_data;
        }
        auto combined = combined_result.ValueOrDie();

        auto timestamp_col = combined->column(0);
        auto equity_col = combined->column(1);

        for (int64_t i = 0; i < combined->num_rows(); ++i) {
            try {
                // Extract date
                std::string date_str;
                auto ts_chunk = timestamp_col->chunk(0);

                if (ts_chunk->type_id() == arrow::Type::STRING) {
                    auto str_array = std::static_pointer_cast<arrow::StringArray>(ts_chunk);
                    if (!str_array->IsNull(i)) {
                        date_str = str_array->GetString(i);
                        if (date_str.length() > 10) {
                            date_str = date_str.substr(0, 10);
                        }
                    }
                } else if (ts_chunk->type_id() == arrow::Type::LARGE_STRING) {
                    auto str_array = std::static_pointer_cast<arrow::LargeStringArray>(ts_chunk);
                    if (!str_array->IsNull(i)) {
                        date_str = str_array->GetString(i);
                        if (date_str.length() > 10) {
                            date_str = date_str.substr(0, 10);
                        }
                    }
                } else if (ts_chunk->type_id() == arrow::Type::TIMESTAMP) {
                    auto ts_array = std::static_pointer_cast<arrow::TimestampArray>(ts_chunk);
                    if (!ts_array->IsNull(i)) {
                        int64_t ts_val = ts_array->Value(i);
                        std::time_t tt = ts_val / 1000000;
                        std::tm* tm = std::gmtime(&tt);
                        std::ostringstream oss;
                        oss << std::put_time(tm, "%Y-%m-%d");
                        date_str = oss.str();
                    }
                }

                // Extract equity
                double equity = 0.0;
                auto eq_chunk = equity_col->chunk(0);

                if (!eq_chunk->IsNull(i)) {
                    if (eq_chunk->type_id() == arrow::Type::STRING) {
                        auto str_array = std::static_pointer_cast<arrow::StringArray>(eq_chunk);
                        equity = std::stod(str_array->GetString(i));
                    } else if (eq_chunk->type_id() == arrow::Type::LARGE_STRING) {
                        auto str_array = std::static_pointer_cast<arrow::LargeStringArray>(eq_chunk);
                        equity = std::stod(str_array->GetString(i));
                    } else if (eq_chunk->type_id() == arrow::Type::DOUBLE) {
                        auto dbl_array = std::static_pointer_cast<arrow::DoubleArray>(eq_chunk);
                        equity = dbl_array->Value(i);
                    } else if (eq_chunk->type_id() == arrow::Type::FLOAT) {
                        auto flt_array = std::static_pointer_cast<arrow::FloatArray>(eq_chunk);
                        equity = static_cast<double>(flt_array->Value(i));
                    }
                }

                if (!date_str.empty()) {
                    dates.push_back(date_str);
                    equity_values.push_back(equity);
                }
            } catch (const std::exception& e) {
                WARN("Error processing row " + std::to_string(i) + ": " + std::string(e.what()));
                continue;
            }
        }

        if (dates.empty() || equity_values.empty()) {
            ERROR("No valid data extracted");
            return chart_data;
        }

        std::vector<ChartPoint> rows;
        rows.reserve(dates.size());
        for (size_t i = 0; i < dates.size(); i++) {
            rows.emplace_back(dates[i], equity_values[i]);
        }
        return ChartSeries::equity_curve(std::move(rows),
                                         read_book_capital(db, strategy_id, portfolio_id, date));

    } catch (const std::exception& e) {
        ERROR("Exception fetching equity curve data: " + std::string(e.what()));
        mark_chart_failed("the data could not be read");
        return chart_data;
    }
}

ChartData ChartGenerator::fetch_pnl_by_symbol_data(std::shared_ptr<DatabaseInterface> db,
                                                   const std::string& strategy_id,
                                                   const std::string& portfolio_id,
                                                   const std::string& date, bool equities) {
    ChartData chart_data;

    if (!db) {
        WARN("Database interface is null, cannot fetch PnL by symbol data");
        return chart_data;
    }

    try {
        // Yesterday's realized PnL by symbol
        std::string query = ChartQueries::pnl_by_symbol(strategy_id, portfolio_id, date);

        INFO("Querying realized PnL by symbol with: " + query);
        auto result = db->execute_query(query);

        if (result.is_error()) {
            ERROR("Failed to query realized PnL by symbol: " + std::string(result.error()->what()));
            mark_chart_failed("the data could not be read");
            return chart_data;
        }

        auto table = result.value();
        if (!table || table->num_rows() == 0) {
            WARN("No realized PnL data available");
            return chart_data;
        }

        auto combined_result = table->CombineChunks();
        if (!combined_result.ok()) {
            ERROR("Failed to combine chunks");
            mark_chart_failed("the data could not be read");
            return chart_data;
        }
        auto combined = combined_result.ValueOrDie();

        std::vector<std::pair<std::string, double>> symbol_pnl_data;

        auto symbol_col   = combined->column(0);
        auto realized_col = combined->column(1);

        for (int64_t i = 0; i < combined->num_rows(); ++i) {
            try {
                // --- symbol ---
                std::string symbol;
                auto sym_chunk = symbol_col->chunk(0);
                if (sym_chunk->type_id() == arrow::Type::STRING) {
                    auto s = std::static_pointer_cast<arrow::StringArray>(sym_chunk);
                    if (!s->IsNull(i)) symbol = s->GetString(i);
                } else if (sym_chunk->type_id() == arrow::Type::LARGE_STRING) {
                    auto s = std::static_pointer_cast<arrow::LargeStringArray>(sym_chunk);
                    if (!s->IsNull(i)) symbol = s->GetString(i);
                }

                // --- realized pnl only ---
                double realized_pnl = 0.0;
                auto real_chunk = realized_col->chunk(0);
                if (!real_chunk->IsNull(i)) {
                    if (real_chunk->type_id() == arrow::Type::DOUBLE) {
                        auto a = std::static_pointer_cast<arrow::DoubleArray>(real_chunk);
                        realized_pnl = a->Value(i);
                    } else if (real_chunk->type_id() == arrow::Type::FLOAT) {
                        auto a = std::static_pointer_cast<arrow::FloatArray>(real_chunk);
                        realized_pnl = static_cast<double>(a->Value(i));
                    } else if (real_chunk->type_id() == arrow::Type::INT64) {
                        auto a = std::static_pointer_cast<arrow::Int64Array>(real_chunk);
                        realized_pnl = static_cast<double>(a->Value(i));
                    } else if (real_chunk->type_id() == arrow::Type::INT32) {
                        auto a = std::static_pointer_cast<arrow::Int32Array>(real_chunk);
                        realized_pnl = static_cast<double>(a->Value(i));
                    } else if (real_chunk->type_id() == arrow::Type::STRING) {
                        auto a = std::static_pointer_cast<arrow::StringArray>(real_chunk);
                        if (!a->IsNull(i)) {
                            try { realized_pnl = std::stod(a->GetString(i)); } catch (...) {}
                        }
                    } else if (real_chunk->type_id() == arrow::Type::LARGE_STRING) {
                        auto a = std::static_pointer_cast<arrow::LargeStringArray>(real_chunk);
                        if (!a->IsNull(i)) {
                            try { realized_pnl = std::stod(a->GetString(i)); } catch (...) {}
                        }
                    }
                }

                if (!symbol.empty()) {
                    symbol_pnl_data.emplace_back(symbol, realized_pnl);
                }
            } catch (const std::exception& e) {
                WARN("Error processing row " + std::to_string(i) + ": " + std::string(e.what()));
                continue;
            }
        }

        if (symbol_pnl_data.empty()) {
            WARN("No valid realized PnL data extracted");
            return chart_data;
        }

        return ChartSeries::pnl_by_symbol(std::move(symbol_pnl_data), date, equities);

    } catch (const std::exception& e) {
        ERROR("Exception fetching realized PnL by symbol: " + std::string(e.what()));
        mark_chart_failed("the data could not be read");
        return chart_data;
    }
}

ChartData ChartGenerator::fetch_daily_pnl_data(
    std::shared_ptr<DatabaseInterface> db,
    const std::string& strategy_id,
                                                  const std::string& portfolio_id,
    const std::string& date,
    int lookback_days)
{
    ChartData chart_data;

    if (!db) {
        WARN("Database interface is null, cannot fetch daily PnL data");
        return chart_data;
    }

    try {
        // Query daily_pnl directly from live_results, excluding today (only up to T-1)
        std::string query = ChartQueries::daily_pnl(strategy_id, portfolio_id, date, lookback_days);

        INFO("Querying daily PnL data with: " + query);
        auto result = db->execute_query(query);

        if (result.is_error()) {
            ERROR("Failed to query daily PnL: " + std::string(result.error()->what()));
            mark_chart_failed("the data could not be read");
            return chart_data;
        }

        auto table = result.value();
        if (!table || table->num_rows() == 0) {
            WARN("No data for daily PnL chart");
            return chart_data;
        }

        INFO("Retrieved " + std::to_string(table->num_rows()) + " rows for daily PnL");

        // Extract data
        auto combined_result = table->CombineChunks();
        if (!combined_result.ok()) {
            ERROR("Failed to combine chunks");
            mark_chart_failed("the data could not be read");
            return chart_data;
        }
        auto combined = combined_result.ValueOrDie();

        auto date_col = combined->column(0);
        auto pnl_col = combined->column(1);
        std::vector<ChartPoint> rows;

        for (int64_t i = 0; i < combined->num_rows(); ++i) {
            try {
                // Extract date
                std::string date_str;
                auto date_chunk = date_col->chunk(0);

                if (date_chunk->type_id() == arrow::Type::STRING) {
                    auto str_array = std::static_pointer_cast<arrow::StringArray>(date_chunk);
                    if (!str_array->IsNull(i)) {
                        date_str = str_array->GetString(i);
                        if (date_str.length() > 10) {
                            date_str = date_str.substr(0, 10);
                        }
                    }
                } else if (date_chunk->type_id() == arrow::Type::LARGE_STRING) {
                    auto str_array = std::static_pointer_cast<arrow::LargeStringArray>(date_chunk);
                    if (!str_array->IsNull(i)) {
                        date_str = str_array->GetString(i);
                        if (date_str.length() > 10) {
                            date_str = date_str.substr(0, 10);
                        }
                    }
                } else if (date_chunk->type_id() == arrow::Type::DATE32) {
                    auto d = std::static_pointer_cast<arrow::Date32Array>(date_chunk);
                    if (!d->IsNull(i)) {
                        int32_t days = d->Value(i);
                        std::time_t tsec = static_cast<std::time_t>(days) * 86400;
                        std::tm* tm = std::gmtime(&tsec);
                        std::ostringstream oss;
                        oss << std::put_time(tm, "%Y-%m-%d");
                        date_str = oss.str();
                    }
                } else if (date_chunk->type_id() == arrow::Type::TIMESTAMP) {
                    auto ts_array = std::static_pointer_cast<arrow::TimestampArray>(date_chunk);
                    if (!ts_array->IsNull(i)) {
                        int64_t ts_val = ts_array->Value(i);
                        std::time_t tt = ts_val / 1000000;
                        std::tm* tm = std::gmtime(&tt);
                        std::ostringstream oss;
                        oss << std::put_time(tm, "%Y-%m-%d");
                        date_str = oss.str();
                    }
                }

                // Extract daily_pnl
                double daily_pnl = 0.0;
                auto pnl_chunk = pnl_col->chunk(0);

                if (!pnl_chunk->IsNull(i)) {
                    if (pnl_chunk->type_id() == arrow::Type::DOUBLE) {
                        auto dbl_array = std::static_pointer_cast<arrow::DoubleArray>(pnl_chunk);
                        daily_pnl = dbl_array->Value(i);
                    } else if (pnl_chunk->type_id() == arrow::Type::FLOAT) {
                        auto flt_array = std::static_pointer_cast<arrow::FloatArray>(pnl_chunk);
                        daily_pnl = static_cast<double>(flt_array->Value(i));
                    } else if (pnl_chunk->type_id() == arrow::Type::STRING) {
                        auto str_array = std::static_pointer_cast<arrow::StringArray>(pnl_chunk);
                        try { daily_pnl = std::stod(str_array->GetString(i)); } catch (...) {}
                    } else if (pnl_chunk->type_id() == arrow::Type::LARGE_STRING) {
                        auto str_array = std::static_pointer_cast<arrow::LargeStringArray>(pnl_chunk);
                        try { daily_pnl = std::stod(str_array->GetString(i)); } catch (...) {}
                    }
                }

                if (!date_str.empty()) {
                    rows.emplace_back(date_str, daily_pnl);
                }
            } catch (const std::exception& e) {
                WARN("Error processing row " + std::to_string(i) + ": " + std::string(e.what()));
                continue;
            }
        }

        if (rows.empty()) {
            ERROR("No valid daily PnL data");
            return chart_data;
        }

        return ChartSeries::daily_pnl(std::move(rows));

    } catch (const std::exception& e) {
        ERROR("Exception fetching daily PnL data: " + std::string(e.what()));
        mark_chart_failed("the data could not be read");
        return chart_data;
    }
}

ChartData ChartGenerator::fetch_cumulative_transaction_costs_data(
    std::shared_ptr<DatabaseInterface> db, const std::string& strategy_id,
    const std::string& portfolio_id, const std::string& date /* today, YYYY-MM-DD */,
    const ChartSeries::MultiplierLookup& multiplier_of) {
    ChartData chart_data;
    if (!db) { WARN("DB null"); return chart_data; }

    try {
        // Each stored day's cost and the fills the book made that day
        std::string query = ChartQueries::cost_per_million(strategy_id, portfolio_id, date);

        INFO("Querying cost per $1M traded with: " + query);
        auto result = db->execute_query(query);
        if (result.is_error()) {
            ERROR("Query failed: " + std::string(result.error()->what()));
            return chart_data;
        }

        auto table = result.value();
        if (!table || table->num_rows() == 0) {
            WARN("No transaction cost rows returned");
            return chart_data;
        }

        auto combined_result = table->CombineChunks();
        if (!combined_result.ok()) {
            ERROR("CombineChunks failed");
            return chart_data;
        }
        auto combined = combined_result.ValueOrDie();
        if (combined->num_columns() < 5) {
            ERROR("Cost per $1M traded: the query answered " +
                  std::to_string(combined->num_columns()) + " columns, 5 expected");
            return chart_data;
        }

        // Expect: col0 = date, col1 = daily_cost, col2 = symbol, col3 = quantity, col4 = price
        // (the three fill columns NULL on a day without a fill)
        std::vector<ChartSeries::CostFillRow> rows;
        for (int64_t i = 0; i < combined->num_rows(); ++i) {
            ChartSeries::CostFillRow row;
            row.date = day_cell(combined->column(0), i);
            if (row.date.empty())
                continue;
            row.daily_cost = number_cell(combined->column(1), i).value_or(0.0);
            row.symbol = text_cell(combined->column(2), i);
            const auto quantity = number_cell(combined->column(3), i);
            const auto price = number_cell(combined->column(4), i);
            if (!row.symbol.empty() && quantity && price) {
                row.quantity = *quantity;
                row.price = *price;
            } else {
                row.symbol.clear();  // no fill on this row
            }
            rows.push_back(std::move(row));
        }

        return ChartSeries::cost_per_million(
            rows,
            multiplier_of ? multiplier_of : ChartSeries::MultiplierLookup(registry_multiplier));

    } catch (const std::exception& e) {
        ERROR(std::string("Exception fetching cost per $1M traded: ") + e.what());
        mark_chart_failed("the data could not be read");
        return chart_data;
    }
}

ChartData ChartGenerator::fetch_margin_posted_data(
    std::shared_ptr<DatabaseInterface> db,
    const std::string& strategy_id,
                                                  const std::string& portfolio_id,
    const std::string& date /* today, YYYY-MM-DD */)
{
    ChartData chart_data;
    if (!db) { WARN("DB null"); return chart_data; }

    try {
        // All days up to and including today (T)
        std::string query = ChartQueries::margin_posted(strategy_id, portfolio_id, date);

        INFO("Querying margin posted with: " + query);
        auto result = db->execute_query(query);
        if (result.is_error()) {
            ERROR("Failed to query margin posted: " + std::string(result.error()->what()));
            mark_chart_failed("the data could not be read");
            return chart_data;
        }

        auto table = result.value();
        if (!table || table->num_rows() == 0) {
            WARN("No margin posted data available");
            return chart_data;
        }

        auto combined_result = table->CombineChunks();
        if (!combined_result.ok()) {
            ERROR("Failed to combine chunks");
            mark_chart_failed("the data could not be read");
            return chart_data;
        }
        auto combined = combined_result.ValueOrDie();

        // Expect: col0 = date, col1 = margin_posted
        auto day_col = combined->column(0);
        auto val_col = combined->column(1);
        auto day_chunk = day_col->chunk(0);
        auto val_chunk = val_col->chunk(0);
        std::vector<ChartPoint> rows;

        for (int64_t i = 0; i < combined->num_rows(); ++i) {
            // ---- parse date -> "YYYY-MM-DD"
            std::string day_str;
            switch (day_chunk->type_id()) {
                case arrow::Type::STRING: {
                    auto s = std::static_pointer_cast<arrow::StringArray>(day_chunk);
                    if (!s->IsNull(i)) {
                        day_str = s->GetString(i);
                        if (day_str.size() > 10) day_str = day_str.substr(0, 10);
                    }
                } break;
                case arrow::Type::LARGE_STRING: {
                    auto s = std::static_pointer_cast<arrow::LargeStringArray>(day_chunk);
                    if (!s->IsNull(i)) {
                        day_str = s->GetString(i);
                        if (day_str.size() > 10) day_str = day_str.substr(0, 10);
                    }
                } break;
                case arrow::Type::DATE32: {
                    auto d = std::static_pointer_cast<arrow::Date32Array>(day_chunk);
                    if (!d->IsNull(i)) {
                        int32_t days = d->Value(i);
                        std::time_t tsec = static_cast<std::time_t>(days) * 86400;
                        std::tm* tm = std::gmtime(&tsec);
                        std::ostringstream oss; oss << std::put_time(tm, "%Y-%m-%d");
                        day_str = oss.str();
                    }
                } break;
                case arrow::Type::TIMESTAMP: {
                    auto ts = std::static_pointer_cast<arrow::TimestampArray>(day_chunk);
                    if (!ts->IsNull(i)) {
                        std::time_t tsec = ts->Value(i) / 1000000; // micro → sec
                        std::tm* tm = std::gmtime(&tsec);
                        std::ostringstream oss; oss << std::put_time(tm, "%Y-%m-%d");
                        day_str = oss.str();
                    }
                } break;
                default: break;
            }
            if (day_str.empty()) continue;

            // ---- parse margin_posted -> double (skip nulls)
            double val = 0.0;
            bool have = false;
            switch (val_chunk->type_id()) {
                case arrow::Type::DOUBLE: {
                    auto a = std::static_pointer_cast<arrow::DoubleArray>(val_chunk);
                    if (!a->IsNull(i)) { val = a->Value(i); have = true; }
                } break;
                case arrow::Type::FLOAT: {
                    auto a = std::static_pointer_cast<arrow::FloatArray>(val_chunk);
                    if (!a->IsNull(i)) { val = static_cast<double>(a->Value(i)); have = true; }
                } break;
                case arrow::Type::INT64: {
                    auto a = std::static_pointer_cast<arrow::Int64Array>(val_chunk);
                    if (!a->IsNull(i)) { val = static_cast<double>(a->Value(i)); have = true; }
                } break;
                case arrow::Type::INT32: {
                    auto a = std::static_pointer_cast<arrow::Int32Array>(val_chunk);
                    if (!a->IsNull(i)) { val = static_cast<double>(a->Value(i)); have = true; }
                } break;
                case arrow::Type::STRING: {
                    auto a = std::static_pointer_cast<arrow::StringArray>(val_chunk);
                    if (!a->IsNull(i)) { try { val = std::stod(a->GetString(i)); have = true; } catch (...) {} }
                } break;
                case arrow::Type::LARGE_STRING: {
                    auto a = std::static_pointer_cast<arrow::LargeStringArray>(val_chunk);
                    if (!a->IsNull(i)) { try { val = std::stod(a->GetString(i)); have = true; } catch (...) {} }
                } break;
                default: break;
            }
            if (!have) continue;  // skip NULL margins

            rows.emplace_back(day_str, val);
        }

        return ChartSeries::margin_posted(std::move(rows));

    } catch (const std::exception& e) {
        ERROR(std::string("Exception fetching margin posted: ") + e.what());
        mark_chart_failed("the data could not be read");
        return chart_data;
    }
}

ChartData ChartGenerator::fetch_portfolio_composition_data(
    const std::unordered_map<std::string, Position>& positions,
    const std::unordered_map<std::string, double>& current_prices,
    const std::string& date)
{
    ChartData chart_data;

    try {
        // Calculate gross notional for each symbol
        std::vector<std::pair<std::string, double>> symbol_notionals;
        double total_notional = 0.0;

        for (const auto& [symbol, position] : positions) {
            if (position.quantity.as_double() == 0.0) continue;

            // Get contract multiplier
            double contract_multiplier = 1.0;
            try {
                auto& registry = InstrumentRegistry::instance();
                
                // Normalize variant-suffixed symbols for lookup
                std::string lookup_sym = symbol;
                auto dotpos = lookup_sym.find(".v.");
                if (dotpos != std::string::npos) {
                    lookup_sym = lookup_sym.substr(0, dotpos);
                }
                dotpos = lookup_sym.find(".c.");
                if (dotpos != std::string::npos) {
                    lookup_sym = lookup_sym.substr(0, dotpos);
                }

                auto instrument = registry.get_instrument(lookup_sym);
                if (!instrument) {
                    ERROR("CRITICAL: Instrument " + lookup_sym + " not found in registry for pie chart!");
                    continue;
                }
                contract_multiplier = instrument->get_multiplier();
            } catch (const std::exception& e) {
                ERROR("Failed to get instrument data for " + symbol + ": " + std::string(e.what()));
                continue;
            }

            // Use current price if available, otherwise average price
            double price = position.average_price.as_double();
            auto price_it = current_prices.find(symbol);
            if (price_it != current_prices.end()) {
                price = price_it->second;
            }

            // Calculate gross notional (absolute value)
            double notional = std::abs(position.quantity.as_double() * price * contract_multiplier);
            
            symbol_notionals.push_back({symbol, notional});
            total_notional += notional;
        }

        if (total_notional == 0.0 || symbol_notionals.empty()) {
            WARN("No valid positions for portfolio composition chart");
            return chart_data;
        }

        // Sort by notional descending
        std::sort(symbol_notionals.begin(), symbol_notionals.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });

        // Convert all symbols to percentages (no grouping, show all individually)
        std::vector<std::pair<std::string, double>> final_data;

        for (const auto& [sym, notional] : symbol_notionals) {
            double percentage = (notional / total_notional) * 100.0;
            final_data.push_back({sym, percentage});
        }

        // Populate ChartData
        for (const auto& [sym, pct] : final_data) {
            chart_data.labels.push_back(sym);
            chart_data.values.push_back(pct);
        }

        // Add date to title if provided
        if (!date.empty()) {
            chart_data.title = "Portfolio Composition by Gross Notional (" + date + ")";
        } else {
            chart_data.title = "Portfolio Composition by Gross Notional";
        }
        chart_data.x_label = "Symbol";
        chart_data.y_label = "Percentage of Portfolio (%)";

        INFO("Portfolio composition chart data prepared with " +
             std::to_string(chart_data.labels.size()) + " categories");

        return chart_data;

    } catch (const std::exception& e) {
        ERROR("Exception fetching portfolio composition data: " + std::string(e.what()));
        mark_chart_failed("the data could not be read");
        return chart_data;
    }
}

ChartData ChartGenerator::fetch_cumulative_pnl_by_symbol_data(
    std::shared_ptr<DatabaseInterface> db,
    const std::string& strategy_id,
                                                  const std::string& portfolio_id,
    const std::string& date)
{
    ChartData chart_data;

    if (!db) {
        WARN("Database interface is null, cannot fetch cumulative PnL data");
        return chart_data;
    }

    try {
        // Query to sum up all realized PnL by symbol across all time
        std::string query = ChartQueries::cumulative_pnl_by_symbol(strategy_id, portfolio_id, date);

        INFO("Querying cumulative PnL by symbol with: " + query);
        auto result = db->execute_query(query);

        if (result.is_error()) {
            ERROR("Failed to query cumulative PnL by symbol: " + std::string(result.error()->what()));
            mark_chart_failed("the data could not be read");
            return chart_data;
        }

        auto table = result.value();
        if (!table || table->num_rows() == 0) {
            WARN("No cumulative PnL data available");
            return chart_data;
        }

        auto combined_result = table->CombineChunks();
        if (!combined_result.ok()) {
            ERROR("Failed to combine chunks");
            mark_chart_failed("the data could not be read");
            return chart_data;
        }
        auto combined = combined_result.ValueOrDie();

        std::vector<std::pair<std::string, double>> symbol_pnl_data;

        auto symbol_col = combined->column(0);
        auto pnl_col = combined->column(1);

        for (int64_t i = 0; i < combined->num_rows(); ++i) {
            try {
                // Extract symbol
                std::string symbol;
                auto sym_chunk = symbol_col->chunk(0);
                if (sym_chunk->type_id() == arrow::Type::STRING) {
                    auto s = std::static_pointer_cast<arrow::StringArray>(sym_chunk);
                    if (!s->IsNull(i)) symbol = s->GetString(i);
                } else if (sym_chunk->type_id() == arrow::Type::LARGE_STRING) {
                    auto s = std::static_pointer_cast<arrow::LargeStringArray>(sym_chunk);
                    if (!s->IsNull(i)) symbol = s->GetString(i);
                }

                // Extract cumulative PnL
                double pnl = 0.0;
                auto pnl_chunk = pnl_col->chunk(0);
                if (!pnl_chunk->IsNull(i)) {
                    if (pnl_chunk->type_id() == arrow::Type::DOUBLE) {
                        auto a = std::static_pointer_cast<arrow::DoubleArray>(pnl_chunk);
                        pnl = a->Value(i);
                    } else if (pnl_chunk->type_id() == arrow::Type::FLOAT) {
                        auto a = std::static_pointer_cast<arrow::FloatArray>(pnl_chunk);
                        pnl = static_cast<double>(a->Value(i));
                    } else if (pnl_chunk->type_id() == arrow::Type::INT64) {
                        auto a = std::static_pointer_cast<arrow::Int64Array>(pnl_chunk);
                        pnl = static_cast<double>(a->Value(i));
                    } else if (pnl_chunk->type_id() == arrow::Type::INT32) {
                        auto a = std::static_pointer_cast<arrow::Int32Array>(pnl_chunk);
                        pnl = static_cast<double>(a->Value(i));
                    } else if (pnl_chunk->type_id() == arrow::Type::STRING) {
                        auto a = std::static_pointer_cast<arrow::StringArray>(pnl_chunk);
                        if (!a->IsNull(i)) {
                            try { pnl = std::stod(a->GetString(i)); } catch (...) {}
                        }
                    } else if (pnl_chunk->type_id() == arrow::Type::LARGE_STRING) {
                        auto a = std::static_pointer_cast<arrow::LargeStringArray>(pnl_chunk);
                        if (!a->IsNull(i)) {
                            try { pnl = std::stod(a->GetString(i)); } catch (...) {}
                        }
                    }
                }

                if (!symbol.empty()) {
                    symbol_pnl_data.emplace_back(symbol, pnl);
                }
            } catch (const std::exception& e) {
                WARN("Error processing row " + std::to_string(i) + ": " + std::string(e.what()));
                continue;
            }
        }

        if (symbol_pnl_data.empty()) {
            WARN("No valid cumulative PnL data extracted");
            return chart_data;
        }

        return ChartSeries::cumulative_pnl_by_symbol(std::move(symbol_pnl_data), date);

    } catch (const std::exception& e) {
        ERROR("Exception fetching cumulative PnL by symbol: " + std::string(e.what()));
        mark_chart_failed("the data could not be read");
        return chart_data;
    }
}

// ============================================================================
// ChartGenerator Implementation - Generic Chart Renderers
// ============================================================================

namespace {
    // Helper: Escape single quotes in strings for gnuplot
    std::string escape_gnuplot_string(const std::string& str) {
        std::string result;
        for (char c : str) {
            if (c == '\'') {
                result += "''";  // Escape single quote in gnuplot
            } else {
                result += c;
            }
        }
        return result;
    }

    // The time one chart process is given (T-DATED R4). A chart is drawn in well under a second.
    constexpr std::chrono::seconds kChartProcessTimeout{30};

    // Helper: Execute gnuplot script and return base64-encoded PNG. The files are private to the
    // call, a hang is started once more, and the end is recorded for the report (core/gnuplot_run.hpp).
    std::string execute_gnuplot(const std::string& script_content, const std::string& data_content) {
        INFO("Executing gnuplot");
        // One light style for every chart (HD 10-13): a quiet border, ticks on the outside, a light
        // grid, readable titles. The scripts' own lines that follow still decide their colours.
        std::string styled = script_content;
        const std::string output_line = "set output 'temp_chart_output.png'\n";
        const auto at = styled.find(output_line);
        if (at != std::string::npos) {
            styled.insert(at + output_line.size(),
                          "set border 3 lw 1 lc rgb '#999999'\nset tics nomirror\n"
                          "set xtics textcolor rgb '#444444'\nset ytics textcolor rgb '#444444'\n"
                          "set title font 'Arial,14' textcolor rgb '#222222'\n"
                          "set grid ytics lc rgb '#e6e6e6' lt 1 lw 0.5\n");
        }
        const core::GnuplotRun run = core::run_gnuplot(
            styled, data_content, kChartProcessTimeout,
            [](const std::string& command, std::chrono::milliseconds limit) {
                return core::run_with_timeout(command, limit);
            });
        g_last_status = run.status;
        g_last_reason = run.reason;
        if (run.status != core::ChartStatus::Drawn) {
            ERROR("Chart not drawn after " + std::to_string(run.attempts) + " start(s): " +
                  run.reason);
            return "";
        }
        return ChartHelpers::encode_to_base64(run.png);
    }
}

core::ChartStatus ChartGenerator::last_chart_status() {
    return g_last_status;
}

std::string ChartGenerator::last_chart_reason() {
    return g_last_reason;
}

std::string ChartGenerator::render_line_chart(const ChartData& data, const ChartConfig& config) {
    if (data.labels.empty() || data.values.empty()) {
        WARN("No data provided for line chart");
        return "";
    }

    if (data.labels.size() != data.values.size()) {
        ERROR("Labels and values size mismatch in line chart");
        return "";
    }

    // The axis runs from the first day to the last; a chart of one day gets a day either side.
    const auto x_range = ChartSeries::line_chart_x_range(data.labels);
    if (x_range.first.empty()) {
        ERROR("Line chart '" + data.title + "' has one point and its label '" +
              data.labels.front() + "' is not a date: no x range, the chart is left out");
        return "";
    }

    try {
        // Build data file content
        std::ostringstream data_content;
        for (size_t i = 0; i < data.labels.size(); ++i) {
            const bool two = data.values2.size() == data.values.size();
            data_content << data.labels[i] << " " << std::fixed << std::setprecision(2)
                        << data.values[i];
            if (two) data_content << " " << data.values2[i];
            data_content << "\n";
        }

        // Build gnuplot script
        std::ostringstream script;
        script << "reset\n";
        script << "set terminal pngcairo size " << config.width << "," << config.height
               << " enhanced font 'Arial," << config.font_size << "'\n";
        script << "set output 'temp_chart_output.png'\n";
        if (config.rotate_x_labels) {
            script << "set bmargin 5\n";
        }
        script << "\n";

        // Styling
        script << "set style line 1 lc rgb '" << config.line_color << "' lt 1 lw 3 pt 7 ps 0.8\n";
        script << "set border lw 1.5\n";
        if (config.show_grid) {
            script << "set grid ytics lc rgb '#e0e0e0' lt 1 lw 0.5\n";
        }
        if (data.values2.size() == data.values.size()) {
            script << "set key top left\n\n";
        } else {
            script << "unset key\n\n";
        }

        // Labels
        if (!data.x_label.empty()) {
            script << "set xlabel '" << escape_gnuplot_string(data.x_label) << "'\n";
        }
        if (!data.y_label.empty()) {
            script << "set ylabel '" << escape_gnuplot_string(data.y_label) << "'\n";
        }
        if (!data.title.empty()) {
            script << "set title '" << escape_gnuplot_string(data.title) << "'\n";
        }

        // X-axis (time data)
        script << "set xdata time\n";
        script << "set timefmt '%Y-%m-%d'\n";
        script << "set format x '%m/%d'\n";
        script << "set xrange ['" << x_range.first << "':'" << x_range.second << "']\n";

        // X-axis ticks
        int num_ticks = std::min(5, static_cast<int>(data.labels.size()));
        script << "set xtics (";
        for (int i = 0; i < num_ticks; i++) {
            int idx = (i * (static_cast<int>(data.labels.size()) - 1)) / std::max(1, num_ticks - 1);
            if (i > 0) script << ", ";
            script << "'" << data.labels[idx] << "' '" << data.labels[idx] << "'";
        }
        script << ")";
        if (config.rotate_x_labels) {
            script << " rotate by " << config.x_label_angle;
        }
        script << "\n\n";

        // Y-axis
        script << "set format y '" << (data.y_format.empty() ? "$%.0f" : data.y_format) << "'\n";
        if (const auto y_range = ChartSeries::line_chart_y_range(data)) {
            script << "set yrange [" << std::fixed << std::setprecision(2) << y_range->first << ":"
                   << y_range->second << "]\n";
        }
        script << "\n";

        // Reference line
        if (data.has_reference_line) {
            script << "set arrow from graph 0, first " << data.reference_line
                   << " to graph 1, first " << data.reference_line
                   << " nohead lc rgb '#666666' lt 2 lw 1 back\n\n";
        }

        // Plot
        if (data.values2.size() == data.values.size()) {
            script << "set style line 2 lc rgb '#d9822b' lt 1 lw 3 pt 7 ps 0.8\n";
            script << "plot 'temp_chart_data.txt' using 1:2 with linespoints ls 1 title '"
                   << escape_gnuplot_string(data.series_name) << "', \\\n"
                   << "     'temp_chart_data.txt' using 1:3 with linespoints ls 2 title '"
                   << escape_gnuplot_string(data.series2_name) << "'\n";
        } else {
            script << "plot 'temp_chart_data.txt' using 1:2 with linespoints ls 1 notitle\n";
        }

        return execute_gnuplot(script.str(), data_content.str());

    } catch (const std::exception& e) {
        ERROR("Exception rendering line chart: " + std::string(e.what()));
        mark_chart_failed("the chart could not be drawn");
        return "";
    }
}

std::string ChartGenerator::render_bar_chart(const ChartData& data, const ChartConfig& config) {
    if (data.labels.empty() || data.values.empty()) {
        WARN("No data provided for bar chart");
        return "";
    }
    if (data.labels.size() != data.values.size()) {
        ERROR("Labels and values size mismatch in bar chart");
        return "";
    }

    try {
        // Build data file: index value
        // (gnuplot likes numeric x with xtics mapping for categories)
        std::ostringstream data_content;
        for (size_t i = 0; i < data.labels.size(); ++i) {
            data_content << i << " " << std::fixed << std::setprecision(2) << data.values[i] << "\n";
        }

        std::ostringstream script;
        script << "reset\n";
        script << "set terminal pngcairo size " << config.width << "," << config.height
               << " enhanced font 'Arial," << config.font_size << "'\n";
        script << "set output 'temp_chart_output.png'\n";
        if (config.rotate_x_labels) script << "set bmargin 5\n";
        script << "\n";

        // Styling
        script << "set border lw 1.5\n";
        if (config.show_grid) script << "set grid ytics lc rgb '#e0e0e0' lt 1 lw 0.5\n";
        script << "unset key\n\n";

        // Labels / Title
        if (!data.x_label.empty()) script << "set xlabel '" << escape_gnuplot_string(data.x_label) << "'\n";
        if (!data.y_label.empty()) script << "set ylabel '" << escape_gnuplot_string(data.y_label) << "'\n";
        if (!data.title.empty())   script << "set title '"  << escape_gnuplot_string(data.title)   << "'\n";

        // X axis: categorical ticks
        script << "unset xdata\n";
        script << "set xrange [-0.5:" << (data.labels.size()-0.5) << "]\n";
        script << "set xtics (";
        for (size_t i = 0; i < data.labels.size(); ++i) {
            if (i) script << ", ";
            script << "'" << data.labels[i] << "' " << i;
        }
        script << ")";
        if (config.rotate_x_labels) script << " rotate by " << config.x_label_angle;
        script << "\n\n";

        // Y axis
        script << "set format y '$%.0f'\n\n";

        // Reference line at 0 (PnL baseline)
        if (data.has_reference_line) {
            script << "set arrow from graph 0, first " << data.reference_line
                   << " to graph 1, first " << data.reference_line
                   << " nohead lc rgb '#666666' lt 2 lw 1\n\n";
        }

        // Bars with conditional color
        script << "set style fill solid border -1\n";
        script << "set boxwidth " << config.box_width << " relative\n";
        script << "plot 'temp_chart_data.txt' using 1:($2>=0?$2:0) with boxes lc rgb '"
               << config.positive_color << "' notitle, \\\n";
        script << "     'temp_chart_data.txt' using 1:($2<0?$2:0)  with boxes lc rgb '"
               << config.negative_color << "' notitle\n";

        return execute_gnuplot(script.str(), data_content.str());
    } catch (const std::exception& e) {
        ERROR("Exception rendering bar chart: " + std::string(e.what()));
        mark_chart_failed("the chart could not be drawn");
        return "";
    }
}


std::string ChartGenerator::render_horizontal_bar_chart(const ChartData& data, const ChartConfig& config) {
    if (data.labels.empty() || data.values.empty()) {
        WARN("No data provided for horizontal bar chart");
        return "";
    }

    if (data.labels.size() != data.values.size()) {
        ERROR("Labels and values size mismatch in horizontal bar chart");
        return "";
    }

    INFO("Rendering horizontal bar chart with " + std::to_string(data.labels.size()) + " items");

    try {
        // Build data file content (symbol pnl)
        std::ostringstream data_content;
        for (size_t i = 0; i < data.labels.size(); ++i) {
            const bool two = data.values2.size() == data.values.size();
            data_content << data.labels[i] << " " << std::fixed << std::setprecision(2)
                        << data.values[i];
            if (two) data_content << " " << data.values2[i];
            data_content << "\n";
        }

        // Calculate dynamic height based on number of items
        int chart_height = std::max(400, static_cast<int>(data.labels.size() * 30));

        // Build gnuplot script
        std::ostringstream script;
        script << "reset\n";
        script << "set terminal pngcairo size 800," << chart_height
               << " enhanced font 'Arial," << config.font_size << "'\n";
        script << "set output 'temp_chart_output.png'\n\n";

        // Styling
        script << "set border lw 1.5\n";
        if (config.show_grid) {
            script << "set grid xtics lc rgb '#e0e0e0' lt 1 lw 0.5\n";
        }
        script << "unset key\n\n";

        // Labels
        if (!data.x_label.empty()) {
            script << "set xlabel '" << escape_gnuplot_string(data.x_label) << "'\n";
        }
        if (!data.y_label.empty()) {
            script << "set ylabel '" << escape_gnuplot_string(data.y_label) << "'\n";
        }
        if (!data.title.empty()) {
            script << "set title '" << escape_gnuplot_string(data.title) << "'\n";
        }

        // X-axis
        script << "set format x '$%.0f'\n\n";

        // Y-axis (categories)
        script << "set style data histogram\n";
        script << "set style histogram cluster gap 0\n";
        script << "set style fill solid border -1\n";
        script << "set boxwidth " << config.box_width << "\n";
        script << "set yrange [-0.5:" << (data.labels.size() - 0.5) << "]\n";
        script << "set ytics (";
        for (size_t i = 0; i < data.labels.size(); ++i) {
            if (i > 0) script << ", ";
            script << "'" << data.labels[i] << "' " << i;
        }
        script << ")\n\n";

        // Plot with conditional coloring
        // For boxxy: using x_center:y_center:x_halfwidth:y_halfwidth
        // Plot positive values (from 0 to value)
        script << "plot 'temp_chart_data.txt' using ($2 > 0 ? $2/2 : 1/0):($0):(abs($2)/2):(" << config.box_width/2
               << ") with boxxy lc rgb '" << config.positive_color << "' notitle, \\\n";
        // Plot negative values (from value to 0)
        script << "     'temp_chart_data.txt' using ($2 < 0 ? $2/2 : 1/0):($0):(abs($2)/2):(" << config.box_width/2
               << ") with boxxy lc rgb '" << config.negative_color << "' notitle, \\\n";
        // Plot zero values as tiny markers (ensures chart appears even when all PnL is zero)
        script << "     'temp_chart_data.txt' using (abs($2) < 0.01 ? 0.1 : 1/0):($0):(0.1):(" << config.box_width/2
               << ") with boxxy lc rgb '#cccccc' notitle\n";

        return execute_gnuplot(script.str(), data_content.str());

    } catch (const std::exception& e) {
        ERROR("Exception rendering horizontal bar chart: " + std::string(e.what()));
        mark_chart_failed("the chart could not be drawn");
        return "";
    }
}

// REPLACE the render_pie_chart function with this version that ensures legend visibility:

std::string ChartGenerator::render_pie_chart(const ChartData& data, const ChartConfig& config) {
    if (data.labels.empty() || data.values.empty()) {
        WARN("No data provided for pie chart");
        return "";
    }

    if (data.labels.size() != data.values.size()) {
        ERROR("Labels and values size mismatch in pie chart");
        return "";
    }

    try {
        // Calculate total and verify non-zero
        double total = 0.0;
        for (double val : data.values) {
            total += std::abs(val);
        }

        if (total < 0.01) {
            WARN("Total is near zero, cannot create pie chart");
            return "";
        }

        // Color palette for pie slices
        std::vector<std::string> colors = {
            "#2c5aa0",  // Blue
            "#1a7f37",  // Green
            "#f59e0b",  // Orange
            "#8b5cf6",  // Purple
            "#ec4899",  // Pink
            "#0891b2",  // Cyan
            "#dc2626",  // Red
            "#65a30d",  // Lime
            "#7c3aed",  // Violet
            "#db2777"   // Rose
        };

        // Build data file - separate blocks for each slice
        std::ostringstream data_content;
        double cumulative_angle = 0.0;
        
        for (size_t i = 0; i < data.labels.size(); ++i) {
            double value = std::abs(data.values[i]);
            double percentage = (value / total) * 100.0;
            (void)percentage;
            double angle_span = (value / total) * 360.0;
            double start_angle = cumulative_angle;
            double end_angle = cumulative_angle + angle_span;
            
            // Create parametric data for this wedge
            int steps = std::max(20, static_cast<int>(angle_span / 3.0));
            for (int j = 0; j <= steps; ++j) {
                double t = static_cast<double>(j) / steps;
                double angle = start_angle + t * angle_span;
                double rad = angle * M_PI / 180.0;
                
                // Output: x y
                data_content << std::fixed << std::setprecision(4)
                            << std::cos(rad) << " " << std::sin(rad) << "\n";
            }
            
            // Blank line separates blocks for gnuplot index
            if (i < data.labels.size() - 1) {
                data_content << "\n\n";
            }
            
            cumulative_angle = end_angle;
        }

        // Build gnuplot script
        std::ostringstream script;
        script << "reset\n";
        script << "set terminal pngcairo size " << config.width << "," << config.height
               << " enhanced font 'Arial," << config.font_size << "'\n";
        script << "set output 'temp_chart_output.png'\n\n";

        if (!data.title.empty()) {
            script << "set title '" << escape_gnuplot_string(data.title) << "' font 'Arial," << (config.font_size + 2) << ",bold'\n";
        }

        // Define colors
        for (size_t i = 0; i < colors.size(); ++i) {
            script << "set linetype " << (i + 1) << " lc rgb '" << colors[i] << "'\n";
        }
        script << "\n";

        // Chart settings - make room for legend on right (closer to pie)
        script << "set size ratio -1\n";
        script << "set xrange [-1.4:2.2]\n";  // Tighter right margin for legend
        script << "set yrange [-1.3:1.3]\n";
        script << "unset xtics\n";
        script << "unset ytics\n";
        script << "unset border\n";
        script << "set key at 1.3,0 center left\n";  // Position legend closer to pie
        script << "set key font 'Arial," << (config.font_size - 1) << "'\n";
        script << "set key spacing 1.2\n";
        script << "set key samplen 1.2\n";
        script << "set key width 0\n";
        script << "set key box lw 1\n\n";  // Box around legend

        // Plot wedges with explicit legend entries
        script << "plot ";
        
        for (size_t i = 0; i < data.labels.size(); ++i) {
            if (i > 0) script << ", \\\n     ";
            
            double percentage = (std::abs(data.values[i]) / total) * 100.0;
            
            // Each wedge as a filled polygon with legend
            script << "'temp_chart_data.txt' index " << i << " ";
            script << "using 1:2 ";
            script << "with filledcurves xy=0,0 ";
            script << "lt " << ((i % colors.size()) + 1) << " ";
            script << "fs solid 0.85 ";
            script << "title '" << data.labels[i] << " (" 
                   << std::fixed << std::setprecision(1) << percentage << "%)'";
        }
        
        script << "\n";

        return execute_gnuplot(script.str(), data_content.str());

    } catch (const std::exception& e) {
        ERROR("Exception rendering pie chart: " + std::string(e.what()));
        mark_chart_failed("the chart could not be drawn");
        return "";
    }
}
// ============================================================================
// ChartGenerator Implementation - High-Level Functions (Original)
// ============================================================================

std::string ChartGenerator::generate_equity_curve_chart(std::shared_ptr<DatabaseInterface> db,
                                                        const std::string& strategy_id,
                                                        const std::string& portfolio_id,
                                                        const std::string& date,
                                                        int lookback_days) {
    begin_chart();
    // Fetch data using modular fetcher
    ChartData data = fetch_equity_curve_data(db, strategy_id, portfolio_id, date, lookback_days);

    if (data.labels.empty() || data.values.empty()) {
        return "";
    }

    // Configure chart appearance
    ChartConfig config;
    config.width = 1000;
    config.height = 500;
    config.rotate_x_labels = true;
    config.x_label_angle = -45;

    // Render using modular renderer
    return render_line_chart(data, config);
}

std::string ChartGenerator::generate_pnl_by_symbol_chart(std::shared_ptr<DatabaseInterface> db,
                                                         const std::string& strategy_id,
                                                         const std::string& portfolio_id,
                                                         const std::string& date, bool equities) {
    begin_chart();
    INFO("Starting PnL by symbol chart generation for strategy: " + strategy_id);

    // Fetch data using modular fetcher
    ChartData data = fetch_pnl_by_symbol_data(db, strategy_id, portfolio_id, date, equities);

    if (data.labels.empty() || data.values.empty()) {
        WARN("No data available for PnL by symbol chart - returning empty string");
        return "";
    }

    // Render using modular renderer
    ChartConfig config;
    config.rotate_x_labels = true;
    config.x_label_angle = -45;
    std::string result = render_bar_chart(data, config);

    if (result.empty()) {
        ERROR("Failed to generate PnL by symbol chart - render_horizontal_bar_chart returned empty string");
    } else {
        INFO("PnL by symbol chart generated successfully, base64 length: " + std::to_string(result.length()));
    }

    return result;
}

std::string ChartGenerator::generate_drawdown_chart(std::shared_ptr<DatabaseInterface> db,
                                                    const std::string& strategy_id,
                                                    const std::string& portfolio_id,
                                                    const std::string& date, int lookback_days) {
    begin_chart();
    // The whole stored curve gives the running peak; the last days are drawn.
    ChartData curve = fetch_equity_curve_data(db, strategy_id, portfolio_id, date, 3650);
    if (curve.labels.size() < 2 || curve.values.size() != curve.labels.size()) return "";
    ChartData data;
    double peak = curve.values.front();
    for (std::size_t i = 0; i < curve.values.size(); ++i) {
        peak = std::max(peak, curve.values[i]);
        data.labels.push_back(curve.labels[i]);
        data.values.push_back(peak > 0.0 ? (curve.values[i] / peak - 1.0) * 100.0 : 0.0);
    }
    const std::size_t keep = static_cast<std::size_t>(std::max(2, lookback_days));
    if (data.labels.size() > keep) {
        data.labels.erase(data.labels.begin(), data.labels.end() - static_cast<long>(keep));
        data.values.erase(data.values.begin(), data.values.end() - static_cast<long>(keep));
    }
    data.title = "Drawdown from the peak";
    data.y_label = "percent";
    data.y_format = "%.1f%%";
    data.has_reference_line = true;
    data.reference_line = 0.0;
    ChartConfig config;
    config.width = 1000;
    config.height = 400;
    config.rotate_x_labels = true;
    config.x_label_angle = -45;
    config.line_color = "#b42318";
    return render_line_chart(data, config);
}

std::string ChartGenerator::generate_gross_leverage_chart(std::shared_ptr<DatabaseInterface> db,
                                                          const std::string& strategy_id,
                                                          const std::string& portfolio_id,
                                                          const std::string& date,
                                                          int lookback_days) {
    begin_chart();
    if (!db) return "";
    const std::string query =
        "SELECT to_char(date, 'YYYY-MM-DD'), gross_leverage::text, net_leverage::text FROM (SELECT date, gross_leverage, net_leverage "
        "FROM trading.live_results WHERE strategy_id = '" + strategy_id + "' AND portfolio_id = '" +
        portfolio_id + "' AND date <= '" + date + "' AND gross_leverage IS NOT NULL AND net_leverage IS NOT NULL ORDER BY date DESC "
        "LIMIT " + std::to_string(lookback_days) + ") t ORDER BY date ASC";
    auto result = db->execute_query(query);
    if (result.is_error()) {
        ERROR("Failed to query gross leverage: " + std::string(result.error()->what()));
        mark_chart_failed("the data could not be read");
        return "";
    }
    auto table = result.value();
    if (!table || table->num_rows() < 2) return "";
    auto combined = table->CombineChunks();
    if (!combined.ok()) {
        mark_chart_failed("the data could not be read");
        return "";
    }
    auto text_of = [](const std::shared_ptr<arrow::Array>& a, int64_t i) -> std::string {
        if (!a || a->IsNull(i)) return "";
        if (a->type_id() == arrow::Type::STRING)
            return std::static_pointer_cast<arrow::StringArray>(a)->GetString(i);
        if (a->type_id() == arrow::Type::LARGE_STRING)
            return std::static_pointer_cast<arrow::LargeStringArray>(a)->GetString(i);
        return "";
    };
    const auto t = combined.ValueOrDie();
    ChartData data;
    for (int64_t i = 0; i < t->num_rows(); ++i) {
        const std::string day = text_of(t->column(0)->chunk(0), i);
        const std::string value = text_of(t->column(1)->chunk(0), i);
        const std::string net = text_of(t->column(2)->chunk(0), i);
        if (day.empty() || value.empty() || net.empty()) continue;
        try {
            const double g = std::stod(value), n = std::stod(net);
            data.values.push_back(g);
            data.values2.push_back(n);
            data.labels.push_back(day);
        } catch (...) {
        }
    }
    if (data.labels.size() < 2) return "";
    data.title = "Leverage on the account value";
    data.y_label = "times the account value";
    data.series_name = "Gross";
    data.series2_name = "Net";
    data.y_format = "%.1fx";
    ChartConfig config;
    config.width = 1000;
    config.height = 400;
    config.rotate_x_labels = true;
    config.x_label_angle = -45;
    return render_line_chart(data, config);
}

std::string ChartGenerator::generate_daily_pnl_chart(
    std::shared_ptr<DatabaseInterface> db,
    const std::string& strategy_id,
                                                  const std::string& portfolio_id,
    const std::string& date,
    int lookback_days)
{
    begin_chart();
    // Fetch data using modular fetcher
    ChartData data = fetch_daily_pnl_data(db, strategy_id, portfolio_id, date, lookback_days);

    if (data.labels.empty() || data.values.empty()) {
        return "";
    }

    // Configure chart appearance
    ChartConfig config;
    config.width = 1000;
    config.height = 500;
    config.rotate_x_labels = true;
    config.x_label_angle = -45;

    // Render using modular renderer
    return render_bar_chart(data, config);
}

std::string ChartGenerator::generate_total_transaction_costs_chart(
    std::shared_ptr<DatabaseInterface> db, const std::string& strategy_id,
    const std::string& portfolio_id,
    const std::string& end_date,  // "YYYY-MM-DD" (today)
    const ChartSeries::MultiplierLookup& multiplier_of) {
    begin_chart();
    ChartData data = fetch_cumulative_transaction_costs_data(db, strategy_id, portfolio_id,
                                                             end_date, multiplier_of);
    if (data.labels.empty() || data.values.empty()) return "";

    ChartConfig config;
    config.width = 1000;
    config.height = 500;
    config.rotate_x_labels = true;
    config.x_label_angle = -45;

    return render_line_chart(data, config);
}

std::string ChartGenerator::generate_margin_posted_chart(
    std::shared_ptr<DatabaseInterface> db,
    const std::string& strategy_id,
                                                  const std::string& portfolio_id,
    const std::string& date /* today */)
{
    begin_chart();
    ChartData data = fetch_margin_posted_data(db, strategy_id, portfolio_id, date);
    if (data.labels.empty() || data.values.empty()) return "";

    ChartConfig config;
    config.width = 1000;
    config.height = 500;
    config.rotate_x_labels = true;
    config.x_label_angle = -45;

    // Title can be set in data or here:
    if (data.title.empty()) data.title = "Margin Posted";

    return render_line_chart(data, config);
}

std::string ChartGenerator::generate_portfolio_composition_chart(
    const std::unordered_map<std::string, Position>& positions,
    const std::unordered_map<std::string, double>& current_prices,
    const std::string& date)
{
    begin_chart();
    INFO("Starting portfolio composition pie chart generation");

    // Fetch data
    ChartData data = fetch_portfolio_composition_data(positions, current_prices, date);

    if (data.labels.empty() || data.values.empty()) {
        WARN("No data available for portfolio composition chart");
        return "";
    }

    // Configure chart appearance
    ChartConfig config;
    config.width = 800;
    config.height = 600;

    // Render using pie chart renderer
    std::string result = render_pie_chart(data, config);

    if (result.empty()) {
        ERROR("Failed to generate portfolio composition chart");
    } else {
        INFO("Portfolio composition chart generated successfully");
    }

    return result;
}

std::string ChartGenerator::generate_cumulative_pnl_by_symbol_chart(
    std::shared_ptr<DatabaseInterface> db,
    const std::string& strategy_id,
                                                  const std::string& portfolio_id,
    const std::string& date)
{
    begin_chart();
    INFO("Starting cumulative PnL by symbol chart generation for strategy: " + strategy_id);

    // Fetch data
    ChartData data = fetch_cumulative_pnl_by_symbol_data(db, strategy_id, portfolio_id, date);

    if (data.labels.empty() || data.values.empty()) {
        WARN("No data available for cumulative PnL by symbol chart");
        return "";
    }

    // Configure chart appearance
    ChartConfig config;
    config.rotate_x_labels = true;
    config.x_label_angle = -45;

    // Render using horizontal bar chart
    std::string result = render_horizontal_bar_chart(data, config);

    if (result.empty()) {
        ERROR("Failed to generate cumulative PnL by symbol chart");
    } else {
        INFO("Cumulative PnL by symbol chart generated successfully");
    }

    return result;
}



} // namespace trade_ngin
