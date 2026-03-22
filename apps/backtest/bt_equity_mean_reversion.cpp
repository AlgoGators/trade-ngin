#include <fstream>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include "trade_ngin/backtest/backtest_coordinator.hpp"
#include "trade_ngin/core/config_loader.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/data/database_pooling.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/instruments/equity.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/strategy/mean_reversion.hpp"

using namespace trade_ngin;
using namespace trade_ngin::backtest;

int main() {
    try {
        StateManager::reset_instance();
        Logger::reset_for_tests();

        auto& logger = Logger::instance();
        LoggerConfig logger_config;
        logger_config.min_level = LogLevel::INFO;
        logger_config.destination = LogDestination::BOTH;
        logger_config.log_directory = "logs";
        logger_config.filename_prefix = "bt_equity_mr";
        logger.initialize(logger_config);

        std::atomic_thread_fence(std::memory_order_seq_cst);

        if (!logger.is_initialized()) {
            std::cerr << "ERROR: Logger initialization failed" << std::endl;
            return 1;
        }

        INFO("=== Equity Mean Reversion Backtest ===");

        // ========================================
        // LOAD CONFIGURATION
        // ========================================
        INFO("Loading configuration...");
        auto app_config_result = ConfigLoader::load("./config", "equity_mr");
        if (app_config_result.is_error()) {
            ERROR("Failed to load equity_mr configuration: " +
                  std::string(app_config_result.error()->what()));
            return 1;
        }
        auto app_config = app_config_result.value();
        INFO("Configuration loaded for portfolio: " + app_config.portfolio_id);

        // ========================================
        // SETUP DATABASE CONNECTION
        // ========================================
        INFO("Initializing database connection pool...");
        std::string conn_string = app_config.database.get_connection_string();
        size_t num_connections = app_config.database.num_connections;

        auto pool_result = DatabasePool::instance().initialize(conn_string, num_connections);
        if (pool_result.is_error()) {
            std::cerr << "Failed to initialize connection pool: " << pool_result.error()->what()
                      << std::endl;
            return 1;
        }

        auto db_guard = DatabasePool::instance().acquire_connection();
        auto db = db_guard.get();

        if (!db || !db->is_connected()) {
            std::cerr << "Failed to acquire database connection" << std::endl;
            return 1;
        }
        INFO("Database connection established");

        // ========================================
        // INITIALIZE INSTRUMENT REGISTRY
        // ========================================
        auto& registry = InstrumentRegistry::instance();
        auto init_result = registry.initialize(db);
        if (init_result.is_error()) {
            std::cerr << "Failed to initialize instrument registry: "
                      << init_result.error()->what() << std::endl;
            return 1;
        }

        // Load existing instruments (futures from DB)
        auto load_result = registry.load_instruments();
        if (load_result.is_error()) {
            WARN("Could not load instruments from DB (may not have futures metadata): " +
                 std::string(load_result.error()->what()));
        }

        // ========================================
        // LOAD EQUITY SYMBOLS FROM CONFIG
        // ========================================
        std::vector<std::string> symbols;
        const auto& mr_strategy_def = app_config.strategies_config["MEAN_REVERSION"];
        if (mr_strategy_def.contains("symbols")) {
            for (const auto& sym : mr_strategy_def["symbols"]) {
                symbols.push_back(sym.get<std::string>());
            }
            INFO("Loaded " + std::to_string(symbols.size()) + " symbols from config");
        }
        if (symbols.empty()) {
            WARN("No symbols in strategy config, falling back to database scan (slow)");
            auto symbols_result = db->get_symbols(AssetClass::EQUITIES);
            if (symbols_result.is_error()) {
                ERROR("Failed to get equity symbols: " + std::string(symbols_result.error()->what()));
                return 1;
            }
            symbols = symbols_result.value();
        }

        if (symbols.empty()) {
            ERROR("No equity symbols found");
            return 1;
        }

        // Register equity instruments in the registry
        auto equity_reg_result = registry.load_equity_instruments(symbols);
        if (equity_reg_result.is_error()) {
            ERROR("Failed to register equity instruments: " +
                  std::string(equity_reg_result.error()->what()));
            return 1;
        }

        // Print symbols
        std::cout << "Symbols (" << symbols.size() << "): ";
        for (size_t i = 0; i < std::min(symbols.size(), size_t(10)); ++i) {
            std::cout << symbols[i] << " ";
        }
        if (symbols.size() > 10) std::cout << "...";
        std::cout << std::endl;

        // ========================================
        // CONFIGURE BACKTEST PARAMETERS
        // ========================================
        auto now = std::chrono::system_clock::now();
        auto now_time_t = std::chrono::system_clock::to_time_t(now);
        std::tm* now_tm = std::localtime(&now_time_t);

        std::tm start_tm = *now_tm;
        start_tm.tm_year -= app_config.backtest.lookback_years;
        auto start_time_t = std::mktime(&start_tm);
        Timestamp start_date = std::chrono::system_clock::from_time_t(start_time_t);
        Timestamp end_date = now;

        double initial_capital = app_config.initial_capital;

        INFO("Backtest period: " + std::to_string(app_config.backtest.lookback_years) + " years");
        std::cout << "Initial capital: $" << std::fixed << std::setprecision(0) << initial_capital
                  << std::endl;

        // ========================================
        // INITIALIZE BACKTEST COORDINATOR
        // ========================================
        BacktestCoordinatorConfig coord_config;
        coord_config.initial_capital = initial_capital;
        coord_config.use_risk_management = app_config.strategy_defaults.use_risk_management;
        coord_config.use_optimization = app_config.strategy_defaults.use_optimization;
        coord_config.store_trade_details = app_config.backtest.store_trade_details;
        coord_config.portfolio_id = app_config.portfolio_id;

        auto coordinator = std::make_unique<BacktestCoordinator>(db, &registry, coord_config);

        auto coord_init_result = coordinator->initialize();
        if (coord_init_result.is_error()) {
            ERROR("Failed to initialize backtest coordinator: " +
                  std::string(coord_init_result.error()->what()));
            return 1;
        }

        // ========================================
        // CREATE MEAN REVERSION STRATEGY
        // ========================================
        // Load mean reversion config from strategy definition
        if (!app_config.strategies_config.contains("MEAN_REVERSION")) {
            ERROR("MEAN_REVERSION strategy not found in config");
            return 1;
        }
        const auto& strategy_def = app_config.strategies_config["MEAN_REVERSION"];
        if (!strategy_def.contains("config")) {
            ERROR("MEAN_REVERSION strategy missing 'config' section");
            return 1;
        }
        const auto& mr_cfg = strategy_def["config"];

        MeanReversionConfig mr_config;
        mr_config.lookback_period = mr_cfg.value("lookback_period", 20);
        mr_config.entry_threshold = mr_cfg.value("entry_threshold", 2.0);
        mr_config.exit_threshold = mr_cfg.value("exit_threshold", 0.5);
        mr_config.risk_target = mr_cfg.value("risk_target", 0.15);
        mr_config.position_size = mr_cfg.value("position_size", 0.1);
        mr_config.vol_lookback = mr_cfg.value("vol_lookback", 20);
        mr_config.use_stop_loss = mr_cfg.value("use_stop_loss", true);
        mr_config.stop_loss_pct = mr_cfg.value("stop_loss_pct", 0.05);
        mr_config.allow_fractional_shares = mr_cfg.value("allow_fractional_shares", true);

        StrategyConfig strategy_config;
        strategy_config.asset_classes = {AssetClass::EQUITIES};
        strategy_config.frequencies = {DataFrequency::DAILY};
        strategy_config.capital_allocation = initial_capital;
        strategy_config.max_drawdown = app_config.max_drawdown;
        strategy_config.max_leverage = app_config.max_leverage;

        for (const auto& symbol : symbols) {
            strategy_config.trading_params[symbol] = {};
            strategy_config.position_limits[symbol] = app_config.execution.position_limit_backtest;
        }

        auto registry_ptr = std::shared_ptr<InstrumentRegistry>(&registry, [](InstrumentRegistry*) {});

        auto strategy = std::make_shared<MeanReversionStrategy>(
            "MEAN_REVERSION", strategy_config, mr_config, db, registry_ptr);

        auto strat_init_result = strategy->initialize();
        if (strat_init_result.is_error()) {
            ERROR("Failed to initialize mean reversion strategy: " +
                  std::string(strat_init_result.error()->what()));
            return 1;
        }

        auto strat_start_result = strategy->start();
        if (strat_start_result.is_error()) {
            ERROR("Failed to start mean reversion strategy: " +
                  std::string(strat_start_result.error()->what()));
            return 1;
        }

        INFO("Mean reversion strategy initialized and started");

        // ========================================
        // CREATE PORTFOLIO AND RUN BACKTEST
        // ========================================
        PortfolioConfig portfolio_config;
        portfolio_config.total_capital = Decimal(initial_capital);
        portfolio_config.reserve_capital = Decimal(initial_capital * app_config.reserve_capital_pct);
        portfolio_config.use_optimization = false;
        portfolio_config.use_risk_management = app_config.strategy_defaults.use_risk_management;
        portfolio_config.risk_config = app_config.risk_config;

        auto portfolio = std::make_shared<PortfolioManager>(portfolio_config);

        auto add_result = portfolio->add_strategy(strategy, 1.0, false,
                                                   portfolio_config.use_risk_management);
        if (add_result.is_error()) {
            ERROR("Failed to add strategy to portfolio: " +
                  std::string(add_result.error()->what()));
            return 1;
        }

        INFO("Running equity mean reversion backtest...");
        auto result = coordinator->run_portfolio(
            portfolio, symbols, start_date, end_date,
            AssetClass::EQUITIES, DataFrequency::DAILY);

        if (result.is_error()) {
            std::cerr << "Backtest failed: " << result.error()->what() << std::endl;
            return 1;
        }

        INFO("Backtest completed successfully");

        // ========================================
        // DISPLAY RESULTS
        // ========================================
        const auto& backtest_results = result.value();

        std::cout << "\n======= Equity Mean Reversion Backtest Results =======" << std::endl;
        std::cout << "Total Return: " << std::fixed << std::setprecision(2)
                  << (backtest_results.total_return * 100.0) << "%" << std::endl;
        std::cout << "Sharpe Ratio: " << std::setprecision(3) << backtest_results.sharpe_ratio
                  << std::endl;
        std::cout << "Sortino Ratio: " << backtest_results.sortino_ratio << std::endl;
        std::cout << "Max Drawdown: " << std::setprecision(2)
                  << (backtest_results.max_drawdown * 100.0) << "%" << std::endl;
        std::cout << "Calmar Ratio: " << std::setprecision(3) << backtest_results.calmar_ratio
                  << std::endl;
        std::cout << "Volatility: " << std::setprecision(2)
                  << (backtest_results.volatility * 100.0) << "%" << std::endl;
        std::cout << "Win Rate: " << (backtest_results.win_rate * 100.0) << "%" << std::endl;
        std::cout << "Total Trades: " << backtest_results.total_trades << std::endl;
        std::cout << "====================================================" << std::endl;

        // Save results
        INFO("Saving backtest results to database...");
        try {
            std::vector<std::string> strategy_names = {"MEAN_REVERSION"};
            std::unordered_map<std::string, double> strategy_allocations = {
                {"MEAN_REVERSION", 1.0}};

            nlohmann::json config_json;
            config_json["strategy_type"] = "MeanReversionStrategy";
            config_json["asset_class"] = "EQUITIES";
            config_json["mean_reversion"] = {
                {"lookback_period", mr_config.lookback_period},
                {"entry_threshold", mr_config.entry_threshold},
                {"exit_threshold", mr_config.exit_threshold},
                {"risk_target", mr_config.risk_target},
                {"position_size", mr_config.position_size},
                {"vol_lookback", mr_config.vol_lookback},
                {"allow_fractional_shares", mr_config.allow_fractional_shares}};

            auto save_result = coordinator->save_portfolio_results_to_db(
                backtest_results, strategy_names, strategy_allocations, portfolio, config_json);

            if (save_result.is_error()) {
                WARN("Failed to save results to database: " +
                     std::string(save_result.error()->what()));
            } else {
                INFO("Results saved to database");
            }
        } catch (const std::exception& e) {
            WARN("Exception during database save: " + std::string(e.what()));
        }

        coordinator.reset();
        INFO("Equity mean reversion backtest completed");

        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Unexpected error: " << e.what() << std::endl;
        return 1;
    }
}
