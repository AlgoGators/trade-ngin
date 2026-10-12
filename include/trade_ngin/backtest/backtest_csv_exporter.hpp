// include/trade_ngin/backtest/backtest_csv_exporter.hpp
#pragma once

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/strategy/strategy_interface.hpp"
#include "trade_ngin/strategy/trend_following.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include <string>
#include <fstream>
#include <unordered_map>
#include <vector>
#include <memory>

namespace trade_ngin {
namespace backtest {

/// The book as the sleeves hold it: sleeve id -> symbol -> the sleeve's position.
using SleevePositions = std::unordered_map<std::string, std::unordered_map<std::string, Position>>;

class BacktestCSVExporter {
public:
    BacktestCSVExporter(const std::string& output_directory);
    ~BacktestCSVExporter();

    Result<void> initialize_files();

    /**
     * @brief The day's rows of positions.csv: one per sleeve and symbol the sleeve holds, by
     *        sleeve then symbol.
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

    Result<void> append_finalized_positions(
        const Timestamp& date,
        const std::unordered_map<std::string, Position>& current_positions,
        const std::unordered_map<std::string, Position>& previous_positions,
        const std::unordered_map<std::string, double>& market_prices
    );

    void finalize();

private:
    std::string output_directory_;
    std::ofstream positions_file_;
    std::ofstream finalized_file_;
    std::ofstream equity_curve_file_;

    std::string format_date(const Timestamp& ts) const;
};

} // namespace backtest
} // namespace trade_ngin
