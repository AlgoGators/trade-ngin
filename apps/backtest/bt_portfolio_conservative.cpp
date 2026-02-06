#include <fstream>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include "trade_ngin/backtest/backtest_coordinator.hpp"
#include "trade_ngin/backtest/transaction_cost_analysis.hpp"
#include "trade_ngin/core/config_loader.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/run_id_generator.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/data/database_pooling.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/strategy/trend_following.hpp"
#include "trade_ngin/strategy/trend_following_fast.hpp"

using namespace trade_ngin;
using namespace trade_ngin::backtest;

int main() {
    try {
        // Reset all singletons to ensure clean state between runs
        StateManager::reset_instance();
        Logger::reset_for_tests();

        // Initialize logger
        auto& logger = Logger::instance();
        LoggerConfig logger_config;
        logger_config.min_level = LogLevel::DEBUG;
        logger_config.destination = LogDestination::BOTH;
        logger_config.log_directory = "logs";
        logger_config.filename_prefix = "bt_portfolio_conservative";
        logger.initialize(logger_config);

        std::atomic_thread_fence(std::memory_order_seq_cst);

        if (!logger.is_initialized()) {
            std::cerr << "ERROR: Logger initialization failed" << std::endl;
            return 1;
        }

        INFO("Logger initialized successfully");

        // ========================================
        // LOAD CONFIGURATION FROM MODULAR CONFIG FILES
        // ========================================
        INFO("Loading configuration from config/portfolios/conservative...");
        auto app_config_result = ConfigLoader::load("./config", "conservative");
        if (app_config_result.is_error()) {
            ERROR("Failed to load configuration: " + std::string(app_config_result.error()->what()));
            std::cerr << "Failed to load configuration: " << app_config_result.error()->what()
                      << std::endl;
            return 1;
        }
        auto app_config = app_config_result.value();
        INFO("Configuration loaded successfully for portfolio: " + app_config.portfolio_id);

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
        INFO("Database connection pool initialized with " + std::to_string(num_connections) +
             " connections");

        // Get a database connection from the pool
        auto db_guard = DatabasePool::instance().acquire_connection();
        auto db = db_guard.get();

        if (!db || !db->is_connected()) {
            std::cerr << "Failed to acquire database connection from pool" << std::endl;
            return 1;
        }
        INFO("Successfully acquired database connection from pool");

        // Initialize instrument registry
        INFO("Initializing instrument registry...");
        auto& registry = InstrumentRegistry::instance();

        auto instrument_registry_init_result = registry.initialize(db);
        if (instrument_registry_init_result.is_error()) {
            std::cerr << "Failed to initialize instrument registry: "
                      << instrument_registry_init_result.error()->what() << std::endl;
            return 1;
        }

        // Load futures instruments
        auto load_result = registry.load_instruments();
        if (load_result.is_error() || registry.get_all_instruments().empty()) {
            std::cerr << "Failed to load futures instruments: " << load_result.error()->what()
                      << std::endl;
            ERROR("Failed to load futures instruments: " +
                  std::string(load_result.error()->what()));
            return 1;
        } else {
            INFO("Successfully loaded futures instruments from database");
        }

        // After loading instruments
        DEBUG("Verifying instrument registry contents");
        auto all_instruments = registry.get_all_instruments();
        INFO("Registry contains " + std::to_string(all_instruments.size()) + " instruments");

        // ========================================
        // CONFIGURE BACKTEST PARAMETERS
        // ========================================
        trade_ngin::backtest::BacktestConfig config;

        // Set portfolio_id from loaded config
        config.portfolio_id = app_config.portfolio_id;

        // Convert timestamps to proper format
        auto now = std::chrono::system_clock::now();
        auto now_time_t = std::chrono::system_clock::to_time_t(now);
        std::tm* now_tm = std::localtime(&now_time_t);

        // Set start date based on lookback_years from config
        std::tm start_tm = *now_tm;
        start_tm.tm_year -= app_config.backtest.lookback_years;
        auto start_time_t = std::mktime(&start_tm);
        config.strategy_config.start_date = std::chrono::system_clock::from_time_t(start_time_t);

        // Set end date to today
        config.strategy_config.end_date = now;

        config.strategy_config.asset_class = trade_ngin::AssetClass::FUTURES;
        config.strategy_config.data_freq = trade_ngin::DataFrequency::DAILY;
        config.store_trade_details = app_config.backtest.store_trade_details;

        // Load symbols from database
        auto symbols_result = db->get_symbols(trade_ngin::AssetClass::FUTURES);
        auto symbols = symbols_result.value();

        if (symbols_result.is_ok()) {
            for (const auto& symbol : symbols) {
                if (symbol.find(".c.0") != std::string::npos ||
                    symbol.find("MES.c.0") != std::string::npos ||
                    symbol.find("ES.v.0") != std::string::npos) {
                    symbols.erase(std::remove(symbols.begin(), symbols.end(), symbol),
                                  symbols.end());
                }
            }
            config.strategy_config.symbols = symbols;
        } else {
            ERROR("Failed to get symbols: " + std::string(symbols_result.error()->what()));
            throw std::runtime_error("Failed to get symbols: " +
                                     symbols_result.error()->to_string());
        }

        std::cout << "Symbols: ";
        for (const auto& symbol : config.strategy_config.symbols) {
            std::cout << symbol << " ";
        }
        std::cout << std::endl;

        // ========================================
        // APPLY CONFIG VALUES TO BACKTEST CONFIG
        // ========================================
        config.portfolio_config.initial_capital = app_config.initial_capital;
        config.portfolio_config.use_risk_management = app_config.strategy_defaults.use_risk_management;
        config.portfolio_config.use_optimization = app_config.strategy_defaults.use_optimization;
        config.strategy_config.initial_capital = config.portfolio_config.initial_capital;

        std::cout << "Retrieved " << config.strategy_config.symbols.size() << " symbols"
                  << std::endl;
        std::cout << "Initial capital: $" << config.portfolio_config.initial_capital
                  << " (CONSERVATIVE)" << std::endl;

        INFO("Configuration loaded successfully. Testing " +
             std::to_string(config.strategy_config.symbols.size()) + " symbols from " +
             std::to_string(
                 std::chrono::system_clock::to_time_t(config.strategy_config.start_date)) +
             " to " +
             std::to_string(std::chrono::system_clock::to_time_t(config.strategy_config.end_date)));

        // Apply risk configuration from loaded config
        config.portfolio_config.risk_config = app_config.risk_config;
        config.portfolio_config.risk_config.capital = config.portfolio_config.initial_capital;

        // Apply optimization configuration from loaded config
        config.portfolio_config.opt_config = app_config.opt_config;
        config.portfolio_config.opt_config.capital =
            config.portfolio_config.initial_capital.as_double();

        // ========================================
        // INITIALIZE BACKTEST COORDINATOR
        // ========================================
        std::cerr << "Before BacktestCoordinator: initialized="
                  << Logger::instance().is_initialized() << std::endl;
        INFO("Initializing backtest coordinator...");

        trade_ngin::backtest::BacktestCoordinatorConfig coord_config;
        coord_config.initial_capital = static_cast<double>(config.portfolio_config.initial_capital);
        coord_config.use_risk_management = config.portfolio_config.use_risk_management;
        coord_config.use_optimization = config.portfolio_config.use_optimization;
        coord_config.store_trade_details = config.store_trade_details;
        coord_config.portfolio_id = config.portfolio_id;

        auto coordinator = std::make_unique<trade_ngin::backtest::BacktestCoordinator>(
            db, &registry, coord_config);

        std::cerr << "After BacktestCoordinator: initialized="
                  << Logger::instance().is_initialized() << std::endl;

        // ========================================
        // SETUP PORTFOLIO CONFIGURATION
        // ========================================
        trade_ngin::PortfolioConfig portfolio_config;
        portfolio_config.total_capital = config.portfolio_config.initial_capital;
        portfolio_config.reserve_capital =
            config.portfolio_config.initial_capital * app_config.reserve_capital_pct;
        portfolio_config.max_strategy_allocation = app_config.strategy_defaults.max_strategy_allocation;
        portfolio_config.min_strategy_allocation = app_config.strategy_defaults.min_strategy_allocation;
        portfolio_config.use_optimization = app_config.strategy_defaults.use_optimization;
        portfolio_config.use_risk_management = app_config.strategy_defaults.use_risk_management;
        portfolio_config.opt_config = config.portfolio_config.opt_config;
        portfolio_config.risk_config = config.portfolio_config.risk_config;

        // ========================================
        // LOAD STRATEGIES FROM CONFIG
        // ========================================
        std::vector<std::shared_ptr<trade_ngin::StrategyInterface>> strategies;
        std::vector<std::string> strategy_names;
        std::unordered_map<std::string, double> strategy_allocations;
        std::unordered_map<std::string, nlohmann::json> strategy_configs_map;

        // Use strategies from loaded config
        auto& strategies_config = app_config.strategies_config;
        if (strategies_config.empty()) {
            ERROR("No strategies found in configuration");
            return 1;
        }

        // Load default allocations from config
        for (const auto& [strategy_id, strategy_def] : strategies_config.items()) {
            if (strategy_def.contains("enabled_backtest") &&
                strategy_def["enabled_backtest"].get<bool>()) {
                double default_allocation = strategy_def.value("default_allocation", 0.5);
                strategy_allocations[strategy_id] = default_allocation;
                strategy_configs_map[strategy_id] = strategy_def;
                strategy_names.push_back(strategy_id);
            }
        }

        if (strategy_names.empty()) {
            ERROR("No enabled strategies found in configuration for backtest");
            return 1;
        }

        // Normalize allocations to sum to 1.0
        double total_allocation = 0.0;
        for (const auto& [_, alloc] : strategy_allocations) {
            total_allocation += alloc;
        }
        if (total_allocation > 0.0) {
            for (auto& [_, alloc] : strategy_allocations) {
                alloc /= total_allocation;
            }
        }

        INFO("Loading " + std::to_string(strategy_names.size()) +
             " strategies from conservative config");

        // Create a shared_ptr that doesn't own the singleton registry
        auto registry_ptr =
            std::shared_ptr<InstrumentRegistry>(&registry, [](InstrumentRegistry*) {});

        // Create base strategy configuration using loaded config values
        trade_ngin::StrategyConfig base_strategy_config;
        base_strategy_config.asset_classes = {trade_ngin::AssetClass::FUTURES};
        base_strategy_config.frequencies = {config.strategy_config.data_freq};
        base_strategy_config.max_drawdown = app_config.max_drawdown;
        base_strategy_config.max_leverage = app_config.max_leverage;

        // Add position limits from config
        for (const auto& symbol : config.strategy_config.symbols) {
            base_strategy_config.position_limits[symbol] = app_config.execution.position_limit_backtest;
        }

        // Create and initialize each strategy
        for (const auto& strategy_id : strategy_names) {
            const auto& strategy_def = strategy_configs_map[strategy_id];
            std::string strategy_type = strategy_def.value("type", "");

            double allocation = strategy_allocations[strategy_id];
            base_strategy_config.capital_allocation =
                config.portfolio_config.initial_capital.as_double() * allocation;

            INFO("Creating strategy: " + strategy_id + " (type: " + strategy_type +
                 ", allocation: " + std::to_string(allocation * 100.0) + "%)");

            std::shared_ptr<trade_ngin::StrategyInterface> strategy;

            if (strategy_type == "TrendFollowingStrategy") {
                trade_ngin::TrendFollowingConfig trend_config;
                if (strategy_def.contains("config")) {
                    const auto& cfg = strategy_def["config"];
                    trend_config.weight = cfg.value("weight", 0.03);
                    trend_config.risk_target = cfg.value("risk_target", 0.15);  // Conservative default
                    trend_config.idm = cfg.value("idm", 2.5);
                    trend_config.use_position_buffering = cfg.value("use_position_buffering", true);
                    if (cfg.contains("ema_windows")) {
                        trend_config.ema_windows.clear();
                        for (const auto& window : cfg["ema_windows"]) {
                            trend_config.ema_windows.push_back(
                                {window[0].get<int>(), window[1].get<int>()});
                        }
                    }
                    trend_config.vol_lookback_short = cfg.value("vol_lookback_short", 32);
                    trend_config.vol_lookback_long = cfg.value("vol_lookback_long", 252);
                }
                // Set FDM from strategy_defaults
                if (trend_config.fdm.empty()) {
                    trend_config.fdm = app_config.strategy_defaults.fdm;
                }

                strategy = std::make_shared<trade_ngin::TrendFollowingStrategy>(
                    strategy_id, base_strategy_config, trend_config, db, registry_ptr);

            } else if (strategy_type == "TrendFollowingFastStrategy") {
                trade_ngin::TrendFollowingFastConfig trend_config;
                if (strategy_def.contains("config")) {
                    const auto& cfg = strategy_def["config"];
                    trend_config.weight = cfg.value("weight", 0.03);
                    trend_config.risk_target = cfg.value("risk_target", 0.20);  // Conservative default
                    trend_config.idm = cfg.value("idm", 2.5);
                    trend_config.use_position_buffering =
                        cfg.value("use_position_buffering", false);
                    if (cfg.contains("ema_windows")) {
                        trend_config.ema_windows.clear();
                        for (const auto& window : cfg["ema_windows"]) {
                            trend_config.ema_windows.push_back(
                                {window[0].get<int>(), window[1].get<int>()});
                        }
                    }
                    trend_config.vol_lookback_short = cfg.value("vol_lookback_short", 16);
                    trend_config.vol_lookback_long = cfg.value("vol_lookback_long", 252);
                }
                // Set FDM from strategy_defaults
                if (trend_config.fdm.empty()) {
                    trend_config.fdm = app_config.strategy_defaults.fdm;
                }

                strategy = std::make_shared<trade_ngin::TrendFollowingFastStrategy>(
                    strategy_id, base_strategy_config, trend_config, db, registry_ptr);

            } else {
                ERROR("Unknown strategy type: " + strategy_type + " for strategy: " + strategy_id);
                return 1;
            }

            // Initialize strategy
            auto init_result = strategy->initialize();
            if (init_result.is_error()) {
                ERROR("Failed to initialize strategy " + strategy_id + ": " +
                      init_result.error()->what());
                return 1;
            }

            // Start strategy
            auto start_result = strategy->start();
            if (start_result.is_error()) {
                ERROR("Failed to start strategy " + strategy_id + ": " +
                      start_result.error()->what());
                return 1;
            }

            strategies.push_back(strategy);
            INFO("Successfully initialized and started strategy: " + strategy_id);
        }

        // ========================================
        // CREATE PORTFOLIO AND RUN BACKTEST
        // ========================================
        INFO("Creating portfolio manager with " + std::to_string(strategies.size()) +
             " strategies...");
        auto portfolio = std::make_shared<trade_ngin::PortfolioManager>(portfolio_config);

        for (size_t i = 0; i < strategies.size(); ++i) {
            const auto& strategy = strategies[i];
            const std::string& strategy_id = strategy_names[i];
            double allocation = strategy_allocations[strategy_id];

            auto add_result = portfolio->add_strategy(strategy, allocation,
                                                      config.portfolio_config.use_optimization,
                                                      config.portfolio_config.use_risk_management);

            if (add_result.is_error()) {
                ERROR("Failed to add strategy " + strategy_id +
                      " to portfolio: " + add_result.error()->what());
                return 1;
            }

            INFO("Added strategy " + strategy_id + " with allocation " +
                 std::to_string(allocation * 100.0) + "%");
        }

        // Run the backtest
        INFO("Running conservative portfolio backtest for time period: " +
             std::to_string(
                 std::chrono::system_clock::to_time_t(config.strategy_config.start_date)) +
             " to " +
             std::to_string(std::chrono::system_clock::to_time_t(config.strategy_config.end_date)));

        auto result = coordinator->run_portfolio(
            portfolio, config.strategy_config.symbols, config.strategy_config.start_date,
            config.strategy_config.end_date, config.strategy_config.asset_class,
            config.strategy_config.data_freq);

        if (result.is_error()) {
            std::cerr << "Backtest failed: " << result.error()->what() << std::endl;
            std::cerr << "Error code: " << static_cast<int>(result.error()->code()) << std::endl;
            return 1;
        }

        INFO("Backtest completed successfully");

        // Analyze and display results
        const auto& backtest_results = result.value();

        INFO("Analyzing performance metrics...");

        std::cout << "======= Conservative Portfolio Backtest Results =======" << std::endl;
        std::cout << "Total Return: " << (backtest_results.total_return * 100.0) << "%"
                  << std::endl;
        std::cout << "Sharpe Ratio: " << backtest_results.sharpe_ratio << std::endl;
        std::cout << "Sortino Ratio: " << backtest_results.sortino_ratio << std::endl;
        std::cout << "Max Drawdown: " << (backtest_results.max_drawdown * 100.0) << "%"
                  << std::endl;
        std::cout << "Calmar Ratio: " << backtest_results.calmar_ratio << std::endl;
        std::cout << "Volatility: " << (backtest_results.volatility * 100.0) << "%" << std::endl;
        std::cout << "Win Rate: " << (backtest_results.win_rate * 100.0) << "%" << std::endl;
        std::cout << "Total Trades: " << backtest_results.total_trades << std::endl;

        // Save portfolio results to database
        INFO("Saving conservative portfolio backtest results to database...");
        try {
            nlohmann::json portfolio_config_json = portfolio_config.to_json();
            portfolio_config_json["strategy_allocations"] = strategy_allocations;
            portfolio_config_json["strategy_names"] = strategy_names;

            auto save_result = coordinator->save_portfolio_results_to_db(
                backtest_results, strategy_names, strategy_allocations, portfolio,
                portfolio_config_json);

            if (save_result.is_error()) {
                std::cerr << "Failed to save portfolio backtest results to database: "
                          << save_result.error()->what() << std::endl;
                ERROR("Failed to save portfolio backtest results to database: " +
                      std::string(save_result.error()->what()));
            } else {
                INFO("Successfully saved conservative portfolio backtest results to database");
            }
        } catch (const std::exception& e) {
            std::cerr << "Exception during database save: " << e.what() << std::endl;
            ERROR("Exception during database save: " + std::string(e.what()));
        }

        // Cleanup
        INFO("Cleaning up backtest coordinator...");
        coordinator.reset();

        INFO("Conservative portfolio backtest application completed successfully");

        std::cerr << "At end of main: initialized=" << Logger::instance().is_initialized()
                  << std::endl;

        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Unexpected error: " << e.what() << std::endl;
        ERROR("Unexpected error: " + std::string(e.what()));
        return 1;
    } catch (...) {
        std::cerr << "Unknown error occurred" << std::endl;
        ERROR("Unknown error occurred");
        return 1;
    }
}