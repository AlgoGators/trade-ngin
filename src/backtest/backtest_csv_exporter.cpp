// src/backtest/backtest_csv_exporter.cpp
#include "trade_ngin/backtest/backtest_csv_exporter.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/strategy/trend_readings.hpp"
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <map>

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
        positions_file_ << "date,strategy,symbol,quantity,market_price,notional,"
                        << "pct_of_gross_notional,pct_of_portfolio_value,forecast,sizing_vol,"
                        << "ema_8,ema_32,ema_64,ema_256\n";

        finalized_file_.open(std::filesystem::path(output_directory_) / "finalized_positions.csv");
        if (!finalized_file_.is_open()) {
            return make_error<void>(
                ErrorCode::CONVERSION_ERROR,
                "Failed to open finalized_positions.csv for writing",
                "BacktestCSVExporter"
            );
        }
        finalized_file_ << "date,symbol,quantity,entry_price,exit_price,realized_pnl\n";

        equity_curve_file_.open(std::filesystem::path(output_directory_) / "equity_curve.csv");
        if (!equity_curve_file_.is_open()) {
            return make_error<void>(
                ErrorCode::CONVERSION_ERROR,
                "Failed to open equity_curve.csv for writing",
                "BacktestCSVExporter"
            );
        }
        equity_curve_file_ << "date,portfolio_value\n";

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
    const Timestamp& date, const SleevePositions& strategy_positions,
    const std::unordered_map<std::string, double>& market_prices, double portfolio_value,
    double gross_notional, double net_notional,
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

        // Each sleeve's own strategy, by id: a row's forecast, sizing volatility and averages
        // are the sleeve's that holds it (one figure per account row was the first trend
        // sleeve's on every row).
        std::unordered_map<std::string, const StrategyInterface*> strategy_of;
        for (const auto& strategy : strategies) {
            if (strategy)
                strategy_of[strategy->get_metadata().id] = strategy.get();
        }

        // By sleeve, then symbol.
        std::map<std::string, std::map<std::string, const Position*>> rows;
        for (const auto& [strategy_id, positions] : strategy_positions) {
            for (const auto& [symbol, position] : positions)
                rows[strategy_id][symbol] = &position;
        }

        for (const auto& [strategy_id, positions] : rows) {
            const auto strategy_it = strategy_of.find(strategy_id);
            const StrategyInterface* strategy =
                strategy_it == strategy_of.end() ? nullptr : strategy_it->second;
            for (const auto& [symbol, position] : positions) {
                double qty = static_cast<double>(position->quantity);
                if (std::abs(qty) < 1e-10)
                    continue;

                double price = 0.0;
                auto price_it = market_prices.find(symbol);
                if (price_it != market_prices.end()) {
                    price = price_it->second;
                }

                // Calculate notional using instrument registry
                double notional = 0.0;
                auto instrument = registry.get_instrument(symbol);
                if (instrument) {
                    // get_notional_value is a magnitude (futures.cpp): a short row keeps its sign.
                    notional = std::copysign(instrument->get_notional_value(qty, price), qty);
                } else {
                    notional = qty * price;
                }

                double pct_gross =
                    (gross_notional > 0) ? (std::abs(notional) / gross_notional) : 0.0;
                double pct_portfolio = (portfolio_value > 0) ? (notional / portfolio_value) : 0.0;

                const TrendReadings readings = trend_readings_of(strategy, symbol);

                positions_file_ << date_str << "," << strategy_id << "," << symbol << "," << qty
                                << "," << price << "," << notional << "," << pct_gross << ","
                                << pct_portfolio << "," << readings.forecast << ","
                                << readings.sizing_vol << "," << readings.ema_8 << ","
                                << readings.ema_32 << "," << readings.ema_64 << ","
                                << readings.ema_256 << "\n";
            }
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

Result<void> BacktestCSVExporter::append_equity_curve(
    const Timestamp& date,
    double portfolio_value) {

    if (!equity_curve_file_.is_open()) {
        return make_error<void>(
            ErrorCode::CONVERSION_ERROR,
            "equity_curve.csv file is not open",
            "BacktestCSVExporter"
        );
    }

    try {
        equity_curve_file_ << format_date(date) << "," << portfolio_value << "\n";
        equity_curve_file_.flush();
        return Result<void>();
    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::CONVERSION_ERROR,
            std::string("Error appending equity curve: ") + e.what(),
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

            double curr_qty = (curr_it != current_positions.end()) ? static_cast<double>(curr_it->second.quantity) : 0.0;
            double prev_qty = (prev_it != previous_positions.end()) ? static_cast<double>(prev_it->second.quantity) : 0.0;

            // Skip if no change
            if (std::abs(curr_qty - prev_qty) < 1e-10) continue;

            double entry_price = (prev_it != previous_positions.end()) ? static_cast<double>(prev_it->second.average_price) : 0.0;
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
            // The closed quantity is the part of prev_qty this change takes off, with prev_qty's
            // sign: all of it when the position is closed or flips through zero (+2 -> -3 closes
            // 2, -3 -> +2 closes 3; the new leg is opened, not realized), prev - curr when it is
            // reduced on the same side, none when it grows.
            double realized_pnl = 0.0;
            double closed_qty = 0.0;
            if (std::abs(prev_qty) > 1e-10) {
                if (curr_qty * prev_qty <= 0.0) {
                    closed_qty = prev_qty;
                } else if (std::abs(curr_qty) < std::abs(prev_qty)) {
                    closed_qty = prev_qty - curr_qty;
                }
            }
            if (closed_qty != 0.0) {
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
    if (equity_curve_file_.is_open()) {
        equity_curve_file_.close();
    }
}

} // namespace backtest
} // namespace trade_ngin
