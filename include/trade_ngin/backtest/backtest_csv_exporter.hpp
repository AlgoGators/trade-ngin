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

class BacktestCSVExporter {
public:
    BacktestCSVExporter(const std::string& output_directory);
    ~BacktestCSVExporter();

    Result<void> initialize_files();

    Result<void> append_daily_positions(
        const Timestamp& date,
        const std::unordered_map<std::string, Position>& positions,
        const std::unordered_map<std::string, double>& market_prices,
        double portfolio_value,
        double gross_notional,
        double net_notional,
        const std::vector<std::shared_ptr<StrategyInterface>>& strategies
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

    std::string format_date(const Timestamp& ts) const;
};

} // namespace backtest
} // namespace trade_ngin
