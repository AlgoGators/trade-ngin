#pragma once

#include <string>
#include <vector>
#include <memory>
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

    ChartData() : reference_line(0.0) {}
};

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
     * @param lookback_days Number of days to fetch
     * @return ChartData with dates and equity values
     */
    static ChartData fetch_equity_curve_data(
        std::shared_ptr<DatabaseInterface> db,
        const std::string& strategy_id,
        int lookback_days = 30
    );

    /**
     * @brief Fetch PnL by symbol data from database
     * @param db Database interface
     * @param strategy_id Strategy identifier
     * @param date Date to query
     * @return ChartData with symbols and PnL values
     */
    static ChartData fetch_pnl_by_symbol_data(
        std::shared_ptr<DatabaseInterface> db,
        const std::string& strategy_id,
        const std::string& date
    );

    /**
     * @brief Fetch daily PnL data from database
     * @param db Database interface
     * @param strategy_id Strategy identifier
     * @param lookback_days Number of days to fetch
     * @return ChartData with dates and daily PnL values
     */
    static ChartData fetch_daily_pnl_data(
        std::shared_ptr<DatabaseInterface> db,
        const std::string& strategy_id,
        int lookback_days = 30
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
     * - Line plot with data points
     * - Reference line at starting value ($500k)
     * - Formatted axes with dates and currency
     * - Performance metrics in the subtitle
     *
     * @param db Database interface to query equity curve data
     * @param strategy_id Strategy identifier to filter data
     * @param lookback_days Number of days to include in the chart
     * @return Base64-encoded PNG chart data, or empty string on error
     */
    static std::string generate_equity_curve_chart(
        std::shared_ptr<DatabaseInterface> db,
        const std::string& strategy_id,
        int lookback_days = 30
    );

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
    static std::string generate_pnl_by_symbol_chart(
        std::shared_ptr<DatabaseInterface> db,
        const std::string& strategy_id,
        const std::string& date
    );

    /**
     * @brief Generate daily PnL chart
     *
     * Creates a vertical bar chart showing daily PnL for the last N trading days.
     * Calculates day-over-day equity changes from equity curve data.
     * Bars are color-coded (green for positive days, red for negative days).
     *
     * @param db Database interface to query equity curve data
     * @param strategy_id Strategy identifier to filter data
     * @param lookback_days Number of days to include in the chart (default: 30)
     * @return Base64-encoded PNG chart data, or empty string on error
     */
    static std::string generate_daily_pnl_chart(
        std::shared_ptr<DatabaseInterface> db,
        const std::string& strategy_id,
        int lookback_days = 30
    );

    // Future chart types can be added here as new static methods:
    // static std::string generate_drawdown_chart(...);
    // static std::string generate_returns_distribution(...);
    // static std::string generate_position_heatmap(...);
    // static std::string generate_rolling_sharpe(...);
};

} // namespace trade_ngin