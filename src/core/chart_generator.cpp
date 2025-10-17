#include "trade_ngin/core/chart_generator.hpp"
#include "trade_ngin/core/logger.hpp"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <chrono>

namespace trade_ngin {

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
// ChartGenerator Implementation - Data Fetchers
// ============================================================================

ChartData ChartGenerator::fetch_equity_curve_data(
    std::shared_ptr<DatabaseInterface> db,
    const std::string& strategy_id,
    int lookback_days)
{
    ChartData chart_data;

    if (!db) {
        WARN("Database interface is null, cannot fetch equity curve data");
        return chart_data;
    }

    try {
        // Query equity curve data
        std::string query =
            "SELECT timestamp, equity "
            "FROM trading.equity_curve "
            "WHERE strategy_id = '" + strategy_id + "' "
            "ORDER BY timestamp ASC "
            "LIMIT " + std::to_string(lookback_days);

        INFO("Querying equity curve with: " + query);
        auto result = db->execute_query(query);

        if (result.is_error()) {
            ERROR("Failed to query equity curve: " + std::string(result.error()->what()));
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
            for (int i = date_equity_pairs.size() - 2; i >= 0; i--) {
                std::tm tm1 = {}, tm2 = {};
                std::istringstream ss1(date_equity_pairs[i].first);
                std::istringstream ss2(recent_data.front().first);
                ss1 >> std::get_time(&tm1, "%Y-%m-%d");
                ss2 >> std::get_time(&tm2, "%Y-%m-%d");

                if (!ss1.fail() && !ss2.fail()) {
                    auto t1 = std::chrono::system_clock::from_time_t(std::mktime(&tm1));
                    auto t2 = std::chrono::system_clock::from_time_t(std::mktime(&tm2));
                    auto days_diff = std::chrono::duration_cast<std::chrono::hours>(t2 - t1).count() / 24;

                    // If gap is more than 5 days, stop here
                    if (days_diff > 5) {
                        INFO("Found data gap of " + std::to_string(days_diff) + " days, using recent data only");
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

        // Remove the last day's data (show up to yesterday only)
        if (dates.size() > 1) {
            std::string removed_date = dates.back();
            dates.pop_back();
            equity_values.pop_back();
            INFO("Removed last day (" + removed_date + ") - showing up to previous day only");
        }

        // Prepend a starting point at $500k one day before the first data point
        std::string first_date = dates.front();
        std::tm tm = {};
        std::istringstream ss(first_date);
        ss >> std::get_time(&tm, "%Y-%m-%d");

        if (!ss.fail()) {
            // Subtract one day
            auto first_timepoint = std::chrono::system_clock::from_time_t(std::mktime(&tm));
            auto day_before = first_timepoint - std::chrono::hours(24);
            auto day_before_time_t = std::chrono::system_clock::to_time_t(day_before);
            std::tm* day_before_tm = std::gmtime(&day_before_time_t);

            std::ostringstream date_oss;
            date_oss << std::put_time(day_before_tm, "%Y-%m-%d");
            std::string starting_date = date_oss.str();

            // Insert starting point at beginning
            dates.insert(dates.begin(), starting_date);
            equity_values.insert(equity_values.begin(), 500000.0);

            INFO("Added starting point at $500k on " + starting_date);
        }

        // Populate ChartData
        chart_data.labels = dates;
        chart_data.values = equity_values;
        chart_data.x_label = "Date";
        chart_data.y_label = "Portfolio Value ($)";
        chart_data.reference_line = 500000.0;
        chart_data.has_reference_line = true;

        return chart_data;

    } catch (const std::exception& e) {
        ERROR("Exception fetching equity curve data: " + std::string(e.what()));
        return chart_data;
    }
}

ChartData ChartGenerator::fetch_pnl_by_symbol_data(
    std::shared_ptr<DatabaseInterface> db,
    const std::string& strategy_id,
    const std::string& date)
{
    ChartData chart_data;

    if (!db) {
        WARN("Database interface is null, cannot fetch PnL by symbol data");
        return chart_data;
    }

    try {
        // Query most recent positions with daily PnL
        std::string query =
            "SELECT symbol, daily_realized_pnl, daily_unrealized_pnl "
            "FROM trading.positions "
            "WHERE strategy_id = '" + strategy_id + "' "
            "ORDER BY last_update DESC";

        INFO("Querying PnL by symbol with: " + query);
        auto result = db->execute_query(query);

        if (result.is_error()) {
            ERROR("Failed to query PnL by symbol: " + std::string(result.error()->what()));
            return chart_data;
        }

        auto table = result.value();
        if (!table || table->num_rows() == 0) {
            WARN("No PnL data available");
            return chart_data;
        }

        INFO("Retrieved " + std::to_string(table->num_rows()) + " symbols with PnL");

        // Extract data
        auto combined_result = table->CombineChunks();
        if (!combined_result.ok()) {
            ERROR("Failed to combine chunks");
            return chart_data;
        }
        auto combined = combined_result.ValueOrDie();

        std::vector<std::pair<std::string, double>> symbol_pnl_data;

        auto symbol_col = combined->column(0);
        auto realized_col = combined->column(1);
        auto unrealized_col = combined->column(2);

        for (int64_t i = 0; i < combined->num_rows(); ++i) {
            try {
                // Extract symbol
                std::string symbol;
                auto sym_chunk = symbol_col->chunk(0);
                if (sym_chunk->type_id() == arrow::Type::STRING) {
                    auto str_array = std::static_pointer_cast<arrow::StringArray>(sym_chunk);
                    if (!str_array->IsNull(i)) {
                        symbol = str_array->GetString(i);
                    }
                } else if (sym_chunk->type_id() == arrow::Type::LARGE_STRING) {
                    auto str_array = std::static_pointer_cast<arrow::LargeStringArray>(sym_chunk);
                    if (!str_array->IsNull(i)) {
                        symbol = str_array->GetString(i);
                    }
                }

                // Extract PnL values
                double realized_pnl = 0.0;
                double unrealized_pnl = 0.0;

                auto real_chunk = realized_col->chunk(0);
                if (!real_chunk->IsNull(i)) {
                    if (real_chunk->type_id() == arrow::Type::DOUBLE) {
                        auto dbl_array = std::static_pointer_cast<arrow::DoubleArray>(real_chunk);
                        realized_pnl = dbl_array->Value(i);
                    } else if (real_chunk->type_id() == arrow::Type::FLOAT) {
                        auto flt_array = std::static_pointer_cast<arrow::FloatArray>(real_chunk);
                        realized_pnl = static_cast<double>(flt_array->Value(i));
                    }
                }

                auto unreal_chunk = unrealized_col->chunk(0);
                if (!unreal_chunk->IsNull(i)) {
                    if (unreal_chunk->type_id() == arrow::Type::DOUBLE) {
                        auto dbl_array = std::static_pointer_cast<arrow::DoubleArray>(unreal_chunk);
                        unrealized_pnl = dbl_array->Value(i);
                    } else if (unreal_chunk->type_id() == arrow::Type::FLOAT) {
                        auto flt_array = std::static_pointer_cast<arrow::FloatArray>(unreal_chunk);
                        unrealized_pnl = static_cast<double>(flt_array->Value(i));
                    }
                }

                double total_pnl = realized_pnl + unrealized_pnl;
                // Include ALL positions to ensure chart appears (no filtering by PnL amount)
                if (!symbol.empty()) {
                    symbol_pnl_data.push_back({symbol, total_pnl});
                }
            } catch (const std::exception& e) {
                WARN("Error processing row " + std::to_string(i) + ": " + std::string(e.what()));
                continue;
            }
        }

        if (symbol_pnl_data.empty()) {
            WARN("No valid PnL data extracted");
            return chart_data;
        }

        // Sort by PnL (descending)
        std::sort(symbol_pnl_data.begin(), symbol_pnl_data.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });

        // Populate ChartData
        for (const auto& [symbol, pnl] : symbol_pnl_data) {
            chart_data.labels.push_back(symbol);
            chart_data.values.push_back(pnl);
        }

        chart_data.x_label = "PnL ($)";
        chart_data.y_label = "Symbol";
        chart_data.reference_line = 0.0;
        chart_data.has_reference_line = false;

        return chart_data;

    } catch (const std::exception& e) {
        ERROR("Exception fetching PnL by symbol data: " + std::string(e.what()));
        return chart_data;
    }
}

ChartData ChartGenerator::fetch_daily_pnl_data(
    std::shared_ptr<DatabaseInterface> db,
    const std::string& strategy_id,
    int lookback_days)
{
    ChartData chart_data;

    if (!db) {
        WARN("Database interface is null, cannot fetch daily PnL data");
        return chart_data;
    }

    try {
        // Query equity curve data - get LAST N+1 days (matching equity curve approach)
        std::string query =
            "SELECT timestamp, equity "
            "FROM trading.equity_curve "
            "WHERE strategy_id = '" + strategy_id + "' "
            "ORDER BY timestamp DESC "
            "LIMIT " + std::to_string(lookback_days + 1);

        INFO("Querying daily PnL data with: " + query);
        auto result = db->execute_query(query);

        if (result.is_error()) {
            ERROR("Failed to query daily PnL: " + std::string(result.error()->what()));
            return chart_data;
        }

        auto table = result.value();
        if (!table || table->num_rows() < 2) {
            WARN("Insufficient data for daily PnL chart (need at least 2 days)");
            return chart_data;
        }

        INFO("Retrieved " + std::to_string(table->num_rows()) + " rows for daily PnL");

        // Extract data
        auto combined_result = table->CombineChunks();
        if (!combined_result.ok()) {
            ERROR("Failed to combine chunks");
            return chart_data;
        }
        auto combined = combined_result.ValueOrDie();

        std::vector<std::string> dates;
        std::vector<double> equity_values;

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

        if (dates.size() < 2) {
            ERROR("Insufficient valid data for daily PnL chart");
            return chart_data;
        }

        // Reverse to get chronological order (DESC query returns newest first)
        std::reverse(dates.begin(), dates.end());
        std::reverse(equity_values.begin(), equity_values.end());

        // Calculate daily PnL
        for (size_t i = 1; i < equity_values.size(); ++i) {
            double daily_pnl = equity_values[i] - equity_values[i-1];
            chart_data.labels.push_back(dates[i]);
            chart_data.values.push_back(daily_pnl);
        }

        chart_data.x_label = "Date";
        chart_data.y_label = "Daily PnL ($)";
        chart_data.reference_line = 0.0;
        chart_data.has_reference_line = true;

        return chart_data;

    } catch (const std::exception& e) {
        ERROR("Exception fetching daily PnL data: " + std::string(e.what()));
        return chart_data;
    }
}

// ============================================================================
// ChartGenerator Implementation - Generic Chart Renderers
// ============================================================================

namespace {
    // Helper: Execute gnuplot script and return base64-encoded PNG
    std::string execute_gnuplot(const std::string& script_content, const std::string& data_content) {
        // Create temporary files
        std::string data_filename = "temp_chart_data.txt";
        std::string script_filename = "temp_chart_script.gnu";
        std::string chart_filename = "temp_chart_output.png";

        // Write data file
        std::ofstream data_file(data_filename);
        if (!data_file.is_open()) {
            ERROR("Failed to create temporary data file");
            return "";
        }
        data_file << data_content;
        data_file.close();

        // Write script file
        std::ofstream script_file(script_filename);
        if (!script_file.is_open()) {
            ERROR("Failed to create gnuplot script file");
            return "";
        }
        script_file << script_content;
        script_file.close();

        // Execute gnuplot
        std::ostringstream cmd;
        cmd << "gnuplot " << script_filename << " 2>&1";
        INFO("Executing gnuplot");

        FILE* pipe = popen(cmd.str().c_str(), "r");
        if (!pipe) {
            ERROR("Failed to execute gnuplot command");
            return "";
        }

        char buffer[256];
        std::string gnuplot_output;
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            gnuplot_output += buffer;
        }

        int return_code = pclose(pipe);
        if (return_code != 0) {
            ERROR("Gnuplot failed with code " + std::to_string(return_code));
            if (!gnuplot_output.empty()) {
                ERROR("Gnuplot output: " + gnuplot_output);
            }
            return "";
        }

        // Read generated PNG
        std::ifstream chart_file(chart_filename, std::ios::binary);
        if (!chart_file.is_open()) {
            ERROR("Failed to open generated chart file");
            return "";
        }

        std::vector<unsigned char> chart_data_bin((std::istreambuf_iterator<char>(chart_file)),
                                                   std::istreambuf_iterator<char>());
        chart_file.close();

        // Cleanup
        std::remove(data_filename.c_str());
        std::remove(script_filename.c_str());
        std::remove(chart_filename.c_str());

        // Base64 encode
        return ChartHelpers::encode_to_base64(chart_data_bin);
    }
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

    try {
        // Build data file content
        std::ostringstream data_content;
        for (size_t i = 0; i < data.labels.size(); ++i) {
            data_content << data.labels[i] << " " << std::fixed << std::setprecision(2)
                        << data.values[i] << "\n";
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
        script << "unset key\n\n";

        // Labels
        if (!data.x_label.empty()) {
            script << "set xlabel '" << data.x_label << "'\n";
        }
        if (!data.y_label.empty()) {
            script << "set ylabel '" << data.y_label << "'\n";
        }
        if (!data.title.empty()) {
            script << "set title '" << data.title << "'\n";
        }

        // X-axis (time data)
        script << "set xdata time\n";
        script << "set timefmt '%Y-%m-%d'\n";
        script << "set format x '%m/%d'\n";
        script << "set xrange ['" << data.labels.front() << "':'" << data.labels.back() << "']\n";

        // X-axis ticks
        int num_ticks = std::min(5, static_cast<int>(data.labels.size()));
        script << "set xtics (";
        for (int i = 0; i < num_ticks; i++) {
            int idx = (i * (data.labels.size() - 1)) / std::max(1, num_ticks - 1);
            if (i > 0) script << ", ";
            script << "'" << data.labels[idx] << "' '" << data.labels[idx] << "'";
        }
        script << ")";
        if (config.rotate_x_labels) {
            script << " rotate by " << config.x_label_angle;
        }
        script << "\n\n";

        // Y-axis
        script << "set format y '$%.0f'\n\n";

        // Reference line
        if (data.has_reference_line) {
            script << "set arrow from graph 0, first " << data.reference_line
                   << " to graph 1, first " << data.reference_line
                   << " nohead lc rgb '#666666' lt 2 lw 1 back\n\n";
        }

        // Plot
        script << "plot 'temp_chart_data.txt' using 1:2 with linespoints ls 1 notitle\n";

        return execute_gnuplot(script.str(), data_content.str());

    } catch (const std::exception& e) {
        ERROR("Exception rendering line chart: " + std::string(e.what()));
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
        // Build data file content
        std::ostringstream data_content;
        for (size_t i = 0; i < data.labels.size(); ++i) {
            data_content << data.labels[i] << " " << std::fixed << std::setprecision(2)
                        << data.values[i] << "\n";
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
        script << "set border lw 1.5\n";
        if (config.show_grid) {
            script << "set grid ytics lc rgb '#e0e0e0' lt 1 lw 0.5\n";
        }
        script << "unset key\n\n";

        // Labels
        if (!data.x_label.empty()) {
            script << "set xlabel '" << data.x_label << "'\n";
        }
        if (!data.y_label.empty()) {
            script << "set ylabel '" << data.y_label << "'\n";
        }
        if (!data.title.empty()) {
            script << "set title '" << data.title << "'\n";
        }

        // X-axis (time data)
        script << "set xdata time\n";
        script << "set timefmt '%Y-%m-%d'\n";
        script << "set format x '%m/%d'\n";
        script << "set xrange ['" << data.labels.front() << "':'" << data.labels.back() << "']\n";

        // X-axis ticks
        int num_ticks = std::min(8, static_cast<int>(data.labels.size()));
        script << "set xtics (";
        for (int i = 0; i < num_ticks; i++) {
            int idx = (i * (data.labels.size() - 1)) / std::max(1, num_ticks - 1);
            if (i > 0) script << ", ";
            script << "'" << data.labels[idx] << "' '" << data.labels[idx] << "'";
        }
        script << ")";
        if (config.rotate_x_labels) {
            script << " rotate by " << config.x_label_angle;
        }
        script << "\n\n";

        // Y-axis
        script << "set format y '$%.0f'\n\n";

        // Reference line (typically at zero for PnL)
        if (data.has_reference_line) {
            script << "set arrow from graph 0, first " << data.reference_line
                   << " to graph 1, first " << data.reference_line
                   << " nohead lc rgb '#666666' lt 2 lw 1\n\n";
        }

        // Bar chart with conditional coloring
        script << "set style fill solid border -1\n";
        script << "set boxwidth " << config.box_width << " relative\n";
        script << "plot 'temp_chart_data.txt' using 1:($2 >= 0 ? $2 : 0) with boxes lc rgb '"
               << config.positive_color << "' notitle, \\\n";
        script << "     'temp_chart_data.txt' using 1:($2 < 0 ? $2 : 0) with boxes lc rgb '"
               << config.negative_color << "' notitle\n";

        return execute_gnuplot(script.str(), data_content.str());

    } catch (const std::exception& e) {
        ERROR("Exception rendering bar chart: " + std::string(e.what()));
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

    try {
        // Build data file content (symbol pnl)
        std::ostringstream data_content;
        for (size_t i = 0; i < data.labels.size(); ++i) {
            data_content << data.labels[i] << " " << std::fixed << std::setprecision(2)
                        << data.values[i] << "\n";
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
            script << "set xlabel '" << data.x_label << "'\n";
        }
        if (!data.y_label.empty()) {
            script << "set ylabel '" << data.y_label << "'\n";
        }
        if (!data.title.empty()) {
            script << "set title '" << data.title << "'\n";
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
        script << "plot 'temp_chart_data.txt' using ($2 >= 0 ? $2 : 0):0:(" << config.box_width
               << "):($2 >= 0 ? 0x" << config.positive_color.substr(1) << " : 0xffffff) with boxxy lc rgb variable notitle, \\\n";
        script << "     'temp_chart_data.txt' using ($2 < 0 ? $2 : 0):0:(" << config.box_width
               << "):($2 < 0 ? 0x" << config.negative_color.substr(1) << " : 0xffffff) with boxxy lc rgb variable notitle\n";

        return execute_gnuplot(script.str(), data_content.str());

    } catch (const std::exception& e) {
        ERROR("Exception rendering horizontal bar chart: " + std::string(e.what()));
        return "";
    }
}

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
        // Calculate percentages
        double total = 0.0;
        for (double val : data.values) {
            total += std::abs(val);  // Use absolute values for pie chart
        }

        if (total == 0.0) {
            WARN("Total is zero, cannot create pie chart");
            return "";
        }

        // Build data file content with percentages
        std::ostringstream data_content;
        for (size_t i = 0; i < data.labels.size(); ++i) {
            double percentage = (std::abs(data.values[i]) / total) * 100.0;
            data_content << data.labels[i] << " " << std::fixed << std::setprecision(1)
                        << percentage << "\n";
        }

        // Build gnuplot script for pie chart
        std::ostringstream script;
        script << "reset\n";
        script << "set terminal pngcairo size " << config.width << "," << config.height
               << " enhanced font 'Arial," << config.font_size << "'\n";
        script << "set output 'temp_chart_output.png'\n\n";

        if (!data.title.empty()) {
            script << "set title '" << data.title << "'\n";
        }

        // Pie chart settings
        script << "set size ratio -1\n";
        script << "set xrange [-1.2:1.2]\n";
        script << "set yrange [-1.2:1.2]\n";
        script << "unset xtics\n";
        script << "unset ytics\n";
        script << "unset border\n";
        script << "set key outside right\n\n";

        // Color palette (using various colors)
        script << "set palette defined (0 '#2c5aa0', 1 '#1a7f37', 2 '#b42318', 3 '#f59e0b', 4 '#8b5cf6', 5 '#ec4899')\n";
        script << "unset colorbox\n\n";

        // Note: Pie charts in gnuplot are complex - this is a simplified version
        // For production, consider using a dedicated plotting library
        script << "# Simplified pie representation using circles\n";
        script << "set style fill solid\n";
        script << "plot 'temp_chart_data.txt' using (0):(0):(1):($0) with circles lc palette notitle\n";

        WARN("Pie chart rendering is simplified - consider using a specialized visualization library for production");
        return execute_gnuplot(script.str(), data_content.str());

    } catch (const std::exception& e) {
        ERROR("Exception rendering pie chart: " + std::string(e.what()));
        return "";
    }
}

// ============================================================================
// ChartGenerator Implementation - High-Level Functions (Original)
// ============================================================================

std::string ChartGenerator::generate_equity_curve_chart(
    std::shared_ptr<DatabaseInterface> db,
    const std::string& strategy_id,
    int lookback_days)
{
    if (!db) {
        WARN("Database interface is null, cannot generate portfolio chart");
        return "";
    }

    try {
        // Query equity curve data
        std::string query =
            "SELECT timestamp, equity "
            "FROM trading.equity_curve "
            "WHERE strategy_id = '" + strategy_id + "' "
            "ORDER BY timestamp ASC "
            "LIMIT " + std::to_string(lookback_days);

        INFO("Querying equity curve with: " + query);
        auto result = db->execute_query(query);

        if (result.is_error()) {
            ERROR("Failed to query equity curve: " + std::string(result.error()->what()));
            return "";
        }

        auto table = result.value();
        if (!table || table->num_rows() == 0) {
            WARN("No equity curve data available");
            return "";
        }

        INFO("Retrieved " + std::to_string(table->num_rows()) + " rows for chart");

        // Create temporary data file
        std::string data_filename = "temp_equity_data.txt";
        std::ofstream data_file(data_filename);
        if (!data_file.is_open()) {
            ERROR("Failed to create temporary data file");
            return "";
        }

        // Extract and write data
        std::vector<std::string> dates;
        std::vector<double> equity_values;

        auto combined_result = table->CombineChunks();
        if (!combined_result.ok()) {
            ERROR("Failed to combine chunks");
            data_file.close();
            return "";
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
                    // Write to data file: date equity
                    data_file << date_str << " " << std::fixed << std::setprecision(2) << equity << "\n";
                }
            } catch (const std::exception& e) {
                WARN("Error processing row " + std::to_string(i) + ": " + std::string(e.what()));
                continue;
            }
        }

        data_file.close();

        if (dates.empty() || equity_values.empty()) {
            ERROR("No valid data extracted");
            return "";
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
            for (int i = date_equity_pairs.size() - 2; i >= 0; i--) {
                std::tm tm1 = {}, tm2 = {};
                std::istringstream ss1(date_equity_pairs[i].first);
                std::istringstream ss2(recent_data.front().first);
                ss1 >> std::get_time(&tm1, "%Y-%m-%d");
                ss2 >> std::get_time(&tm2, "%Y-%m-%d");

                if (!ss1.fail() && !ss2.fail()) {
                    auto t1 = std::chrono::system_clock::from_time_t(std::mktime(&tm1));
                    auto t2 = std::chrono::system_clock::from_time_t(std::mktime(&tm2));
                    auto days_diff = std::chrono::duration_cast<std::chrono::hours>(t2 - t1).count() / 24;

                    // If gap is more than 5 days, stop here
                    if (days_diff > 5) {
                        INFO("Found data gap of " + std::to_string(days_diff) + " days, using recent data only");
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
            return "";
        }

        // Remove the last day's data (show up to yesterday only)
        if (dates.size() > 1) {
            std::string removed_date = dates.back();
            dates.pop_back();
            equity_values.pop_back();
            INFO("Removed last day (" + removed_date + ") - showing up to previous day only");
        }

        // Prepend a starting point at $500k one day before the first data point
        std::string first_date = dates.front();
        std::tm tm = {};
        std::istringstream ss(first_date);
        ss >> std::get_time(&tm, "%Y-%m-%d");

        if (!ss.fail()) {
            // Subtract one day
            auto first_timepoint = std::chrono::system_clock::from_time_t(std::mktime(&tm));
            auto day_before = first_timepoint - std::chrono::hours(24);
            auto day_before_time_t = std::chrono::system_clock::to_time_t(day_before);
            std::tm* day_before_tm = std::gmtime(&day_before_time_t);

            std::ostringstream date_oss;
            date_oss << std::put_time(day_before_tm, "%Y-%m-%d");
            std::string starting_date = date_oss.str();

            // Insert starting point at beginning
            dates.insert(dates.begin(), starting_date);
            equity_values.insert(equity_values.begin(), 500000.0);

            INFO("Added starting point at $500k on " + starting_date);
        }

        // Rewrite data file with filtered and starting data
        std::ofstream data_file_rewrite(data_filename);
        if (!data_file_rewrite.is_open()) {
            ERROR("Failed to rewrite data file");
            return "";
        }

        for (size_t i = 0; i < dates.size(); i++) {
            data_file_rewrite << dates[i] << " " << std::fixed << std::setprecision(2)
                             << equity_values[i] << "\n";
        }
        data_file_rewrite.close();

        INFO("Data file rewritten with " + std::to_string(dates.size()) + " points");

        // Calculate statistics
        double first_val = 500000.0;  // Always start from initial capital
        double last_val = equity_values.back();
        double change = last_val - first_val;
        double change_pct = (first_val != 0) ? (change / first_val) * 100.0 : 0.0;
        double min_val = *std::min_element(equity_values.begin(), equity_values.end());
        double max_val = *std::max_element(equity_values.begin(), equity_values.end());

        // Create gnuplot script
        std::string script_filename = "temp_plot_script.gnu";
        std::ofstream script_file(script_filename);
        if (!script_file.is_open()) {
            ERROR("Failed to create gnuplot script file");
            return "";
        }

        std::string chart_filename = "portfolio_chart.png";
        std::string perf_sign = (change >= 0) ? "+" : "";

        // Format title with proper percentage
        std::ostringstream title_stream;
        title_stream << "Equity Curve - " << perf_sign << "$" << ChartHelpers::format_currency(change)
                    << " (" << perf_sign << std::fixed << std::setprecision(2) << change_pct << "%)";
        std::string title = title_stream.str();

        script_file << "# Gnuplot script for equity curve\n";
        script_file << "reset\n";
        script_file << "set terminal pngcairo size 1000,500 enhanced font 'Arial,11'\n";
        script_file << "set output '" << chart_filename << "'\n";
        script_file << "set bmargin 5\n\n";  // Increase bottom margin for rotated labels

        // Styling
        script_file << "# Colors and style\n";
        script_file << "set style line 1 lc rgb '#2c5aa0' lt 1 lw 3 pt 7 ps 0.8\n";
        script_file << "set style line 2 lc rgb '#666666' lt 2 lw 1 dt 2\n";
        script_file << "set border lw 1.5\n";
        script_file << "set grid ytics lc rgb '#e0e0e0' lt 1 lw 0.5\n";
        script_file << "unset key\n\n";  // Remove legend

        // Title and labels
        script_file << "# Title and labels\n";
        script_file << "set xlabel 'Date' font 'Arial,11'\n";
        script_file << "set ylabel 'Portfolio Value ($)' font 'Arial,11'\n\n";

        // X-axis (dates)
        script_file << "# X-axis settings\n";
        script_file << "set xdata time\n";
        script_file << "set timefmt '%Y-%m-%d'\n";
        script_file << "set format x '%m/%d'\n";

        // Calculate proper date range and ticks
        script_file << "set xrange ['" << dates.front() << "':'" << dates.back() << "']\n";

        // Show only specific evenly-spaced dates to avoid overlap
        // Generate explicit tic list
        int num_ticks = std::min(5, (int)dates.size());  // Show at most 5 dates
        script_file << "set xtics (";
        for (int i = 0; i < num_ticks; i++) {
            int idx = (i * (dates.size() - 1)) / std::max(1, num_ticks - 1);
            if (i > 0) script_file << ", ";
            script_file << "'" << dates[idx] << "' '" << dates[idx] << "'";
        }
        script_file << ") rotate by -45 font 'Arial,10'\n\n";

        // Y-axis (currency)
        script_file << "# Y-axis settings\n";
        script_file << "set format y '$%.0f'\n";
        script_file << "set decimalsign '.'\n";
        // Set range with fixed $2,000 padding above and below
        double y_padding = 2000.0;
        double y_min = std::max(0.0, min_val - y_padding);  // Don't go below 0
        double y_max = max_val + y_padding;
        script_file << "set yrange [" << y_min << ":" << y_max << "]\n";
        // Use better tick spacing
        double y_range = y_max - y_min;
        double tick_size = std::pow(10, std::floor(std::log10(y_range / 5)));  // ~5 ticks
        script_file << "set ytics " << tick_size << "\n\n";

        // Reference line at starting value
        script_file << "# Reference line at starting value\n";
        script_file << "set arrow from graph 0, first 500000"
                   << " to graph 1, first 500000"
                   << " nohead ls 2 back\n\n";

        // Plot without filled area
        script_file << "# Plot\n";
        script_file << "plot '" << data_filename << "' using 1:2 with linespoints ls 1 notitle, \\\n";
        script_file << "     500000 with lines ls 2 notitle\n";

        script_file.close();
        INFO("Gnuplot script created");

        // Execute gnuplot
        std::ostringstream cmd;
        cmd << "gnuplot " << script_filename << " 2>&1";

        INFO("Executing: " + cmd.str());

        FILE* pipe = popen(cmd.str().c_str(), "r");
        if (!pipe) {
            ERROR("Failed to execute gnuplot command");
            return "";
        }

        // Read output
        char buffer[256];
        std::string gnuplot_output;
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            gnuplot_output += buffer;
        }

        int return_code = pclose(pipe);

        if (return_code != 0) {
            ERROR("Gnuplot failed with code " + std::to_string(return_code));
            if (!gnuplot_output.empty()) {
                ERROR("Gnuplot output: " + gnuplot_output);
            }
            return "";
        }

        INFO("Gnuplot executed successfully");

        // Read the generated PNG
        std::ifstream chart_file(chart_filename, std::ios::binary);
        if (!chart_file.is_open()) {
            ERROR("Failed to open generated chart file: " + chart_filename);
            return "";
        }

        std::vector<unsigned char> chart_data((std::istreambuf_iterator<char>(chart_file)),
                                               std::istreambuf_iterator<char>());
        chart_file.close();

        INFO("Chart file read, size: " + std::to_string(chart_data.size()) + " bytes");

        // Base64 encode using helper
        std::string chart_base64 = ChartHelpers::encode_to_base64(chart_data);

        INFO("Chart base64 encoded, length: " + std::to_string(chart_base64.length()));

        // Clean up temporary files
        std::remove(data_filename.c_str());
        std::remove(script_filename.c_str());
        std::remove(chart_filename.c_str());

        INFO("Successfully generated portfolio chart with gnuplot");
        return chart_base64;

    } catch (const std::exception& e) {
        ERROR("Exception generating portfolio chart: " + std::string(e.what()));
        return "";
    }
}

std::string ChartGenerator::generate_pnl_by_symbol_chart(
    std::shared_ptr<DatabaseInterface> db,
    const std::string& strategy_id,
    const std::string& date)
{
    if (!db) {
        WARN("Database interface is null, cannot generate PnL by symbol chart");
        return "";
    }

    try {
        // Query most recent positions with daily PnL
        // Note: daily_realized_pnl and daily_unrealized_pnl contain YESTERDAY's PnL
        // because they are daily metrics. The positions table only stores current positions,
        // not historical snapshots, so we get the latest positions which have yesterday's PnL data.
        // We get ALL positions and filter in code to avoid missing data due to restrictive SQL filters
        std::string query =
            "SELECT symbol, daily_realized_pnl, daily_unrealized_pnl "
            "FROM trading.positions "
            "WHERE strategy_id = '" + strategy_id + "' "
            "ORDER BY last_update DESC";

        INFO("Querying PnL by symbol with: " + query);
        auto result = db->execute_query(query);

        if (result.is_error()) {
            ERROR("Failed to query PnL by symbol: " + std::string(result.error()->what()));
            return "";
        }

        auto table = result.value();
        if (!table || table->num_rows() == 0) {
            WARN("No PnL data available");
            return "";
        }

        INFO("Retrieved " + std::to_string(table->num_rows()) + " symbols with PnL");

        // Extract data
        auto combined_result = table->CombineChunks();
        if (!combined_result.ok()) {
            ERROR("Failed to combine chunks");
            return "";
        }
        auto combined = combined_result.ValueOrDie();

        std::vector<std::pair<std::string, double>> symbol_pnl_data;

        auto symbol_col = combined->column(0);
        auto realized_col = combined->column(1);
        auto unrealized_col = combined->column(2);

        for (int64_t i = 0; i < combined->num_rows(); ++i) {
            try {
                // Extract symbol
                std::string symbol;
                auto sym_chunk = symbol_col->chunk(0);
                if (sym_chunk->type_id() == arrow::Type::STRING) {
                    auto str_array = std::static_pointer_cast<arrow::StringArray>(sym_chunk);
                    if (!str_array->IsNull(i)) {
                        symbol = str_array->GetString(i);
                    }
                } else if (sym_chunk->type_id() == arrow::Type::LARGE_STRING) {
                    auto str_array = std::static_pointer_cast<arrow::LargeStringArray>(sym_chunk);
                    if (!str_array->IsNull(i)) {
                        symbol = str_array->GetString(i);
                    }
                }

                // Extract PnL values
                double realized_pnl = 0.0;
                double unrealized_pnl = 0.0;

                auto real_chunk = realized_col->chunk(0);
                if (!real_chunk->IsNull(i)) {
                    if (real_chunk->type_id() == arrow::Type::DOUBLE) {
                        auto dbl_array = std::static_pointer_cast<arrow::DoubleArray>(real_chunk);
                        realized_pnl = dbl_array->Value(i);
                    } else if (real_chunk->type_id() == arrow::Type::FLOAT) {
                        auto flt_array = std::static_pointer_cast<arrow::FloatArray>(real_chunk);
                        realized_pnl = static_cast<double>(flt_array->Value(i));
                    }
                }

                auto unreal_chunk = unrealized_col->chunk(0);
                if (!unreal_chunk->IsNull(i)) {
                    if (unreal_chunk->type_id() == arrow::Type::DOUBLE) {
                        auto dbl_array = std::static_pointer_cast<arrow::DoubleArray>(unreal_chunk);
                        unrealized_pnl = dbl_array->Value(i);
                    } else if (unreal_chunk->type_id() == arrow::Type::FLOAT) {
                        auto flt_array = std::static_pointer_cast<arrow::FloatArray>(unreal_chunk);
                        unrealized_pnl = static_cast<double>(flt_array->Value(i));
                    }
                }

                double total_pnl = realized_pnl + unrealized_pnl;
                // Include ALL positions to ensure chart appears (no filtering by PnL amount)
                if (!symbol.empty()) {
                    symbol_pnl_data.push_back({symbol, total_pnl});
                }
            } catch (const std::exception& e) {
                WARN("Error processing row " + std::to_string(i) + ": " + std::string(e.what()));
                continue;
            }
        }

        if (symbol_pnl_data.empty()) {
            WARN("No valid PnL data extracted");
            return "";
        }

        // Sort by PnL (descending)
        std::sort(symbol_pnl_data.begin(), symbol_pnl_data.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });

        // Create temporary data file
        std::string data_filename = "temp_pnl_symbol_data.txt";
        std::ofstream data_file(data_filename);
        if (!data_file.is_open()) {
            ERROR("Failed to create temporary data file");
            return "";
        }

        // Write data: symbol pnl
        for (const auto& [symbol, pnl] : symbol_pnl_data) {
            data_file << symbol << " " << std::fixed << std::setprecision(2) << pnl << "\n";
        }
        data_file.close();

        // Create gnuplot script
        std::string script_filename = "temp_pnl_symbol_script.gnu";
        std::ofstream script_file(script_filename);
        if (!script_file.is_open()) {
            ERROR("Failed to create gnuplot script file");
            return "";
        }

        std::string chart_filename = "pnl_by_symbol_chart.png";
        int chart_height = std::max(400, static_cast<int>(symbol_pnl_data.size() * 30));

        script_file << "# Gnuplot script for PnL by symbol\n";
        script_file << "reset\n";
        script_file << "set terminal pngcairo size 800," << chart_height << " enhanced font 'Arial,11'\n";
        script_file << "set output '" << chart_filename << "'\n\n";

        script_file << "# Styling\n";
        script_file << "set border lw 1.5\n";
        script_file << "set grid xtics lc rgb '#e0e0e0' lt 1 lw 0.5\n";
        script_file << "unset key\n\n";

        script_file << "# Axes\n";
        script_file << "set xlabel 'PnL ($)' font 'Arial,11'\n";
        script_file << "set ylabel 'Symbol' font 'Arial,11'\n";
        script_file << "set format x '$%.0f'\n\n";

        script_file << "# Horizontal bar chart\n";
        script_file << "set style data histogram\n";
        script_file << "set style histogram cluster gap 0\n";
        script_file << "set style fill solid border -1\n";
        script_file << "set boxwidth 0.8\n";
        script_file << "set yrange [-0.5:" << (symbol_pnl_data.size() - 0.5) << "]\n";
        script_file << "set ytics (";
        for (size_t i = 0; i < symbol_pnl_data.size(); ++i) {
            if (i > 0) script_file << ", ";
            script_file << "'" << symbol_pnl_data[i].first << "' " << i;
        }
        script_file << ")\n\n";

        script_file << "# Color by positive/negative\n";
        script_file << "plot '" << data_filename << "' using ($2 >= 0 ? $2 : 0):0:(0.8):($2 >= 0 ? 0x1a7f37 : 0xffffff) with boxxy lc rgb variable notitle, \\\n";
        script_file << "     '" << data_filename << "' using ($2 < 0 ? $2 : 0):0:(0.8):($2 < 0 ? 0xb42318 : 0xffffff) with boxxy lc rgb variable notitle\n";

        script_file.close();

        // Execute gnuplot
        std::ostringstream cmd;
        cmd << "gnuplot " << script_filename << " 2>&1";
        INFO("Executing: " + cmd.str());

        FILE* pipe = popen(cmd.str().c_str(), "r");
        if (!pipe) {
            ERROR("Failed to execute gnuplot command");
            return "";
        }

        char buffer[256];
        std::string gnuplot_output;
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            gnuplot_output += buffer;
        }

        int return_code = pclose(pipe);
        if (return_code != 0) {
            ERROR("Gnuplot failed with code " + std::to_string(return_code));
            if (!gnuplot_output.empty()) {
                ERROR("Gnuplot output: " + gnuplot_output);
            }
            return "";
        }

        INFO("Gnuplot executed successfully");

        // Read the generated PNG
        std::ifstream chart_file(chart_filename, std::ios::binary);
        if (!chart_file.is_open()) {
            ERROR("Failed to open generated chart file: " + chart_filename);
            return "";
        }

        std::vector<unsigned char> chart_data((std::istreambuf_iterator<char>(chart_file)),
                                               std::istreambuf_iterator<char>());
        chart_file.close();

        INFO("Chart file read, size: " + std::to_string(chart_data.size()) + " bytes");

        // Base64 encode
        std::string chart_base64 = ChartHelpers::encode_to_base64(chart_data);

        // Clean up temporary files
        std::remove(data_filename.c_str());
        std::remove(script_filename.c_str());
        std::remove(chart_filename.c_str());

        INFO("Successfully generated PnL by symbol chart");
        return chart_base64;

    } catch (const std::exception& e) {
        ERROR("Exception generating PnL by symbol chart: " + std::string(e.what()));
        return "";
    }
}

std::string ChartGenerator::generate_daily_pnl_chart(
    std::shared_ptr<DatabaseInterface> db,
    const std::string& strategy_id,
    int lookback_days)
{
    if (!db) {
        WARN("Database interface is null, cannot generate daily PnL chart");
        return "";
    }

    try {
        // Query equity curve data - get LAST N+1 days (matching equity curve approach)
        std::string query =
            "SELECT timestamp, equity "
            "FROM trading.equity_curve "
            "WHERE strategy_id = '" + strategy_id + "' "
            "ORDER BY timestamp DESC "
            "LIMIT " + std::to_string(lookback_days + 1);  // +1 to calculate first day's change

        INFO("Querying daily PnL data with: " + query);
        auto result = db->execute_query(query);

        if (result.is_error()) {
            ERROR("Failed to query daily PnL: " + std::string(result.error()->what()));
            return "";
        }

        auto table = result.value();
        if (!table || table->num_rows() < 2) {
            WARN("Insufficient data for daily PnL chart (need at least 2 days)");
            return "";
        }

        INFO("Retrieved " + std::to_string(table->num_rows()) + " rows for daily PnL");

        // Extract data
        auto combined_result = table->CombineChunks();
        if (!combined_result.ok()) {
            ERROR("Failed to combine chunks");
            return "";
        }
        auto combined = combined_result.ValueOrDie();

        std::vector<std::string> dates;
        std::vector<double> equity_values;

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

        if (dates.size() < 2) {
            ERROR("Insufficient valid data for daily PnL chart");
            return "";
        }

        // Reverse to get chronological order (DESC query returns newest first)
        std::reverse(dates.begin(), dates.end());
        std::reverse(equity_values.begin(), equity_values.end());

        // Calculate daily PnL
        std::vector<std::pair<std::string, double>> daily_pnl_data;
        for (size_t i = 1; i < equity_values.size(); ++i) {
            double daily_pnl = equity_values[i] - equity_values[i-1];
            daily_pnl_data.push_back({dates[i], daily_pnl});
        }

        // Create temporary data file
        std::string data_filename = "temp_daily_pnl_data.txt";
        std::ofstream data_file(data_filename);
        if (!data_file.is_open()) {
            ERROR("Failed to create temporary data file");
            return "";
        }

        // Write data: date pnl
        for (const auto& [date, pnl] : daily_pnl_data) {
            data_file << date << " " << std::fixed << std::setprecision(2) << pnl << "\n";
        }
        data_file.close();

        // Create gnuplot script
        std::string script_filename = "temp_daily_pnl_script.gnu";
        std::ofstream script_file(script_filename);
        if (!script_file.is_open()) {
            ERROR("Failed to create gnuplot script file");
            return "";
        }

        std::string chart_filename = "daily_pnl_chart.png";

        script_file << "# Gnuplot script for daily PnL\n";
        script_file << "reset\n";
        script_file << "set terminal pngcairo size 1000,500 enhanced font 'Arial,11'\n";
        script_file << "set output '" << chart_filename << "'\n";
        script_file << "set bmargin 5\n\n";  // Increase bottom margin for rotated labels

        script_file << "# Styling\n";
        script_file << "set border lw 1.5\n";
        script_file << "set grid ytics lc rgb '#e0e0e0' lt 1 lw 0.5\n";
        script_file << "unset key\n\n";

        script_file << "# Axes\n";
        script_file << "set xlabel 'Date' font 'Arial,11'\n";
        script_file << "set ylabel 'Daily PnL ($)' font 'Arial,11'\n";
        script_file << "set xdata time\n";
        script_file << "set timefmt '%Y-%m-%d'\n";
        script_file << "set format x '%m/%d'\n";
        script_file << "set format y '$%.0f'\n\n";

        // Set x-range
        script_file << "set xrange ['" << daily_pnl_data.front().first << "':'" << daily_pnl_data.back().first << "']\n";

        // Explicitly control which dates to show (prevent overlapping labels)
        int num_ticks = std::min(8, static_cast<int>(daily_pnl_data.size()));  // Show at most 8 dates for 30 days
        if (num_ticks > 0) {
            script_file << "set xtics (";
            for (int i = 0; i < num_ticks; i++) {
                int idx = (i * (daily_pnl_data.size() - 1)) / std::max(1, num_ticks - 1);
                if (i > 0) script_file << ", ";
                script_file << "'" << daily_pnl_data[idx].first << "' '" << daily_pnl_data[idx].first << "'";
            }
            script_file << ") rotate by -45 font 'Arial,10'\n\n";
        } else {
            script_file << "set xtics rotate by -45 font 'Arial,10'\n\n";
        }

        // Reference line at zero
        script_file << "set arrow from graph 0, first 0 to graph 1, first 0 nohead lc rgb '#666666' lt 2 lw 1\n\n";

        script_file << "# Bar chart with conditional coloring\n";
        script_file << "set style fill solid border -1\n";
        script_file << "set boxwidth 0.8 relative\n";
        script_file << "plot '" << data_filename << "' using 1:($2 >= 0 ? $2 : 0) with boxes lc rgb '#1a7f37' notitle, \\\n";
        script_file << "     '" << data_filename << "' using 1:($2 < 0 ? $2 : 0) with boxes lc rgb '#b42318' notitle\n";

        script_file.close();

        // Execute gnuplot
        std::ostringstream cmd;
        cmd << "gnuplot " << script_filename << " 2>&1";
        INFO("Executing: " + cmd.str());

        FILE* pipe = popen(cmd.str().c_str(), "r");
        if (!pipe) {
            ERROR("Failed to execute gnuplot command");
            return "";
        }

        char buffer[256];
        std::string gnuplot_output;
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            gnuplot_output += buffer;
        }

        int return_code = pclose(pipe);
        if (return_code != 0) {
            ERROR("Gnuplot failed with code " + std::to_string(return_code));
            if (!gnuplot_output.empty()) {
                ERROR("Gnuplot output: " + gnuplot_output);
            }
            return "";
        }

        INFO("Gnuplot executed successfully");

        // Read the generated PNG
        std::ifstream chart_file(chart_filename, std::ios::binary);
        if (!chart_file.is_open()) {
            ERROR("Failed to open generated chart file: " + chart_filename);
            return "";
        }

        std::vector<unsigned char> chart_data((std::istreambuf_iterator<char>(chart_file)),
                                               std::istreambuf_iterator<char>());
        chart_file.close();

        INFO("Chart file read, size: " + std::to_string(chart_data.size()) + " bytes");

        // Base64 encode
        std::string chart_base64 = ChartHelpers::encode_to_base64(chart_data);

        // Clean up temporary files
        std::remove(data_filename.c_str());
        std::remove(script_filename.c_str());
        std::remove(chart_filename.c_str());

        INFO("Successfully generated daily PnL chart");
        return chart_base64;

    } catch (const std::exception& e) {
        ERROR("Exception generating daily PnL chart: " + std::string(e.what()));
        return "";
    }
}

} // namespace trade_ngin