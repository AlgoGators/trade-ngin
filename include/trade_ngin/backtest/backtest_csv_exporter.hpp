// include/trade_ngin/backtest/backtest_csv_exporter.hpp
#pragma once

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/strategy/strategy_interface.hpp"
#include "trade_ngin/strategy/trend_following.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include <string>
#include <fstream>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>
#include <memory>

namespace trade_ngin {
namespace backtest {

/// The book as the sleeves hold it: sleeve id -> symbol -> the sleeve's position.
using SleevePositions = std::unordered_map<std::string, std::unordered_map<std::string, Position>>;

/// Sleeve id -> symbol -> the price the sleeve's fills of that symbol traded at on the day.
using SleeveFillPrices = std::unordered_map<std::string, std::unordered_map<std::string, double>>;

/// One row of position_changes.csv: a sleeve's quantity of a symbol changed on the day. A figure
/// that does not exist is absent (an empty cell), never 0.
struct PositionChange {
    std::string strategy;
    std::string symbol;
    /// The sleeve's quantity after the change.
    double quantity{0.0};
    /// The price of the sleeve's row of the day before, as that row is stored (its
    /// average_price: on a futures row the close the row was entered and valued from, one bar
    /// before the row's own date); absent on a new position.
    std::optional<double> previous_mark;
    /// The price the sleeve's fills of the symbol traded at that day; absent when it has none.
    std::optional<double> fill_price;
    /// The realized P&L of the sleeve's row as the run stores it for the day
    /// (backtest.final_positions); absent when the run stores no row (a futures sleeve's
    /// position closed to zero).
    std::optional<double> realized_pnl;
};

/**
 * @brief The four CSV files a backtest writes beside its stored rows
 *
 * positions.csv (a row per day, sleeve and held symbol), position_changes.csv (a row per day,
 * sleeve and symbol whose quantity changed), equity_curve.csv and portfolio_summary.csv (a row
 * per day: the portfolio value, the gross and the net notional). Each file starts with its
 * column names and holds nothing but rows. Dates are the UTC date of the bar's timestamp;
 * numbers print ten significant digits. A run writes into its own portfolio's directory
 * (portfolio_directory), so the files of one portfolio's run do not replace another's.
 */
class BacktestCSVExporter {
public:
    BacktestCSVExporter(const std::string& output_directory);
    ~BacktestCSVExporter();

    /**
     * @brief The directory a portfolio's backtest writes its files into: `base_directory` /
     *        `portfolio_id`
     *
     * A character of the id that is not a letter, a digit, '_', '-' or '.' is written '_'. An
     * id that is empty, "." or ".." names no directory of its own: the base directory.
     */
    static std::string portfolio_directory(const std::string& base_directory,
                                           const std::string& portfolio_id);

    Result<void> initialize_files();

    /**
     * @brief The day's rows of positions.csv: one per sleeve and symbol the sleeve holds, by
     *        sleeve then symbol; and the day's row of portfolio_summary.csv (the portfolio
     *        value, the gross and the net notional handed in).
     *
     * quantity and notional are the sleeve's; the two shares divide the row's notional by the
     * account's gross notional and portfolio value. forecast, sizing_vol and the four averages
     * are that sleeve's own (trend_readings_of, the function the live positions file reads a
     * sleeve through; a sleeve is matched to its strategy by id). sizing_vol is the volatility
     * estimate the sleeve sized the position on, annualised, a fraction, as of the bar the
     * forecast was computed on; it is not the run's realised volatility statistic. A sleeve
     * that is not a trend sleeve prints 0 in the six columns.
     */
    Result<void> append_daily_positions(
        const Timestamp& date, const SleevePositions& strategy_positions,
        const std::unordered_map<std::string, double>& market_prices, double portfolio_value,
        double gross_notional, double net_notional,
        const std::vector<std::shared_ptr<StrategyInterface>>& strategies);

    Result<void> append_equity_curve(
        const Timestamp& date,
        double portfolio_value
    );

    /**
     * @brief The price each sleeve's fills of each symbol traded at on one day
     *
     * The quantity-weighted mean price of the sleeve's fills of the symbol that are the
     * strategy's own (a roll leg and a borrow row change no position and are left out); the
     * price itself when every such fill traded at one price.
     *
     * @param day_fills Sleeve id -> the sleeve's execution reports of the day
     */
    static SleeveFillPrices fill_prices(
        const std::unordered_map<std::string, std::vector<ExecutionReport>>& day_fills);

    /**
     * @brief The day's rows of position_changes.csv: one per sleeve and symbol whose sleeve
     *        quantity differs from the day before, by sleeve then symbol
     *
     * Nothing is computed here: the previous mark is the previous day's row's price, the fill
     * price comes from `fills`, and the realized P&L is the current row's own, on the rows the
     * run stores (backtest_position_row_is_stored, the writer's predicate).
     *
     * @param current The sleeves' rows after the day's P&L was booked
     * @param previous The same the day before
     * @param fills fill_prices of the day
     * @param keeps_closed_rows Whether a sleeve stores its rows closed to zero (a cash book)
     */
    static std::vector<PositionChange> position_changes(
        const SleevePositions& current, const SleevePositions& previous,
        const SleeveFillPrices& fills,
        const std::function<bool(const std::string&)>& keeps_closed_rows);

    Result<void> append_position_changes(const Timestamp& date,
                                         const std::vector<PositionChange>& changes);

    void finalize();

private:
    std::string output_directory_;
    std::ofstream positions_file_;
    std::ofstream changes_file_;
    std::ofstream equity_curve_file_;
    std::ofstream summary_file_;
};

} // namespace backtest
} // namespace trade_ngin
