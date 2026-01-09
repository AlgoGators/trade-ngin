#include "trade_ngin/core/chart_generator.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
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
            "ORDER BY timestamp DESC "
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

        // Yesterday's realized PnL by symbol
        std::string query =
            "SELECT symbol, daily_realized_pnl "
            "FROM trading.positions "
            "WHERE strategy_id = '" + strategy_id + "' "
            "AND DATE(last_update) = DATE('" + date + "') - INTERVAL '1 day' "
            "ORDER BY last_update DESC";

        INFO("Querying realized PnL by symbol with: " + query);
        auto result = db->execute_query(query);

        if (result.is_error()) {
            ERROR("Failed to query realized PnL by symbol: " + std::string(result.error()->what()));
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

        // Sort by realized PnL (descending)
        std::sort(symbol_pnl_data.begin(), symbol_pnl_data.end(),
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
            chart_data.title = "Yesterday's PnL by Symbol (" + yesterday_date + ")";
        }
        chart_data.x_label = "Symbol";
        chart_data.y_label = "Realized PnL ($)";
        chart_data.reference_line = 0.0;
        chart_data.has_reference_line = true;

        INFO("Chart data prepared with " + std::to_string(chart_data.labels.size()) +
             " labels and " + std::to_string(chart_data.values.size()) + " values");

        return chart_data;

    } catch (const std::exception& e) {
        ERROR("Exception fetching realized PnL by symbol: " + std::string(e.what()));
        return chart_data;
    }
}


ChartData ChartGenerator::fetch_daily_pnl_data(
    std::shared_ptr<DatabaseInterface> db,
    const std::string& strategy_id,
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
        std::string query =
            "SELECT date, daily_pnl "
            "FROM trading.live_results "
            "WHERE strategy_id = '" + strategy_id + "' "
            "AND DATE(date) < DATE('" + date + "') "
            "ORDER BY date DESC "
            "LIMIT " + std::to_string(lookback_days);

        INFO("Querying daily PnL data with: " + query);
        auto result = db->execute_query(query);

        if (result.is_error()) {
            ERROR("Failed to query daily PnL: " + std::string(result.error()->what()));
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
            return chart_data;
        }
        auto combined = combined_result.ValueOrDie();

        auto date_col = combined->column(0);
        auto pnl_col = combined->column(1);

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
                    chart_data.labels.push_back(date_str);
                    chart_data.values.push_back(daily_pnl);
                }
            } catch (const std::exception& e) {
                WARN("Error processing row " + std::to_string(i) + ": " + std::string(e.what()));
                continue;
            }
        }

        if (chart_data.labels.empty()) {
            ERROR("No valid daily PnL data");
            return chart_data;
        }

        // Reverse to get chronological order (DESC query returns newest first)
        std::reverse(chart_data.labels.begin(), chart_data.labels.end());
        std::reverse(chart_data.values.begin(), chart_data.values.end());

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

ChartData ChartGenerator::fetch_cumulative_commissions_data(
    std::shared_ptr<DatabaseInterface> db,
    const std::string& strategy_id,
    const std::string& date /* today, YYYY-MM-DD */)
{
    ChartData chart_data;
    if (!db) { WARN("DB null"); return chart_data; }

    try {
        // Query to get daily commissions, gross notional, and count of trades per day
        std::string query =
            "SELECT "
            "    lr.date, "
            "    COALESCE(lr.daily_commissions, 0) as daily_cost, "
            "    COALESCE(lr.gross_notional, 0) as total_notional, "
            "    COALESCE(trade_counts.num_trades, 0) as num_trades "
            "FROM trading.live_results lr "
            "LEFT JOIN ( "
            "    SELECT DATE(execution_time) as trade_date, COUNT(*) as num_trades "
            "    FROM trading.executions "
            "    GROUP BY DATE(execution_time) "
            ") trade_counts ON DATE(lr.date) = trade_counts.trade_date "
            "WHERE lr.strategy_id = '" + strategy_id + "' "
            "AND DATE(lr.date) <= DATE('" + date + "') "
            "ORDER BY lr.date ASC";

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

        // Expect: col0 = date, col1 = daily_cost, col2 = total_notional, col3 = num_trades
        auto day_col = combined->column(0);
        auto cost_col = combined->column(1);
        auto notional_col = combined->column(2);
        auto trades_col = combined->column(3);

        for (int64_t i = 0; i < combined->num_rows(); ++i) {
            // ---- parse date -> "YYYY-MM-DD"
            std::string day_str;
            auto day_chunk = day_col->chunk(0);
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

            // ---- parse daily_cost -> double
            double daily_cost = 0.0;
            auto cost_chunk = cost_col->chunk(0);
            if (!cost_chunk->IsNull(i)) {
                switch (cost_chunk->type_id()) {
                    case arrow::Type::DOUBLE: {
                        auto a = std::static_pointer_cast<arrow::DoubleArray>(cost_chunk);
                        daily_cost = a->Value(i);
                    } break;
                    case arrow::Type::FLOAT: {
                        auto a = std::static_pointer_cast<arrow::FloatArray>(cost_chunk);
                        daily_cost = static_cast<double>(a->Value(i));
                    } break;
                    case arrow::Type::INT64: {
                        auto a = std::static_pointer_cast<arrow::Int64Array>(cost_chunk);
                        daily_cost = static_cast<double>(a->Value(i));
                    } break;
                    case arrow::Type::INT32: {
                        auto a = std::static_pointer_cast<arrow::Int32Array>(cost_chunk);
                        daily_cost = static_cast<double>(a->Value(i));
                    } break;
                    case arrow::Type::STRING: {
                        auto a = std::static_pointer_cast<arrow::StringArray>(cost_chunk);
                        if (!a->IsNull(i)) {
                            try { daily_cost = std::stod(a->GetString(i)); } catch (...) {}
                        }
                    } break;
                    case arrow::Type::LARGE_STRING: {
                        auto a = std::static_pointer_cast<arrow::LargeStringArray>(cost_chunk);
                        if (!a->IsNull(i)) {
                            try { daily_cost = std::stod(a->GetString(i)); } catch (...) {}
                        }
                    } break;
                    default: break;
                }
            }

            // ---- parse total_notional -> double
            double total_notional = 0.0;
            auto notional_chunk = notional_col->chunk(0);
            if (!notional_chunk->IsNull(i)) {
                switch (notional_chunk->type_id()) {
                    case arrow::Type::DOUBLE: {
                        auto a = std::static_pointer_cast<arrow::DoubleArray>(notional_chunk);
                        total_notional = a->Value(i);
                    } break;
                    case arrow::Type::FLOAT: {
                        auto a = std::static_pointer_cast<arrow::FloatArray>(notional_chunk);
                        total_notional = static_cast<double>(a->Value(i));
                    } break;
                    case arrow::Type::INT64: {
                        auto a = std::static_pointer_cast<arrow::Int64Array>(notional_chunk);
                        total_notional = static_cast<double>(a->Value(i));
                    } break;
                    case arrow::Type::INT32: {
                        auto a = std::static_pointer_cast<arrow::Int32Array>(notional_chunk);
                        total_notional = static_cast<double>(a->Value(i));
                    } break;
                    case arrow::Type::STRING: {
                        auto a = std::static_pointer_cast<arrow::StringArray>(notional_chunk);
                        if (!a->IsNull(i)) {
                            try { total_notional = std::stod(a->GetString(i)); } catch (...) {}
                        }
                    } break;
                    case arrow::Type::LARGE_STRING: {
                        auto a = std::static_pointer_cast<arrow::LargeStringArray>(notional_chunk);
                        if (!a->IsNull(i)) {
                            try { total_notional = std::stod(a->GetString(i)); } catch (...) {}
                        }
                    } break;
                    default: break;
                }
            }

            // ---- parse num_trades -> int
            int64_t num_trades = 0;
            auto trades_chunk = trades_col->chunk(0);
            if (!trades_chunk->IsNull(i)) {
                switch (trades_chunk->type_id()) {
                    case arrow::Type::INT64: {
                        auto a = std::static_pointer_cast<arrow::Int64Array>(trades_chunk);
                        num_trades = a->Value(i);
                    } break;
                    case arrow::Type::INT32: {
                        auto a = std::static_pointer_cast<arrow::Int32Array>(trades_chunk);
                        num_trades = static_cast<int64_t>(a->Value(i));
                    } break;
                    default: break;
                }
            }

            // Calculate cost per $1M traded
            // Formula: (daily_cost / total_notional) * 1,000,000
            // Skip if notional is zero to avoid division by zero
            if (total_notional > 0.0) {
                double cost_per_million = (daily_cost / total_notional) * 1000000.0;
                chart_data.labels.push_back(day_str);
                chart_data.values.push_back(cost_per_million);

                INFO("Date: " + day_str + ", Cost: $" + std::to_string(daily_cost) +
                     ", Notional: $" + std::to_string(total_notional) +
                     ", Trades: " + std::to_string(num_trades) +
                     ", Cost per $1M: $" + std::to_string(cost_per_million));
            }
        }

        if (chart_data.labels.empty()) {
            WARN("No valid cost per $1M traded data points");
            return chart_data;
        }

        if (chart_data.labels.size() == 1) {
            // guard against single-point x-range (gnuplot needs width)
            std::tm tm{}; std::istringstream ss(chart_data.labels[0]);
            ss >> std::get_time(&tm, "%Y-%m-%d");
            if (!ss.fail()) {
                auto t = std::chrono::system_clock::from_time_t(std::mktime(&tm));
                auto t_prev = t - std::chrono::hours(24);
                std::time_t tt = std::chrono::system_clock::to_time_t(t_prev);
                std::tm* g = std::gmtime(&tt);
                std::ostringstream os; os << std::put_time(g, "%Y-%m-%d");
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

    } catch (const std::exception& e) {
        ERROR(std::string("Exception fetching cost per $1M traded: ") + e.what());
        return chart_data;
    }
}

ChartData ChartGenerator::fetch_margin_posted_data(
    std::shared_ptr<DatabaseInterface> db,
    const std::string& strategy_id,
    const std::string& date /* today, YYYY-MM-DD */)
{
    ChartData chart_data;
    if (!db) { WARN("DB null"); return chart_data; }

    try {
        // All days up to and including today (T)
        std::string query =
            "SELECT date, margin_posted "
            "FROM trading.live_results "
            "WHERE strategy_id = '" + strategy_id + "' "
            "AND DATE(date) <= DATE('" + date + "') "
            "ORDER BY date ASC";

        INFO("Querying margin posted with: " + query);
        auto result = db->execute_query(query);
        if (result.is_error()) {
            ERROR("Failed to query margin posted: " + std::string(result.error()->what()));
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
            return chart_data;
        }
        auto combined = combined_result.ValueOrDie();

        // Expect: col0 = date, col1 = margin_posted
        auto day_col = combined->column(0);
        auto val_col = combined->column(1);
        auto day_chunk = day_col->chunk(0);
        auto val_chunk = val_col->chunk(0);

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

            chart_data.labels.push_back(day_str);
            chart_data.values.push_back(val);
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

    } catch (const std::exception& e) {
        ERROR(std::string("Exception fetching margin posted: ") + e.what());
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
        return chart_data;
    }
}

ChartData ChartGenerator::fetch_cumulative_pnl_by_symbol_data(
    std::shared_ptr<DatabaseInterface> db,
    const std::string& strategy_id,
    const std::string& date)
{
    ChartData chart_data;

    if (!db) {
        WARN("Database interface is null, cannot fetch cumulative PnL data");
        return chart_data;
    }

    try {
        // Query to sum up all realized PnL by symbol across all time
        std::string query =
            "SELECT symbol, SUM(daily_realized_pnl) as cumulative_pnl "
            "FROM trading.positions "
            "WHERE strategy_id = '" + strategy_id + "' "
            "AND DATE(last_update) <= DATE('" + date + "') "
            "GROUP BY symbol "
            "HAVING SUM(daily_realized_pnl) IS NOT NULL "
            "AND SUM(daily_realized_pnl) != 0 "
            "ORDER BY cumulative_pnl DESC";

        INFO("Querying cumulative PnL by symbol with: " + query);
        auto result = db->execute_query(query);

        if (result.is_error()) {
            ERROR("Failed to query cumulative PnL by symbol: " + std::string(result.error()->what()));
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

        // Already sorted by query (DESC), but let's ensure it
        std::sort(symbol_pnl_data.begin(), symbol_pnl_data.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });

        // Populate ChartData
        INFO("Populating cumulative PnL chart data with " + std::to_string(symbol_pnl_data.size()) + " symbols");
        for (const auto& [sym, pnl] : symbol_pnl_data) {
            chart_data.labels.push_back(sym);
            chart_data.values.push_back(pnl);
        }

        chart_data.title = "Cumulative Realized PnL by Symbol (All-Time as of " + date + ")";
        chart_data.x_label = "Cumulative Realized PnL ($)";
        chart_data.y_label = "Symbol";
        chart_data.reference_line = 0.0;
        chart_data.has_reference_line = true;

        INFO("Chart data prepared with " + std::to_string(chart_data.labels.size()) +
             " labels and " + std::to_string(chart_data.values.size()) + " values");

        return chart_data;

    } catch (const std::exception& e) {
        ERROR("Exception fetching cumulative PnL by symbol: " + std::string(e.what()));
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

        // Cleanup (keep files for debugging if generation fails)
        if (!chart_data_bin.empty()) {
            std::remove(data_filename.c_str());
            std::remove(script_filename.c_str());
            std::remove(chart_filename.c_str());
        } else {
            ERROR("Chart generation failed - keeping temp files for debugging:");
            ERROR("  Data file: " + data_filename);
            ERROR("  Script file: " + script_filename);
            ERROR("  Output file: " + chart_filename);
        }

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
    // Fetch data using modular fetcher
    ChartData data = fetch_equity_curve_data(db, strategy_id, lookback_days);

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

std::string ChartGenerator::generate_pnl_by_symbol_chart(
    std::shared_ptr<DatabaseInterface> db,
    const std::string& strategy_id,
    const std::string& date)
{
    INFO("Starting PnL by symbol chart generation for strategy: " + strategy_id);

    // Fetch data using modular fetcher
    ChartData data = fetch_pnl_by_symbol_data(db, strategy_id, date);

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

std::string ChartGenerator::generate_daily_pnl_chart(
    std::shared_ptr<DatabaseInterface> db,
    const std::string& strategy_id,
    const std::string& date,
    int lookback_days)
{
    // Fetch data using modular fetcher
    ChartData data = fetch_daily_pnl_data(db, strategy_id, date, lookback_days);

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

std::string ChartGenerator::generate_total_commissions_chart(
    std::shared_ptr<DatabaseInterface> db,
    const std::string& strategy_id,
    const std::string& end_date)   // "YYYY-MM-DD" (today)
{
    ChartData data = fetch_cumulative_commissions_data(db, strategy_id, end_date);
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
    const std::string& date /* today */)
{
    ChartData data = fetch_margin_posted_data(db, strategy_id, date);
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
    const std::string& date)
{
    INFO("Starting cumulative PnL by symbol chart generation for strategy: " + strategy_id);

    // Fetch data
    ChartData data = fetch_cumulative_pnl_by_symbol_data(db, strategy_id, date);

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