#include <fstream>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include "trade_ngin/backtest/backtest_coordinator.hpp"
#include "trade_ngin/backtest/transaction_cost_analysis.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/run_id_generator.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/data/credential_store.hpp"
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
        logger_config.filename_prefix = "bt_portfolio";

        // Determine config file name (default to config.json)
        std::string config_filename = "./config.json";
        logger.initialize(logger_config);

        std::atomic_thread_fence(std::memory_order_seq_cst);

        if (!logger.is_initialized()) {
            std::cerr << "ERROR: Logger initialization failed" << std::endl;
            return 1;
        }

        INFO("Logger initialized successfully");

        std::cerr << "After Logger initialization: initialized="
                  << Logger::instance().is_initialized() << std::endl;

        // Setup database connection pool
        INFO("Initializing database connection pool...");
        auto credentials = std::make_shared<trade_ngin::CredentialStore>(config_filename);

        auto username_result = credentials->get<std::string>("database", "username");
        if (username_result.is_error()) {
            std::cerr << "Failed to get username: " << username_result.error()->what() << std::endl;
            return 1;
        }
        std::string username = username_result.value();

        auto password_result = credentials->get<std::string>("database", "password");
        if (password_result.is_error()) {
            std::cerr << "Failed to get password: " << password_result.error()->what() << std::endl;
            return 1;
        }
        std::string password = password_result.value();

        auto host_result = credentials->get<std::string>("database", "host");
        if (host_result.is_error()) {
            std::cerr << "Failed to get host: " << host_result.error()->what() << std::endl;
            return 1;
        }
        std::string host = host_result.value();

        auto port_result = credentials->get<std::string>("database", "port");
        if (port_result.is_error()) {
            std::cerr << "Failed to get port: " << port_result.error()->what() << std::endl;
            return 1;
        }
        std::string port = port_result.value();

        auto db_name_result = credentials->get<std::string>("database", "name");
        if (db_name_result.is_error()) {
            std::cerr << "Failed to get database name: " << db_name_result.error()->what()
                      << std::endl;
            return 1;
        }
        std::string db_name = db_name_result.value();

        std::string conn_string =
            "postgresql://" + username + ":" + password + "@" + host + ":" + port + "/" + db_name;

        // Initialize only the connection pool with sufficient connections
        size_t num_connections = 5;
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

        // Configure backtest parameters
        INFO("Loading configuration...");

        // Load portfolio configuration from config.json FIRST (before creating engine)
        std::ifstream config_file(config_filename);
        if (!config_file.is_open()) {
            ERROR("Failed to open config.json");
            return 1;
        }
        nlohmann::json config_json;
        try {
            config_file >> config_json;
        } catch (const std::exception& e) {
            ERROR("Failed to parse config.json: " + std::string(e.what()));
            return 1;
        }

        // Read portfolio_id from config (default to BASE_PORTFOLIO)
        std::string portfolio_id = "BASE_PORTFOLIO";
        if (config_json.contains("portfolio_id")) {
            portfolio_id = config_json["portfolio_id"].get<std::string>();
        } else if (config_json.contains("portfolio") &&
                   config_json["portfolio"].contains("portfolio_id")) {
            portfolio_id = config_json["portfolio"]["portfolio_id"].get<std::string>();
        }
        INFO("Using portfolio_id: " + portfolio_id);

        trade_ngin::backtest::BacktestConfig config;

        // Set portfolio_id in backtest config BEFORE creating engine
        config.portfolio_id = portfolio_id;

        // Convert timestamps to proper format
        auto now = std::chrono::system_clock::now();
        auto now_time_t = std::chrono::system_clock::to_time_t(now);
        std::tm* now_tm = std::localtime(&now_time_t);

        // Set start date to 2 years ago
        std::tm start_tm = *now_tm;
        start_tm.tm_year -= 2;  // 2 years ago
        auto start_time_t = std::mktime(&start_tm);
        config.strategy_config.start_date = std::chrono::system_clock::from_time_t(start_time_t);

        // Set end date to today
        config.strategy_config.end_date = now;

        config.strategy_config.asset_class = trade_ngin::AssetClass::FUTURES;
        config.strategy_config.data_freq = trade_ngin::DataFrequency::DAILY;
        // warmup_days will be calculated dynamically from strategy lookbacks

        // MEMORY FIXED: Restored original symbol loading with memory management
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
            // Detailed error logging
            ERROR("Failed to get symbols: " + std::string(symbols_result.error()->what()));
            throw std::runtime_error("Failed to get symbols: " +
                                     symbols_result.error()->to_string());
        }

        std::cout << "Symbols: ";
        for (const auto& symbol : config.strategy_config.symbols) {
            std::cout << symbol << " ";
        }
        std::cout << std::endl;

        // Configure portfolio settings
        config.portfolio_config.initial_capital = 500000.0;  // $500k
        config.portfolio_config.use_risk_management = true;
        config.portfolio_config.use_optimization = true;

        // Set strategy_config.initial_capital to match portfolio_config.initial_capital to avoid
        // confusion (This is stored in run_metadata for reference, but
        // portfolio_config.initial_capital is what's actually used)
        config.strategy_config.initial_capital = config.portfolio_config.initial_capital;

        std::cout << "Retrieved " << config.strategy_config.symbols.size() << " symbols"
                  << std::endl;
        std::cout << "Initial capital: $" << config.portfolio_config.initial_capital << std::endl;

        INFO("Configuration loaded successfully. Testing " +
             std::to_string(config.strategy_config.symbols.size()) + " symbols from " +
             std::to_string(
                 std::chrono::system_clock::to_time_t(config.strategy_config.start_date)) +
             " to " +
             std::to_string(std::chrono::system_clock::to_time_t(config.strategy_config.end_date)));

        // Configure portfolio risk management
        config.portfolio_config.risk_config.capital = config.portfolio_config.initial_capital;
        config.portfolio_config.risk_config.confidence_level = 0.99;
        config.portfolio_config.risk_config.lookback_period = 252;
        config.portfolio_config.risk_config.var_limit = 0.15;
        config.portfolio_config.risk_config.jump_risk_limit = 0.10;
        config.portfolio_config.risk_config.max_correlation = 0.7;
        config.portfolio_config.risk_config.max_gross_leverage = 4.0;
        config.portfolio_config.risk_config.max_net_leverage = 2.0;

        // Configure portfolio optimization
        config.portfolio_config.opt_config.tau = 1.0;
        config.portfolio_config.opt_config.capital =
            config.portfolio_config.initial_capital.as_double();
        config.portfolio_config.opt_config.cost_penalty_scalar = 50.0;
        config.portfolio_config.opt_config.asymmetric_risk_buffer = 0.1;
        config.portfolio_config.opt_config.max_iterations = 100;
        config.portfolio_config.opt_config.convergence_threshold = 1e-6;
        config.portfolio_config.opt_config.use_buffering = true;
        config.portfolio_config.opt_config.buffer_size_factor = 0.05;

        // Initialize backtest coordinator
        // Right before creating BacktestCoordinator
        std::cerr << "Before BacktestCoordinator: initialized="
                  << Logger::instance().is_initialized() << std::endl;
        INFO("Initializing backtest coordinator...");

        // Create BacktestCoordinatorConfig from BacktestConfig
        trade_ngin::backtest::BacktestCoordinatorConfig coord_config;
        coord_config.initial_capital = static_cast<double>(config.portfolio_config.initial_capital);
        coord_config.use_risk_management = config.portfolio_config.use_risk_management;
        coord_config.use_optimization = config.portfolio_config.use_optimization;
        coord_config.store_trade_details = config.store_trade_details;
        coord_config.portfolio_id = config.portfolio_id;

        auto coordinator = std::make_unique<trade_ngin::backtest::BacktestCoordinator>(
            db, &registry, coord_config);

        // After creating BacktestCoordinator
        std::cerr << "After BacktestCoordinator: initialized="
                  << Logger::instance().is_initialized() << std::endl;

        // Setup portfolio configuration
        trade_ngin::PortfolioConfig portfolio_config;
        portfolio_config.total_capital = config.portfolio_config.initial_capital;
        portfolio_config.reserve_capital =
            config.portfolio_config.initial_capital * 0.1;  // 10% reserve
        portfolio_config.max_strategy_allocation = 1.0;
        portfolio_config.min_strategy_allocation = 0.1;
        portfolio_config.use_optimization = true;
        portfolio_config.use_risk_management = true;
        portfolio_config.opt_config = config.portfolio_config.opt_config;
        portfolio_config.risk_config = config.portfolio_config.risk_config;

        // Load strategies from config
        std::vector<std::shared_ptr<trade_ngin::StrategyInterface>> strategies;
        std::vector<std::string> strategy_names;
        std::unordered_map<std::string, double> strategy_allocations;
        std::unordered_map<std::string, nlohmann::json> strategy_configs;

        if (!config_json.contains("portfolio") ||
            !config_json["portfolio"].contains("strategies")) {
            ERROR("No portfolio.strategies section found in config.json");
            return 1;
        }

        auto strategies_config = config_json["portfolio"]["strategies"];

        // Step 1: Load default allocations from config.json
        for (const auto& [strategy_id, strategy_def] : strategies_config.items()) {
            if (strategy_def.contains("enabled_backtest") &&
                strategy_def["enabled_backtest"].get<bool>()) {
                double default_allocation = strategy_def.value("default_allocation", 0.5);
                strategy_allocations[strategy_id] = default_allocation;
                strategy_configs[strategy_id] = strategy_def;
                strategy_names.push_back(strategy_id);
            }
        }

        if (strategy_names.empty()) {
            ERROR("No enabled strategies found in config.json for backtest");
            return 1;
        }

        // Step 2: Normalize allocations to sum to 1.0
        double total_allocation = 0.0;
        for (const auto& [_, alloc] : strategy_allocations) {
            total_allocation += alloc;
        }
        if (total_allocation > 0.0) {
            for (auto& [_, alloc] : strategy_allocations) {
                alloc /= total_allocation;
            }
        }

        // Step 3: Override allocations in code (optional - uncomment to use hardcoded values)
        // This will override whatever is in config.json
        // Uncomment the lines below to set 90-10 allocation:
        // strategy_allocations["TREND_FOLLOWING"] = 0.9;      // 90% normal trend following
        // strategy_allocations["TREND_FOLLOWING_FAST"] = 0.1;  // 10% fast trend following

        // Re-normalize after override to ensure they sum to 1.0 (uncomment if using override above)
        // total_allocation = 0.0;
        // for (const auto& [_, alloc] : strategy_allocations) {
        //     total_allocation += alloc;
        // }
        // if (total_allocation > 0.0) {
        //     for (auto& [_, alloc] : strategy_allocations) {
        //         alloc /= total_allocation;
        //     }
        // }

        INFO("Loading " + std::to_string(strategy_names.size()) + " strategies from config");

        // Create a shared_ptr that doesn't own the singleton registry
        auto registry_ptr =
            std::shared_ptr<InstrumentRegistry>(&registry, [](InstrumentRegistry*) {});

        // Create base strategy configuration
        trade_ngin::StrategyConfig base_strategy_config;
        base_strategy_config.asset_classes = {trade_ngin::AssetClass::FUTURES};
        base_strategy_config.frequencies = {config.strategy_config.data_freq};
        base_strategy_config.max_drawdown = 0.4;
        base_strategy_config.max_leverage = 4.0;

        // Add position limits and contract sizes
        for (const auto& symbol : config.strategy_config.symbols) {
            base_strategy_config.position_limits[symbol] = 1000.0;
        }

        // Create and initialize each strategy
        for (const auto& strategy_id : strategy_names) {
            const auto& strategy_def = strategy_configs[strategy_id];
            std::string strategy_type = strategy_def.value("type", "");

            // Calculate capital allocation for this strategy
            double allocation = strategy_allocations[strategy_id];
            base_strategy_config.capital_allocation =
                config.portfolio_config.initial_capital.as_double() * allocation;

            INFO("Creating strategy: " + strategy_id + " (type: " + strategy_type +
                 ", allocation: " + std::to_string(allocation * 100.0) + "%)");

            std::shared_ptr<trade_ngin::StrategyInterface> strategy;

            if (strategy_type == "TrendFollowingStrategy") {
                // Create TrendFollowingStrategy
                trade_ngin::TrendFollowingConfig trend_config;
                if (strategy_def.contains("config")) {
                    const auto& cfg = strategy_def["config"];
                    trend_config.weight = cfg.value("weight", 0.03);
                    trend_config.risk_target = cfg.value("risk_target", 0.2);
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
                // Set default FDM if not in config
                if (trend_config.fdm.empty()) {
                    trend_config.fdm = {{1, 1.0},  {2, 1.03}, {3, 1.08},
                                        {4, 1.13}, {5, 1.19}, {6, 1.26}};
                }

                strategy = std::make_shared<trade_ngin::TrendFollowingStrategy>(
                    strategy_id, base_strategy_config, trend_config, db, registry_ptr);

            } else if (strategy_type == "TrendFollowingFastStrategy") {
                // Create TrendFollowingFastStrategy
                trade_ngin::TrendFollowingFastConfig trend_config;
                if (strategy_def.contains("config")) {
                    const auto& cfg = strategy_def["config"];
                    trend_config.weight = cfg.value("weight", 0.03);
                    trend_config.risk_target = cfg.value("risk_target", 0.25);
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
                // Set default FDM if not in config
                if (trend_config.fdm.empty()) {
                    trend_config.fdm = {{1, 1.0},  {2, 1.03}, {3, 1.08},
                                        {4, 1.13}, {5, 1.19}, {6, 1.26}};
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

        // Create portfolio manager and add all strategies
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
        INFO("Running backtest for time period: " +
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

        std::cout << "======= Backtest Results =======" << std::endl;
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

        // Save portfolio results to database with enhanced error handling
        INFO("Saving portfolio backtest results to database...");
        try {
            // Generate portfolio config JSON
            nlohmann::json portfolio_config_json = portfolio_config.to_json();
            portfolio_config_json["strategy_allocations"] = strategy_allocations;
            portfolio_config_json["strategy_names"] = strategy_names;

            // Save portfolio-level results with per-strategy attribution
            auto save_result = coordinator->save_portfolio_results_to_db(
                backtest_results, strategy_names, strategy_allocations, portfolio,
                portfolio_config_json);

            if (save_result.is_error()) {
                std::cerr << "Failed to save portfolio backtest results to database: "
                          << save_result.error()->what() << std::endl;
                ERROR("Failed to save portfolio backtest results to database: " +
                      std::string(save_result.error()->what()));
            } else {
                INFO("Successfully saved portfolio backtest results to database");
            }
        } catch (const std::exception& e) {
            std::cerr << "Exception during database save: " << e.what() << std::endl;
            ERROR("Exception during database save: " + std::string(e.what()));
        }

        // Explicitly reset the coordinator to trigger cleanup before program exit
        INFO("Cleaning up backtest coordinator...");
        coordinator.reset();

        INFO("Backtest application completed successfully");

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
