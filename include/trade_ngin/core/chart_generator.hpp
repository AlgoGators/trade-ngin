#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <memory>
#include "trade_ngin/core/gnuplot_run.hpp"
#include "trade_ngin/data/database_interface.hpp"

namespace trade_ngin {

/**
 * @brief Helper utilities for chart generation
 */
namespace ChartHelpers {
    /**
     * @brief Encode binary data to base64 string
     * @param data Vector of binary data
     * @return Base64 encoded string
     */
    std::string encode_to_base64(const std::vector<unsigned char>& data);

    /**
     * @brief Format a currency value with commas
     * @param value The value to format
     * @param precision Number of decimal places (default: 0)
     * @return Formatted string (e.g., "1,234,567.89")
     */
    std::string format_currency(double value, int precision = 0);
}

/**
 * @brief Data structure for chart rendering
 *
 * Contains all data needed to render a chart, independent of data source.
 */
struct ChartData {
    std::vector<std::string> labels;      ///< X-axis labels or categories
    std::vector<double> values;           ///< Primary Y-axis values
    std::vector<double> values2;          ///< Secondary values (optional, for stacked charts)
    std::string title;                    ///< Chart title
    std::string x_label;                  ///< X-axis label
    std::string y_label;                  ///< Y-axis label
    double reference_line;                ///< Reference line value (e.g., 0 or starting value)
    bool has_reference_line = false;      ///< Whether to draw reference line
    bool reference_line_on_axis = false;  ///< Line charts: widen the value axis to hold the line

    ChartData() : reference_line(0.0) {}
};

/// One row of a chart query: a date or a symbol, and its number.
using ChartPoint = std::pair<std::string, double>;

/**
 * @brief The words of a chart's heading in the report and of its title in the image.
 */
namespace ChartTitles {
/// "Yesterday's PnL by Symbol". On an equity book the bars are the day's REALIZED P&L only (a
/// futures row's daily figure is its whole settled P&L), and the heading says so (T-8D-2 R64).
inline std::string pnl_by_symbol_heading(bool equities) {
    return equities ? "Yesterday's Realized PnL by Symbol" : "Yesterday's PnL by Symbol";
}
/// The daily PnL chart holds the last `stored_days` STORED days before the run date, which on a
/// book with missing days is a longer span than that many calendar days (T-8D-2 R62).
inline std::string daily_pnl_heading(int stored_days) {
    return "Daily PnL (last " + std::to_string(stored_days) + " stored days)";
}
/// The cumulative chart sums each symbol's stored daily realized P&L; no cost is taken off
/// (T-8D-2 R62).
inline std::string cumulative_pnl_heading() {
    return "Cumulative PnL by Symbol (realized, gross of costs)";
}
/// The cumulative chart's title in the image: the heading and the day the sums run through.
inline std::string cumulative_pnl_title(const std::string& date) {
    return "Cumulative PnL by Symbol (realized, gross of costs; all-time as of " + date + ")";
}
}  // namespace ChartTitles

/**
 * @brief The text of each database chart's query, from the book's ids and the run date.
 *
 * A fetcher sends exactly this text; a test calls these to read what a chart asks for.
 */
namespace ChartQueries {
/// The newest `lookback_days` stored rows BEFORE the run date `date` (T-8D-2 R63).
std::string equity_curve(const std::string& strategy_id, const std::string& portfolio_id,
                         const std::string& date, int lookback_days);
/// The book's starting capital (total_capital of its latest live_run_metadata row on or before
/// the run date): the equity curve's reference line.
std::string book_capital(const std::string& strategy_id, const std::string& portfolio_id,
                         const std::string& date);
std::string pnl_by_symbol(const std::string& strategy_id, const std::string& portfolio_id,
                          const std::string& date);
std::string daily_pnl(const std::string& strategy_id, const std::string& portfolio_id,
                      const std::string& date, int lookback_days);
std::string cost_per_million(const std::string& strategy_id, const std::string& portfolio_id,
                             const std::string& date);
std::string margin_posted(const std::string& strategy_id, const std::string& portfolio_id,
                          const std::string& date);
std::string cumulative_pnl_by_symbol(const std::string& strategy_id,
                                     const std::string& portfolio_id, const std::string& date);
}  // namespace ChartQueries

/**
 * @brief From a chart query's rows to the points the chart plots (no database, no gnuplot).
 *
 * A series with no label is a chart that is not drawn.
 */
namespace ChartSeries {
/// One row of the cost chart's query: a stored day of the book, its cost, and ONE fill the book
/// made that day (a day of several fills is several rows; a day without a fill is one row whose
/// symbol is empty).
struct CostFillRow {
    std::string date;
    double daily_cost = 0.0;  ///< the day's stored cost total (charged after netting)
    std::string symbol;       ///< empty: the day's row with no fill
    double quantity = 0.0;
    double price = 0.0;
};

/// A symbol's contract multiplier (a futures root's contract size, 1 for a share), or none when
/// it is not on record.
using MultiplierLookup = std::function<std::optional<double>(const std::string& symbol)>;

/// rows: (date, equity) of the days before the run date, in any order. The points are those rows
/// and no other; the title names the last day as settled; the reference line is the book's
/// starting capital, or there is none when `book_capital` holds no positive number.
ChartData equity_curve(std::vector<ChartPoint> rows, std::optional<double> book_capital);
/// The first and last day of a line chart's time axis. A chart of ONE day gets the day before and
/// the day after, because an axis from a day to itself is empty and cannot be drawn. An empty
/// pair: no label, or a single label that is not a "YYYY-MM-DD" date.
std::pair<std::string, std::string> line_chart_x_range(const std::vector<std::string>& labels);
/// The value axis of a line chart whose reference line must be ON the chart (the equity curve's
/// capital line: a book that sits wholly below or above its capital would otherwise leave it off
/// the picture): from the lowest of the points and the line to the highest, with a margin. None
/// for every other chart, which the renderer scales to its points.
std::optional<std::pair<double, double>> line_chart_y_range(const ChartData& data);
/// rows: (symbol, realized PnL); date: the run date; equities: the book is an equity book. One
/// bar per symbol: rows of the same symbol are summed.
ChartData pnl_by_symbol(std::vector<ChartPoint> rows, const std::string& date,
                        bool equities = false);
/// rows: (date, daily PnL), newest first as the query returns them.
ChartData daily_pnl(std::vector<ChartPoint> rows);
/// rows: grouped by day, oldest first. A day's point is its cost / the notional TRADED that day
/// (the sum of quantity x price x multiplier over the day's fills) x 1,000,000 (T-8D-2 R64). A
/// day without a fill has no point; neither has a day with a fill whose multiplier is not on
/// record.
ChartData cost_per_million(const std::vector<CostFillRow>& rows,
                           const MultiplierLookup& multiplier_of);
/// rows: (date, margin posted), oldest first.
ChartData margin_posted(std::vector<ChartPoint> rows);
/// rows: (symbol, cumulative realized PnL); date: the run date.
ChartData cumulative_pnl_by_symbol(std::vector<ChartPoint> rows, const std::string& date);
}  // namespace ChartSeries

/**
 * @brief Configuration for chart appearance
 */
struct ChartConfig {
    int width = 1000;                     ///< Chart width in pixels
    int height = 500;                     ///< Chart height in pixels
    std::string format = "png";           ///< Output format (png, svg, etc.)
    int font_size = 11;                   ///< Base font size
    bool show_grid = true;                ///< Show grid lines
    std::string line_color = "#2c5aa0";   ///< Primary line/bar color
    std::string positive_color = "#1a7f37"; ///< Color for positive values
    std::string negative_color = "#b42318"; ///< Color for negative values
    double box_width = 0.8;               ///< Bar width (relative)
    bool rotate_x_labels = false;         ///< Rotate X-axis labels
    int x_label_angle = -45;              ///< X-axis label rotation angle
};

/**
 * @brief Chart generation class using gnuplot
 *
 * This class provides extensible chart generation capabilities using gnuplot.
 * All chart functions are static and return HTML strings with embedded base64-encoded images.
 *
 * To add a new chart type:
 * 1. Add a new static method declaration here
 * 2. Implement the method in chart_generator.cpp following the gnuplot pattern
 * 3. Use ChartHelpers utilities for common tasks
 */
class ChartGenerator {
public:
    // ========================================================================
    // MODULAR CHART COMPONENTS - Mix and match data fetchers with renderers
    // ========================================================================

    /**
     * @brief Data Fetchers - Extract data from database and format for charting
     */

    /**
     * @brief Fetch equity curve data from database
     * @param db Database interface
     * @param strategy_id Strategy identifier
     * @param date The run date (YYYY-MM-DD): the chart holds the stored days before it
     * @param lookback_days Number of days to fetch
     * @return ChartData with dates and equity values
     */
    static ChartData fetch_equity_curve_data(std::shared_ptr<DatabaseInterface> db,
                                             const std::string& strategy_id,
                                             const std::string& portfolio_id,
                                             const std::string& date, int lookback_days = 30);

    /**
     * @brief Fetch PnL by symbol data from database
     * @param db Database interface
     * @param strategy_id Strategy identifier
     * @param date Date to query
     * @return ChartData with symbols and PnL values
     */
    static ChartData fetch_pnl_by_symbol_data(std::shared_ptr<DatabaseInterface> db,
                                              const std::string& strategy_id,
                                              const std::string& portfolio_id,
                                              const std::string& date, bool equities = false);

    /**
     * @brief Fetch daily PnL data from database
     * @param db Database interface
     * @param strategy_id Strategy identifier
     * @param date Current date (to exclude today and only show up to T-1)
     * @param lookback_days Number of days to fetch
     * @return ChartData with dates and daily PnL values
     */
    static ChartData fetch_daily_pnl_data(
        std::shared_ptr<DatabaseInterface> db,
        const std::string& strategy_id,
        const std::string& portfolio_id,
        const std::string& date,
        int lookback_days = 30
    );

    /// Cost per $1M traded. `multiplier_of` empty: the instrument registry's contract multiplier.
    static ChartData fetch_cumulative_transaction_costs_data(
        std::shared_ptr<DatabaseInterface> db, const std::string& strategy_id,
        const std::string& portfolio_id, const std::string& date /* today, YYYY-MM-DD */,
        const ChartSeries::MultiplierLookup& multiplier_of = {});

    static ChartData fetch_margin_posted_data(
        std::shared_ptr<DatabaseInterface> db,
        const std::string& strategy_id,
        const std::string& portfolio_id,
        const std::string& date /* today, YYYY-MM-DD */);
    
    /**
     * @brief Fetch cumulative realized PnL by symbol (all-time)
     * @param db Database interface
     * @param strategy_id Strategy identifier
     * @param date Current date (to query up to this date)
     * @return ChartData with symbols and their cumulative realized PnL
     */
    static ChartData fetch_cumulative_pnl_by_symbol_data(
        std::shared_ptr<DatabaseInterface> db,
        const std::string& strategy_id,
        const std::string& portfolio_id,
        const std::string& date
    );

    /**
     * @brief Fetch portfolio composition data from current positions
     * @param positions Current portfolio positions
     * @param current_prices Current market prices for calculating notional
     * @param date Current date for the title (YYYY-MM-DD format)
     * @return ChartData with symbols and their gross notional percentages
     */
    static ChartData fetch_portfolio_composition_data(
        const std::unordered_map<std::string, Position>& positions,
        const std::unordered_map<std::string, double>& current_prices,
        const std::string& date = ""
    );

    /**
     * @brief Generic Chart Renderers - Render any ChartData with specified config
     */

    /**
     * @brief Render a line chart with optional points
     * @param data Chart data (labels = dates/x-values, values = y-values)
     * @param config Chart configuration
     * @return Base64-encoded PNG chart data, or empty string on error
     */
    static std::string render_line_chart(
        const ChartData& data,
        const ChartConfig& config = ChartConfig()
    );

    /**
     * @brief Render a vertical bar chart
     * @param data Chart data (labels = categories, values = bar heights)
     * @param config Chart configuration
     * @return Base64-encoded PNG chart data, or empty string on error
     */
    static std::string render_bar_chart(
        const ChartData& data,
        const ChartConfig& config = ChartConfig()
    );

    /**
     * @brief Render a horizontal bar chart
     * @param data Chart data (labels = categories, values = bar lengths)
     * @param config Chart configuration
     * @return Base64-encoded PNG chart data, or empty string on error
     */
    static std::string render_horizontal_bar_chart(
        const ChartData& data,
        const ChartConfig& config = ChartConfig()
    );

    /**
     * @brief Render a pie chart
     * @param data Chart data (labels = slice names, values = slice sizes)
     * @param config Chart configuration
     * @return Base64-encoded PNG chart data, or empty string on error
     */
    static std::string render_pie_chart(
        const ChartData& data,
        const ChartConfig& config = ChartConfig()
    );

    // ========================================================================
    // HIGH-LEVEL CONVENIENCE FUNCTIONS - Backward compatible
    // ========================================================================

    /**
     * @brief Generate portfolio equity curve chart
     *
     * Creates a time-series chart showing portfolio value over time using gnuplot.
     * The chart includes:
     * - Line plot with data points: the stored days before the run date, through the last
     *   settled day
     * - Reference line at the book's starting capital (from its run metadata), or none
     * - Formatted axes with dates and currency
     *
     * @param db Database interface to query equity curve data
     * @param strategy_id Strategy identifier to filter data
     * @param date The run date (YYYY-MM-DD)
     * @param lookback_days Number of days to include in the chart
     * @return Base64-encoded PNG chart data, or empty string on error
     */
    static std::string generate_equity_curve_chart(std::shared_ptr<DatabaseInterface> db,
                                                   const std::string& strategy_id,
                                                   const std::string& portfolio_id,
                                                   const std::string& date, int lookback_days = 30);

    /**
     * @brief Generate PnL by symbol chart for previous day
     *
     * Creates a horizontal bar chart showing realized + unrealized PnL per symbol
     * for the previous trading day. Bars are color-coded (green for positive, red for negative).
     *
     * @param db Database interface to query positions data
     * @param strategy_id Strategy identifier to filter data
     * @param date Date to query (will show previous day's PnL)
     * @return Base64-encoded PNG chart data, or empty string on error
     */
    static std::string generate_pnl_by_symbol_chart(std::shared_ptr<DatabaseInterface> db,
                                                    const std::string& strategy_id,
                                                    const std::string& portfolio_id,
                                                    const std::string& date, bool equities = false);

    /**
     * @brief Generate daily PnL chart
     *
     * Creates a vertical bar chart showing daily PnL for the last N trading days.
     * Uses daily_pnl from live_results table, showing data up to T-1 (yesterday).
     * Bars are color-coded (green for positive days, red for negative days).
     *
     * @param db Database interface to query live_results data
     * @param strategy_id Strategy identifier to filter data
     * @param date Current date (to exclude today and only show up to T-1)
     * @param lookback_days Number of days to include in the chart (default: 30)
     * @return Base64-encoded PNG chart data, or empty string on error
     */
    static std::string generate_daily_pnl_chart(
        std::shared_ptr<DatabaseInterface> db,
        const std::string& strategy_id,
        const std::string& portfolio_id,
        const std::string& date,
        int lookback_days = 30
    );

    static std::string generate_total_transaction_costs_chart(
        std::shared_ptr<DatabaseInterface> db, const std::string& strategy_id,
        const std::string& portfolio_id, const std::string& end_date /* today, YYYY-MM-DD */,
        const ChartSeries::MultiplierLookup& multiplier_of = {});

    static std::string generate_margin_posted_chart(
        std::shared_ptr<DatabaseInterface> db,
        const std::string& strategy_id,
        const std::string& portfolio_id,
        const std::string& date);
    
    /**
     * @brief Generate portfolio composition pie chart
     *
     * Creates a pie chart showing what percentage of total portfolio exposure
     * each symbol represents, based on gross notional values. Symbols representing
     * less than 5% are grouped into "Other" category.
     *
     * @param positions Current portfolio positions
     * @param current_prices Current market prices for calculating notional
     * @param date Current date for the title (YYYY-MM-DD format)
     * @return Base64-encoded PNG chart data, or empty string on error
     */
    static std::string generate_portfolio_composition_chart(
        const std::unordered_map<std::string, Position>& positions,
        const std::unordered_map<std::string, double>& current_prices,
        const std::string& date = ""
    );

    /**
     * @brief Generate cumulative PnL by symbol chart (all-time)
     *
     * Creates a horizontal bar chart showing total realized PnL for each symbol
     * across the entire trading history. Shows which symbols have been the best
     * and worst performers historically.
     *
     * @param db Database interface to query positions data
     * @param strategy_id Strategy identifier to filter data
     * @param date Current date (to query up to this date)
     * @return Base64-encoded PNG chart data, or empty string on error
     */
    static std::string generate_cumulative_pnl_by_symbol_chart(
        std::shared_ptr<DatabaseInterface> db,
        const std::string& strategy_id,
        const std::string& portfolio_id,
        const std::string& date
    );


    /// How the chart most recently asked for on this thread ended: Drawn, NothingToPlot (no data,
    /// no process started) or Failed (the data could not be read, or the chart program hung twice,
    /// errored or left no image). `last_chart_reason` says why in plain words when it Failed.
    static core::ChartStatus last_chart_status();
    static std::string last_chart_reason();

    // Future chart types can be added here as new static methods:
    // static std::string generate_drawdown_chart(...);
    // static std::string generate_returns_distribution(...);
    // static std::string generate_position_heatmap(...);
    // static std::string generate_rolling_sharpe(...);
};

} // namespace trade_ngin
