// trade_ngin/backtest/strategy_backtester.hpp
#pragma once

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/config_base.hpp"
#include "trade_ngin/strategy/strategy_interface.hpp"
#include "trade_ngin/backtest/slippage_models.hpp"
#include <memory>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <map>

namespace trade_ngin {

class PostgresDatabase;

namespace backtest {

// Strategy-level backtest configuration
struct StrategyBacktestConfig : public ConfigBase {
    std::vector<std::string> symbols;
    AssetClass asset_class = AssetClass::FUTURES;
    DataFrequency data_freq = DataFrequency::DAILY;
    std::string data_type = "ohlcv";
    Timestamp start_date;
    Timestamp end_date;
    double initial_capital = 1000000.0;  // $1M for strategy allocation
    double commission_rate = 0.0005;     // 5 basis points
    double slippage_model = 1.0;         // 1 bp
    bool store_trade_details = true;

    // Configuration metadata
    std::string version{"1.0.0"};

    // Helper method to format timestamp
    std::string format_timestamp(const Timestamp& ts) const {
        return std::to_string(std::chrono::duration_cast<std::chrono::seconds>(ts.time_since_epoch()).count());
    }

    // JSON serialization
    nlohmann::json to_json() const override {
        nlohmann::json j;
        j["symbols"] = symbols;
        j["asset_class"] = asset_class;
        j["data_freq"] = data_freq;
        j["data_type"] = data_type;
        j["start_date"] = format_timestamp(start_date);
        j["end_date"] = format_timestamp(end_date);
        j["initial_capital"] = initial_capital;
        j["commission_rate"] = commission_rate;
        j["slippage_model"] = slippage_model;
        j["store_trade_details"] = store_trade_details;
        j["version"] = version;
        return j;
    }

    void from_json(const nlohmann::json& j) override {
        if (j.contains("symbols")) symbols = j.at("symbols").get<std::vector<std::string>>();
        if (j.contains("asset_class")) asset_class = j.at("asset_class").get<AssetClass>();
        if (j.contains("data_freq")) data_freq = j.at("data_freq").get<DataFrequency>();
        if (j.contains("data_type")) data_type = j.at("data_type").get<std::string>();
        if (j.contains("start_date")) start_date = Timestamp(std::chrono::seconds(j.at("start_date").get<int64_t>()));
        if (j.contains("end_date")) end_date = Timestamp(std::chrono::seconds(j.at("end_date").get<int64_t>()));
        if (j.contains("initial_capital")) initial_capital = j.at("initial_capital").get<double>();
        if (j.contains("commission_rate")) commission_rate = j.at("commission_rate").get<double>();
        if (j.contains("slippage_model")) slippage_model = j.at("slippage_model").get<double>();
        if (j.contains("store_trade_details")) store_trade_details = j.at("store_trade_details").get<bool>();
        if (j.contains("version")) version = j.at("version").get<std::string>();
    }
};

// Strategy backtest results
struct StrategyBacktestResults {
    // Performance metrics
    double total_return = 0.0;
    double volatility = 0.0;
    double sharpe_ratio = 0.0;
    double sortino_ratio = 0.0;
    double max_drawdown = 0.0;
    double calmar_ratio = 0.0;
    
    // Risk metrics
    double var_95 = 0.0;
    double cvar_95 = 0.0;
    double beta = 0.0;
    double correlation = 0.0;
    double downside_volatility = 0.0;
    
    // Trading metrics
    int total_trades = 0;
    double win_rate = 0.0;
    double profit_factor = 0.0;
    double avg_win = 0.0;
    double avg_loss = 0.0;
    double max_win = 0.0;
    double max_loss = 0.0;
    
    // Trade details
    std::vector<ExecutionReport> executions;
    std::vector<Position> positions;
    
    // Time series
    std::vector<std::pair<Timestamp, double>> equity_curve;
    std::vector<std::pair<Timestamp, double>> drawdown_curve;
    
    // Aggregated results
    std::map<std::string, double> monthly_returns;
    std::map<std::string, double> symbol_pnl;
};

/**
 * @brief Standalone backtester for individual strategy testing
 * 
 * This class provides a simplified backtesting framework focused on
 * testing individual strategy logic without portfolio-level constraints.
 */
class StrategyBacktester {
public:
    /**
     * @brief Construct a new Strategy Backtester
     * 
     * @param config Configuration for the backtest
     * @param db Database connection for market data
     */
    StrategyBacktester(
        StrategyBacktestConfig config,
        std::shared_ptr<PostgresDatabase> db);
    
    /**
     * @brief Run backtest for a single strategy
     * 
     * @param strategy Strategy implementation to test
     * @return Result<StrategyBacktestResults> Backtest results or error
     */
    Result<StrategyBacktestResults> run(std::shared_ptr<StrategyInterface> strategy);
    
    /**
     * @brief Save backtest results to database
     * 
     * @param results Results to save
     * @param run_id Optional ID for the backtest run
     * @return Result<void> Success or error
     */
    Result<void> save_results(
        const StrategyBacktestResults& results,
        const std::string& run_id = "") const;
    
    /**
     * @brief Load backtest results from database
     * 
     * @param run_id ID of the backtest run to load
     * @return Result<StrategyBacktestResults> Loaded results or error
     */
    Result<StrategyBacktestResults> load_results(
        const std::string& run_id) const;

protected:
    /**
     * @brief Process market data and generate signals for strategy
     * 
     * @param bars Market data bars for current time period
     * @param strategy Strategy to process data
     * @param current_positions Current position map to update
     * @param executions Vector to store generated executions
     * @param equity_curve Vector to store equity curve points
     * @return Result<void> Success or error
     */
    Result<void> process_bar(
        const std::vector<Bar>& bars,
        std::shared_ptr<StrategyInterface> strategy,
        std::unordered_map<std::string, Position>& current_positions,
        std::vector<ExecutionReport>& executions,
        std::vector<std::pair<Timestamp, double>>& equity_curve);
    
    /**
     * @brief Load historical market data from database
     * 
     * @return Result<std::vector<Bar>> Market data bars or error
     */
    Result<std::vector<Bar>> load_market_data() const;
    
    /**
     * @brief Calculate transaction costs for an execution
     * 
     * @param execution Execution report
     * @return double Transaction cost amount
     */
    double calculate_transaction_costs(const ExecutionReport& execution) const;
    
    /**
     * @brief Apply slippage model to price
     * 
     * @param price Base price
     * @param quantity Trade quantity
     * @param side Trade side (BUY/SELL)
     * @return double Price with slippage applied
     */
    double apply_slippage(double price, double quantity, Side side) const;
    
    /**
     * @brief Calculate performance metrics from equity curve
     * 
     * @param equity_curve Equity curve time series
     * @param executions List of executions
     * @return StrategyBacktestResults Calculated metrics
     */
    StrategyBacktestResults calculate_metrics(
        const std::vector<std::pair<Timestamp, double>>& equity_curve,
        const std::vector<ExecutionReport>& executions) const;
    
    /**
     * @brief Calculate drawdown time series from equity curve
     * 
     * @param equity_curve Equity curve time series
     * @return std::vector<std::pair<Timestamp, double>> Drawdown time series
     */
    std::vector<std::pair<Timestamp, double>> calculate_drawdowns(
        const std::vector<std::pair<Timestamp, double>>& equity_curve) const;
    
    /**
     * @brief Calculate risk metrics from return series
     * 
     * @param returns Vector of period returns
     * @return std::unordered_map<std::string, double> Risk metrics
     */
    std::unordered_map<std::string, double> calculate_risk_metrics(
        const std::vector<double>& returns) const;
    
    /**
     * @brief Format timestamp for database storage
     * 
     * @param ts Timestamp to format
     * @return std::string Formatted timestamp string
     */
    std::string format_timestamp(const Timestamp& ts) const;

private:
    StrategyBacktestConfig config_;
    std::shared_ptr<PostgresDatabase> db_;
    std::unique_ptr<SlippageModel> slippage_model_;
};

} // namespace backtest
} // namespace trade_ngin
