// include/trade_ngin/backtest/backtest_engine.hpp
#pragma once

#include <chrono>
#include <memory>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <vector>
#include "trade_ngin/backtest/slippage_models.hpp"
#include "trade_ngin/core/config_base.hpp"
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/optimization/dynamic_optimizer.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/risk/risk_manager.hpp"
#include "trade_ngin/strategy/strategy_interface.hpp"

namespace trade_ngin {
namespace backtest {

/**
 * @brief Helper function to format timestamp as string
 * @param tp Timestamp to format
 * @return Formatted timestamp string
 */
inline std::string format_timestamp(const std::chrono::system_clock::time_point& tp) {
    auto time_t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm;
    core::safe_localtime(&time_t, &tm);
    std::stringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

/**
 * @brief Strategy configuration for backtest simulation
 */
struct StrategyBacktestConfig : public ConfigBase {
    std::vector<std::string> symbols;
    AssetClass asset_class = AssetClass::FUTURES;
    DataFrequency data_freq = DataFrequency::DAILY;
    std::string data_type = "ohlcv";
    Timestamp start_date;
    Timestamp end_date;
    Decimal initial_capital = Decimal(1000000.0);  // $1M for strategy allocation
    Decimal commission_rate = Decimal(0.0005);     // 5 basis points
    Decimal slippage_model = Decimal(1.0);         // 1 bp
    bool store_trade_details = true;
    int warmup_days = 0;  // Number of trading days to exclude from results (for strategy warmup)

    // Configuration metadata
    std::string version{"1.0.0"};

    // Helper method to format timestamp
    std::string format_timestamp(const Timestamp& ts) const {
        return std::to_string(
            std::chrono::duration_cast<std::chrono::seconds>(ts.time_since_epoch()).count());
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
        j["initial_capital"] = static_cast<double>(initial_capital);
        j["commission_rate"] = static_cast<double>(commission_rate);
        j["slippage_model"] = static_cast<double>(slippage_model);
        j["store_trade_details"] = store_trade_details;
        j["version"] = version;
        return j;
    }

    void from_json(const nlohmann::json& j) override {
        if (j.contains("symbols"))
            symbols = j.at("symbols").get<std::vector<std::string>>();
        if (j.contains("asset_class"))
            asset_class = j.at("asset_class").get<AssetClass>();
        if (j.contains("data_freq"))
            data_freq = j.at("data_freq").get<DataFrequency>();
        if (j.contains("data_type"))
            data_type = j.at("data_type").get<std::string>();
        if (j.contains("start_date"))
            start_date = Timestamp(std::chrono::seconds(j.at("start_date").get<int64_t>()));
        if (j.contains("end_date"))
            end_date = Timestamp(std::chrono::seconds(j.at("end_date").get<int64_t>()));
        if (j.contains("initial_capital"))
            initial_capital = Decimal(j.at("initial_capital").get<double>());
        if (j.contains("commission_rate"))
            commission_rate = Decimal(j.at("commission_rate").get<double>());
        if (j.contains("slippage_model"))
            slippage_model = Decimal(j.at("slippage_model").get<double>());
        if (j.contains("store_trade_details"))
            store_trade_details = j.at("store_trade_details").get<bool>();
        if (j.contains("version"))
            version = j.at("version").get<std::string>();
    }
};

/**
 * @brief Portfolio configuration for backtest simulation
 */
struct PortfolioBacktestConfig : public ConfigBase {
    Decimal initial_capital{Decimal(1000000.0)};  // Initial capital for portfolio
    bool use_risk_management{false};              // Enable risk management
    bool use_optimization{false};                 // Enable optimization
    RiskConfig risk_config;
    DynamicOptConfig opt_config;

    // Portfolio configuration
    std::vector<double> strategy_weights;  // Initial capital allocation to each strategy
    bool auto_rebalance;
    int rebalance_period;  // In days

    // Configuration metadata
    std::string version{"1.0.0"};

    // JSON serialization
    nlohmann::json to_json() const override {
        nlohmann::json j;
        j["initial_capital"] = static_cast<double>(initial_capital);
        j["use_risk_management"] = use_risk_management;
        j["use_optimization"] = use_optimization;
        j["risk_config"] = risk_config.to_json();
        j["opt_config"] = opt_config.to_json();
        j["strategy_weights"] = strategy_weights;
        j["auto_rebalance"] = auto_rebalance;
        j["rebalance_period"] = rebalance_period;
        j["version"] = version;
        return j;
    }

    void from_json(const nlohmann::json& j) override {
        if (j.contains("initial_capital"))
            initial_capital = Decimal(j.at("initial_capital").get<double>());
        if (j.contains("use_risk_management"))
            use_risk_management = j.at("use_risk_management").get<bool>();
        if (j.contains("use_optimization"))
            use_optimization = j.at("use_optimization").get<bool>();
        if (j.contains("risk_config"))
            risk_config.from_json(j.at("risk_config"));
        if (j.contains("opt_config"))
            opt_config.from_json(j.at("opt_config"));
        if (j.contains("strategy_weights"))
            strategy_weights = j.at("strategy_weights").get<std::vector<double>>();
        if (j.contains("auto_rebalance"))
            auto_rebalance = j.at("auto_rebalance").get<bool>();
        if (j.contains("rebalance_period"))
            rebalance_period = j.at("rebalance_period").get<int>();
        if (j.contains("version"))
            version = j.at("version").get<std::string>();
    }
};

/**
 * @brief Backtest configuration for simulation
 */
struct BacktestConfig : public ConfigBase {
    PortfolioBacktestConfig portfolio_config;
    StrategyBacktestConfig strategy_config;
    std::string results_db_schema = "backtest";
    bool store_trade_details = true;
    std::string csv_output_path = "apps/backtest/results";

    // Configuration metadata
    std::string version{"1.0.0"};

    // JSON serialization
    nlohmann::json to_json() const override {
        nlohmann::json j;
        j["strategy_config"] = strategy_config.to_json();
        j["portfolio_config"] = portfolio_config.to_json();
        j["results_db_schema"] = results_db_schema;
        j["store_trade_details"] = store_trade_details;
        j["version"] = version;
        return j;
    }

    void from_json(const nlohmann::json& j) override {
        if (j.contains("strategy_config"))
            strategy_config.from_json(j.at("strategy_config"));
        if (j.contains("portfolio_config"))
            portfolio_config.from_json(j.at("portfolio_config"));
        if (j.contains("results_db_schema"))
            results_db_schema = j.at("results_db_schema").get<std::string>();
        if (j.contains("store_trade_details"))
            store_trade_details = j.at("store_trade_details").get<bool>();
        if (j.contains("version"))
            version = j.at("version").get<std::string>();
    }
};

/**
 * @brief Strategy backtest results
 */
struct BacktestResults {
    // Performance metrics
    double total_return{0.0};
    double volatility{0.0};
    double sharpe_ratio{0.0};
    double sortino_ratio{0.0};
    double max_drawdown{0.0};
    double calmar_ratio{0.0};

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
    std::vector<ExecutionReport> executions;  // All position changes (8000 number)
    std::vector<ExecutionReport> actual_trades;  // Only actual trades that close positions (2000 number)
    std::vector<Position> positions;
    std::vector<std::pair<Timestamp, double>> equity_curve;
    std::vector<std::pair<Timestamp, double>> drawdown_curve;

    // Additional analysis
    std::unordered_map<std::string, double> monthly_returns;
    std::unordered_map<std::string, double> symbol_pnl;
    std::vector<std::pair<Timestamp, RiskResult>> risk_metrics;
    
    // Strategy signals collected during backtest
    std::map<std::pair<Timestamp, std::string>, double> signals;  // (timestamp, symbol) -> signal
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
     * @brief Destructor
     */
    ~BacktestEngine();

    /**
     * @brief Run backtest simulation for a single strategy with portfolio-level constraints
     * @param strategy Strategy to test
     * @return Result containing backtest results
     */
    Result<BacktestResults> run(std::shared_ptr<StrategyInterface> strategy);

    /**
     * @brief Run portfolio backtest simulation
     * @param portfolio Portfolio manager with multiple strategies
     * @return Result containing backtest results
     */
    Result<BacktestResults> run_portfolio(std::shared_ptr<PortfolioManager> portfolio);

    /**
     * @brief Save backtest results to database
     * @param results Results to save
     * @param strategy_id Strategy identifier (used for run_id generation if run_id is empty)
     * @param run_id Optional identifier for this run
     * @return Result indicating success or failure
     */
    Result<void> save_results_to_db(const BacktestResults& results,
                                    const std::string& strategy_id = "TREND_FOLLOWING",
                                    const std::string& run_id = "") const;

    // Multi-strategy version: save portfolio-level results with per-strategy attribution
    Result<void> save_portfolio_results_to_db(
        const BacktestResults& results,
        const std::vector<std::string>& strategy_names,
        const std::unordered_map<std::string, double>& strategy_allocations,
        std::shared_ptr<PortfolioManager> portfolio,
        const nlohmann::json& portfolio_config) const;

    /**
     * @brief Save backtest results to CSV
     * @param results Results to save
     * @param filename Output file name
     * @return Result indicating success or failure
     */
    Result<void> save_results_to_csv(const BacktestResults& results,
                                     const std::string& run_id) const;

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
    std::string backtest_component_id_;
    mutable std::string current_run_id_;  // Store run_id for daily position storage

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
    Result<void> process_bar(const std::vector<Bar>& bars,
                             std::shared_ptr<StrategyInterface> strategy,
                             std::map<std::string, Position>& current_positions,
                             std::vector<std::pair<Timestamp, double>>& equity_curve,
                             std::vector<RiskResult>& risk_metrics);

    /**
     * @brief Apply portfolio-level constraints (risk management and optimization)
     * @param current_positions Current portfolio positions
     * @param equity_curve Equity curve points
     * @param risk_metrics Risk metrics to update
     * @return Result indicating success or failure
     */
    Result<void> apply_portfolio_constraints(
        const std::vector<Bar>& bars, std::map<std::string, Position>& current_positions,
        std::vector<std::pair<Timestamp, double>>& equity_curve,
        std::vector<RiskResult>& risk_metrics);

    /**
     * @brief Helper methods for portfolio backtesting
     * @param strategy_positions Vector of strategy positions
     * @param portfolio_positions Combined portfolio positions
     */
    void combine_positions(
        const std::vector<std::map<std::string, Position>>& strategy_positions,
        std::map<std::string, Position>& portfolio_positions);

    /**
     * @brief Redistribute positions based on strategy weights
     * @param portfolio_positions Current portfolio positions
     * @param strategy_positions Vector of strategy positions
     * @param strategies Vector of strategy instances
     */
    void redistribute_positions(
        const std::map<std::string, Position>& portfolio_positions,
        std::vector<std::map<std::string, Position>>& strategy_positions,
        const std::vector<std::shared_ptr<StrategyInterface>>& strategies);

    /**
     * @brief Process portfolio data for a single time step
     * @param timestamp Current timestamp
     * @param bars Market data bars for this period
     * @param portfolio Portfolio manager
     * @param executions Vector to store generated executions
     * @param equity_curve Vector to store equity curve points
     * @param risk_metrics Vector to store risk metrics
     * @return Result indicating success or failure
     */
    Result<void> process_portfolio_data(const Timestamp& timestamp, const std::vector<Bar>& bars,
                                        std::shared_ptr<PortfolioManager> portfolio,
                                        std::vector<ExecutionReport>& executions,
                                        std::vector<std::pair<Timestamp, double>>& equity_curve,
                                        std::vector<RiskResult>& risk_metrics);

    /**
     * @brief Process strategy signals and generate executions
     * @param bars Market data bars
     * @param strategy Strategy being tested
     * @param current_positions Current portfolio positions
     * @param executions Vector to store generated executions
     * @param equity_curve Equity curve to update
     * @param signals Map to store strategy signals with timestamps
     * @return Result indicating success or failure
     */
    Result<void> process_strategy_signals(
        const std::vector<Bar>& bars, std::shared_ptr<StrategyInterface> strategy,
        std::map<std::string, Position>& current_positions,
        std::vector<ExecutionReport>& executions,
        std::vector<std::pair<Timestamp, double>>& equity_curve,
        std::map<std::pair<Timestamp, std::string>, double>& signals);

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
    BacktestResults calculate_metrics(const std::vector<std::pair<Timestamp, double>>& equity_curve,
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
     * @param ts Timestamp to format
     * @return Formatted string
     */
    std::string format_timestamp(const Timestamp& ts) const;
};

}  // namespace backtest
}  // namespace trade_ngin