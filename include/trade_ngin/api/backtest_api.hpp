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
#include "trade_ngin/risk/risk_module_config.hpp"
#include "trade_ngin/strategy/base_strategy.hpp"

namespace trade_ngin {
namespace api {

struct StrategyContext {
    StrategyConfig base_strategy_config;
    std::shared_ptr<PostgresDatabase> db;
    std::shared_ptr<InstrumentRegistry> registry;
    /// The run's market data source (null on a database run). A trend sleeve takes its
    /// instrument weights from it when it has no database.
    std::shared_ptr<MarketDataSource> data_source;
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
    double max_drawdown{0.4};
    double max_leverage{4.0};
    double position_limit{1000.0};
    double max_strategy_allocation{1.0};
    double min_strategy_allocation{0.0};
    bool use_optimization{false};

    /// K, the sessions a year the run's statistics are annualised with. There is no
    /// engine default: a run without a positive value is refused. 252 suits daily bars.
    double statistics_sessions_per_year{252.0};

    /// The portfolio's risk modules. The engine has no default: a book assigns a module, or
    /// states that it runs none, with who ruled so and when. Leave this empty and fill the
    /// three no_risk_* fields instead to state the latter (the Python wrapper does).
    std::vector<RiskModuleConfig> risk_modules;
    std::string no_risk_reason;    ///< why the run has no risk layer
    std::string no_risk_ruled_by;  ///< who ruled so
    std::string no_risk_ruled_on;  ///< YYYY-MM-DD

    /// Optional overrides. Defaults are used when unset.
    std::optional<RiskConfig> risk_config;
    std::optional<DynamicOptConfig> opt_config;

    // ---- Strategies ----
    /// Strategies to run. Allocations are normalized to sum to 1.0.
    std::vector<StrategySpec> strategies;

    // ---- A book's own configuration ----
    /// A config directory in the layout the engine's runners read (<base>/defaults.json and
    /// <base>/portfolios/<portfolio_name>/portfolio.json, risk.json, ...), the portfolio being the
    /// name given to initialize_with_config. When set, the book decides everything its files
    /// decide in apps/backtest/bt_portfolio.cpp (capital, sleeves and their allocations and keys,
    /// risk modules and the overlay's limits, the loop's keys, the equity slow rule, the trading
    /// rule removals, the statistics' K, the position limit), loaded by the same ConfigLoader and
    /// assembled by the same steps, so the run equals the engine's own on the same bars. The
    /// fields above that the book decides are then ignored; symbols, dates and the data source
    /// are still the caller's.
    std::string config_base_path;

    // ---- Output ----
    std::string portfolio_id{"BASE_PORTFOLIO"};
    /// Persist results to the database. Forced off without a connection.
    bool store_results{false};
    /// Write per-day CSV output. The default path is relative to the cwd.
    bool export_csv{false};
    std::string csv_output_path{"apps/backtest/results"};
};

/**
 * @brief The engine's own trend-following sleeve as a strategy factory
 *
 * Builds TrendFollowingStrategy exactly as the engine's runners do (apps/backtest/bt_portfolio.cpp),
 * from the same `config` keys: risk_target, idm and vol_lookback_short are required (no default),
 * ema_windows and vol_lookback_long (252) are optional. `fast` starts from the FAST sleeve's
 * configuration. The run's capital, symbols and limits come from the StrategyContext.
 *
 * Two further optional keys carry what the runners take from portfolio.json for the book's first
 * sleeve: "equity_slow": {"symbols": [...], "pairs": [[a, b], ...]} and
 * "rule_removals": {"ZR": [[a, b], ...]} (the pairs a contract does not run).
 */
StrategyFactory make_trend_following_factory(std::string strategy_id, bool fast = false);

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
        PortfolioConfig portfolio_config,
        const backtest::BacktestCoordinatorConfig& coord_config,
        std::shared_ptr<PostgresDatabase> db, std::shared_ptr<MarketDataSource> data_source,
        std::shared_ptr<InstrumentRegistry> registry);

    std::unordered_map<std::string, StrategyFactory> registered_strategies_;
    std::string portfolio_name_;
    std::optional<BacktestRunConfig> run_config_;
};

}  // namespace api
}  // namespace trade_ngin
