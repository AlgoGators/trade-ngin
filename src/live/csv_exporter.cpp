#include "trade_ngin/live/csv_exporter.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/data/database_interface.hpp"
#include "trade_ngin/strategy/trend_following.hpp"
#include "trade_ngin/strategy/trend_following_slow.hpp"
#include "trade_ngin/strategy/base_strategy.hpp"
#include "trade_ngin/core/logger.hpp"
#include <iomanip>
#include <sstream>
#include <arrow/api.h>
#include <arrow/type.h>

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

std::string CSVExporter::format_date_for_filename(const std::chrono::system_clock::time_point& date) const {
    auto time_t = std::chrono::system_clock::to_time_t(date);
    std::tm* tm = std::gmtime(&time_t);

    std::string day_str = std::string(2 - std::to_string(tm->tm_mday).length(), '0') + std::to_string(tm->tm_mday);
    std::string month_str = std::string(2 - std::to_string(tm->tm_mon + 1).length(), '0') + std::to_string(tm->tm_mon + 1);
    std::string year_str = std::to_string(tm->tm_year + 1900);

    return day_str + "-" + month_str + "-" + year_str;
}

std::string CSVExporter::format_date_for_display(const std::chrono::system_clock::time_point& date) const {
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

double CSVExporter::calculate_notional(const std::string& symbol, double quantity, double price) const {
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
            ERROR("CSVExporter: Invalid multiplier " + std::to_string(contract_multiplier) + " for " + clean_symbol);
            throw std::runtime_error("Invalid multiplier for: " + clean_symbol);
        }
    } catch (const std::exception& e) {
        ERROR("CSVExporter: Failed to get multiplier for " + symbol + ": " + e.what());
        throw;
    }

    return quantity * price * contract_multiplier;
}

void CSVExporter::write_portfolio_header(std::ofstream& file,
                                        double portfolio_value,
                                        double gross_notional,
                                        double net_notional,
                                        const std::string& date) const {
    file << "# Portfolio Value: " << std::fixed << std::setprecision(2) << portfolio_value
         << ", Gross Notional: " << gross_notional
         << ", Net Notional: " << net_notional
         << ", Date: " << date << "\n";
}

Result<std::string> CSVExporter::export_current_positions(
    const std::chrono::system_clock::time_point& date,
    const std::unordered_map<std::string, Position>& positions,
    const std::unordered_map<std::string, double>& market_prices,
    double portfolio_value,
    double gross_notional,
    double net_notional,
    ITrendFollowingStrategy* strategy,
    const std::unordered_map<std::string, double>& symbol_commissions) {

    try {
        INFO("CSVExporter: Exporting current positions...");

        // Generate filename: DD-MM-YYYY_positions.csv
        std::string date_str = format_date_for_filename(date);
        std::string filename = output_directory_ + date_str + "_positions.csv";

        std::ofstream file(filename);
        if (!file.is_open()) {
            return Result<std::string>(std::make_unique<TradeError>(ErrorCode::FILE_IO_ERROR, "Failed to open file for writing: " + filename));
        }

        // Write portfolio header
        write_portfolio_header(file, portfolio_value, gross_notional, net_notional,
                              format_date_for_display(date));

        // Write CSV header
        file << "symbol,quantity,market_price,notional,pct_of_gross_notional,pct_of_portfolio_value,"
             << "forecast,volatility,ema_8,ema_32,ema_64,ema_256\n";

        // Write position data
        for (const auto& [symbol, position] : positions) {
            double quantity = position.quantity.as_double();

            // Get market price (Day T-1 close)
            double market_price = position.average_price.as_double(); // Default fallback
            auto price_it = market_prices.find(symbol);
            if (price_it != market_prices.end()) {
                market_price = price_it->second;
            }

            // Calculate notional
            double notional = calculate_notional(symbol, quantity, market_price);

            // Calculate percentages
            double pct_of_gross = (gross_notional != 0.0) ?
                (std::abs(notional) / gross_notional) * 100.0 : 0.0;
            double pct_of_portfolio = (portfolio_value != 0.0) ?
                (std::abs(notional) / std::abs(portfolio_value)) * 100.0 : 0.0;

            // Get forecast from strategy (support both TrendFollowingStrategy and TrendFollowingSlowStrategy)
            double forecast = 0.0;
            if (strategy != nullptr) {
                auto* tf_strategy = dynamic_cast<TrendFollowingStrategy*>(strategy);
                auto* tf_slow_strategy = dynamic_cast<TrendFollowingSlowStrategy*>(strategy);
                if (tf_strategy != nullptr) {
                    forecast = tf_strategy->get_forecast(symbol);
                } else if (tf_slow_strategy != nullptr) {
                    forecast = tf_slow_strategy->get_forecast(symbol);
                }
            }

            // Get volatility from strategy
            double volatility = 0.0;
            if (strategy != nullptr) {
                auto* tf_strategy = dynamic_cast<TrendFollowingStrategy*>(strategy);
                auto* tf_slow_strategy = dynamic_cast<TrendFollowingSlowStrategy*>(strategy);
                if (tf_strategy != nullptr) {
                    auto instrument_data = tf_strategy->get_instrument_data(symbol);
                    if (instrument_data != nullptr) {
                        volatility = instrument_data->current_volatility;
                    }
                } else if (tf_slow_strategy != nullptr) {
                    auto instrument_data = tf_slow_strategy->get_instrument_data(symbol);
                    if (instrument_data != nullptr) {
                        volatility = instrument_data->current_volatility;
                    }
                }
            }

            // Get EMA values
            double ema_8 = 0.0, ema_32 = 0.0, ema_64 = 0.0, ema_256 = 0.0;
            if (strategy != nullptr) {
                auto* tf_strategy = dynamic_cast<TrendFollowingStrategy*>(strategy);
                auto* tf_slow_strategy = dynamic_cast<TrendFollowingSlowStrategy*>(strategy);
                if (tf_strategy != nullptr) {
                    auto ema_values = tf_strategy->get_ema_values(symbol, {8, 32, 64, 256});
                    ema_8 = ema_values.count(8) ? ema_values[8] : 0.0;
                    ema_32 = ema_values.count(32) ? ema_values[32] : 0.0;
                    ema_64 = ema_values.count(64) ? ema_values[64] : 0.0;
                    ema_256 = ema_values.count(256) ? ema_values[256] : 0.0;
                } else if (tf_slow_strategy != nullptr) {
                    auto ema_values = tf_slow_strategy->get_ema_values(symbol, {8, 32, 64, 256});
                    ema_8 = ema_values.count(8) ? ema_values[8] : 0.0;
                    ema_32 = ema_values.count(32) ? ema_values[32] : 0.0;
                    ema_64 = ema_values.count(64) ? ema_values[64] : 0.0;
                    ema_256 = ema_values.count(256) ? ema_values[256] : 0.0;
                }
            }

            // Write position row
            file << symbol << ","
                 << quantity << ","
                 << market_price << ","
                 << notional << ","
                 << pct_of_gross << ","
                 << pct_of_portfolio << ","
                 << forecast << ","
                 << std::fixed << std::setprecision(6) << volatility << ","
                 << ema_8 << ","
                 << ema_32 << ","
                 << ema_64 << ","
                 << ema_256 << "\n";
        }

        file.close();
        INFO("CSVExporter: Current positions saved to " + filename);
        return Result<std::string>(filename);

    } catch (const std::exception& e) {
        ERROR("CSVExporter: Exception in export_current_positions: " + std::string(e.what()));
        return Result<std::string>(std::make_unique<TradeError>(ErrorCode::FILE_IO_ERROR, "Failed to export current positions: " + std::string(e.what())));
    }
}

Result<std::string> CSVExporter::export_finalized_positions(
    const std::chrono::system_clock::time_point& date,
    const std::chrono::system_clock::time_point& yesterday_date,
    std::shared_ptr<IDatabase> db,
    const std::unordered_map<std::string, Position>& fallback_positions,
    const std::unordered_map<std::string, double>& entry_prices,
    const std::unordered_map<std::string, double>& exit_prices) {

    try {
        INFO("CSVExporter: Exporting finalized positions...");

        // Generate filename: DD-MM-YYYY_positions_asof_DD-MM-YYYY.csv
        std::string yesterday_str = format_date_for_filename(yesterday_date);
        std::string today_str = format_date_for_filename(date);
        std::string filename = output_directory_ + yesterday_str + "_positions_asof_" + today_str + ".csv";

        std::ofstream file(filename);
        if (!file.is_open()) {
            return Result<std::string>(std::make_unique<TradeError>(ErrorCode::FILE_IO_ERROR, "Failed to open file for writing: " + filename));
        }

        // Write CSV header
        file << "symbol,quantity,entry_price,exit_price,realized_pnl\n";

        // Try to load finalized positions from database
        bool use_database = false;
        std::shared_ptr<arrow::RecordBatch> table;

        // TODO: Re-enable database loading after fixing includes
        // For now, always use fallback positions
        WARN("CSVExporter: Using fallback positions (database loading temporarily disabled)");

        if (false && db != nullptr) {
            // Process database results
            auto symbol_arr = std::static_pointer_cast<arrow::StringArray>(table->column(0));
            auto quantity_arr = std::static_pointer_cast<arrow::StringArray>(table->column(1));
            auto realized_pnl_arr = std::static_pointer_cast<arrow::StringArray>(table->column(3));

            for (int64_t i = 0; i < table->num_rows(); ++i) {
                if (!symbol_arr->IsNull(i) && !quantity_arr->IsNull(i)) {
                    std::string symbol = symbol_arr->GetString(i);
                    double quantity = std::stod(quantity_arr->GetString(i));
                    double realized_pnl = std::stod(realized_pnl_arr->GetString(i));

                    // Skip zero positions
                    if (std::abs(quantity) < 0.0001) continue;

                    // Get entry price (Day T-2 close)
                    double entry_price = 0.0;
                    auto entry_it = entry_prices.find(symbol);
                    if (entry_it != entry_prices.end()) {
                        entry_price = entry_it->second;
                    }

                    // Get exit price (Day T-1 close)
                    double exit_price = 0.0;
                    auto exit_it = exit_prices.find(symbol);
                    if (exit_it != exit_prices.end()) {
                        exit_price = exit_it->second;
                    }

                    // Write row
                    file << symbol << ","
                         << quantity << ","
                         << entry_price << ","
                         << exit_price << ","
                         << realized_pnl << "\n";
                }
            }
        } else {
            // Use fallback positions
            for (const auto& [symbol, position] : fallback_positions) {
                double quantity = position.quantity.as_double();

                // Skip zero positions
                if (std::abs(quantity) < 0.0001) continue;

                // Get entry price (Day T-2 close)
                double entry_price = 0.0;
                auto entry_it = entry_prices.find(symbol);
                if (entry_it != entry_prices.end()) {
                    entry_price = entry_it->second;
                }

                // Get exit price (Day T-1 close)
                double exit_price = 0.0;
                auto exit_it = exit_prices.find(symbol);
                if (exit_it != exit_prices.end()) {
                    exit_price = exit_it->second;
                }

                // Realized PnL from position
                double realized_pnl = position.realized_pnl.as_double();

                // Write row
                file << symbol << ","
                     << quantity << ","
                     << entry_price << ","
                     << exit_price << ","
                     << realized_pnl << "\n";
            }
        }

        file.close();
        INFO("CSVExporter: Finalized positions saved to " + filename);
        return Result<std::string>(filename);

    } catch (const std::exception& e) {
        ERROR("CSVExporter: Exception in export_finalized_positions: " + std::string(e.what()));
        return Result<std::string>(std::make_unique<TradeError>(ErrorCode::FILE_IO_ERROR, "Failed to export finalized positions: " + std::string(e.what())));
    }
}

} // namespace trade_ngin