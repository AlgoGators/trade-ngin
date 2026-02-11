// src/backtest/backtest_csv_exporter.cpp
#include "trade_ngin/backtest/backtest_csv_exporter.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <cmath>

namespace trade_ngin {
namespace backtest {

BacktestCSVExporter::BacktestCSVExporter(const std::string& output_directory)
    : output_directory_(output_directory) {
}

BacktestCSVExporter::~BacktestCSVExporter() {
    finalize();
}

Result<void> BacktestCSVExporter::initialize_files() {
    try {
        std::filesystem::create_directories(output_directory_);

        positions_file_.open(std::filesystem::path(output_directory_) / "positions.csv");
        if (!positions_file_.is_open()) {
            return make_error<void>(
                ErrorCode::CONVERSION_ERROR,
                "Failed to open positions.csv for writing",
                "BacktestCSVExporter"
            );
        }
        positions_file_ << "date,symbol,quantity,market_price,notional,pct_of_gross_notional,"
                        << "pct_of_portfolio_value,forecast,volatility,ema_8,ema_32,ema_64,ema_256\n";

        finalized_file_.open(std::filesystem::path(output_directory_) / "finalized_positions.csv");
        if (!finalized_file_.is_open()) {
            return make_error<void>(
                ErrorCode::CONVERSION_ERROR,
                "Failed to open finalized_positions.csv for writing",
                "BacktestCSVExporter"
            );
        }
        finalized_file_ << "date,symbol,quantity,entry_price,exit_price,realized_pnl\n";

        return Result<void>();
    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::CONVERSION_ERROR,
            std::string("Error initializing CSV files: ") + e.what(),
            "BacktestCSVExporter"
        );
    }
}

std::string BacktestCSVExporter::format_date(const Timestamp& ts) const {
    auto time_t = std::chrono::system_clock::to_time_t(ts);
    std::tm tm;
    core::safe_localtime(&time_t, &tm);
    std::stringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%d");
    return ss.str();
}

Result<void> BacktestCSVExporter::append_daily_positions(
    const Timestamp& date,
    const std::unordered_map<std::string, Position>& positions,
    const std::unordered_map<std::string, double>& market_prices,
    double portfolio_value,
    double gross_notional,
    double net_notional,
    const std::vector<std::shared_ptr<StrategyInterface>>& strategies) {

    if (!positions_file_.is_open()) {
        return make_error<void>(
            ErrorCode::CONVERSION_ERROR,
            "positions.csv file is not open",
            "BacktestCSVExporter"
        );
    }

    try {
        std::string date_str = format_date(date);
        auto& registry = InstrumentRegistry::instance();

        // Write portfolio header comment
        positions_file_ << "# Portfolio Value: " << portfolio_value
                        << ", Gross Notional: " << gross_notional
                        << ", Net Notional: " << net_notional
                        << ", Date: " << date_str << "\n";

        // Try to cast the first strategy to TrendFollowingStrategy for EMA/forecast/volatility
        TrendFollowingStrategy* trend_strategy = nullptr;
        for (const auto& strategy : strategies) {
            trend_strategy = dynamic_cast<TrendFollowingStrategy*>(strategy.get());
            if (trend_strategy) break;
        }

        for (const auto& [symbol, position] : positions) {
            if (std::abs(position.quantity) < 1e-10) continue;

            double price = 0.0;
            auto price_it = market_prices.find(symbol);
            if (price_it != market_prices.end()) {
                price = price_it->second;
            }

            // Calculate notional using instrument registry
            double notional = 0.0;
            auto instrument = registry.get_instrument(symbol);
            if (instrument) {
                notional = instrument->get_notional_value(position.quantity, price);
            } else {
                notional = position.quantity * price;
            }

            double pct_gross = (gross_notional > 0) ? (std::abs(notional) / gross_notional) : 0.0;
            double pct_portfolio = (portfolio_value > 0) ? (notional / portfolio_value) : 0.0;

            // Get forecast, volatility, and EMAs from trend strategy
            double forecast = 0.0;
            double volatility = 0.0;
            double ema_8 = 0.0, ema_32 = 0.0, ema_64 = 0.0, ema_256 = 0.0;

            if (trend_strategy) {
                forecast = trend_strategy->get_forecast(symbol);
                volatility = trend_strategy->get_volatility(symbol);
                auto ema_values = trend_strategy->get_ema_values(symbol, {8, 32, 64, 256});
                if (ema_values.count(8)) ema_8 = ema_values.at(8);
                if (ema_values.count(32)) ema_32 = ema_values.at(32);
                if (ema_values.count(64)) ema_64 = ema_values.at(64);
                if (ema_values.count(256)) ema_256 = ema_values.at(256);
            }

            positions_file_ << date_str << ","
                            << symbol << ","
                            << position.quantity << ","
                            << price << ","
                            << notional << ","
                            << pct_gross << ","
                            << pct_portfolio << ","
                            << forecast << ","
                            << volatility << ","
                            << ema_8 << ","
                            << ema_32 << ","
                            << ema_64 << ","
                            << ema_256 << "\n";
        }

        positions_file_.flush();
        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::CONVERSION_ERROR,
            std::string("Error appending daily positions: ") + e.what(),
            "BacktestCSVExporter"
        );
    }
}

Result<void> BacktestCSVExporter::append_finalized_positions(
    const Timestamp& date,
    const std::unordered_map<std::string, Position>& current_positions,
    const std::unordered_map<std::string, Position>& previous_positions,
    const std::unordered_map<std::string, double>& market_prices) {

    if (!finalized_file_.is_open()) {
        return make_error<void>(
            ErrorCode::CONVERSION_ERROR,
            "finalized_positions.csv file is not open",
            "BacktestCSVExporter"
        );
    }

    try {
        std::string date_str = format_date(date);

        // Collect all symbols from both current and previous positions
        std::unordered_map<std::string, bool> all_symbols;
        for (const auto& [sym, _] : current_positions) all_symbols[sym] = true;
        for (const auto& [sym, _] : previous_positions) all_symbols[sym] = true;

        for (const auto& [symbol, _] : all_symbols) {
            auto curr_it = current_positions.find(symbol);
            auto prev_it = previous_positions.find(symbol);

            double curr_qty = (curr_it != current_positions.end()) ? curr_it->second.quantity : 0.0;
            double prev_qty = (prev_it != previous_positions.end()) ? prev_it->second.quantity : 0.0;

            // Skip if no change
            if (std::abs(curr_qty - prev_qty) < 1e-10) continue;

            double entry_price = (prev_it != previous_positions.end()) ? prev_it->second.average_price : 0.0;
            double exit_price = 0.0;
            auto price_it = market_prices.find(symbol);
            if (price_it != market_prices.end()) {
                exit_price = price_it->second;
            }

            // If entering a new position, entry_price is the current market price
            if (std::abs(prev_qty) < 1e-10 && std::abs(curr_qty) > 1e-10) {
                entry_price = exit_price;
                exit_price = 0.0; // Not exited yet
            }

            // Calculate realized PnL for closed/reduced positions
            double realized_pnl = 0.0;
            if (std::abs(prev_qty) > 1e-10 && std::abs(curr_qty) < std::abs(prev_qty)) {
                // Position reduced or closed
                double closed_qty = prev_qty - curr_qty;
                auto instrument = InstrumentRegistry::instance().get_instrument(symbol);
                double multiplier = instrument ? instrument->get_multiplier() : 1.0;
                realized_pnl = closed_qty * (exit_price - entry_price) * multiplier;
            }

            finalized_file_ << date_str << ","
                            << symbol << ","
                            << curr_qty << ","
                            << entry_price << ","
                            << exit_price << ","
                            << realized_pnl << "\n";
        }

        finalized_file_.flush();
        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::CONVERSION_ERROR,
            std::string("Error appending finalized positions: ") + e.what(),
            "BacktestCSVExporter"
        );
    }
}

void BacktestCSVExporter::finalize() {
    if (positions_file_.is_open()) {
        positions_file_.close();
    }
    if (finalized_file_.is_open()) {
        finalized_file_.close();
    }
}

} // namespace backtest
} // namespace trade_ngin
