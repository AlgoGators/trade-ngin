// src/backtest/backtest_csv_exporter.cpp
#include "trade_ngin/backtest/backtest_csv_exporter.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/data/backtest_position_row.hpp"
#include "trade_ngin/strategy/trend_readings.hpp"
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <map>
#include <set>

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

        changes_file_.open(std::filesystem::path(output_directory_) / "position_changes.csv");
        if (!changes_file_.is_open()) {
            return make_error<void>(ErrorCode::CONVERSION_ERROR,
                                    "Failed to open position_changes.csv for writing",
                                    "BacktestCSVExporter");
        }
        // One row per sleeve and symbol whose quantity changed that day: the quantity after
        // the change, the price the position was marked at the day before, the price the
        // day's fills traded at, and the realized P&L of the row the run stores for the day.
        changes_file_ << "date,strategy,symbol,quantity,previous_mark,fill_price,realized_pnl\n";

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

SleeveFillPrices BacktestCSVExporter::fill_prices(
    const std::unordered_map<std::string, std::vector<ExecutionReport>>& day_fills) {
    struct Sum {
        double quantity{0.0};
        double value{0.0};
        double first_price{0.0};
        bool one_price{true};
    };
    SleeveFillPrices prices;
    for (const auto& [strategy_id, fills] : day_fills) {
        std::unordered_map<std::string, Sum> sums;
        for (const auto& fill : fills) {
            if (fill.execution_type != ExecutionType::STRATEGY)
                continue;
            const double quantity = std::abs(static_cast<double>(fill.filled_quantity));
            if (quantity < 1e-10)
                continue;
            const double price = static_cast<double>(fill.fill_price);
            Sum& sum = sums[fill.symbol];
            if (sum.quantity == 0.0) {
                sum.first_price = price;
            } else if (price != sum.first_price) {
                sum.one_price = false;
            }
            sum.quantity += quantity;
            sum.value += quantity * price;
        }
        for (const auto& [symbol, sum] : sums) {
            prices[strategy_id][symbol] =
                sum.one_price ? sum.first_price : sum.value / sum.quantity;
        }
    }
    return prices;
}

std::vector<PositionChange> BacktestCSVExporter::position_changes(
    const SleevePositions& current, const SleevePositions& previous, const SleeveFillPrices& fills,
    const std::function<bool(const std::string&)>& keeps_closed_rows) {
    // By sleeve, then symbol, over both days.
    std::map<std::string, std::set<std::string>> keys;
    for (const auto* book : {&current, &previous}) {
        for (const auto& [strategy_id, positions] : *book) {
            for (const auto& [symbol, _] : positions)
                keys[strategy_id].insert(symbol);
        }
    }
    const auto row_of = [](const SleevePositions& book, const std::string& strategy_id,
                           const std::string& symbol) -> const Position* {
        const auto sleeve = book.find(strategy_id);
        if (sleeve == book.end())
            return nullptr;
        const auto row = sleeve->second.find(symbol);
        return row == sleeve->second.end() ? nullptr : &row->second;
    };

    std::vector<PositionChange> changes;
    for (const auto& [strategy_id, symbols] : keys) {
        const bool keep_closed = keeps_closed_rows ? keeps_closed_rows(strategy_id) : false;
        for (const auto& symbol : symbols) {
            const Position* curr = row_of(current, strategy_id, symbol);
            const Position* prev = row_of(previous, strategy_id, symbol);
            const double curr_qty = curr ? static_cast<double>(curr->quantity) : 0.0;
            const double prev_qty = prev ? static_cast<double>(prev->quantity) : 0.0;
            if (std::abs(curr_qty - prev_qty) < 1e-10) continue;

            PositionChange change;
            change.strategy = strategy_id;
            change.symbol = symbol;
            change.quantity = curr_qty;
            if (prev && std::abs(prev_qty) >= 1e-10) {
                change.previous_mark = static_cast<double>(prev->average_price);
            }
            if (const auto sleeve = fills.find(strategy_id); sleeve != fills.end()) {
                if (const auto fill = sleeve->second.find(symbol); fill != sleeve->second.end()) {
                    change.fill_price = fill->second;
                }
            }
            if (curr) {
                const double realized = static_cast<double>(curr->realized_pnl);
                if (backtest_position_row_is_stored(curr_qty, realized, keep_closed)) {
                    change.realized_pnl = realized;
                }
            }
            changes.push_back(std::move(change));
        }
    }
    return changes;
}

Result<void> BacktestCSVExporter::append_position_changes(
    const Timestamp& date, const std::vector<PositionChange>& changes) {
    if (!changes_file_.is_open()) {
        return make_error<void>(ErrorCode::CONVERSION_ERROR,
                                "position_changes.csv file is not open", "BacktestCSVExporter");
    }

    try {
        const std::string date_str = format_date(date);
        const auto cell = [&](const std::optional<double>& value) {
            changes_file_ << ",";
            if (value)
                changes_file_ << *value;
        };
        for (const auto& change : changes) {
            changes_file_ << date_str << "," << change.strategy << "," << change.symbol << ","
                          << change.quantity;
            cell(change.previous_mark);
            cell(change.fill_price);
            cell(change.realized_pnl);
            changes_file_ << "\n";
        }
        changes_file_.flush();
        return Result<void>();
    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::CONVERSION_ERROR,
                                std::string("Error appending position changes: ") + e.what(),
                                "BacktestCSVExporter");
    }
}

void BacktestCSVExporter::finalize() {
    if (positions_file_.is_open()) {
        positions_file_.close();
    }
    if (changes_file_.is_open()) {
        changes_file_.close();
    }
    if (equity_curve_file_.is_open()) {
        equity_curve_file_.close();
    }
}

} // namespace backtest
} // namespace trade_ngin
