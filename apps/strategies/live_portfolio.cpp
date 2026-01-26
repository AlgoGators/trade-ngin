#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include "trade_ngin/core/email_sender.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/data/conversion_utils.hpp"
#include "trade_ngin/data/credential_store.hpp"
#include "trade_ngin/data/database_pooling.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/instruments/futures.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/live/csv_exporter.hpp"
#include "trade_ngin/live/execution_manager.hpp"
#include "trade_ngin/live/live_data_loader.hpp"
#include "trade_ngin/live/live_metrics_calculator.hpp"
#include "trade_ngin/live/live_pnl_manager.hpp"
#include "trade_ngin/live/live_price_manager.hpp"
#include "trade_ngin/live/live_trading_coordinator.hpp"
#include "trade_ngin/live/margin_manager.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/storage/live_results_manager.hpp"
#include "trade_ngin/strategy/trend_following.hpp"
#include "trade_ngin/strategy/trend_following_fast.hpp"
#include "trade_ngin/strategy/trend_following_slow.hpp"

using namespace trade_ngin;

int main(int argc, char* argv[]) {
    try {
        // Parse command-line arguments for date override and email flag
        std::chrono::system_clock::time_point target_date;
        bool use_override_date = false;
        bool send_email = false;  // Default to false for historical runs

        // Parse command-line arguments
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];

            // Check for email flag
            if (arg == "--send-email") {
                send_email = true;
                continue;
            }

            // Try to parse as date
            std::tm tm = {};
            std::istringstream ss(arg);
            ss >> std::get_time(&tm, "%Y-%m-%d");
            if (!ss.fail()) {
                target_date = std::chrono::system_clock::from_time_t(std::mktime(&tm));
                use_override_date = true;
                std::cout << "Running for historical date: " << arg << std::endl;
            } else if (arg != "--send-email") {
                std::cerr << "Invalid argument: " << arg << std::endl;
                std::cerr << "Usage: " << argv[0] << " [YYYY-MM-DD] [--send-email]" << std::endl;
                std::cerr << "Example: " << argv[0] << " 2025-01-01 --send-email" << std::endl;
                return 1;
            }
        }

        // If no date override, enable email by default for real-time runs
        if (!use_override_date) {
            send_email = true;
        }

        if (send_email && use_override_date) {
            std::cout << "Email sending enabled for historical run" << std::endl;
        }
        // Initialize the logger
        auto& logger = Logger::instance();
        LoggerConfig logger_config;
        logger_config.min_level = LogLevel::INFO;
        logger_config.destination = LogDestination::BOTH;
        logger_config.log_directory = "logs";
        logger_config.filename_prefix = "live_trend";
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
        auto credentials = std::make_shared<trade_ngin::CredentialStore>("./config.json");

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

        // Configure daily position generation parameters
        INFO("Loading configuration...");

        // ========================================
        // PHASE 1: CONFIG-DRIVEN STRATEGY LOADING
        // Load strategies from config.json using enabled_live flag
        // ========================================
        std::string config_filename = "./config.json";
        std::ifstream config_stream(config_filename);
        if (!config_stream.is_open()) {
            ERROR("Failed to open " + config_filename);
            return 1;
        }
        nlohmann::json config_json;
        try {
            config_stream >> config_json;
        } catch (const std::exception& e) {
            ERROR("Failed to parse " + config_filename + ": " + std::string(e.what()));
            return 1;
        }

        // Tier 1: Read portfolio_id from config
        std::string portfolio_id = "BASE_PORTFOLIO";
        if (config_json.contains("portfolio_id")) {
            portfolio_id = config_json["portfolio_id"].get<std::string>();
        }
        INFO("Using portfolio_id: " + portfolio_id);

        // Load strategies from config (mirror bt_portfolio.cpp pattern)
        std::vector<std::string> strategy_names;
        std::unordered_map<std::string, double> strategy_allocations;
        std::unordered_map<std::string, nlohmann::json> strategy_configs;

        if (!config_json.contains("portfolio") ||
            !config_json["portfolio"].contains("strategies")) {
            ERROR("No portfolio.strategies section found in " + config_filename);
            return 1;
        }

        auto strategies_config = config_json["portfolio"]["strategies"];
        for (const auto& [strategy_id, strategy_def] : strategies_config.items()) {
            // Use enabled_live flag for live portfolio
            if (strategy_def.contains("enabled_live") && strategy_def["enabled_live"].get<bool>()) {
                double default_allocation = strategy_def.value("default_allocation", 0.5);
                strategy_allocations[strategy_id] = default_allocation;
                strategy_configs[strategy_id] = strategy_def;
                strategy_names.push_back(strategy_id);
                INFO("Loaded strategy: " + strategy_id +
                     " with allocation: " + std::to_string(default_allocation * 100.0) + "%");
            }
        }

        if (strategy_names.empty()) {
            ERROR("No enabled_live strategies found in " + config_filename);
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

        // Sort strategy names for deterministic combined ID (Tier 2)
        std::sort(strategy_names.begin(), strategy_names.end());

        // Generate combined strategy_id: LIVE_<sorted_names_joined_by_&>
        std::string combined_strategy_id = "LIVE_";
        for (size_t i = 0; i < strategy_names.size(); ++i) {
            if (i > 0)
                combined_strategy_id += "_";
            combined_strategy_id += strategy_names[i];
        }
        INFO("Combined strategy_id (Tier 2): " + combined_strategy_id);
        INFO("Total strategies enabled: " + std::to_string(strategy_names.size()));

        // Log normalized allocations
        for (const auto& [name, alloc] : strategy_allocations) {
            INFO("Strategy " + name + " normalized allocation: " + std::to_string(alloc * 100.0) +
                 "%");
        }

        // Get current date for daily processing (or use override date)
        auto now = use_override_date ? target_date : std::chrono::system_clock::now();
        auto now_time_t = std::chrono::system_clock::to_time_t(now);
        std::tm* now_tm = std::localtime(&now_time_t);

        // Set start date to 300 days ago for sufficient historical data
        auto start_date = now - std::chrono::hours(24 * 300);  // 300 days ago

        // Set end date based on run type to avoid lookahead bias
        auto end_date = use_override_date ? (now - std::chrono::hours(24)) : now;
        // For historical runs: exclude current day's data (use previous day)
        // For live runs: include current day's data (use current day)

        INFO("DEBUG: Run type: " + std::string(use_override_date ? "HISTORICAL" : "LIVE"));
        INFO("DEBUG: Start date: " +
             std::to_string(std::chrono::system_clock::to_time_t(start_date)));
        INFO("DEBUG: End date: " + std::to_string(std::chrono::system_clock::to_time_t(end_date)));
        INFO("DEBUG: Target date (now): " +
             std::to_string(std::chrono::system_clock::to_time_t(now)));

        double initial_capital = 500000.0;  // $500k

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
        } else {
            // Detailed error logging
            ERROR("Failed to get symbols: " + std::string(symbols_result.error()->what()));
            throw std::runtime_error("Failed to get symbols: " +
                                     symbols_result.error()->to_string());
        }

        std::cout << "Symbols: ";
        for (const auto& symbol : symbols) {
            std::cout << symbol << " ";
        }
        std::cout << std::endl;

        std::cout << "Retrieved " << symbols.size() << " symbols" << std::endl;
        std::cout << "Initial capital: $" << initial_capital << std::endl;

        INFO("Configuration loaded successfully. Processing " + std::to_string(symbols.size()) +
             " symbols from " + std::to_string(std::chrono::system_clock::to_time_t(start_date)) +
             " to " + std::to_string(std::chrono::system_clock::to_time_t(end_date)));

        // Pre-run margin metadata validation for futures instruments
        // Ensure initial and maintenance margins are present and positive
        INFO("Validating margin metadata for futures instruments...");
        int futures_margin_issues = 0;
        for (const auto& sym : symbols) {
            try {
                // Normalize variant-suffixed symbols (e.g., 6B.v.0 -> 6B) for registry lookups only
                std::string lookup_sym = sym;
                auto dotpos = lookup_sym.find(".v.");
                if (dotpos != std::string::npos) {
                    lookup_sym = lookup_sym.substr(0, dotpos);
                }
                dotpos = lookup_sym.find(".c.");
                if (dotpos != std::string::npos) {
                    lookup_sym = lookup_sym.substr(0, dotpos);
                }

                auto inst = registry.get_instrument(lookup_sym);
                if (!inst) {
                    WARN("Instrument not found in registry: " + sym);
                    futures_margin_issues++;
                    continue;
                }
                auto fut = std::dynamic_pointer_cast<trade_ngin::FuturesInstrument>(inst);
                if (!fut) {
                    // Symbol list should be futures; warn if not futures
                    WARN("Symbol not a futures instrument: " + sym);
                    continue;
                }
                double im = fut->get_margin_requirement();
                double mm = fut->get_maintenance_margin();
                if (!(im > 0.0)) {
                    WARN("Missing or non-positive initial margin for " + sym);
                    futures_margin_issues++;
                }
                if (!(mm > 0.0)) {
                    WARN("Missing or non-positive maintenance margin for " + sym);
                    futures_margin_issues++;
                }
            } catch (const std::exception& e) {
                WARN("Exception validating margins for " + sym + ": " + std::string(e.what()));
                futures_margin_issues++;
            }
        }
        if (futures_margin_issues > 0) {
            ERROR(
                "Margin metadata validation failed for one or more futures instruments. Aborting "
                "run.");
            return 1;
        }

        // Configure portfolio risk management
        RiskConfig risk_config;
        risk_config.capital = Decimal(initial_capital);
        risk_config.confidence_level = 0.99;
        risk_config.lookback_period = 252;
        risk_config.var_limit = 0.15;
        risk_config.jump_risk_limit = 0.10;
        risk_config.max_correlation = 0.7;
        risk_config.max_gross_leverage = 4.0;
        risk_config.max_net_leverage = 2.0;

        // Configure portfolio optimization
        DynamicOptConfig opt_config;
        opt_config.tau = 1.0;
        opt_config.capital = initial_capital;
        opt_config.cost_penalty_scalar = 50.0;
        opt_config.asymmetric_risk_buffer = 0.1;
        opt_config.max_iterations = 100;
        opt_config.convergence_threshold = 1e-6;
        opt_config.use_buffering = true;
        opt_config.buffer_size_factor = 0.05;

        // Setup portfolio configuration
        trade_ngin::PortfolioConfig portfolio_config;
        portfolio_config.total_capital = initial_capital;
        portfolio_config.reserve_capital = initial_capital * 0.10;  // 10% reserve (match bt)
        portfolio_config.max_strategy_allocation = 1.0;  // Only have one strategy currently
        portfolio_config.min_strategy_allocation = 0.1;
        portfolio_config.use_optimization = true;
        portfolio_config.use_risk_management = true;
        portfolio_config.opt_config = opt_config;
        portfolio_config.risk_config = risk_config;

        // ========================================
        // PHASE 2: STRATEGY INSTANCE FACTORY
        // Create strategies based on type from config.json
        // ========================================

        // Base strategy configuration (used by all strategies)
        trade_ngin::StrategyConfig base_strategy_config;
        base_strategy_config.asset_classes = {trade_ngin::AssetClass::FUTURES};
        base_strategy_config.frequencies = {trade_ngin::DataFrequency::DAILY};
        base_strategy_config.max_drawdown = 0.4;
        base_strategy_config.max_leverage = 4.0;
        base_strategy_config.save_positions = false;  // Manual position saving
        base_strategy_config.save_signals = false;
        base_strategy_config.save_executions = false;

        // Add position limits and costs for all symbols
        for (const auto& symbol : symbols) {
            base_strategy_config.position_limits[symbol] = 500.0;
        }

        // Create a shared_ptr that doesn't own the singleton registry
        auto registry_ptr =
            std::shared_ptr<InstrumentRegistry>(&registry, [](InstrumentRegistry*) {});

        // Vector to hold all strategy instances
        std::vector<std::shared_ptr<trade_ngin::StrategyInterface>> strategies;

        INFO("Creating " + std::to_string(strategy_names.size()) + " strategies from config");

        // Factory loop: create each strategy based on type
        for (const auto& strategy_name : strategy_names) {
            const auto& strategy_def = strategy_configs[strategy_name];
            std::string strategy_type = strategy_def.value("type", "TrendFollowingStrategy");
            double allocation = strategy_allocations[strategy_name];

            // Calculate capital allocation for this strategy
            trade_ngin::StrategyConfig strategy_config = base_strategy_config;
            strategy_config.capital_allocation = initial_capital * allocation;

            INFO("Creating strategy: " + strategy_name + " (type: " + strategy_type +
                 ", allocation: " + std::to_string(allocation * 100.0) + "%)");

            std::shared_ptr<trade_ngin::StrategyInterface> strategy;

            if (strategy_type == "TrendFollowingStrategy") {
                // Create TrendFollowingStrategy (normal speed)
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
                // Set default FDM if not loaded
                if (trend_config.fdm.empty()) {
                    trend_config.fdm = {{1, 1.0},  {2, 1.03}, {3, 1.08},
                                        {4, 1.13}, {5, 1.19}, {6, 1.26}};
                }

                strategy = std::make_shared<trade_ngin::TrendFollowingStrategy>(
                    strategy_name, strategy_config, trend_config, db, registry_ptr);

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
                if (trend_config.fdm.empty()) {
                    trend_config.fdm = {{1, 1.0},  {2, 1.03}, {3, 1.08},
                                        {4, 1.13}, {5, 1.19}, {6, 1.26}};
                }

                strategy = std::make_shared<trade_ngin::TrendFollowingFastStrategy>(
                    strategy_name, strategy_config, trend_config, db, registry_ptr);

            } else if (strategy_type == "TrendFollowingSlowStrategy") {
                // Create TrendFollowingSlowStrategy (legacy support)
                trade_ngin::TrendFollowingSlowConfig trend_config;
                if (strategy_def.contains("config")) {
                    const auto& cfg = strategy_def["config"];
                    trend_config.weight = cfg.value("weight", 0.03);
                    trend_config.risk_target = cfg.value("risk_target", 0.15);
                    trend_config.idm = cfg.value("idm", 2.5);
                    trend_config.use_position_buffering = cfg.value("use_position_buffering", true);
                    if (cfg.contains("ema_windows")) {
                        trend_config.ema_windows.clear();
                        for (const auto& window : cfg["ema_windows"]) {
                            trend_config.ema_windows.push_back(
                                {window[0].get<int>(), window[1].get<int>()});
                        }
                    }
                    trend_config.vol_lookback_short = cfg.value("vol_lookback_short", 64);
                    trend_config.vol_lookback_long = cfg.value("vol_lookback_long", 252);
                } else {
                    // Use hardcoded defaults for slow strategy
                    trend_config.weight = 0.03;
                    trend_config.risk_target = 0.15;
                    trend_config.idm = 2.5;
                    trend_config.use_position_buffering = true;
                    trend_config.ema_windows = {{4, 16},   {8, 32},   {16, 64},
                                                {32, 128}, {64, 256}, {128, 512}};
                    trend_config.vol_lookback_short = 64;
                    trend_config.vol_lookback_long = 252;
                }
                if (trend_config.fdm.empty()) {
                    trend_config.fdm = {{1, 1.0},  {2, 1.03}, {3, 1.08},
                                        {4, 1.13}, {5, 1.19}, {6, 1.26}};
                }

                strategy = std::make_shared<trade_ngin::TrendFollowingSlowStrategy>(
                    strategy_name, strategy_config, trend_config, db, registry_ptr);

            } else {
                ERROR("Unknown strategy type: " + strategy_type +
                      " for strategy: " + strategy_name);
                return 1;
            }

            // Initialize strategy
            auto init_result = strategy->initialize();
            if (init_result.is_error()) {
                ERROR("Failed to initialize strategy " + strategy_name + ": " +
                      init_result.error()->what());
                return 1;
            }
            INFO("Strategy " + strategy_name + " initialization successful");

            // Start strategy
            auto start_result = strategy->start();
            if (start_result.is_error()) {
                ERROR("Failed to start strategy " + strategy_name + ": " +
                      start_result.error()->what());
                return 1;
            }
            INFO("Strategy " + strategy_name + " started successfully");

            strategies.push_back(strategy);
        }

        INFO("Successfully created " + std::to_string(strategies.size()) + " strategies");

        // Create map from strategy name to strategy instance for CSV export
        trade_ngin::StrategyInstancesMap strategy_instances_map;
        for (size_t i = 0; i < strategies.size(); ++i) {
            auto* base_strategy = dynamic_cast<trade_ngin::BaseStrategy*>(strategies[i].get());
            if (base_strategy != nullptr) {
                strategy_instances_map[strategy_names[i]] = base_strategy;
            }
        }
        INFO("Created strategy instances map with " + std::to_string(strategy_instances_map.size()) + " entries");

        // Get reference to first strategy for single-strategy compatibility (Phase 3 will fix this)
        auto tf_strategy = strategies[0];

        // Cast to TrendFollowingStrategy for methods like get_forecast/get_position (Phase 3 will
        // iterate all) Note: This works for both TrendFollowingStrategy and its subclasses
        // (Slow/Fast)
        auto tf_strategy_typed =
            std::dynamic_pointer_cast<trade_ngin::TrendFollowingStrategy>(tf_strategy);

        // ========================================
        // PHASE 3: PORTFOLIO MANAGER LOOP
        // Add all strategies to portfolio with normalized allocations
        // ========================================
        INFO("Creating portfolio manager with " + std::to_string(strategies.size()) +
             " strategies...");
        auto portfolio = std::make_shared<trade_ngin::PortfolioManager>(portfolio_config);

        for (size_t i = 0; i < strategies.size(); ++i) {
            const auto& strategy = strategies[i];
            const std::string& strat_name = strategy_names[i];
            double allocation = strategy_allocations[strat_name];

            INFO("Adding strategy " + strat_name + " with allocation " +
                 std::to_string(allocation * 100.0) + "%");

            auto add_result =
                portfolio->add_strategy(strategy, allocation, portfolio_config.use_optimization,
                                        portfolio_config.use_risk_management);

            if (add_result.is_error()) {
                ERROR("Failed to add strategy " + strat_name +
                      " to portfolio: " + add_result.error()->what());
                return 1;
            }
            INFO("Strategy " + strat_name + " added to portfolio successfully");
        }

        INFO("All " + std::to_string(strategies.size()) + " strategies added to portfolio");

        // ========================================
        // STORE LIVE RUN METADATA
        // Save run metadata (allocations, configs) for this trading day
        // ========================================
        INFO("Storing live run metadata for this trading day...");
        {
            // Build portfolio config JSON
            nlohmann::json portfolio_config_json;
            portfolio_config_json["total_capital"] =
                static_cast<double>(portfolio_config.total_capital);
            portfolio_config_json["reserve_capital"] =
                static_cast<double>(portfolio_config.reserve_capital);
            portfolio_config_json["use_optimization"] = portfolio_config.use_optimization;
            portfolio_config_json["use_risk_management"] = portfolio_config.use_risk_management;

            // Convert strategy_allocations to JSON
            nlohmann::json strategy_alloc_json(strategy_allocations);

            // strategy_configs is already nlohmann::json
            auto metadata_result = db->store_live_run_metadata(
                now, combined_strategy_id, portfolio_id, strategy_alloc_json, portfolio_config_json,
                strategy_configs  // already nlohmann::json
            );

            if (metadata_result.is_error()) {
                WARN("Failed to store live run metadata: " +
                     std::string(metadata_result.error()->what()));
            } else {
                INFO("Successfully stored live run metadata for date");
            }
        }

        // Create LiveTradingCoordinator to manage all live trading components
        INFO("Creating LiveTradingCoordinator for centralized component management");
        LiveTradingConfig coordinator_config;
        coordinator_config.strategy_id = combined_strategy_id;  // From config (Phase 1)
        coordinator_config.portfolio_id = portfolio_id;         // From config.json
        coordinator_config.schema = "trading";
        coordinator_config.initial_capital = initial_capital;
        coordinator_config.store_results = true;
        coordinator_config.calculate_risk_metrics = true;

        auto coordinator =
            std::make_unique<LiveTradingCoordinator>(db, registry, coordinator_config);

        // Initialize the coordinator
        auto init_coord_result = coordinator->initialize();
        if (init_coord_result.is_error()) {
            ERROR("Failed to initialize LiveTradingCoordinator: " +
                  std::string(init_coord_result.error()->what()));
            return 1;
        }
        INFO("LiveTradingCoordinator initialized successfully");

        // Get component references from coordinator
        auto* data_loader = coordinator->get_data_loader();
        auto* metrics_calculator = coordinator->get_metrics_calculator();
        auto* results_manager = coordinator->get_results_manager();
        auto* price_manager = coordinator->get_price_manager();
        auto* pnl_manager = coordinator->get_pnl_manager();

        // Create Phase 3 managers
        INFO("Creating ExecutionManager and MarginManager for Phase 3");
        auto execution_manager = std::make_unique<ExecutionManager>();
        auto margin_manager = std::make_unique<MarginManager>(registry);

        // Create Phase 4 CSV exporter with portfolio-specific directory
        INFO("Creating CSVExporter for Phase 4");
        std::string csv_output_dir = "apps/strategies/results/" + portfolio_id;
        std::filesystem::create_directories(csv_output_dir);
        INFO("CSV output directory: " + csv_output_dir);
        auto csv_exporter = std::make_unique<CSVExporter>(csv_output_dir);

        // Load market data for daily processing
        INFO("Loading market data for daily processing...");
        auto market_data_result =
            db->get_market_data(symbols, start_date, end_date, trade_ngin::AssetClass::FUTURES,
                                trade_ngin::DataFrequency::DAILY, "ohlcv");

        if (market_data_result.is_error()) {
            ERROR("Failed to load market data: " + std::string(market_data_result.error()->what()));
            return 1;
        }

        // Convert Arrow table to Bars using the same conversion as backtest
        auto conversion_result =
            trade_ngin::DataConversionUtils::arrow_table_to_bars(market_data_result.value());
        if (conversion_result.is_error()) {
            ERROR("Failed to convert market data to bars: " +
                  std::string(conversion_result.error()->what()));
            return 1;
        }

        auto all_bars = conversion_result.value();
        INFO("Loaded " + std::to_string(all_bars.size()) + " total bars");

        // Update price manager with bars to extract T-1 and T-2 prices
        if (price_manager) {
            auto price_update_result = price_manager->update_from_bars(all_bars, now);
            if (price_update_result.is_error()) {
                ERROR("Failed to update price manager with bar data: " +
                      std::string(price_update_result.error()->what()));
                return 1;
            }
            INFO("Price manager updated - extracted T-1 and T-2 prices from bars");
        } else {
            ERROR("Price manager not initialized");
            return 1;
        }

        if (all_bars.empty()) {
            ERROR("No historical data loaded. Cannot calculate positions.");
            ERROR("This may be due to missing market data for the requested date.");
            ERROR("Please check if market data exists for " +
                  std::to_string(std::chrono::system_clock::to_time_t(now)) +
                  " and the 300 days prior.");
            return 1;
        }

        // ========================================
        // UPDATE TRANSACTION COST MANAGER WITH MARKET DATA
        // Feed rolling ADV and volatility for accurate cost calculations
        // ========================================
        INFO("Updating execution manager with market data for transaction cost tracking...");

        // Build map of latest bars per symbol (T-1 data)
        std::unordered_map<std::string, Bar> latest_bars_per_symbol;
        std::unordered_map<std::string, Bar> previous_bars_per_symbol;

        for (const auto& bar : all_bars) {
            auto it = latest_bars_per_symbol.find(bar.symbol);
            if (it == latest_bars_per_symbol.end() || bar.timestamp > it->second.timestamp) {
                // Save previous latest as "previous" before updating
                if (it != latest_bars_per_symbol.end()) {
                    previous_bars_per_symbol[bar.symbol] = it->second;
                }
                latest_bars_per_symbol[bar.symbol] = bar;
            } else if (!previous_bars_per_symbol.count(bar.symbol) &&
                       bar.timestamp < latest_bars_per_symbol[bar.symbol].timestamp) {
                // Track the second-most-recent bar as previous
                auto prev_it = previous_bars_per_symbol.find(bar.symbol);
                if (prev_it == previous_bars_per_symbol.end() ||
                    bar.timestamp > prev_it->second.timestamp) {
                    previous_bars_per_symbol[bar.symbol] = bar;
                }
            }
        }

        // Update execution manager with daily market data for each symbol
        int symbols_updated = 0;
        for (const auto& [symbol, latest_bar] : latest_bars_per_symbol) {
            double close = static_cast<double>(latest_bar.close);
            double volume = latest_bar.volume;
            double prev_close = close;  // Default to same if no previous

            auto prev_it = previous_bars_per_symbol.find(symbol);
            if (prev_it != previous_bars_per_symbol.end()) {
                prev_close = static_cast<double>(prev_it->second.close);
            }

            // Update the transaction cost manager with market data
            execution_manager->update_market_data(symbol, volume, close);
            symbols_updated++;

            DEBUG("Updated market data for " + symbol + ": volume=" + std::to_string(volume) +
                  ", close=" + std::to_string(close) +
                  ", prev_close=" + std::to_string(prev_close));
        }

        INFO("Updated transaction cost manager with market data for " +
             std::to_string(symbols_updated) + " symbols");

        // Pre-warm strategy state so portfolio can pull price history for optimization/risk
        INFO("Preprocessing data in strategy to populate price history...");
        auto strat_prewarm = tf_strategy->on_data(all_bars);
        if (strat_prewarm.is_error()) {
            std::cerr << "Failed to preprocess data in strategy: " << strat_prewarm.error()->what()
                      << std::endl;
            return 1;
        }

        // Process data through portfolio pipeline (optimization + risk), mirroring backtest
        INFO("Processing data through portfolio manager (optimization + risk)...");
        auto port_process_result = portfolio->process_market_data(all_bars);
        if (port_process_result.is_error()) {
            std::cerr << "Failed to process data in portfolio manager: "
                      << port_process_result.error()->what() << std::endl;
            return 1;
        }
        INFO("Portfolio processing completed");

        // ========================================
        // PHASE 4: PER-STRATEGY SIGNALS STORAGE
        // Extract and store signals from each strategy after portfolio processing
        // ========================================
        INFO("PHASE 4: Storing per-strategy signals to database...");

        for (const auto& strategy : strategies) {
            const auto& metadata = strategy->get_metadata();
            std::string strategy_name = metadata.id;

            // Try to extract signals from either TrendFollowingStrategy or
            // TrendFollowingFastStrategy
            std::unordered_map<std::string, double> signals_map;
            bool signals_extracted = false;

            // Try TrendFollowingStrategy first
            auto tf_strategy_ptr = std::dynamic_pointer_cast<TrendFollowingStrategy>(strategy);
            if (tf_strategy_ptr) {
                // Get all instrument data (contains signals for all symbols)
                const auto& all_instrument_data = tf_strategy_ptr->get_all_instrument_data();

                // Extract signals (current_forecast) from instrument data
                for (const auto& [symbol, data] : all_instrument_data) {
                    // Use current_forecast as the signal value
                    signals_map[symbol] = data.current_forecast;
                }
                signals_extracted = true;
            } else {
                // Try TrendFollowingFastStrategy
                auto tf_fast_ptr = std::dynamic_pointer_cast<TrendFollowingFastStrategy>(strategy);
                if (tf_fast_ptr) {
                    // Get all instrument data from fast strategy
                    const auto& all_instrument_data = tf_fast_ptr->get_all_instrument_data();

                    // Extract signals (current_forecast) from instrument data
                    for (const auto& [symbol, data] : all_instrument_data) {
                        signals_map[symbol] = data.current_forecast;
                    }
                    signals_extracted = true;
                }
            }

            if (signals_extracted) {
                INFO("DEBUG PHASE 4: Strategy '" + strategy_name + "' has " +
                     std::to_string(signals_map.size()) + " signals");

                if (!signals_map.empty()) {
                    auto save_result =
                        db->store_signals(signals_map,
                                          combined_strategy_id,  // Combined strategy_id for tier 2
                                          strategy_name,  // Individual strategy_name for tier 3
                                          portfolio_id,   // Portfolio identifier
                                          now, "trading.signals");

                    if (save_result.is_error()) {
                        ERROR("Failed to store signals for strategy " + strategy_name + ": " +
                              std::string(save_result.error()->what()));
                    } else {
                        INFO("Successfully stored " + std::to_string(signals_map.size()) +
                             " signals for strategy: " + strategy_name);
                    }
                } else {
                    WARN("No signals to store for strategy: " + strategy_name);
                }
            } else {
                WARN("Strategy " + strategy_name +
                     " does not support signal extraction (not TrendFollowing or "
                     "TrendFollowingFast)");
            }
        }

        // Get optimized portfolio positions (integer-rounded after optimization/risk)
        INFO("Retrieving optimized portfolio positions...");
        auto positions = portfolio->get_portfolio_positions();

        // Extract per-strategy positions map (needed for Phase 4 & 5)
        INFO("Extracting per-strategy positions from PortfolioManager...");
        auto strategy_positions_map = portfolio->get_strategy_positions();
        INFO("DEBUG: Retrieved " + std::to_string(strategy_positions_map.size()) +
             " strategies from PortfolioManager");

        // Load previous day positions for PnL calculation
        INFO("Loading previous day positions for PnL calculation...");
        auto previous_date = now - std::chrono::hours(24);
        auto previous_positions_result =
            db->load_positions_by_date(combined_strategy_id, "", coordinator_config.portfolio_id,
                                       previous_date, "trading.positions");
        std::unordered_map<std::string, Position> previous_positions;

        if (previous_positions_result.is_ok()) {
            previous_positions = previous_positions_result.value();
            INFO("Loaded " + std::to_string(previous_positions.size()) + " previous day positions");
        } else {
            INFO("No previous day positions found (first run or no data): " +
                 std::string(previous_positions_result.error()->what()));
        }

        INFO("DEBUG: Previous date used for lookup: " +
             std::to_string(std::chrono::system_clock::to_time_t(previous_date)));
        INFO("DEBUG: Current date: " + std::to_string(std::chrono::system_clock::to_time_t(now)));
        INFO("DEBUG: Previous positions loaded: " + std::to_string(previous_positions.size()));
        for (const auto& [symbol, pos] : previous_positions) {
            INFO("DEBUG: Previous position - " + symbol + ": " +
                 std::to_string(pos.quantity.as_double()));
        }

        // Get market prices from PriceManager - already extracted from bars
        INFO("Getting market prices for PnL lag model from PriceManager...");

        // PriceManager has already extracted T-1 and T-2 prices from bars
        // Make copies since we need to use [] operator in many places
        std::unordered_map<std::string, double> previous_day_close_prices =
            price_manager->get_all_previous_day_prices();
        std::unordered_map<std::string, double> two_days_ago_close_prices =
            price_manager->get_all_two_days_ago_prices();

        INFO("Retrieved prices from PriceManager: " +
             std::to_string(previous_day_close_prices.size()) + " Day T-1, " +
             std::to_string(two_days_ago_close_prices.size()) + " Day T-2");

        // Verify we have prices for all required symbols
        std::set<std::string> all_symbols;
        for (const auto& [symbol, position] : positions) {
            if (position.quantity.as_double() != 0.0) {
                all_symbols.insert(symbol);
            }
        }
        for (const auto& [symbol, position] : previous_positions) {
            all_symbols.insert(symbol);
        }

        for (const auto& symbol : all_symbols) {
            if (previous_day_close_prices.find(symbol) == previous_day_close_prices.end()) {
                WARN("Missing T-1 price for symbol: " + symbol);
            }
            if (two_days_ago_close_prices.find(symbol) == two_days_ago_close_prices.end() &&
                previous_positions.find(symbol) != previous_positions.end()) {
                WARN("Missing T-2 price for symbol: " + symbol + " (needed for PnL finalization)");
            }
        }

        // ========================================
        // PHASE 5: PER-STRATEGY DAY T-1 FINALIZATION
        // Finalize previous day positions FOR EACH STRATEGY
        // ========================================
        INFO("PHASE 5: Finalizing Day T-1 PnL per-strategy using PnLManager...");

        // Check if we have T-1 price data for finalization
        if (previous_day_close_prices.empty() && !previous_positions.empty()) {
            WARN(
                "No T-1 close prices available (likely weekend/holiday) - all positions will have "
                "0 PnL");
            INFO("This is expected behavior when Day T-1 (" +
                 std::to_string(std::chrono::system_clock::to_time_t(previous_date)) +
                 ") was a non-trading day");
        }

        INFO("PnLManager initialized with InstrumentRegistry access");

        double aggregate_yesterday_total_pnl = 0.0;

        if (!two_days_ago_close_prices.empty() && pnl_manager) {
            INFO("Finalizing Day T-1 positions per-strategy...");

            // Finalize for each strategy separately
            for (const auto& [strategy_name, current_positions_map] : strategy_positions_map) {
                // Load previous day positions for THIS strategy
                // Filter by BOTH combined_strategy_id AND individual strategy_name
                // to ensure we only get positions from this specific run
                auto prev_strategy_positions_result =
                    db->load_positions_by_date(combined_strategy_id,  // Combined strategy_id
                                               strategy_name,         // Individual strategy_name
                                               coordinator_config.portfolio_id,  // Portfolio ID
                                               previous_date, "trading.positions");

                if (prev_strategy_positions_result.is_error()) {
                    INFO("No previous positions found for strategy " + strategy_name +
                         " (first run or no data): " +
                         std::string(prev_strategy_positions_result.error()->what()));
                    continue;  // Skip this strategy
                }

                auto prev_strategy_positions_map = prev_strategy_positions_result.value();

                if (prev_strategy_positions_map.empty()) {
                    INFO("No previous positions to finalize for strategy: " + strategy_name);
                    continue;
                }

                INFO("DEBUG PHASE 5: Strategy '" + strategy_name + "' has " +
                     std::to_string(prev_strategy_positions_map.size()) +
                     " previous day positions to finalize");

                // Convert map to vector for PnLManager
                std::vector<Position> prev_positions_vec;
                prev_positions_vec.reserve(prev_strategy_positions_map.size());
                for (const auto& [symbol, pos] : prev_strategy_positions_map) {
                    prev_positions_vec.push_back(pos);
                }

                // Get this strategy's allocation for capital calculation
                double strategy_allocation = 1.0;  // Default to full allocation
                if (strategy_allocations.find(strategy_name) != strategy_allocations.end()) {
                    strategy_allocation = strategy_allocations[strategy_name];
                }
                double strategy_capital = initial_capital * strategy_allocation;

                // Use PnLManager to finalize previous day for this strategy
                auto finalization_result =
                    pnl_manager->finalize_previous_day(prev_positions_vec,
                                                       previous_day_close_prices,  // T-1 prices
                                                       two_days_ago_close_prices,  // T-2 prices
                                                       strategy_capital,
                                                       0.0  // Commissions (will be handled later)
                    );

                if (finalization_result.is_ok()) {
                    auto& result = finalization_result.value();
                    double strategy_yesterday_pnl = result.finalized_daily_pnl;
                    aggregate_yesterday_total_pnl += strategy_yesterday_pnl;

                    INFO("DEBUG PHASE 5: Strategy '" + strategy_name +
                         "' finalized Day T-1 PnL: $" + std::to_string(strategy_yesterday_pnl));

                    // Log individual position PnLs for this strategy
                    for (const auto& [symbol, pnl] : result.position_realized_pnl) {
                        DEBUG("PHASE 5: " + strategy_name + " - Position " + symbol +
                              " finalized PnL: $" + std::to_string(pnl));
                    }

                    // Store updated positions for yesterday (Day T-1) in database FOR THIS STRATEGY
                    if (!result.finalized_positions.empty()) {
                        auto update_result =
                            db->store_positions(result.finalized_positions,
                                                combined_strategy_id,  // Combined strategy_id
                                                strategy_name,         // Individual strategy_name
                                                portfolio_id,          // Portfolio identifier
                                                "trading.positions");

                        if (update_result.is_error()) {
                            ERROR("Failed to update Day T-1 positions for strategy " +
                                  strategy_name + ": " +
                                  std::string(update_result.error()->what()));
                        } else {
                            INFO("Successfully updated " +
                                 std::to_string(result.finalized_positions.size()) +
                                 " Day T-1 positions with finalized PnL for strategy: " +
                                 strategy_name);
                        }
                    }
                } else {
                    ERROR("PnLManager failed to finalize Day T-1 for strategy " + strategy_name +
                          ": " + std::string(finalization_result.error()->what()));
                }
            }

            INFO("PHASE 5: Total finalized Day T-1 PnL across all strategies: $" +
                 std::to_string(aggregate_yesterday_total_pnl));
        } else {
            INFO("Skipping Day T-1 finalization (no two_days_ago prices or no PnLManager)");
        }

        // ========================================
        // STEP 2: CREATE TODAY'S (Day T) POSITIONS WITH ZERO PnL
        // ========================================
        INFO("STEP 2: Creating Day T positions with zero PnL (placeholders)...");

        double total_daily_transaction_costs = 0.0;  // Will be calculated from executions

        // Update all current positions to have:
        // - average_price = Day T-1 close (execution price)
        // - market_price = Day T-1 close (last known price)
        // - realized_pnl = 0 (placeholder, will be finalized tomorrow)
        // - unrealized_pnl = 0 (always 0 for futures)

        for (auto& [symbol, current_position] : positions) {
            // Get Day T-1 close price for this symbol
            double yesterday_close = current_position.average_price.as_double();  // Default
            if (previous_day_close_prices.find(symbol) != previous_day_close_prices.end()) {
                yesterday_close = previous_day_close_prices[symbol];
            }

            // Set position fields for Day T
            current_position.average_price = Decimal(yesterday_close);  // Entry at Day T-1 close
            current_position.realized_pnl =
                Decimal(0.0);  // PLACEHOLDER - will be finalized tomorrow
            current_position.unrealized_pnl = Decimal(0.0);  // Always 0 for futures
            current_position.last_update = now;              // Today's timestamp

            INFO("Day T position for " + symbol +
                 ": qty=" + std::to_string(current_position.quantity.as_double()) +
                 " entry_price=" + std::to_string(yesterday_close) +
                 " realized_pnl=0 (placeholder)");
        }

        // ========================================
        // PHASE 4: PER-STRATEGY EXECUTIONS GENERATION
        // Generate executions for each strategy based on their position changes
        // Load previous positions per-strategy (Option A)
        // ========================================
        INFO("PHASE 4: Generating per-strategy executions...");

        // Load previous day per-strategy positions
        INFO("DEBUG PHASE 4: Loading previous day positions per-strategy...");
        std::unordered_map<std::string, std::unordered_map<std::string, Position>>
            previous_strategy_positions;

        for (const auto& [strategy_name, _] : strategy_positions_map) {
            // Load previous positions filtering by BOTH combined_strategy_id AND individual
            // strategy_name to ensure we only get positions from this specific run
            auto prev_result =
                db->load_positions_by_date(combined_strategy_id,  // Combined strategy_id
                                           strategy_name,         // Individual strategy_name
                                           coordinator_config.portfolio_id,  // Portfolio ID
                                           previous_date, "trading.positions");

            if (prev_result.is_ok()) {
                previous_strategy_positions[strategy_name] = prev_result.value();
                INFO("DEBUG PHASE 4: Loaded " + std::to_string(prev_result.value().size()) +
                     " previous positions for strategy: " + strategy_name);

                // Log individual previous positions for debugging
                for (const auto& [symbol, pos] : prev_result.value()) {
                    DEBUG("DEBUG PHASE 4: Previous " + strategy_name + " - " + symbol +
                          " qty=" + std::to_string(pos.quantity.as_double()));
                }
            } else {
                INFO("No previous positions found for strategy: " + strategy_name +
                     " (first run or no data): " + std::string(prev_result.error()->what()));
                previous_strategy_positions[strategy_name] = {};
            }
        }

        // Generate executions for each strategy
        std::unordered_map<std::string, std::vector<ExecutionReport>> all_strategy_executions;
        int total_executions = 0;
        // total_daily_transaction_costs already declared earlier at line 881

        for (const auto& [strategy_name, current_positions_map] : strategy_positions_map) {
            auto prev_positions_map = previous_strategy_positions[strategy_name];

            INFO("DEBUG PHASE 4: Generating executions for strategy '" + strategy_name +
                 "' (current=" + std::to_string(current_positions_map.size()) +
                 ", previous=" + std::to_string(prev_positions_map.size()) + ")");

            auto exec_result = execution_manager->generate_daily_executions(
                current_positions_map, prev_positions_map, previous_day_close_prices, now);

            if (exec_result.is_ok()) {
                std::vector<ExecutionReport> strategy_executions = exec_result.value();

                INFO("DEBUG PHASE 4: Strategy '" + strategy_name + "' generated " +
                     std::to_string(strategy_executions.size()) + " executions");

                // Log each execution for debugging
                for (const auto& exec : strategy_executions) {
                    INFO("DEBUG PHASE 4: " + strategy_name + " execution - " + exec.symbol + " " +
                         (exec.side == Side::BUY ? "BUY" : "SELL") + " " +
                         std::to_string(exec.filled_quantity.as_double()) + " @ " +
                         std::to_string(exec.fill_price) + " commission=$" +
                         std::to_string(exec.total_transaction_costs.as_double()));

                    total_daily_transaction_costs += exec.total_transaction_costs.as_double();
                }

                all_strategy_executions[strategy_name] = strategy_executions;
                total_executions += strategy_executions.size();
            } else {
                ERROR("Failed to generate executions for strategy " + strategy_name + ": " +
                      std::string(exec_result.error()->what()));
                all_strategy_executions[strategy_name] = {};
            }
        }

        INFO("PHASE 4: Total executions across all strategies: " +
             std::to_string(total_executions));
        INFO("PHASE 4: Total daily transaction costs: $" +
             std::to_string(total_daily_transaction_costs));

        // Store executions for each strategy
        for (const auto& [strategy_name, executions] : all_strategy_executions) {
            if (!executions.empty()) {
                // Before inserting, delete any stale executions for today with the same order_ids
                try {
                    // Build unique order_id list
                    std::set<std::string> unique_order_ids;
                    for (const auto& exec : executions) {
                        unique_order_ids.insert(exec.order_id);
                    }

                    if (!unique_order_ids.empty()) {
                        // Convert set to vector for the delete method
                        std::vector<std::string> order_ids_vector(unique_order_ids.begin(),
                                                                  unique_order_ids.end());

                        INFO("Deleting stale executions for strategy " + strategy_name + " with " +
                             std::to_string(order_ids_vector.size()) + " order_ids");

                        // Use the delete_stale_executions method with strategy name
                        auto del_res = db->delete_stale_executions(
                            order_ids_vector, now, strategy_name, "trading.executions");
                        if (del_res.is_error()) {
                            WARN("Failed to delete stale executions for strategy " + strategy_name +
                                 ": " + std::string(del_res.error()->what()));
                        } else {
                            INFO("Stale executions (if any) deleted successfully for strategy: " +
                                 strategy_name);
                        }
                    }
                } catch (const std::exception& e) {
                    WARN("Exception while deleting stale executions for strategy " + strategy_name +
                         ": " + std::string(e.what()));
                }

                // Store executions with combined strategy_id and individual strategy_name
                auto save_result =
                    db->store_executions(executions,
                                         combined_strategy_id,  // Combined strategy_id for tier 2
                                         strategy_name,  // Individual strategy_name for tier 3
                                         portfolio_id,   // Portfolio identifier
                                         "trading.executions");

                if (save_result.is_error()) {
                    ERROR("Failed to store executions for strategy " + strategy_name + ": " +
                          std::string(save_result.error()->what()));
                } else {
                    INFO("Successfully stored " + std::to_string(executions.size()) +
                         " executions for strategy: " + strategy_name);
                }
            } else {
                INFO("No executions to store for strategy: " + strategy_name);
            }
        }

        std::cout << "\n======= Daily Position Report =======" << std::endl;
        std::cout << "Date: " << (now_tm->tm_year + 1900) << "-" << std::setfill('0')
                  << std::setw(2) << (now_tm->tm_mon + 1) << "-" << std::setfill('0')
                  << std::setw(2) << now_tm->tm_mday << std::endl;
        std::cout << "Total Positions: " << positions.size() << std::endl;
        std::cout << std::endl;

        // Add header for position table
        std::cout << std::setw(10) << "Symbol"
                  << " | " << std::setw(10) << "Quantity"
                  << " | " << std::setw(10) << "Mkt Price"
                  << " | " << std::setw(12) << "Notional"
                  << " | " << std::setw(10) << "Unreal PnL" << std::endl;
        std::cout << std::string(60, '-') << std::endl;

        // Use MarginManager for margin calculations
        INFO("Using MarginManager to calculate margin requirements...");

        auto margin_result = margin_manager->calculate_margin_requirements(
            positions, previous_day_close_prices, initial_capital);

        double gross_notional = 0.0;
        double net_notional = 0.0;
        double total_posted_margin = 0.0;  // Sum of per-contract initial margins times contracts
        double maintenance_requirement_today =
            0.0;  // Sum of per-contract maintenance margins times contracts
        int active_positions = 0;

        if (margin_result.is_ok()) {
            auto& metrics = margin_result.value();
            gross_notional = metrics.gross_notional;
            net_notional = metrics.net_notional;
            active_positions = metrics.active_positions;
            total_posted_margin = metrics.total_posted_margin;
            maintenance_requirement_today = metrics.maintenance_requirement;

            INFO("MarginManager calculated: gross_notional=$" + std::to_string(gross_notional) +
                 ", posted_margin=$" + std::to_string(total_posted_margin) +
                 ", active_positions=" + std::to_string(active_positions));
        } else {
            ERROR("MarginManager failed: " + std::string(margin_result.error()->what()));
            // No fallback - component is required to work
            throw std::runtime_error("MarginManager failed");
        }

        std::cout << std::endl;
        std::cout << "Active Positions: " << active_positions << std::endl;
        std::cout << "Gross Notional: $" << std::fixed << std::setprecision(2) << gross_notional
                  << std::endl;
        std::cout << "Net Notional: $" << std::fixed << std::setprecision(2) << net_notional
                  << std::endl;
        std::cout << "Portfolio Leverage (gross/current): " << std::fixed << std::setprecision(2)
                  << (gross_notional / initial_capital) << "x" << std::endl;
        // Posted margin should never be zero if there are active positions; enforce and warn
        if (active_positions > 0 && total_posted_margin <= 0.0) {
            ERROR(
                "Computed posted margin is non-positive while positions are active. Check "
                "instrument metadata.");
        }
        // Equity-to-Margin Ratio = gross_notional / total_posted_margin
        // This metric shows how many times the gross notional exposure is covered by posted margin
        // Higher values indicate more leverage relative to margin requirements
        double equity_to_margin_ratio =
            (total_posted_margin > 0.0) ? (gross_notional / total_posted_margin) : 0.0;
        if (equity_to_margin_ratio <= 1.0 && active_positions > 0) {
            WARN(
                "Equity-to-Margin Ratio (gross_notional / posted_margin) is <= 1.0; verify "
                "margins.");
        }

        // ========================================
        // PHASE 4: PER-STRATEGY POSITIONS STORAGE
        // Extract per-strategy positions from PortfolioManager
        // Each strategy's positions are stored separately with strategy_name tag
        // strategy_positions_map already extracted above for use by executions section
        // ========================================
        INFO("PHASE 4: Storing per-strategy positions to database...");

        // Store positions for each strategy with strategy_name tag
        int total_positions_saved = 0;
        for (const auto& [strategy_name, positions_map] : strategy_positions_map) {
            std::vector<Position> strategy_positions_vec;
            strategy_positions_vec.reserve(positions_map.size());

            INFO("DEBUG PHASE 4: Strategy '" + strategy_name + "' has " +
                 std::to_string(positions_map.size()) + " positions");

            for (const auto& [symbol, pos] : positions_map) {
                // Only save positions with non-zero quantity
                // Zero-quantity positions (closed positions) should NOT be stored
                bool has_quantity = std::abs(pos.quantity.as_double()) > 1e-10;

                if (!has_quantity) {
                    DEBUG("Skipping zero-quantity position: " + symbol);
                    continue;
                }

                // Create a new position with validated values
                Position validated_position;
                validated_position.symbol = pos.symbol;
                validated_position.quantity = pos.quantity;
                validated_position.last_update = now;  // Use current timestamp

                // CRITICAL: For PnL lag model, Day T positions must have ZERO PnL (placeholders)
                // The PnL will be finalized tomorrow when we run for Day T+1
                // Do NOT use pos.realized_pnl which contains calculated PnL from strategy
                // processing
                validated_position.realized_pnl =
                    Decimal(0.0);  // PLACEHOLDER - will be finalized tomorrow
                validated_position.unrealized_pnl = Decimal(0.0);  // Always 0 for futures

                // For Day T positions, average_price should be Day T-1 close (entry price)
                // This is the price at which positions were "executed" (opened at yesterday's
                // close)
                double avg_price_double =
                    previous_day_close_prices.find(symbol) != previous_day_close_prices.end()
                        ? previous_day_close_prices[symbol]
                        : static_cast<double>(pos.average_price);

                // Decimal limit is approximately 92,233,720,368,547.75807
                const double DECIMAL_MAX = 9.223372036854775807e13;  // INT64_MAX / SCALE
                if (avg_price_double > DECIMAL_MAX || avg_price_double < -DECIMAL_MAX) {
                    WARN("Position " + symbol + " has average_price " +
                         std::to_string(avg_price_double) +
                         " which exceeds Decimal limit, using Day T-1 close instead");
                    // Use Day T-1 close if available
                    if (previous_day_close_prices.find(symbol) != previous_day_close_prices.end()) {
                        validated_position.average_price =
                            Decimal(previous_day_close_prices[symbol]);
                    } else {
                        validated_position.average_price = Decimal(1.0);
                    }
                } else {
                    try {
                        validated_position.average_price = pos.average_price;
                    } catch (const std::exception& e) {
                        ERROR("Failed to validate average_price for " + symbol + ": " +
                              std::string(e.what()));
                        if (previous_day_close_prices.find(symbol) !=
                            previous_day_close_prices.end()) {
                            validated_position.average_price =
                                Decimal(previous_day_close_prices[symbol]);
                        } else {
                            validated_position.average_price = Decimal(1.0);
                        }
                    }
                }

                strategy_positions_vec.push_back(validated_position);

                INFO("DEBUG PHASE 4: " + strategy_name + " - " + symbol + " qty=" +
                     std::to_string(validated_position.quantity.as_double()) + " avg_price=" +
                     std::to_string(static_cast<double>(validated_position.average_price)) +
                     " realized_pnl=" +
                     std::to_string(static_cast<double>(validated_position.realized_pnl)));
            }

            if (!strategy_positions_vec.empty()) {
                INFO("Attempting to save " + std::to_string(strategy_positions_vec.size()) +
                     " positions for strategy: " + strategy_name);
                DEBUG("Database connection status: " +
                      std::string(db->is_connected() ? "connected" : "disconnected"));

                // Store with combined strategy_id and individual strategy_name
                auto save_result =
                    db->store_positions(strategy_positions_vec,
                                        combined_strategy_id,  // Combined strategy_id for tier 2
                                        strategy_name,  // Individual strategy_name for tier 3
                                        portfolio_id,   // Portfolio identifier
                                        "trading.positions");

                if (save_result.is_error()) {
                    ERROR("Failed to store positions for strategy " + strategy_name + ": " +
                          std::string(save_result.error()->what()));
                } else {
                    INFO("Successfully stored " + std::to_string(strategy_positions_vec.size()) +
                         " positions for strategy: " + strategy_name);
                    total_positions_saved += strategy_positions_vec.size();
                }
            } else {
                INFO("No non-zero positions to store for strategy: " + strategy_name);
            }
        }

        INFO("PHASE 4: Total positions saved across all strategies: " +
             std::to_string(total_positions_saved));

        // Compute portfolio-level snapshot metrics using RiskManager on today's state
        INFO("Retrieving strategy metrics...");
        trade_ngin::RiskManager snapshot_rm(risk_config);
        auto market_data_snapshot = snapshot_rm.create_market_data(all_bars);
        auto risk_eval = snapshot_rm.process_positions(positions, market_data_snapshot);

        std::cout << "\n======= Strategy Metrics =======" << std::endl;
        if (risk_eval.is_ok()) {
            const auto& r = risk_eval.value();
            // Use portfolio_var as annualized volatility proxy
            std::cout << "Volatility: " << std::fixed << std::setprecision(2)
                      << (r.portfolio_var * 100.0) << "%" << std::endl;
            std::cout << "Gross Leverage: " << std::fixed << std::setprecision(2)
                      << r.gross_leverage << std::endl;
            std::cout << "Net Leverage: " << std::fixed << std::setprecision(2) << r.net_leverage
                      << std::endl;
            std::cout << "Max Correlation: " << std::fixed << std::setprecision(2)
                      << r.correlation_risk << std::endl;
            std::cout << "Jump Risk (99th): " << std::fixed << std::setprecision(2) << r.jump_risk
                      << std::endl;
            std::cout << "Risk Scale: " << std::fixed << std::setprecision(2) << r.recommended_scale
                      << std::endl;
        } else {
            std::cout << "Volatility: N/A" << std::endl;
            std::cout << "Gross Leverage: N/A" << std::endl;
            std::cout << "Net Leverage: N/A" << std::endl;
            std::cout << "Max Correlation: N/A" << std::endl;
            std::cout << "Jump Risk (99th): N/A" << std::endl;
            std::cout << "Risk Scale: N/A" << std::endl;
        }
        // ========================================
        // STEP 3: CALCULATE TRANSACTION COSTS AND Day T PnL (ZERO)
        // ========================================
        INFO("STEP 3: Calculating transaction costs and Day T PnL...");

        // total_daily_transaction_costs already calculated in per-strategy executions loop above
        INFO("Total daily transaction costs (from per-strategy executions): $" +
             std::to_string(total_daily_transaction_costs));

        // Day T PnL is ZERO (placeholder) - positions were just opened at Day T-1 close
        // Update PnLManager with today's positions (all with 0 PnL as placeholders)
        for (const auto& [symbol, position] : positions) {
            pnl_manager->update_position_pnl(symbol, 0.0, 0.0);  // Zero PnL for Day T
        }

        double daily_realized_pnl = 0.0;
        double daily_unrealized_pnl = 0.0;
        double daily_pnl_for_today = -total_daily_transaction_costs;  // Only transaction costs on Day T

        INFO("Day T PnL (placeholder): $0.00");
        INFO("Day T transaction costs: $" + std::to_string(total_daily_transaction_costs));
        INFO("Day T total impact: $" + std::to_string(daily_pnl_for_today));

        // ========================================
        // STEP 4: UPDATE Day T-1 live_results AND equity_curve WITH FINALIZED PnL
        // ========================================
        // Skip if this is the first trading day (no previous positions to finalize)
        bool is_first_trading_day =
            previous_positions.empty() ||
            (previous_positions.size() > 0 &&
             std::all_of(previous_positions.begin(), previous_positions.end(),
                         [](const auto& p) { return p.second.quantity.as_double() == 0.0; }));

        // Declare yesterday's daily metrics outside the block so they're available for email
        double yesterday_daily_return_for_email = 0.0;
        double yesterday_daily_pnl_for_email = 0.0;
        double yesterday_realized_pnl_for_email = 0.0;
        double yesterday_unrealized_pnl_for_email = 0.0;

        if (!two_days_ago_close_prices.empty() && aggregate_yesterday_total_pnl != 0.0 &&
            !is_first_trading_day) {
            INFO("STEP 4: Updating Day T-1 live_results with finalized PnL: $" +
                 std::to_string(aggregate_yesterday_total_pnl));

            // Get yesterday's transaction costs and other existing metrics from database
            double yesterday_transaction_costs = 0.0;
            double yesterday_total_transaction_costs = 0.0;
            double yesterday_gross_notional = 0.0;
            double yesterday_net_notional = 0.0;
            int yesterday_active_positions = 0;
            double yesterday_margin_posted = 0.0;

            std::stringstream yesterday_date_ss;
            auto yesterday_time_t = std::chrono::system_clock::to_time_t(previous_date);
            yesterday_date_ss << std::put_time(std::gmtime(&yesterday_time_t), "%Y-%m-%d");

            // Use LiveDataLoader to get yesterday's metrics
            try {
                INFO("Using LiveDataLoader to query yesterday's metrics for date: " +
                     yesterday_date_ss.str());
                auto live_results = data_loader->load_live_results(
                    combined_strategy_id, coordinator_config.portfolio_id, previous_date);

                if (live_results.is_ok()) {
                    auto& row = live_results.value();
                    yesterday_transaction_costs = row.daily_transaction_costs;
                    yesterday_total_transaction_costs =
                        row.daily_transaction_costs;  // Note: total_transaction_costs field may not exist
                    yesterday_gross_notional = row.gross_notional;
                    yesterday_net_notional =
                        row.gross_notional;  // Note: using gross_notional as net_notional not in
                                             // LiveResultsRow
                    yesterday_active_positions = row.active_positions;
                    yesterday_margin_posted = row.margin_posted;

                    INFO("Successfully loaded yesterday's metrics via LiveDataLoader:");
                    INFO("  yesterday_transaction_costs: $" +
                         std::to_string(yesterday_transaction_costs));
                    INFO("  yesterday_gross_notional: $" +
                         std::to_string(yesterday_gross_notional));
                    INFO("  yesterday_margin_posted: $" + std::to_string(yesterday_margin_posted));
                } else {
                    WARN("LiveDataLoader failed to get yesterday's metrics: " +
                         std::string(live_results.error()->what()));
                    INFO("Using default values (0) for yesterday's metrics");
                }
            } catch (const std::exception& e) {
                WARN("Failed to get yesterday's metrics: " + std::string(e.what()));
            }

            // Use the commission value already loaded from LiveDataLoader
            double yesterday_transaction_costs_for_calc = yesterday_transaction_costs;
            INFO("Using yesterday_transaction_costs_for_calc from LiveDataLoader: $" +
                 std::to_string(yesterday_transaction_costs_for_calc));

            // Use the queried value from earlier (which may be 0 if query failed)
            double yesterday_daily_pnl_finalized =
                aggregate_yesterday_total_pnl - yesterday_transaction_costs;

            INFO("Day T-1 PnL breakdown:");
            INFO("  Position PnL (aggregate_yesterday_total_pnl): $" +
                 std::to_string(aggregate_yesterday_total_pnl));
            INFO("  Transaction costs (yesterday_transaction_costs): $" +
                 std::to_string(yesterday_transaction_costs));
            INFO("  Net PnL (yesterday_daily_pnl_finalized): $" +
                 std::to_string(yesterday_daily_pnl_finalized));

            // Get the day BEFORE yesterday's portfolio value, total_pnl, and total_transaction_costs
            double day_before_yesterday_portfolio_value = initial_capital;
            double day_before_aggregate_yesterday_total_pnl = 0.0;
            double day_before_yesterday_total_transaction_costs = 0.0;
            try {
                auto db_ptr = std::dynamic_pointer_cast<PostgresDatabase>(db);
                if (db_ptr) {
                    auto prev_agg = db_ptr->get_previous_live_aggregates(
                        combined_strategy_id, coordinator_config.portfolio_id, previous_date,
                        "trading.live_results");
                    if (prev_agg.is_ok()) {
                        std::tie(day_before_yesterday_portfolio_value,
                                 day_before_aggregate_yesterday_total_pnl,
                                 day_before_yesterday_total_transaction_costs) = prev_agg.value();
                        INFO("Loaded day-before-yesterday aggregates: portfolio=$" +
                             std::to_string(day_before_yesterday_portfolio_value) +
                             ", total_pnl=$" +
                             std::to_string(day_before_aggregate_yesterday_total_pnl) +
                             ", total_transaction_costs=$" +
                             std::to_string(day_before_yesterday_total_transaction_costs));
                    }
                }
            } catch (const std::exception& e) {
                INFO("Could not load day-before-yesterday aggregates: " + std::string(e.what()));
            }

            // Calculate yesterday's cumulative values
            // NOTE: Since we may not have correct transaction costs, the cumulative values will be
            // recalculated by SQL using the daily_pnl formula (daily_realized_pnl -
            // daily_transaction_costs)
            double aggregate_yesterday_total_pnl_cumulative =
                day_before_aggregate_yesterday_total_pnl + yesterday_daily_pnl_finalized;
            double yesterday_total_transaction_costs_cumulative =
                day_before_yesterday_total_transaction_costs + yesterday_transaction_costs;
            double yesterday_total_realized_pnl_cumulative =
                aggregate_yesterday_total_pnl_cumulative +
                yesterday_total_transaction_costs_cumulative;
            double yesterday_portfolio_value_finalized =
                day_before_yesterday_portfolio_value + yesterday_daily_pnl_finalized;

            // Calculate yesterday's returns using LiveMetricsCalculator
            double yesterday_daily_return = metrics_calculator->calculate_daily_return(
                yesterday_daily_pnl_finalized, day_before_yesterday_portfolio_value);

            // Note: Yesterday's metrics for email will be loaded from database after update

            // Calculate yesterday's total cumulative return (non-annualized)
            double yesterday_total_cumulative_return = metrics_calculator->calculate_total_return(
                yesterday_portfolio_value_finalized, initial_capital);

            double yesterday_total_return_decimal = 0.0;
            if (initial_capital > 0.0) {
                yesterday_total_return_decimal =
                    (yesterday_portfolio_value_finalized - initial_capital) / initial_capital;
            }
            double yesterday_total_cumulative_return_pct =
                yesterday_total_cumulative_return;  // Already in %

            // Get trading days count for annualization using PostgreSQL function
            // This avoids issues with row multiplication/duplication in the database
            // Uses trading.strategy_trading_days_metadata table for live_start_date
            int trading_days_count = 1;
            try {
                // Call PostgreSQL function to calculate trading days
                std::string trading_days_query = "SELECT trading.get_trading_days('" +
                                                 combined_strategy_id + "', DATE '" +
                                                 yesterday_date_ss.str() + "')";

                INFO("TRADING_DAYS_CALC [Day T-1]: Querying trading days...");
                INFO("TRADING_DAYS_CALC [Day T-1]: Query: " + trading_days_query);
                INFO("TRADING_DAYS_CALC [Day T-1]: Strategy ID: " + combined_strategy_id);
                INFO("TRADING_DAYS_CALC [Day T-1]: Target Date: " + yesterday_date_ss.str());

                auto trading_days_result = db->execute_query(trading_days_query);

                if (trading_days_result.is_ok()) {
                    auto table = trading_days_result.value();
                    if (table && table->num_rows() > 0 && table->num_columns() > 0) {
                        // execute_query returns StringArray for all columns
                        auto arr = std::static_pointer_cast<arrow::StringArray>(
                            table->column(0)->chunk(0));
                        if (arr && arr->length() > 0 && !arr->IsNull(0)) {
                            trading_days_count = std::max<int>(1, std::stoi(arr->GetString(0)));
                            INFO("TRADING_DAYS_CALC [Day T-1]: Result from DB: " +
                                 std::to_string(trading_days_count) + " trading days");
                            INFO(
                                "TRADING_DAYS_CALC [Day T-1]: This value comes from "
                                "strategy_trading_days_metadata.live_start_date");
                        }
                    }
                } else {
                    WARN("TRADING_DAYS_CALC [Day T-1]: Could not call get_trading_days function: " +
                         std::string(trading_days_result.error()->what()));
                }
            } catch (const std::exception& e) {
                WARN("TRADING_DAYS_CALC [Day T-1]: Failed to get trading days: " +
                     std::string(e.what()));
            }

            // Calculate yesterday's annualized return using LiveMetricsCalculator
            // Formula: annualized_return = ((1 + total_return)^(252/trading_days) - 1) * 100
            INFO("ANNUALIZED_RETURN_CALC [Day T-1]: Calculating annualized return...");
            INFO("ANNUALIZED_RETURN_CALC [Day T-1]: Input: total_return_decimal = " +
                 std::to_string(yesterday_total_return_decimal) + " (" +
                 std::to_string(yesterday_total_return_decimal * 100.0) + "%)");
            INFO("ANNUALIZED_RETURN_CALC [Day T-1]: Input: trading_days_count = " +
                 std::to_string(trading_days_count));
            INFO("ANNUALIZED_RETURN_CALC [Day T-1]: Formula: ((1 + " +
                 std::to_string(yesterday_total_return_decimal) + ")^(252/" +
                 std::to_string(trading_days_count) + ") - 1) * 100");

            double yesterday_total_return_annualized =
                metrics_calculator->calculate_annualized_return(yesterday_total_return_decimal,
                                                                trading_days_count);

            INFO("ANNUALIZED_RETURN_CALC [Day T-1]: Result: " +
                 std::to_string(yesterday_total_return_annualized) + "%");

            // Calculate yesterday's leverage and risk metrics
            // IMPORTANT: We MUST preserve existing values from the database
            // These were calculated correctly when Day T-1 was originally processed
            double yesterday_portfolio_leverage = 0.0;
            double yesterday_equity_to_margin_ratio = 0.0;

            // Load existing values from database using LiveDataLoader - DO NOT RECALCULATE
            try {
                auto margin_metrics = data_loader->load_margin_metrics(
                    combined_strategy_id, coordinator_config.portfolio_id, previous_date);
                if (margin_metrics.is_ok() && margin_metrics.value().valid) {
                    auto& metrics = margin_metrics.value();
                    yesterday_portfolio_leverage = metrics.portfolio_leverage;
                    yesterday_equity_to_margin_ratio = metrics.equity_to_margin_ratio;

                    // Also update the gross_notional and margin_posted if available
                    yesterday_gross_notional = metrics.gross_notional;
                    yesterday_margin_posted = metrics.margin_posted;

                    INFO("Preserved existing metrics from database via LiveDataLoader: leverage=" +
                         std::to_string(yesterday_portfolio_leverage) +
                         ", equity_to_margin=" + std::to_string(yesterday_equity_to_margin_ratio) +
                         ", gross_notional=" + std::to_string(yesterday_gross_notional) +
                         ", margin_posted=" + std::to_string(yesterday_margin_posted));
                } else {
                    INFO("No existing margin metrics found for yesterday via LiveDataLoader");
                }
            } catch (const std::exception& e) {
                WARN("Failed to load existing metrics: " + std::string(e.what()));
            }

            // DO NOT recalculate these values - they should remain as loaded from database
            // These values were correctly calculated when the day was originally processed
            double yesterday_cash_available =
                yesterday_portfolio_value_finalized - yesterday_margin_posted;

            // UPDATE yesterday's live_results with ALL recalculated metrics
            // Note: We calculate daily_pnl, total_pnl, and current_portfolio_value in SQL
            // to properly incorporate the EXISTING daily_transaction_costs value
            // IMPORTANT: Only update portfolio_leverage and equity_to_margin_ratio if they are NULL
            // or 0
            std::string update_query =
                "WITH day_before AS ("
                "  SELECT COALESCE(current_portfolio_value, " +
                std::to_string(initial_capital) +
                ") as portfolio, "
                "         COALESCE(total_pnl, 0.0) as total_pnl, "
                "         COALESCE(total_realized_pnl, 0.0) as total_realized_pnl_prev "
                "  FROM trading.live_results "
                "  WHERE strategy_id = '" +
                combined_strategy_id + "' AND portfolio_id = '" + coordinator_config.portfolio_id +
                "' AND DATE(date) < '" + yesterday_date_ss.str() +
                "' "
                "  ORDER BY date DESC LIMIT 1"
                ") "
                "UPDATE trading.live_results SET "
                "daily_realized_pnl = " +
                std::to_string(aggregate_yesterday_total_pnl) +
                ", "
                "daily_pnl = " +
                std::to_string(aggregate_yesterday_total_pnl) +
                " - COALESCE(daily_transaction_costs, 0.0), "
                "total_pnl = COALESCE((SELECT total_pnl FROM day_before), 0.0) + (" +
                std::to_string(aggregate_yesterday_total_pnl) +
                " - COALESCE(daily_transaction_costs, 0.0)), "
                "total_realized_pnl = " +
                std::to_string(yesterday_total_realized_pnl_cumulative) +
                ", "
                "current_portfolio_value = COALESCE((SELECT portfolio FROM day_before), " +
                std::to_string(initial_capital) + ") + (" +
                std::to_string(aggregate_yesterday_total_pnl) +
                " - COALESCE(daily_transaction_costs, 0.0)), "
                "daily_return = CASE WHEN COALESCE((SELECT portfolio FROM day_before), " +
                std::to_string(initial_capital) +
                ") > 0 "
                "               THEN ((" +
                std::to_string(aggregate_yesterday_total_pnl) +
                " - COALESCE(daily_transaction_costs, 0.0)) / COALESCE((SELECT portfolio FROM "
                "day_before), " +
                std::to_string(initial_capital) +
                ")) * 100.0 "
                "               ELSE 0.0 END, "
                "total_cumulative_return = " +
                std::to_string(yesterday_total_cumulative_return_pct) +
                ", "
                "total_annualized_return = " +
                std::to_string(yesterday_total_return_annualized) +
                ", "
                "portfolio_leverage = CASE WHEN portfolio_leverage IS NULL OR portfolio_leverage = "
                "0 THEN " +
                std::to_string(yesterday_portfolio_leverage) +
                " ELSE portfolio_leverage END, "
                "equity_to_margin_ratio = CASE WHEN equity_to_margin_ratio IS NULL OR "
                "equity_to_margin_ratio = 0 THEN " +
                std::to_string(yesterday_equity_to_margin_ratio) +
                " ELSE equity_to_margin_ratio END, "
                "cash_available = COALESCE((SELECT portfolio FROM day_before), " +
                std::to_string(initial_capital) + ") + (" +
                std::to_string(aggregate_yesterday_total_pnl) +
                " - COALESCE(daily_transaction_costs, 0.0)) - COALESCE(margin_posted, 0.0) "
                "WHERE strategy_id = '" +
                combined_strategy_id + "' AND portfolio_id = '" + coordinator_config.portfolio_id +
                "' AND DATE(date) = '" + yesterday_date_ss.str() + "'";

            INFO("Executing UPDATE query for Day T-1 live_results...");
            INFO("UPDATE will set current_portfolio_value for date: " + yesterday_date_ss.str());

            auto update_result = db->execute_direct_query(update_query);
            if (update_result.is_error()) {
                ERROR("Failed to update Day T-1 live_results: " +
                      std::string(update_result.error()->what()));
            } else {
                INFO(
                    "Successfully updated Day T-1 live_results with finalized PnL and all metrics");

                // Log the expected value
                INFO(
                    "Expected current_portfolio_value calculation: day_before_portfolio + "
                    "(yesterday_pnl - commissions)");
                INFO("  aggregate_yesterday_total_pnl: $" +
                     std::to_string(aggregate_yesterday_total_pnl));
                INFO("  yesterday_transaction_costs: $" +
                     std::to_string(yesterday_transaction_costs_for_calc));
            }

            // UPDATE yesterday's equity_curve using LiveResultsManager
            INFO("Updating Day T-1 equity_curve...");

            // Query the current portfolio value from updated live_results
            std::string get_equity_query =
                "SELECT current_portfolio_value FROM trading.live_results "
                "WHERE strategy_id = '" +
                combined_strategy_id + "' AND portfolio_id = '" + coordinator_config.portfolio_id +
                "' AND DATE(date) = '" + yesterday_date_ss.str() + "'";

            INFO("Querying for portfolio value with date: " + yesterday_date_ss.str());

            auto equity_result = db->execute_query(get_equity_query);
            if (equity_result.is_error()) {
                ERROR("Failed to get portfolio value for equity update: " +
                      std::string(equity_result.error()->what()));
            } else {
                auto table = equity_result.value();
                INFO("Query returned " + std::to_string(table->num_rows()) + " rows");

                if (table->num_rows() > 0) {
                    // NOTE: execute_query returns StringArray for all columns
                    auto array =
                        std::static_pointer_cast<arrow::StringArray>(table->column(0)->chunk(0));

                    // Check for NULL value before reading
                    if (array->IsNull(0)) {
                        ERROR(
                            "Cannot update Day T-1 equity_curve: current_portfolio_value is NULL "
                            "for date " +
                            yesterday_date_ss.str());
                    } else {
                        // Parse string to double
                        double portfolio_value = std::stod(array->GetString(0));
                        INFO("Raw value read from database: " + std::to_string(portfolio_value));

                        // Validate the value before using it
                        if (portfolio_value <= 0.0 || std::isnan(portfolio_value) ||
                            std::isinf(portfolio_value) || portfolio_value < 1000.0) {
                            ERROR("Invalid portfolio value for Day T-1 equity update: " +
                                  std::to_string(portfolio_value) + " (date: " +
                                  yesterday_date_ss.str() + "). Skipping equity_curve update.");
                            ERROR("  Validation failed: <= 0.0? " +
                                  std::string(portfolio_value <= 0.0 ? "YES" : "NO") + ", isnan? " +
                                  std::string(std::isnan(portfolio_value) ? "YES" : "NO") +
                                  ", isinf? " +
                                  std::string(std::isinf(portfolio_value) ? "YES" : "NO") +
                                  ", < 1000? " +
                                  std::string(portfolio_value < 1000.0 ? "YES" : "NO"));
                        } else {
                            INFO("✓ Valid portfolio value for Day T-1: $" +
                                 std::to_string(portfolio_value));

                            // DEBUG: Log the exact timestamp being used for the update
                            auto prev_time_t = std::chrono::system_clock::to_time_t(previous_date);
                            std::stringstream prev_ts_ss;
                            prev_ts_ss
                                << std::put_time(std::gmtime(&prev_time_t), "%Y-%m-%d %H:%M:%S");
                            INFO("DEBUG: previous_date timestamp for equity curve update: " +
                                 prev_ts_ss.str());

                            // DEBUG: Query existing equity_curve timestamp for this date
                            std::string debug_eq_query =
                                "SELECT timestamp, equity FROM trading.equity_curve "
                                "WHERE strategy_id = '" +
                                combined_strategy_id + "' AND portfolio_id = '" +
                                coordinator_config.portfolio_id +
                                "' "
                                "AND DATE(timestamp) = '" +
                                yesterday_date_ss.str() +
                                "' "
                                "ORDER BY timestamp";
                            auto debug_result = db->execute_query(debug_eq_query);
                            if (debug_result.is_ok() && debug_result.value()->num_rows() > 0) {
                                INFO("DEBUG: Existing equity_curve entries for " +
                                     yesterday_date_ss.str() + ":");
                                auto debug_table = debug_result.value();
                                for (int64_t i = 0; i < debug_table->num_rows(); ++i) {
                                    auto ts_arr = std::static_pointer_cast<arrow::StringArray>(
                                        debug_table->column(0)->chunk(0));
                                    // execute_query returns StringArray for all columns
                                    auto eq_arr = std::static_pointer_cast<arrow::StringArray>(
                                        debug_table->column(1)->chunk(0));
                                    if (!ts_arr->IsNull(i) && !eq_arr->IsNull(i)) {
                                        INFO("DEBUG:   Existing row: timestamp=" +
                                             ts_arr->GetString(i) +
                                             ", equity=" + eq_arr->GetString(i));
                                    }
                                }
                            } else {
                                INFO("DEBUG: No existing equity_curve entry found for " +
                                     yesterday_date_ss.str());
                            }

                            // Create a temporary LiveResultsManager for Day T-1 equity update
                            auto yesterday_manager = std::make_unique<LiveResultsManager>(
                                db, true, combined_strategy_id, coordinator_config.portfolio_id);
                            yesterday_manager->set_equity(portfolio_value);

                            auto update_equity_result =
                                yesterday_manager->save_equity_curve(previous_date);
                            if (update_equity_result.is_error()) {
                                ERROR("Failed to update Day T-1 equity_curve: " +
                                      std::string(update_equity_result.error()->what()));
                            } else {
                                INFO("Successfully updated Day T-1 equity_curve with value: " +
                                     std::to_string(portfolio_value));

                                // DEBUG: Verify what was actually saved
                                auto verify_result = db->execute_query(debug_eq_query);
                                if (verify_result.is_ok() &&
                                    verify_result.value()->num_rows() > 0) {
                                    INFO("DEBUG: After update, equity_curve entries for " +
                                         yesterday_date_ss.str() + ":");
                                    auto verify_table = verify_result.value();
                                    for (int64_t i = 0; i < verify_table->num_rows(); ++i) {
                                        auto ts_arr = std::static_pointer_cast<arrow::StringArray>(
                                            verify_table->column(0)->chunk(0));
                                        // execute_query returns StringArray for all columns
                                        auto eq_arr = std::static_pointer_cast<arrow::StringArray>(
                                            verify_table->column(1)->chunk(0));
                                        if (!ts_arr->IsNull(i) && !eq_arr->IsNull(i)) {
                                            INFO("DEBUG:   Row after update: timestamp=" +
                                                 ts_arr->GetString(i) +
                                                 ", equity=" + eq_arr->GetString(i));
                                        }
                                    }
                                }
                            }
                        }
                    }
                } else {
                    WARN("No live_results found for date " + yesterday_date_ss.str() +
                         ", skipping equity_curve update");
                }
            }

            // Load updated metrics from database for email - MUST do this AFTER the UPDATE
            try {
                std::string metrics_query =
                    "SELECT daily_return, daily_pnl, daily_realized_pnl, daily_unrealized_pnl, "
                    "portfolio_leverage, equity_to_margin_ratio "
                    "FROM trading.live_results "
                    "WHERE strategy_id = '" +
                    combined_strategy_id + "' AND portfolio_id = '" +
                    coordinator_config.portfolio_id + "' AND DATE(date) = '" +
                    yesterday_date_ss.str() + "'";

                INFO("Loading yesterday's metrics from database with query: " + metrics_query);
                auto metrics_result = db->execute_query(metrics_query);

                if (metrics_result.is_ok() && metrics_result.value()->num_rows() > 0) {
                    auto table = metrics_result.value();
                    if (table->num_columns() >= 4) {
                        auto daily_return_arr = std::static_pointer_cast<arrow::DoubleArray>(
                            table->column(0)->chunk(0));
                        auto daily_pnl_arr = std::static_pointer_cast<arrow::DoubleArray>(
                            table->column(1)->chunk(0));
                        auto daily_realized_arr = std::static_pointer_cast<arrow::DoubleArray>(
                            table->column(2)->chunk(0));
                        auto daily_unrealized_arr = std::static_pointer_cast<arrow::DoubleArray>(
                            table->column(3)->chunk(0));

                        if (daily_return_arr && daily_return_arr->length() > 0 &&
                            !daily_return_arr->IsNull(0)) {
                            yesterday_daily_return_for_email = daily_return_arr->Value(0);
                            INFO("Loaded yesterday's daily_return: " +
                                 std::to_string(yesterday_daily_return_for_email));
                        }
                        if (daily_pnl_arr && daily_pnl_arr->length() > 0 &&
                            !daily_pnl_arr->IsNull(0)) {
                            yesterday_daily_pnl_for_email = daily_pnl_arr->Value(0);
                            INFO("Loaded yesterday's daily_pnl: " +
                                 std::to_string(yesterday_daily_pnl_for_email));
                        }
                        if (daily_realized_arr && daily_realized_arr->length() > 0 &&
                            !daily_realized_arr->IsNull(0)) {
                            yesterday_realized_pnl_for_email = daily_realized_arr->Value(0);
                            INFO("Loaded yesterday's daily_realized_pnl: " +
                                 std::to_string(yesterday_realized_pnl_for_email));
                        } else {
                            // If daily_realized_pnl is null or 0, use aggregate_yesterday_total_pnl
                            // as fallback
                            yesterday_realized_pnl_for_email = aggregate_yesterday_total_pnl;
                            INFO(
                                "Using calculated aggregate_yesterday_total_pnl as realized PnL: " +
                                std::to_string(yesterday_realized_pnl_for_email));
                        }
                        if (daily_unrealized_arr && daily_unrealized_arr->length() > 0 &&
                            !daily_unrealized_arr->IsNull(0)) {
                            yesterday_unrealized_pnl_for_email = daily_unrealized_arr->Value(0);
                            INFO("Loaded yesterday's daily_unrealized_pnl: " +
                                 std::to_string(yesterday_unrealized_pnl_for_email));
                        }

                        // For futures, unrealized PnL should always be 0, realized PnL is the total
                        // daily PnL
                        yesterday_unrealized_pnl_for_email = 0.0;  // Futures have no unrealized PnL

                        INFO("Successfully loaded yesterday's metrics from database for email");
                    }
                } else {
                    WARN("No metrics found in database for yesterday, using calculated values");
                    // Use the calculated values as fallback
                    yesterday_realized_pnl_for_email = aggregate_yesterday_total_pnl;
                    yesterday_daily_pnl_for_email =
                        aggregate_yesterday_total_pnl;  // For futures, daily PnL = realized PnL
                    yesterday_unrealized_pnl_for_email = 0.0;  // No unrealized for futures
                }
            } catch (const std::exception& e) {
                WARN("Failed to load updated yesterday's metrics: " + std::string(e.what()));
                // Use calculated values as fallback
                yesterday_realized_pnl_for_email = aggregate_yesterday_total_pnl;
                yesterday_daily_pnl_for_email = aggregate_yesterday_total_pnl;
                yesterday_unrealized_pnl_for_email = 0.0;
            }
        } else {
            if (is_first_trading_day) {
                INFO(
                    "Skipping Day T-1 update (first trading day - no previous positions to "
                    "finalize)");
            } else {
                INFO("Skipping Day T-1 live_results update (no two_days_ago prices or zero PnL)");
            }
        }

        // ========================================
        // STEP 5: LOAD UPDATED PREVIOUS DAY AGGREGATES AND CALCULATE Day T CUMULATIVE VALUES
        // ========================================
        INFO(
            "STEP 5: Loading updated previous day aggregates and calculating Day T cumulative "
            "values...");

        // Load previous day's aggregates (portfolio value, total pnl, total transaction costs)
        // This is done AFTER updating Day T-1 live_results to ensure we get the finalized values
        double previous_portfolio_value = initial_capital;  // Default to initial capital
        double previous_total_pnl = 0.0;
        double previous_total_transaction_costs = 0.0;

        try {
            auto db_ptr = std::dynamic_pointer_cast<PostgresDatabase>(db);
            if (db_ptr) {
                auto prev_agg = db_ptr->get_previous_live_aggregates(
                    combined_strategy_id, coordinator_config.portfolio_id, now,
                    "trading.live_results");
                if (prev_agg.is_ok()) {
                    std::tie(previous_portfolio_value, previous_total_pnl,
                             previous_total_transaction_costs) = prev_agg.value();
                    INFO("Loaded updated previous aggregates - portfolio_value: $" +
                         std::to_string(previous_portfolio_value) + ", total_pnl: $" +
                         std::to_string(previous_total_pnl) + ", total_transaction_costs: $" +
                         std::to_string(previous_total_transaction_costs));
                } else {
                    INFO("No previous aggregates found: " + std::string(prev_agg.error()->what()));
                }
            }
        } catch (const std::exception& e) {
            INFO("Could not load previous day aggregates: " + std::string(e.what()));
        }

        // Calculate cumulative values for Day T
        double total_pnl = previous_total_pnl + daily_pnl_for_today;
        double current_portfolio_value = previous_portfolio_value + daily_pnl_for_today;
        double daily_pnl = daily_pnl_for_today;  // Only transaction costs on Day T
        double total_transaction_costs_cumulative =
            previous_total_transaction_costs + total_daily_transaction_costs;

        // Since it's futures, all PnL is realized
        // total_realized_pnl = total_pnl + total_transaction_costs (GROSS)
        double total_realized_pnl = total_pnl + total_transaction_costs_cumulative;
        double total_unrealized_pnl = 0.0;

        // Calculate returns using LiveMetricsCalculator
        double daily_return =
            metrics_calculator->calculate_daily_return(daily_pnl, previous_portfolio_value);

        // Calculate total cumulative return (non-annualized)
        double total_cumulative_return =
            metrics_calculator->calculate_total_return(current_portfolio_value, initial_capital);

        double total_return_decimal = 0.0;
        if (initial_capital > 0.0) {
            total_return_decimal = (current_portfolio_value - initial_capital) / initial_capital;
        }
        double total_cumulative_return_pct = total_cumulative_return;  // Already in %

        // Get n = number of trading days using PostgreSQL function (robust against row duplication)
        // Uses trading.strategy_trading_days_metadata table for live_start_date
        int trading_days_count = 1;  // Default to 1 to avoid division by zero on first day
        try {
            // Format today's date for SQL query
            auto now_time_t_for_query = std::chrono::system_clock::to_time_t(now);
            std::stringstream now_date_ss;
            now_date_ss << std::put_time(std::gmtime(&now_time_t_for_query), "%Y-%m-%d");

            // Call PostgreSQL function to calculate trading days
            std::string trading_days_query = "SELECT trading.get_trading_days('" +
                                             combined_strategy_id + "', DATE '" +
                                             now_date_ss.str() + "')";

            INFO("TRADING_DAYS_CALC [Day T]: Querying trading days...");
            INFO("TRADING_DAYS_CALC [Day T]: Query: " + trading_days_query);
            INFO("TRADING_DAYS_CALC [Day T]: Strategy ID: " + combined_strategy_id);
            INFO("TRADING_DAYS_CALC [Day T]: Target Date: " + now_date_ss.str());

            auto trading_days_result = db->execute_query(trading_days_query);

            if (trading_days_result.is_ok()) {
                auto table = trading_days_result.value();
                if (table && table->num_rows() > 0 && table->num_columns() > 0) {
                    // execute_query returns StringArray for all columns
                    auto arr =
                        std::static_pointer_cast<arrow::StringArray>(table->column(0)->chunk(0));
                    if (arr && arr->length() > 0 && !arr->IsNull(0)) {
                        trading_days_count = std::max<int>(1, std::stoi(arr->GetString(0)));
                        INFO("TRADING_DAYS_CALC [Day T]: Result from DB: " +
                             std::to_string(trading_days_count) + " trading days");
                        INFO(
                            "TRADING_DAYS_CALC [Day T]: This value comes from "
                            "strategy_trading_days_metadata.live_start_date");
                    }
                }
            } else {
                WARN("TRADING_DAYS_CALC [Day T]: Could not call get_trading_days function: " +
                     std::string(trading_days_result.error()->what()));
            }
        } catch (const std::exception& e) {
            WARN("TRADING_DAYS_CALC [Day T]: Failed to get trading days: " + std::string(e.what()));
        }

        // Calculate annualized return using LiveMetricsCalculator
        // Formula: annualized_return = ((1 + total_return)^(252/trading_days) - 1) * 100
        INFO("ANNUALIZED_RETURN_CALC [Day T]: Calculating annualized return...");
        INFO("ANNUALIZED_RETURN_CALC [Day T]: Input: total_return_decimal = " +
             std::to_string(total_return_decimal) + " (" +
             std::to_string(total_return_decimal * 100.0) + "%)");
        INFO("ANNUALIZED_RETURN_CALC [Day T]: Input: trading_days_count = " +
             std::to_string(trading_days_count));
        INFO("ANNUALIZED_RETURN_CALC [Day T]: Formula: ((1 + " +
             std::to_string(total_return_decimal) + ")^(252/" + std::to_string(trading_days_count) +
             ") - 1) * 100");

        double total_return_annualized = metrics_calculator->calculate_annualized_return(
            total_return_decimal, trading_days_count);

        INFO("ANNUALIZED_RETURN_CALC [Day T]: Result: " + std::to_string(total_return_annualized) +
             "%");

        INFO("Portfolio value calculation:");
        INFO("  Previous portfolio value: $" + std::to_string(previous_portfolio_value));
        INFO("  Daily PnL: $" + std::to_string(daily_pnl));
        INFO("  Current portfolio value: $" + std::to_string(current_portfolio_value));
        INFO("  Total PnL: $" + std::to_string(total_pnl));
        INFO("  Daily return: " + std::to_string(daily_return) + "%");
        INFO("  Annualized return: " + std::to_string(total_return_annualized) + "%");

        std::cout << "Total P&L: $" << std::fixed << std::setprecision(2) << total_pnl << std::endl;
        std::cout << "Realized P&L: $" << std::fixed << std::setprecision(2) << total_realized_pnl
                  << std::endl;
        std::cout << "Unrealized P&L: $" << std::fixed << std::setprecision(2)
                  << total_unrealized_pnl << std::endl;
        std::cout << "Current Portfolio Value: $" << std::fixed << std::setprecision(2)
                  << current_portfolio_value << std::endl;
        std::cout << "Total Return (Cumulative): " << std::fixed << std::setprecision(2)
                  << total_cumulative_return_pct << "%" << std::endl;
        std::cout << "Total Return (Annualized): " << std::fixed << std::setprecision(2)
                  << total_return_annualized << "%" << std::endl;
        std::cout << "Daily Return: " << std::fixed << std::setprecision(2) << daily_return << "%"
                  << std::endl;
        std::cout << "Portfolio Leverage: " << std::fixed << std::setprecision(2)
                  << (gross_notional / current_portfolio_value) << "x" << std::endl;
        std::cout << "Posted Margin (Initial×Contracts): $" << std::fixed << std::setprecision(2)
                  << total_posted_margin << std::endl;
        std::cout << "Equity-to-Margin Ratio: " << std::fixed << std::setprecision(2)
                  << equity_to_margin_ratio << "x" << std::endl;
        double margin_cushion = 0.0;
        if (maintenance_requirement_today > 0.0) {
            // Correct formula: margin_cushion = (equity - maintenance) / equity
            // This shows how much cushion we have above maintenance margin requirements
            margin_cushion =
                (current_portfolio_value - maintenance_requirement_today) / current_portfolio_value;
        } else {
            margin_cushion = -1.0;  // Invalid if no maintenance requirement
        }

        // Warnings per thresholds
        if (total_posted_margin > current_portfolio_value) {
            WARN("Posted margin exceeds current portfolio value; check sizing and risk limits.");
        }
        if (margin_cushion < 0.20) {
            WARN("Margin cushion below 20%.");
        }
        if (equity_to_margin_ratio > 4.0) {
            WARN("Equity-to-Margin Ratio above 4x.");
        }

        // Get forecasts for all symbols
        INFO("Retrieving current forecasts...");
        std::cout << "\n======= Current Forecasts =======" << std::endl;
        std::cout << std::setw(10) << "Symbol"
                  << " | " << std::setw(12) << "Forecast"
                  << " | " << std::setw(12) << "Position" << std::endl;
        std::cout << std::string(40, '-') << std::endl;

        // Collect signals for database storage
        std::unordered_map<std::string, double> signals_to_store;

        for (const auto& symbol : symbols) {
            double forecast = tf_strategy_typed ? tf_strategy_typed->get_forecast(symbol) : 0.0;
            double position = tf_strategy_typed ? tf_strategy_typed->get_position(symbol) : 0.0;

            signals_to_store[symbol] = forecast;

            std::cout << std::setw(10) << symbol << " | " << std::setw(12) << std::fixed
                      << std::setprecision(4) << forecast << " | " << std::setw(12) << std::fixed
                      << std::setprecision(2) << position << std::endl;
        }

        // NOTE: Signals are already stored per-strategy in PHASE 4 above.
        // Do NOT call results_manager->set_signals() here as it would cause duplicate
        // storage with the combined_strategy_id as both strategy_id AND strategy_name,
        // which creates incorrect duplicate entries in the signals table.
        // The signals_to_store map is only used for display purposes above.

        // Save trading results to results table
        INFO("Saving trading results to database...");
        try {
            // Calculate current date for results (use override date if specified)
            auto current_date = now;

            // Use the calculated returns from above
            double sharpe_ratio = 0.0;   // Would need historical data to calculate
            double sortino_ratio = 0.0;  // Would need historical data to calculate
            double max_drawdown = 0.0;   // Would need historical data to calculate
            double calmar_ratio = 0.0;   // Would need historical data to calculate
            double volatility = 0.0;
            int total_trades = 0;             // No trades in daily position generation
            double win_rate = 0.0;            // No trades in daily position generation
            double profit_factor = 0.0;       // No trades in daily position generation
            double avg_win = 0.0;             // No trades in daily position generation
            double avg_loss = 0.0;            // No trades in daily position generation
            double max_win = 0.0;             // No trades in daily position generation
            double max_loss = 0.0;            // No trades in daily position generation
            double avg_holding_period = 0.0;  // No trades in daily position generation
            double var_95 = 0.0;
            double cvar_95 = 0.0;
            double beta = 0.0;
            double correlation = 0.0;
            double downside_volatility = 0.0;

            // Get volatility from risk evaluation if available
            if (risk_eval.is_ok()) {
                const auto& r = risk_eval.value();
                volatility = r.portfolio_var * 100.0;  // Convert to percentage
                var_95 = r.portfolio_var * 100.0;      // Use portfolio VaR as proxy
                cvar_95 =
                    r.portfolio_var * 100.0;       // Use portfolio VaR as proxy (no CVaR available)
                beta = 0.0;                        // No beta available in RiskResult
                correlation = r.correlation_risk;  // Use correlation risk
            }

            // Create configuration JSON
            nlohmann::json config_json;
            config_json["strategy_type"] = combined_strategy_id;  // From config (Phase 1)
            config_json["capital_allocation"] = initial_capital;
            config_json["max_leverage"] = base_strategy_config.max_leverage;
            config_json["weight"] = 0.03;      // Default weight
            config_json["risk_target"] = 0.2;  // Default risk target
            config_json["idm"] = 2.5;          // Default IDM
            config_json["active_positions"] = active_positions;
            config_json["gross_notional"] = gross_notional;
            config_json["net_notional"] = net_notional;
            config_json["portfolio_leverage"] = gross_notional / initial_capital;

            // Create SQL insert for live_results table with correct schema
            std::stringstream date_ss;
            auto time_t = std::chrono::system_clock::to_time_t(current_date);
            date_ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%d %H:%M:%S");

            // Use calculated metrics from position analysis
            double portfolio_var = 0.0;
            double gross_leverage = 0.0;
            double net_leverage = 0.0;
            double max_correlation = 0.0;
            double jump_risk = 0.0;
            double risk_scale = 1.0;

            if (risk_eval.is_ok()) {
                const auto& r = risk_eval.value();
                portfolio_var = r.portfolio_var;
                gross_leverage = r.gross_leverage;
                net_leverage = r.net_leverage;
                max_correlation = r.correlation_risk;
                jump_risk = r.jump_risk;
                risk_scale = r.recommended_scale;
            }

            // Use LiveMetricsCalculator for portfolio metrics
            double portfolio_leverage = metrics_calculator->calculate_portfolio_leverage(
                gross_notional, current_portfolio_value);
            // equity_to_margin_ratio and margin_cushion already computed above

            // Use the LiveResultsManager
            INFO("Setting metrics in LiveResultsManager...");

            // Prepare metrics maps
            std::unordered_map<std::string, double> double_metrics = {
                {"total_cumulative_return", total_cumulative_return_pct},
                {"total_annualized_return", total_return_annualized},
                {"volatility", volatility},
                {"total_pnl", total_pnl},
                {"total_unrealized_pnl", total_unrealized_pnl},
                {"total_realized_pnl", total_realized_pnl},
                {"current_portfolio_value", current_portfolio_value},
                {"portfolio_var", portfolio_var},
                {"gross_leverage", gross_leverage},
                {"net_leverage", net_leverage},
                {"portfolio_leverage", portfolio_leverage},
                {"equity_to_margin_ratio", equity_to_margin_ratio},
                {"margin_cushion", margin_cushion},
                {"max_correlation", max_correlation},
                {"jump_risk", jump_risk},
                {"risk_scale", risk_scale},
                {"gross_notional", gross_notional},
                {"net_notional", net_notional},
                {"daily_return", daily_return},
                {"daily_pnl", daily_pnl},
                {"total_transaction_costs", total_transaction_costs_cumulative},
                {"daily_realized_pnl", daily_realized_pnl},
                {"daily_unrealized_pnl", daily_unrealized_pnl},
                {"daily_transaction_costs", total_daily_transaction_costs},
                {"margin_posted", total_posted_margin},
                {"cash_available", current_portfolio_value - total_posted_margin}};

            std::unordered_map<std::string, int> int_metrics = {
                {"active_positions", active_positions}};

            // Set all metrics at once
            results_manager->set_metrics(double_metrics, int_metrics);

            // Set config
            results_manager->set_config(config_json);

            // Set equity for equity curve tracking
            results_manager->set_equity(current_portfolio_value);
        } catch (const std::exception& e) {
            ERROR("Exception while saving trading results: " + std::string(e.what()));
        }

        // Phase 4: Use CSVExporter for position export
        INFO("Using CSVExporter to save positions to file...");

        // Query daily commissions per symbol using LiveDataLoader
        std::unordered_map<std::string, double> symbol_commissions;
        try {
            auto commission_result =
                data_loader->load_commissions_by_symbol(coordinator_config.portfolio_id, now);
            if (commission_result.is_ok()) {
                symbol_commissions = commission_result.value();
                INFO("Loaded commissions for " + std::to_string(symbol_commissions.size()) +
                     " symbols via LiveDataLoader");
            } else {
                WARN("Failed to query commissions via LiveDataLoader: " +
                     std::string(commission_result.error()->what()));
            }
        } catch (const std::exception& e) {
            WARN("Exception querying commissions: " + std::string(e.what()));
        }

        // Export current positions with per-strategy breakdown
        std::string today_filename;
        auto current_export_result = csv_exporter->export_current_positions(
            now, strategy_positions_map,
            previous_day_close_prices,  // Market prices (Day T-1 close)
            current_portfolio_value, gross_notional, net_notional,
            strategy_instances_map);

        if (current_export_result.is_ok()) {
            today_filename = current_export_result.value();
            INFO("Today's positions saved to " + today_filename);
        } else {
            ERROR("Failed to export current positions: " +
                  std::string(current_export_result.error()->what()));
        }

        // Export yesterday's finalized positions with per-strategy breakdown (if not first trading day)
        std::string yesterday_filename;
        if (!is_first_trading_day && !previous_strategy_positions.empty()) {
            INFO("Exporting yesterday's finalized positions with per-strategy breakdown...");

            auto yesterday_time = now - std::chrono::hours(24);

            auto finalized_export_result = csv_exporter->export_finalized_positions(
                now, yesterday_time,
                previous_strategy_positions,
                two_days_ago_close_prices,  // Entry prices (T-2)
                previous_day_close_prices   // Exit prices (T-1)
            );

            if (finalized_export_result.is_ok()) {
                yesterday_filename = finalized_export_result.value();
                INFO("Yesterday's finalized positions saved to " + yesterday_filename);
            } else {
                ERROR("Failed to export finalized positions: " +
                      std::string(finalized_export_result.error()->what()));
            }
        }
        // Store equity curve and save all results to database
        // Use the new LiveResultsManager - save all results at once
        INFO("Saving all live trading results using LiveResultsManager...");

        auto save_result = results_manager->save_all_results(combined_strategy_id, now);
        if (save_result.is_error()) {
            ERROR("Failed to save all live results: " + std::string(save_result.error()->what()));
        } else {
            INFO("Successfully saved all live trading results to database");
        }

        // Stop the strategy
        INFO("Stopping strategy...");
        auto stop_result = tf_strategy->stop();
        if (stop_result.is_error()) {
            ERROR("Failed to stop strategy: " + std::string(stop_result.error()->what()));
        } else {
            INFO("Strategy stopped successfully");
        }

        std::cout << "\n======= Daily Processing Complete =======" << std::endl;
        std::cout << "Today's positions file: " << today_filename << std::endl;
        // Removed yesterday finalized positions file output per request
        // Only show processing time for real-time runs, not historical
        if (!use_override_date) {
            std::cout << "Total processing time: "
                      << std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now() - now)
                             .count()
                      << "ms" << std::endl;
        }

        INFO("Daily trend following position generation completed successfully");

        // Send email report with trading results (based on send_email flag)
        if (send_email) {
            INFO("Sending email report...");
            try {
                auto email_sender = std::make_shared<EmailSender>(credentials);
                auto email_init_result = email_sender->initialize();
                if (email_init_result.is_error()) {
                    ERROR("Failed to initialize email sender: " +
                          std::string(email_init_result.error()->what()));
                } else {
                    // Prepare email data
                    std::string date_str =
                        std::to_string(now_tm->tm_year + 1900) + "-" +
                        std::string(2 - std::to_string(now_tm->tm_mon + 1).length(), '0') +
                        std::to_string(now_tm->tm_mon + 1) + "-" +
                        std::string(2 - std::to_string(now_tm->tm_mday).length(), '0') +
                        std::to_string(now_tm->tm_mday);

                    std::string subject = "Daily Trading Report - " + date_str;

                    // Load yesterday's finalized positions for email display
                    std::unordered_map<std::string, Position> yesterday_positions_finalized;
                    std::map<std::string, double> yesterday_daily_metrics_final;
                    std::unordered_map<std::string, double>
                        yesterday_entry_prices;                                     // Day T-2 close
                    std::unordered_map<std::string, double> yesterday_exit_prices;  // Day T-1 close

                    // Calculate yesterday's date for email
                    auto yesterday_time_email = now - std::chrono::hours(24);
                    auto yesterday_time_t_email =
                        std::chrono::system_clock::to_time_t(yesterday_time_email);

                    // Load finalized positions from database for email
                    std::string yesterday_date_for_email;
                    std::ostringstream yss_email;
                    yss_email << std::put_time(std::gmtime(&yesterday_time_t_email), "%Y-%m-%d");
                    yesterday_date_for_email = yss_email.str();

                    INFO("Loading yesterday's finalized positions for email: " +
                         yesterday_date_for_email);

                    std::string positions_query_email =
                        "SELECT symbol, quantity, average_price, daily_realized_pnl, "
                        "daily_unrealized_pnl, last_update "
                        "FROM trading.positions "
                        "WHERE strategy_id = '" +
                        combined_strategy_id + "' AND portfolio_id = '" +
                        coordinator_config.portfolio_id +
                        "' AND DATE(last_update) = "
                        "'" +
                        yesterday_date_for_email + "'";

                    auto positions_result_email = db->execute_query(positions_query_email);

                    if (positions_result_email.is_ok() &&
                        positions_result_email.value()->num_rows() > 0) {
                        auto table_email = positions_result_email.value();
                        // All columns are StringArrays from generic converter
                        auto symbol_arr = std::static_pointer_cast<arrow::StringArray>(
                            table_email->column(0)->chunk(0));
                        auto quantity_arr = std::static_pointer_cast<arrow::StringArray>(
                            table_email->column(1)->chunk(0));
                        auto avg_price_arr = std::static_pointer_cast<arrow::StringArray>(
                            table_email->column(2)->chunk(0));
                        auto realized_pnl_arr = std::static_pointer_cast<arrow::StringArray>(
                            table_email->column(3)->chunk(0));

                        for (int64_t i = 0; i < table_email->num_rows(); ++i) {
                            if (!symbol_arr->IsNull(i) && !quantity_arr->IsNull(i)) {
                                std::string symbol = symbol_arr->GetString(i);
                                double quantity = std::stod(quantity_arr->GetString(i));
                                double avg_price = std::stod(avg_price_arr->GetString(i));
                                double realized_pnl = std::stod(realized_pnl_arr->GetString(i));

                                // Skip positions with zero quantity
                                if (std::abs(quantity) < 0.0001)
                                    continue;

                                // Create Position object for yesterday's finalized position
                                Position pos;
                                pos.symbol = symbol;
                                pos.quantity = Decimal(quantity);
                                pos.average_price = Decimal(avg_price);
                                pos.realized_pnl = Decimal(realized_pnl);

                                yesterday_positions_finalized[symbol] = pos;

                                // Populate entry and exit prices
                                if (two_days_ago_close_prices.find(symbol) !=
                                    two_days_ago_close_prices.end()) {
                                    yesterday_entry_prices[symbol] =
                                        two_days_ago_close_prices[symbol];
                                }
                                if (previous_day_close_prices.find(symbol) !=
                                    previous_day_close_prices.end()) {
                                    yesterday_exit_prices[symbol] =
                                        previous_day_close_prices[symbol];
                                }
                            }
                        }
                        INFO("Loaded " + std::to_string(yesterday_positions_finalized.size()) +
                             " finalized positions for email");

                        // Load yesterday's daily metrics from database for accurate display
                        std::string yesterday_metrics_query =
                            "SELECT daily_return, daily_unrealized_pnl, daily_realized_pnl, "
                            "daily_pnl, daily_transaction_costs"
                            "FROM trading.live_results "
                            "WHERE strategy_id = '" +
                            combined_strategy_id + "' AND portfolio_id = '" +
                            coordinator_config.portfolio_id + "' AND date = '" +
                            yesterday_date_for_email +
                            "' "
                            "ORDER BY date DESC LIMIT 1";

                        INFO("Loading yesterday's daily metrics from live_results: " +
                             yesterday_metrics_query);
                        auto yesterday_metrics_result = db->execute_query(yesterday_metrics_query);

                        if (yesterday_metrics_result.is_ok() &&
                            yesterday_metrics_result.value()->num_rows() > 0) {
                            auto metrics_table = yesterday_metrics_result.value();
                            INFO("Retrieved " + std::to_string(metrics_table->num_rows()) +
                                 " rows from live_results");

                            auto daily_return_arr = std::static_pointer_cast<arrow::StringArray>(
                                metrics_table->column(0)->chunk(0));
                            auto daily_unrealized_arr =
                                std::static_pointer_cast<arrow::StringArray>(
                                    metrics_table->column(1)->chunk(0));
                            auto daily_realized_arr = std::static_pointer_cast<arrow::StringArray>(
                                metrics_table->column(2)->chunk(0));
                            auto daily_total_arr = std::static_pointer_cast<arrow::StringArray>(
                                metrics_table->column(3)->chunk(0));
                            auto daily_commissions_arr =
                                std::static_pointer_cast<arrow::StringArray>(
                                    metrics_table->column(4)->chunk(0));

                            if (!daily_return_arr->IsNull(0)) {
                                yesterday_daily_metrics_final["Daily Return"] =
                                    std::stod(daily_return_arr->GetString(0));
                                INFO("Daily Return: " + daily_return_arr->GetString(0));
                            }
                            if (!daily_unrealized_arr->IsNull(0)) {
                                yesterday_daily_metrics_final["Daily Unrealized PnL"] =
                                    std::stod(daily_unrealized_arr->GetString(0));
                                INFO("Daily Unrealized PnL: " + daily_unrealized_arr->GetString(0));
                            }
                            if (!daily_realized_arr->IsNull(0)) {
                                yesterday_daily_metrics_final["Daily Realized PnL"] =
                                    std::stod(daily_realized_arr->GetString(0));
                                INFO("Daily Realized PnL: " + daily_realized_arr->GetString(0));
                            }
                            if (!daily_total_arr->IsNull(0)) {
                                yesterday_daily_metrics_final["Daily Total PnL"] =
                                    std::stod(daily_total_arr->GetString(0));
                                INFO("Daily Total PnL: " + daily_total_arr->GetString(0));
                            }

                            if (!daily_commissions_arr->IsNull(0)) {
                                yesterday_daily_metrics_final["Daily Transaction Costs"] =
                                    std::stod(daily_commissions_arr->GetString(0));
                                INFO("Daily Transaction Costs: " +
                                     daily_commissions_arr->GetString(0));
                            }

                            INFO("Successfully loaded yesterday's daily metrics from live_results");
                        } else {
                            if (yesterday_metrics_result.is_error()) {
                                ERROR("Failed to query live_results: " +
                                      std::string(yesterday_metrics_result.error()->what()));
                            } else {
                                WARN("No rows found in live_results for date: " +
                                     yesterday_date_for_email);
                            }
                            // Fallback: calculate from positions if database query fails
                            double yesterday_daily_realized = 0.0;
                            for (const auto& [symbol, pos] : yesterday_positions_finalized) {
                                yesterday_daily_realized += pos.realized_pnl.as_double();
                            }
                            yesterday_daily_metrics_final["Daily Realized PnL"] =
                                yesterday_daily_realized;
                            INFO(
                                "Calculated yesterday's metrics from positions (fallback) - Daily "
                                "Realized PnL: " +
                                std::to_string(yesterday_daily_realized));
                        }

                    } else {
                        INFO("No finalized positions found for yesterday's email table");
                    }

                    // Create strategy metrics map with all relevant metrics organized by category
                    std::map<std::string, double> strategy_metrics;

                    // Performance Metrics
                    strategy_metrics["Daily Return"] = daily_return;
                    strategy_metrics["Daily Unrealized PnL"] = daily_unrealized_pnl;
                    strategy_metrics["Daily Realized PnL"] = daily_realized_pnl;
                    strategy_metrics["Daily Total PnL"] = daily_pnl;
                    strategy_metrics["Total Cumulative Return"] = total_cumulative_return_pct;
                    strategy_metrics["Total Annualized Return"] = total_return_annualized;
                    strategy_metrics["Total Unrealized PnL"] = total_unrealized_pnl;
                    strategy_metrics["Total Realized PnL"] = total_realized_pnl;
                    strategy_metrics["Total PnL"] = total_pnl;
                    if (risk_eval.is_ok()) {
                        strategy_metrics["Volatility"] = risk_eval.value().portfolio_var * 100.0;
                    }
                    strategy_metrics["Total Transaction Costs"] =
                        total_transaction_costs_cumulative;
                    strategy_metrics["Current Portfolio Value"] = current_portfolio_value;

                    // Leverage Metrics - Calculate values from position analysis
                    double gross_leverage_calc = (current_portfolio_value != 0.0)
                                                     ? (gross_notional / current_portfolio_value)
                                                     : 0.0;
                    double net_leverage_calc = (current_portfolio_value != 0.0)
                                                   ? (net_notional / current_portfolio_value)
                                                   : 0.0;
                    double portfolio_leverage_calc =
                        (current_portfolio_value != 0.0)
                            ? (gross_notional / current_portfolio_value)
                            : 0.0;

                    strategy_metrics["Gross Leverage"] = gross_leverage_calc;
                    strategy_metrics["Net Leverage"] = net_leverage_calc;
                    strategy_metrics["Portfolio Leverage"] = portfolio_leverage_calc;
                    strategy_metrics["Equity-to-Margin Ratio"] = equity_to_margin_ratio;

                    // Risk & Liquidity Metrics
                    strategy_metrics["Margin Cushion"] =
                        margin_cushion * 100.0;  // Convert to percentage
                    strategy_metrics["Margin Posted"] = total_posted_margin;
                    strategy_metrics["Cash Available"] =
                        current_portfolio_value - total_posted_margin;

                    // Note: yesterday_daily_metrics_final is now loaded AFTER database updates
                    // above So we don't need to create it here anymore

                    // Generate email body with is_daily_strategy flag set to true and current
                    // prices. Pass strategy_positions_map and all_strategy_executions for per-strategy tables.
                    std::string email_body = email_sender->generate_trading_report_body(
                        strategy_positions_map,         // Per-strategy positions for grouped tables
                        positions,
                        risk_eval.is_ok() ? std::make_optional(risk_eval.value()) : std::nullopt,
                        strategy_metrics, all_strategy_executions, date_str,
                        portfolio_id,                   // Portfolio name for email header
                        true,                           // is_daily_strategy
                        previous_day_close_prices,      // Pass Day T-1 close prices for today's
                                                        // positions
                        db,                             // Pass database for symbols reference table
                        previous_strategy_positions,    // Per-strategy yesterday's positions for
                                                        // grouped tables
                        yesterday_exit_prices,   // Day T-1 close prices for yesterday's positions
                        yesterday_entry_prices,  // Day T-2 close prices for yesterday's positions
                        yesterday_daily_metrics_final  // Yesterday's metrics
                    );

                    // Send email with CSV attachments: today's positions and yesterday's finalized
                    // (if available)
                    std::vector<std::string> attachments = {today_filename};
                    if (!yesterday_filename.empty()) {
                        attachments.push_back(yesterday_filename);
                    }

                    auto send_result =
                        email_sender->send_email(subject, email_body, true, attachments);
                    if (send_result.is_error()) {
                        ERROR("Failed to send email: " + std::string(send_result.error()->what()));
                    } else {
                        std::string attachment_list = today_filename;
                        if (!yesterday_filename.empty()) {
                            attachment_list += ", " + yesterday_filename;
                        }
                        INFO("Email report sent successfully with CSV attachments: " +
                             attachment_list);
                    }
                }
            } catch (const std::exception& e) {
                ERROR("Exception during email sending: " + std::string(e.what()));
            }
        } else {
            INFO("Email reporting disabled");
        }

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
