#include "trade_ngin/live/csv_exporter.hpp"
#include <arrow/api.h>
#include <arrow/type.h>
#include <cmath>
#include <iomanip>
#include <set>
#include <sstream>
#include <unordered_set>
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/data/conversion_utils.hpp"
#include "trade_ngin/data/database_interface.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/strategy/base_strategy.hpp"
#include "trade_ngin/strategy/trend_following.hpp"
#include "trade_ngin/strategy/trend_readings.hpp"

namespace trade_ngin {

CSVExporter::CSVExporter(const std::string& output_directory)
    : output_directory_(output_directory) {
    if (!output_directory_.empty() && output_directory_.back() != '/') {
        output_directory_ += '/';
    }
}

void CSVExporter::set_output_directory(const std::string& directory) {
    output_directory_ = directory;
    if (!output_directory_.empty() && output_directory_.back() != '/') {
        output_directory_ += '/';
    }
}

std::string CSVExporter::format_date_for_filename(
    const std::chrono::system_clock::time_point& date) const {
    auto time_t = std::chrono::system_clock::to_time_t(date);
    std::ostringstream ss;
    ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%d");
    return ss.str();
}

std::string CSVExporter::format_date_for_display(
    const std::chrono::system_clock::time_point& date) const {
    auto time_t = std::chrono::system_clock::to_time_t(date);
    std::ostringstream ss;
    ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%d");
    return ss.str();
}

std::string CSVExporter::get_clean_symbol(const std::string& symbol) const {
    std::string lookup_sym = symbol;
    auto dotpos = lookup_sym.find(".v.");
    if (dotpos != std::string::npos) {
        lookup_sym = lookup_sym.substr(0, dotpos);
    }
    dotpos = lookup_sym.find(".c.");
    if (dotpos != std::string::npos) {
        lookup_sym = lookup_sym.substr(0, dotpos);
    }
    return lookup_sym;
}

double CSVExporter::calculate_notional(const std::string& symbol, double quantity,
                                       double price) const {
    std::string clean_symbol = get_clean_symbol(symbol);

    double contract_multiplier = 0.0;
    try {
        auto& registry = InstrumentRegistry::instance();
        auto instrument_ptr = registry.get_instrument(clean_symbol);
        if (!instrument_ptr) {
            ERROR("CSVExporter: Instrument " + clean_symbol + " not found in registry!");
            throw std::runtime_error("Missing instrument in registry: " + clean_symbol);
        }

        contract_multiplier = instrument_ptr->get_multiplier();
        if (contract_multiplier <= 0) {
            ERROR("CSVExporter: Invalid multiplier " + std::to_string(contract_multiplier) +
                  " for " + clean_symbol);
            throw std::runtime_error("Invalid multiplier for: " + clean_symbol);
        }
    } catch (const std::exception& e) {
        ERROR("CSVExporter: Failed to get multiplier for " + symbol + ": " + e.what());
        throw;
    }

    return quantity * price * contract_multiplier;
}

std::string CSVExporter::pct_of_portfolio_value_note() {
    return "pct_of_portfolio_value: the row's notional as a percent of the portfolio value on "
           "the first line (as written: today's costs booked).";
}

std::string CSVExporter::sizing_vol_note() {
    return "sizing_vol: the volatility estimate the sleeve sized the position on (70 percent a "
           "short-run estimate, 30 percent its long-run average), annualised, a fraction, as of "
           "the bar the forecast was computed on; not the account's realised volatility "
           "statistic.";
}

void CSVExporter::write_portfolio_header(std::ofstream& file, double portfolio_value,
                                         double gross_notional, double net_notional,
                                         const std::string& date) const {
    // The value is the day's row as this run wrote it: yesterday's settled value less today's
    // costs. Today's P&L settles in tomorrow's run.
    file << "# Portfolio Value (as written: today's costs booked): " << std::fixed
         << std::setprecision(2) << portfolio_value << ", Gross Notional: " << gross_notional
         << ", Net Notional: " << net_notional << ", Date: " << date << "\n";
}

Result<std::string> CSVExporter::export_finalized_positions(
    const std::chrono::system_clock::time_point& date,
    const std::chrono::system_clock::time_point& yesterday_date,
    const StrategyPositionsMap& strategy_positions,
    const std::unordered_map<std::string, double>& prior_marks,
    const std::unordered_map<std::string, double>& marks) {
    try {
        INFO("CSVExporter: Exporting per-strategy finalized positions...");

        // Generate filename: YYYY-MM-DD_positions_asof_YYYY-MM-DD.csv
        std::string yesterday_str = format_date_for_filename(yesterday_date);
        std::string today_str = format_date_for_filename(date);
        std::string filename =
            output_directory_ + yesterday_str + "_positions_asof_" + today_str + ".csv";

        std::ofstream file(filename);
        if (!file.is_open()) {
            return Result<std::string>(std::make_unique<TradeError>(
                ErrorCode::FILE_IO_ERROR, "Failed to open file for writing: " + filename));
        }

        // Write CSV header with strategy column
        // prior_mark is the close two days before the run date, mark the close of the day
        // before it (the day this file settles); the report's settled table names them "Prior
        // mark (T-2 close)" and "Mark (T-1 close)". No comment line: this file starts with
        // its column names.
        file << "strategy,symbol,quantity,prior_mark,mark,realized_pnl\n";
        // One number format for every row of the file, set once, after the header.
        file << std::fixed << std::setprecision(6);

        // Sort strategies alphabetically for consistent ordering
        std::vector<std::string> strategy_names;
        for (const auto& [name, _] : strategy_positions) {
            strategy_names.push_back(name);
        }
        std::sort(strategy_names.begin(), strategy_names.end());

        // Write position data grouped by strategy
        for (const auto& strategy_name : strategy_names) {
            const auto& positions = strategy_positions.at(strategy_name);
            std::string display_name = format_strategy_display_name(strategy_name);

            for (const auto& [symbol, position] : positions) {
                double quantity = position.quantity.as_double();

                // Skip zero positions
                if (std::abs(quantity) < 0.0001) {
                    continue;
                }

                // Realized PnL from position
                double realized_pnl = position.realized_pnl.as_double();

                // The two marks: a symbol with no close on that day has no mark, and its cell
                // is left empty (a 0 would read as a price).
                file << display_name << "," << symbol << "," << quantity << ",";
                auto prior_it = prior_marks.find(symbol);
                if (prior_it != prior_marks.end())
                    file << prior_it->second;
                file << ",";
                auto mark_it = marks.find(symbol);
                if (mark_it != marks.end())
                    file << mark_it->second;
                file << "," << realized_pnl << "\n";
            }
        }

        file.close();
        INFO("CSVExporter: Per-strategy finalized positions saved to " + filename);
        return Result<std::string>(filename);

    } catch (const std::exception& e) {
        ERROR("CSVExporter: Exception in export_finalized_positions (per-strategy): " + std::string(e.what()));
        return Result<std::string>(std::make_unique<TradeError>(
            ErrorCode::FILE_IO_ERROR,
            "Failed to export per-strategy finalized positions: " + std::string(e.what())));
    }
}

std::string CSVExporter::format_strategy_display_name(const std::string& strategy_id) const {
    std::string result;
    bool capitalize_next = true;

    for (size_t i = 0; i < strategy_id.size(); ++i) {
        char c = strategy_id[i];
        if (c == '_') {
            result += ' ';
            capitalize_next = true;
        } else if (capitalize_next) {
            result += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            capitalize_next = false;
        } else {
            result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }

    return result;
}

Result<std::string> CSVExporter::export_current_positions(
    const std::chrono::system_clock::time_point& date,
    const StrategyPositionsMap& strategy_positions,
    const std::unordered_map<std::string, double>& market_prices,
    double portfolio_value,
    double gross_notional,
    double net_notional,
    const StrategyInstancesMap& strategy_instances) {
    try {
        INFO("CSVExporter: Exporting per-strategy positions...");

        // Generate filename: YYYY-MM-DD_positions.csv
        std::string date_str = format_date_for_filename(date);
        std::string filename = output_directory_ + date_str + "_positions.csv";

        std::ofstream file(filename);
        if (!file.is_open()) {
            return Result<std::string>(std::make_unique<TradeError>(
                ErrorCode::FILE_IO_ERROR, "Failed to open file for writing: " + filename));
        }

        // Write portfolio header
        write_portfolio_header(file, portfolio_value, gross_notional, net_notional,
                               format_date_for_display(date));

        // Write CSV header with strategy column
        // What sizing_vol is, on a comment line of its own between the portfolio line and the
        // column names (the file already opens with a comment line).
        file << "# " << sizing_vol_note() << "\n";
        file << "# " << pct_of_portfolio_value_note() << "\n";
        file << "strategy,symbol,quantity,market_price,notional,pct_of_gross_notional,pct_of_"
                "portfolio_value,"
             << "forecast,sizing_vol,ema_8,ema_32,ema_64,ema_256\n";
        // One number format for every row of the file, set once, after the header (set inside
        // the row it left the first row at the portfolio line's two decimals).
        file << std::fixed << std::setprecision(6);

        // Sort strategies alphabetically for consistent ordering
        std::vector<std::string> strategy_names;
        for (const auto& [name, _] : strategy_positions) {
            strategy_names.push_back(name);
        }
        std::sort(strategy_names.begin(), strategy_names.end());

        // Write position data grouped by strategy
        for (const auto& strategy_name : strategy_names) {
            const auto& positions = strategy_positions.at(strategy_name);
            std::string display_name = format_strategy_display_name(strategy_name);

            // Get strategy instance for this strategy (if available)
            ITrendFollowingStrategy* strategy = nullptr;
            auto strategy_it = strategy_instances.find(strategy_name);
            if (strategy_it != strategy_instances.end()) {
                strategy = strategy_it->second;
            }

            // Get all tradeable symbols from strategy to include even zero positions
            std::unordered_set<std::string> all_symbols;
            if (strategy != nullptr) {
                auto* tf_strategy = dynamic_cast<TrendFollowingStrategy*>(strategy);
                if (tf_strategy != nullptr) {
                    const auto& instrument_data = tf_strategy->get_all_instrument_data();
                    for (const auto& [symbol, _] : instrument_data) {
                        all_symbols.insert(symbol);
                    }
                }
            }
            // Also include symbols from actual positions
            for (const auto& [symbol, _] : positions) {
                all_symbols.insert(symbol);
            }

            for (const auto& symbol : all_symbols) {
                // Get position quantity (0 if not in positions map)
                double quantity = 0.0;
                auto pos_it = positions.find(symbol);
                if (pos_it != positions.end()) {
                    quantity = pos_it->second.quantity.as_double();
                }

                // Get market price (Day T-1 close)
                double market_price = 0.0;
                if (pos_it != positions.end()) {
                    market_price = pos_it->second.average_price.as_double();  // Default fallback
                }
                auto price_it = market_prices.find(symbol);
                if (price_it != market_prices.end()) {
                    market_price = price_it->second;
                }

                // Calculate notional
                double notional = calculate_notional(symbol, quantity, market_price);

                // Calculate percentages
                double pct_of_gross =
                    (gross_notional != 0.0) ? (std::abs(notional) / gross_notional) * 100.0 : 0.0;
                double pct_of_portfolio = (portfolio_value != 0.0)
                                              ? (std::abs(notional) / std::abs(portfolio_value)) * 100.0
                                              : 0.0;

                // The sleeve's forecast, sizing volatility and moving averages (0 when the
                // sleeve is not a trend sleeve or holds no data on the symbol).
                const TrendReadings readings = trend_readings_of(strategy, symbol);

                // Write position row with strategy column
                file << display_name << "," << symbol << "," << quantity << "," << market_price
                     << "," << notional << "," << pct_of_gross << "," << pct_of_portfolio << ","
                     << readings.forecast << "," << readings.sizing_vol << "," << readings.ema_8
                     << "," << readings.ema_32 << "," << readings.ema_64 << "," << readings.ema_256
                     << "\n";
            }
        }

        file.close();
        INFO("CSVExporter: Per-strategy positions saved to " + filename);
        return Result<std::string>(filename);

    } catch (const std::exception& e) {
        ERROR("CSVExporter: Exception in export_current_positions (per-strategy): " + std::string(e.what()));
        return Result<std::string>(std::make_unique<TradeError>(
            ErrorCode::FILE_IO_ERROR,
            "Failed to export per-strategy positions: " + std::string(e.what())));
    }
}

Result<std::string> CSVExporter::export_carried_positions(
    const std::chrono::system_clock::time_point& date,
    const StrategyPositionsMap& strategy_positions,
    const std::unordered_map<std::string, CarriedMark>& last_marks,
    const std::unordered_map<std::string, CarriedForecasts>& last_forecasts,
    double portfolio_value, double gross_notional, double net_notional,
    const std::string& no_session_reason) {
    try {
        const std::string date_str = format_date_for_filename(date);
        const std::string filename = output_directory_ + date_str + "_positions.csv";
        INFO("CSVExporter: no session on " + date_str + " (" + no_session_reason +
             "): the positions file carries the held book, the last marks and the last computed "
             "forecasts; nothing was computed");

        std::ofstream file(filename);
        if (!file.is_open()) {
            return Result<std::string>(std::make_unique<TradeError>(
                ErrorCode::FILE_IO_ERROR, "Failed to open file for writing: " + filename));
        }

        write_portfolio_header(file, portfolio_value, gross_notional, net_notional,
                               format_date_for_display(date));
        // The stored forecasts carry the date of the run that wrote them; that run computed
        // them from the bars through the day before it. One label per distinct run date.
        std::set<std::string> forecast_labels;
        for (const auto& [name, carried] : last_forecasts) {
            const std::string label = carried_forecast_label(carried.session_date);
            if (!label.empty())
                forecast_labels.insert(label);
        }
        std::string forecast_source;
        for (const auto& label : forecast_labels) {
            forecast_source += (forecast_source.empty() ? "" : "; ") + label;
        }
        if (forecast_source.empty())
            forecast_source = "none is stored";
        file << "# NO SESSION: " << no_session_reason
             << ". Nothing below was computed today; every value is CARRIED, not computed: "
                "quantity is the held book (the previous day's stored positions), market_price "
                "is the symbol's last close (mark_date), forecast is the sleeve's last stored "
                "forecast ("
             << forecast_source
             << "; forecast_run_date is the date of that run). sizing_vol and the EMAs are not "
                "stored and are left empty.\n";
        file << "strategy,symbol,quantity,market_price,notional,pct_of_gross_notional,"
                "pct_of_portfolio_value,forecast,sizing_vol,ema_8,ema_32,ema_64,ema_256,"
                "mark_date,forecast_run_date\n";
        file << std::fixed << std::setprecision(6);

        // Every sleeve that holds something or has stored forecasts, alphabetically.
        std::set<std::string> strategy_names;
        for (const auto& [name, _] : strategy_positions) strategy_names.insert(name);
        for (const auto& [name, _] : last_forecasts) strategy_names.insert(name);

        size_t rows = 0;
        for (const auto& strategy_name : strategy_names) {
            const std::string display_name = format_strategy_display_name(strategy_name);
            static const std::unordered_map<std::string, Position> kNoPositions;
            static const CarriedForecasts kNoForecasts;
            auto held_it = strategy_positions.find(strategy_name);
            const auto& held = held_it == strategy_positions.end() ? kNoPositions : held_it->second;
            auto fc_it = last_forecasts.find(strategy_name);
            const auto& carried = fc_it == last_forecasts.end() ? kNoForecasts : fc_it->second;

            // The held symbols and every symbol the sleeve last forecast, alphabetically.
            std::set<std::string> symbols;
            for (const auto& [symbol, _] : held) symbols.insert(symbol);
            for (const auto& [symbol, _] : carried.forecasts) symbols.insert(symbol);

            for (const auto& symbol : symbols) {
                double quantity = 0.0;
                auto pos_it = held.find(symbol);
                if (pos_it != held.end()) quantity = pos_it->second.quantity.as_double();

                // The last mark; a held symbol with no loaded bar keeps its stored row's price.
                double market_price = 0.0;
                std::string mark_date;
                auto mark_it = last_marks.find(symbol);
                if (mark_it != last_marks.end()) {
                    market_price = mark_it->second.close;
                    mark_date = mark_it->second.date;
                } else if (pos_it != held.end()) {
                    market_price = pos_it->second.average_price.as_double();
                    mark_date = "stored row";
                }

                const double notional = calculate_notional(symbol, quantity, market_price);
                const double pct_of_gross =
                    (gross_notional != 0.0) ? (std::abs(notional) / gross_notional) * 100.0 : 0.0;
                const double pct_of_portfolio =
                    (portfolio_value != 0.0)
                        ? (std::abs(notional) / std::abs(portfolio_value)) * 100.0
                        : 0.0;

                file << display_name << "," << symbol << "," << quantity << "," << market_price
                     << "," << notional << "," << pct_of_gross << "," << pct_of_portfolio << ",";
                auto f_it = carried.forecasts.find(symbol);
                if (f_it != carried.forecasts.end()) file << f_it->second;
                // sizing_vol, ema_8, ema_32, ema_64, ema_256: not stored, left empty
                file << ",,,,,," << mark_date << ","
                     << (f_it != carried.forecasts.end() ? carried.session_date : std::string())
                     << "\n";
                ++rows;
            }
        }

        file.close();
        INFO("CSVExporter: Carried positions saved to " + filename + " (" + std::to_string(rows) +
             " rows)");
        return Result<std::string>(filename);

    } catch (const std::exception& e) {
        ERROR("CSVExporter: Exception in export_carried_positions: " + std::string(e.what()));
        return Result<std::string>(std::make_unique<TradeError>(
            ErrorCode::FILE_IO_ERROR,
            "Failed to export carried positions: " + std::string(e.what())));
    }
}

}  // namespace trade_ngin