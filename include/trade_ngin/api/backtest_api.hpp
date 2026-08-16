#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/backtest/backtest_coordinator.hpp"
#include "trade_ngin/backtest/backtest_types.hpp"
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/data/market_data_source.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/optimization/dynamic_optimizer.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/risk/risk_manager.hpp"
#include "trade_ngin/strategy/base_strategy.hpp"

namespace trade_ngin {
namespace api {

struct StrategyContext {
    StrategyConfig base_strategy_config;
    std::shared_ptr<PostgresDatabase> db;
    std::shared_ptr<InstrumentRegistry> registry;
};

using StrategyFactory =
    std::function<std::shared_ptr<BaseStrategy>(const StrategyContext&, const nlohmann::json&)>;

/**
 * @brief One strategy to run, with its allocation and configuration
 *
 * Replaces the "strategies" block of the on-disk portfolio config. The
 * strategy_id must match an id passed to BacktestRunner::register_strategy().
 */
struct StrategySpec {
    std::string strategy_id;
    double allocation{1.0};
    nlohmann::json config = nlohmann::json::object();
};

/**
 * @brief Complete, self-contained description of a backtest run
 *
 * Supplying this to BacktestRunner makes the run argument-driven instead of
 * config-file-driven: neither the config/portfolios/<name> tree nor a
 * PostgreSQL connection is required. Anything left unset falls back to the
 * same defaults the shipped config templates use.
 */
struct BacktestRunConfig {
    // ---- Universe and data ----
    std::vector<std::string> symbols;
    Timestamp start_date{};
    Timestamp end_date{};
    AssetClass asset_class{AssetClass::FUTURES};
    DataFrequency data_freq{DataFrequency::DAILY};

    /// Source of historical bars. Required for a database-free run.
    std::shared_ptr<MarketDataSource> data_source;

    // ---- Capital and risk ----
    double initial_capital{500000.0};
    double reserve_capital_pct{0.10};
    double max_drawdown{0.4};
    double max_leverage{4.0};
    double position_limit{1000.0};
    double max_strategy_allocation{1.0};
    double min_strategy_allocation{0.0};
    bool use_risk_management{false};
    bool use_optimization{false};

    /// Optional overrides. Defaults are used when unset.
    std::optional<RiskConfig> risk_config;
    std::optional<DynamicOptConfig> opt_config;

    // ---- Strategies ----
    /// Strategies to run. Allocations are normalized to sum to 1.0.
    std::vector<StrategySpec> strategies;

    // ---- Output ----
    std::string portfolio_id{"BASE_PORTFOLIO"};
    /// Persist results to the database. Forced off without a connection.
    bool store_results{false};
    /// Write per-day CSV output. The default path is relative to the cwd.
    bool export_csv{false};
    std::string csv_output_path{"apps/backtest/results"};
};

class BacktestRunner {
public:
    /**
     * @brief Initialize a config-file-driven run
     * @param portfolio_name Portfolio directory under ./config/portfolios/
     *
     * Requires ./config/portfolios/<portfolio_name> and a reachable database.
     */
    void initialize(std::string portfolio_name);

    /**
     * @brief Initialize a fully argument-driven run
     * @param portfolio_name Name used for log files only
     * @param config Complete run description
     *
     * Neither the on-disk config tree nor a database is consulted when a
     * data source is supplied on @p config.
     */
    void initialize_with_config(std::string portfolio_name, BacktestRunConfig config);

    /**
     * @brief Supply the market data source for a database-free run
     */
    void set_data_source(std::shared_ptr<MarketDataSource> source);

    Result<backtest::BacktestResults> run_backtest();

    Result<void> register_strategy(const std::string& strategy_id, StrategyFactory factory);

private:
    /// Argument-driven path. Used when run_config_ is set.
    Result<backtest::BacktestResults> run_backtest_from_config();

    /// Engine driver for the argument-driven path. The legacy config-file
    /// path in run_backtest() still has its own inline copy of this flow.
    Result<backtest::BacktestResults> execute(
        const std::vector<std::string>& symbols, const Timestamp& start_date,
        const Timestamp& end_date, AssetClass asset_class, DataFrequency data_freq,
        const std::vector<StrategySpec>& strategies, const StrategyConfig& base_strategy_config,
        const PortfolioConfig& portfolio_config,
        const backtest::BacktestCoordinatorConfig& coord_config,
        std::shared_ptr<PostgresDatabase> db, std::shared_ptr<MarketDataSource> data_source,
        std::shared_ptr<InstrumentRegistry> registry);

    std::unordered_map<std::string, StrategyFactory> registered_strategies_;
    std::string portfolio_name_;
    std::optional<BacktestRunConfig> run_config_;
};

}  // namespace api
}  // namespace trade_ngin
