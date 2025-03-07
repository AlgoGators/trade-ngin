// include/trade_ngin/backtest/engine.hpp
#pragma once

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/config_base.hpp"
#include "trade_ngin/backtest/slippage_models.hpp"
#include "trade_ngin/strategy/strategy_interface.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/risk/risk_manager.hpp"
#include "trade_ngin/optimization/dynamic_optimizer.hpp"
#include <memory>
#include <vector>
#include <unordered_map>
#include <chrono>

namespace trade_ngin {
namespace backtest {

/**
 * @brief Configuration for backtest simulation
 */
struct BacktestConfig : public ConfigBase {
    // Time parameters
    Timestamp start_date;
    Timestamp end_date;
    bool use_calendar_time{true};     // Whether to use calendar time or trading time

    // Asset parameters
    std::vector<std::string> symbols;
    AssetClass asset_class{AssetClass::FUTURES};
    DataFrequency data_freq{DataFrequency::DAILY};

    // Data parameters
    std::string data_source{"database"};
    std::string data_type{"ohlcv"};

    // Trading parameters
    double initial_capital{1000000.0};
    bool reinvest_profits{true};
    double commission_rate{0.0};
    double slippage_model{0.0};       // Basis points
    
    // Risk parameters
    RiskConfig risk_config;
    bool use_risk_management{true};
    
    // Optimization parameters
    DynamicOptConfig opt_config;
    bool use_optimization{true};

    // Analysis parameters
    bool calc_intraday_metrics{false};
    bool store_trade_details{true};
    bool calc_risk_metrics{true};
    std::string results_db_schema{"backtest_results"};

    // Configuration metadata
    std::string version{"1.0.0"};

    // Implement serialization/deserialization methods
    nlohmann::json to_json() const override {
        nlohmann::json j;
        j["start_date"] = start_date.time_since_epoch().count();
        j["end_date"] = end_date.time_since_epoch().count();
        j["use_calendar_time"] = use_calendar_time;
        j["symbols"] = symbols;
        j["asset_class"] = static_cast<int>(asset_class);
        j["data_freq"] = static_cast<int>(data_freq);
        j["data_source"] = data_source;
        j["data_type"] = data_type;
        j["initial_capital"] = initial_capital;
        j["reinvest_profits"] = reinvest_profits;
        j["commission_rate"] = commission_rate;
        j["slippage_model"] = slippage_model;
        j["risk_config"] = risk_config.to_json();
        j["use_risk_management"] = use_risk_management;
        j["opt_config"] = opt_config.to_json();
        j["use_optimization"] = use_optimization;
        j["calc_intraday_metrics"] = calc_intraday_metrics;
        j["store_trade_details"] = store_trade_details;
        j["calc_risk_metrics"] = calc_risk_metrics;
        j["results_db_schema"] = results_db_schema;
        j["version"] = version;
        return j;
    }

    void from_json(const nlohmann::json& j) override {
        if (j.contains("start_date")) start_date = Timestamp(std::chrono::milliseconds(j.at("start_date").get<int64_t>()));
        if (j.contains("end_date")) end_date = Timestamp(std::chrono::milliseconds(j.at("end_date").get<int64_t>()));
        if (j.contains("use_calendar_time")) use_calendar_time = j.at("use_calendar_time").get<bool>();
        if (j.contains("symbols")) symbols = j.at("symbols").get<std::vector<std::string>>();
        if (j.contains("asset_class")) asset_class = static_cast<AssetClass>(j.at("asset_class").get<int>());
        if (j.contains("data_freq")) data_freq = static_cast<DataFrequency>(j.at("data_freq").get<int>());
        if (j.contains("data_source")) data_source = j.at("data_source").get<std::string>();
        if (j.contains("data_type")) data_type = j.at("data_type").get<std::string>();
        if (j.contains("initial_capital")) initial_capital = j.at("initial_capital").get<double>();
        if (j.contains("reinvest_profits")) reinvest_profits = j.at("reinvest_profits").get<bool>();
        if (j.contains("commission_rate")) commission_rate = j.at("commission_rate").get<double>();
        if (j.contains("slippage_model")) slippage_model = j.at("slippage_model").get<double>();
        if (j.contains("risk_config")) risk_config.from_json(j.at("risk_config"));
        if (j.contains("use_risk_management")) use_risk_management = j.at("use_risk_management").get<bool>();
        if (j.contains("opt_config")) opt_config.from_json(j.at("opt_config"));
        if (j.contains("use_optimization")) use_optimization = j.at("use_optimization").get<bool>();
        if (j.contains("calc_intraday_metrics")) calc_intraday_metrics = j.at("calc_intraday_metrics").get<bool>();
        if (j.contains("store_trade_details")) store_trade_details = j.at("store_trade_details").get<bool>();
        if (j.contains("calc_risk_metrics")) calc_risk_metrics = j.at("calc_risk_metrics").get<bool>();
        if (j.contains("results_db_schema")) results_db_schema = j.at("results_db_schema").get<std::string>();
        if (j.contains("version")) version = j.at("version").get<std::string>();
    }
};

/**
 * @brief Results from a backtest run
 */
struct BacktestResults {
    // Performance metrics
    double total_return{0.0};
    double sharpe_ratio{0.0};
    double sortino_ratio{0.0};
    double max_drawdown{0.0};
    double calmar_ratio{0.0};
    double volatility{0.0};
    
    // Trading metrics
    int total_trades{0};
    double win_rate{0.0};
    double profit_factor{0.0};
    double avg_win{0.0};
    double avg_loss{0.0};
    double max_win{0.0};
    double max_loss{0.0};
    double avg_holding_period{0.0};
    
    // Risk metrics
    double var_95{0.0};
    double cvar_95{0.0};
    double beta{0.0};
    double correlation{0.0};
    double downside_volatility{0.0};
    
    // Trade details
    std::vector<ExecutionReport> executions;
    std::vector<Position> positions;
    std::vector<std::pair<Timestamp, double>> equity_curve;
    std::vector<std::pair<Timestamp, double>> drawdown_curve;
    
    // Additional analysis
    std::unordered_map<std::string, double> monthly_returns;
    std::unordered_map<std::string, double> symbol_pnl;
    std::vector<std::pair<Timestamp, RiskResult>> risk_metrics;
};

/**
 * @brief Backtesting engine for strategy simulation
 */
class BacktestEngine {
public:
    /**
     * @brief Constructor
     * @param config Backtest configuration
     * @param db Database interface for market data
     */
    BacktestEngine(BacktestConfig config, std::shared_ptr<PostgresDatabase> db);

    /**
     * @brief Run backtest simulation
     * @param strategy Strategy to test
     * @return Result containing backtest results
     */
    Result<BacktestResults> run(std::shared_ptr<StrategyInterface> strategy);

    /**
     * @brief Save backtest results to database
     * @param results Results to save
     * @param run_id Optional identifier for this run
     * @return Result indicating success or failure
     */
    Result<void> save_results(
        const BacktestResults& results,
        const std::string& run_id = "") const;

    /**
     * @brief Load historical results from database
     * @param run_id Run identifier
     * @return Result containing backtest results
     */
    Result<BacktestResults> load_results(const std::string& run_id) const;

    /**
     * @brief Compare multiple backtest results
     * @param results Vector of results to compare
     * @return Result containing comparison metrics
     */
    static Result<std::unordered_map<std::string, double>> compare_results(
        const std::vector<BacktestResults>& results);

private:
    BacktestConfig config_;
    std::shared_ptr<PostgresDatabase> db_;
    std::unique_ptr<RiskManager> risk_manager_;
    std::unique_ptr<DynamicOptimizer> optimizer_;
    std::unique_ptr<SlippageModel> slippage_model_;

    /**
     * @brief Load market data for simulation
     * @return Result containing market data bars
     */
    Result<std::vector<Bar>> load_market_data() const;

    /**
     * @brief Process single market data update
     * @param bar Market data bar
     * @param strategy Strategy being tested
     * @param current_positions Current portfolio positions
     * @param equity_curve Equity curve to update
     * @return Result indicating success or failure
     */
    Result<void> process_bar(
        const std::vector<Bar>& bars,
        std::shared_ptr<StrategyInterface> strategy,
        std::unordered_map<std::string, Position>& current_positions,
        std::vector<std::pair<Timestamp, double>>& equity_curve,
        std::vector<RiskResult>& risk_metrics);

    /**
     * @brief Calculate transaction costs
     * @param execution Execution report
     * @return Transaction cost in currency units
     */
    double calculate_transaction_costs(const ExecutionReport& execution) const;

    /**
     * @brief Apply slippage model
     * @param price Original price
     * @param quantity Trade quantity
     * @param side Trade side
     * @return Adjusted price after slippage
     */
    double apply_slippage(double price, double quantity, Side side) const;

    /**
     * @brief Calculate performance metrics
     * @param equity_curve Vector of equity points
     * @param executions Vector of trades
     * @return Performance metrics
     */
    BacktestResults calculate_metrics(
        const std::vector<std::pair<Timestamp, double>>& equity_curve,
        const std::vector<ExecutionReport>& executions) const;

    /**
     * @brief Calculate drawdown series
     * @param equity_curve Equity curve points
     * @return Vector of drawdown points
     */
    std::vector<std::pair<Timestamp, double>> calculate_drawdowns(
        const std::vector<std::pair<Timestamp, double>>& equity_curve) const;

    /**
     * @brief Calculate risk metrics
     * @param returns Vector of returns
     * @return Risk metrics
     */
    std::unordered_map<std::string, double> calculate_risk_metrics(
        const std::vector<double>& returns) const;

    /**
     * @brief Helper function for timestamp formatting
     * @param timestamp Timestamp to format
     * @return Formatted string
     */
    std::string format_timestamp(const Timestamp& ts) const;
};

} // namespace backtest
} // namespace trade_ngin