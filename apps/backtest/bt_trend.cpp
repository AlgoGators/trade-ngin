#include "trade_ngin/backtest/backtest_engine.hpp"
#include "trade_ngin/strategy/trend_following.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/data/credential_store.hpp"
#include "trade_ngin/data/database_pooling.hpp"
#include "trade_ngin/backtest/transaction_cost_analysis.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include <iostream>
#include <iomanip>
#include <fstream>

using namespace trade_ngin;
using namespace trade_ngin::backtest;

int main() {
    try {
        // Initialize the logger
        auto& logger = Logger::instance();
        LoggerConfig logger_config;
        logger_config.min_level = LogLevel::DEBUG;
        logger_config.destination = LogDestination::BOTH;
        logger_config.log_directory = "logs";
        logger_config.filename_prefix = "bt_trend";
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
            std::cerr << "Failed to get database name: " << db_name_result.error()->what() << std::endl;
            return 1;
        }
        std::string db_name = db_name_result.value();

        std::string conn_string = "postgresql://" + username + ":" + password + "@" + host + ":" + port + "/" + db_name;

        // Initialize only the connection pool with sufficient connections
        size_t num_connections = 5;
        auto pool_result = DatabasePool::instance().initialize(conn_string, num_connections);
        if (pool_result.is_error()) {
            std::cerr << "Failed to initialize connection pool: " << pool_result.error()->what() << std::endl;
            return 1;
        }
        INFO("Database connection pool initialized with " 
            + std::to_string(num_connections) + " connections");
        
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
            std::cerr << "Failed to initialize instrument registry: " << 
            instrument_registry_init_result.error()->what() << std::endl;
            return 1;
        }
        
        // Load futures instruments
        auto load_result = registry.load_instruments();
        if (load_result.is_error() || registry.get_all_instruments().empty()) {
            std::cerr << "Failed to load futures instruments: " << load_result.error()->what() << std::endl;
            ERROR("Failed to load futures instruments: " + std::string(load_result.error()->what()));
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

        trade_ngin::backtest::BacktestConfig config;

        // Convert timestamps to proper format
        auto now = std::chrono::system_clock::now();
        auto now_time_t = std::chrono::system_clock::to_time_t(now);
        std::tm* now_tm = std::localtime(&now_time_t);

        // Set start date to 2 years ago
        std::tm start_tm = *now_tm;
        start_tm.tm_year -= 2; // 2 years ago
        auto start_time_t = std::mktime(&start_tm);
        config.strategy_config.start_date = std::chrono::system_clock::from_time_t(start_time_t);

        // Set end date to today
        config.strategy_config.end_date = now;

        config.strategy_config.asset_class = trade_ngin::AssetClass::FUTURES;
        config.strategy_config.data_freq = trade_ngin::DataFrequency::DAILY;
        config.strategy_config.commission_rate = 0.0005; // 5 basis points
        config.strategy_config.slippage_model = 1.0; // 1 basis point
        
        // auto symbols = std::vector<std::string>{"GC.v.0", "ES.v.0", "CL.v.0"};
        // config.strategy_config.symbols = symbols;
       
        auto symbols_result = db->get_symbols(trade_ngin::AssetClass::FUTURES);
        auto symbols = symbols_result.value();

        if (symbols_result.is_ok()) {
            for (const auto& symbol : symbols) {
                if (symbol.find(".c.0") != std::string::npos || symbol.find("MES.c.0") != std::string::npos || symbol.find("ES.v.0") != std::string::npos) {
                    symbols.erase(
                        std::remove(symbols.begin(), 
                                   symbols.end(), 
                                   symbol),
                        symbols.end());
                }
            }
            config.strategy_config.symbols = symbols;
        } else {
            // Detailed error logging
            ERROR("Failed to get symbols: " + std::string(symbols_result.error()->what()));
            throw std::runtime_error("Failed to get symbols: " + symbols_result.error()->to_string());
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
        // config.portfolio_config.use_risk_management = false;
        // config.portfolio_config.use_optimization = false;

        
        std::cout << "Retrieved " << config.strategy_config.symbols.size() << " symbols" << std::endl;
        std::cout << "Initial capital: $" << config.portfolio_config.initial_capital << std::endl;
        std::cout << "Commission rate: " << (config.strategy_config.commission_rate * 100) << " bps" << std::endl;
        std::cout << "Slippage model: " << config.strategy_config.slippage_model << " bps" << std::endl;

        INFO("Configuration loaded successfully. Testing " + 
            std::to_string(config.strategy_config.symbols.size()) + " symbols from " + 
            std::to_string(std::chrono::system_clock::to_time_t(config.strategy_config.start_date)) + " to " + 
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
        config.portfolio_config.opt_config.capital = config.portfolio_config.initial_capital.as_double();
        config.portfolio_config.opt_config.cost_penalty_scalar = 50.0;
        config.portfolio_config.opt_config.asymmetric_risk_buffer = 0.1;
        config.portfolio_config.opt_config.max_iterations = 100;
        config.portfolio_config.opt_config.convergence_threshold = 1e-6;
        config.portfolio_config.opt_config.use_buffering = true;
        config.portfolio_config.opt_config.buffer_size_factor = 0.05;
        
        // Initialize backtest engine
        // Right before creating BacktestEngine
        std::cerr << "Before BacktestEngine: initialized=" 
            << Logger::instance().is_initialized() << std::endl;
        INFO("Initializing backtest engine...");
        auto engine = std::make_unique<trade_ngin::backtest::BacktestEngine>(config, db);
        // After creating BacktestEngine
        std::cerr << "After BacktestEngine: initialized=" 
            << Logger::instance().is_initialized() << std::endl;

        // Setup portfolio configuration
        trade_ngin::PortfolioConfig portfolio_config;
        portfolio_config.total_capital = config.portfolio_config.initial_capital;
        portfolio_config.reserve_capital = config.portfolio_config.initial_capital * 0.1; // 10% reserve
        portfolio_config.max_strategy_allocation = 1.0; // Only have one strategy currently
        portfolio_config.min_strategy_allocation = 0.1;
        portfolio_config.use_optimization = true;
        portfolio_config.use_risk_management = true;
        portfolio_config.opt_config = config.portfolio_config.opt_config;
        portfolio_config.risk_config = config.portfolio_config.risk_config;
        
        // Create trend following strategy configuration
        trade_ngin::StrategyConfig tf_config;
        tf_config.capital_allocation = config.portfolio_config.initial_capital.as_double();
        tf_config.asset_classes = {trade_ngin::AssetClass::FUTURES};
        tf_config.frequencies = {config.strategy_config.data_freq};
        tf_config.max_drawdown = 0.4;  // 40% max drawdown
        tf_config.max_leverage = 4.0;
        tf_config.save_positions = false;
        tf_config.save_signals = false;
        tf_config.save_executions = false;
        
        // Add position limits and contract sizes
        for (const auto& symbol : config.strategy_config.symbols) {
            tf_config.position_limits[symbol] = 1000.0;  // Max 1000 units per symbol
            tf_config.costs[symbol] = config.strategy_config.commission_rate.as_double();
        }
        
        // Configure trend following parameters
        trade_ngin::TrendFollowingConfig trend_config;
        trend_config.weight = 0.03;           // 3% weight per symbol
        trend_config.risk_target = 0.2;       // Target 20% annualized risk
        trend_config.idm = 2.5;               // Instrument diversification multiplier
        trend_config.use_position_buffering = false;
        trend_config.ema_windows = {
            {2, 8}, {4, 16}, {8, 32}, {16, 64}, {32, 128}, {64, 256}
        };
        trend_config.vol_lookback_short = 32;  // Short vol lookback
        trend_config.vol_lookback_long = 252;  // Long vol lookback
        trend_config.fdm = {
            {1, 1.0}, {2, 1.03}, {3, 1.08}, {4, 1.13}, {5, 1.19}, {6, 1.26}
        };
        
        // Create and initialize the strategies
        // Before TrendFollowingStrategy
        std::cerr << "Before TrendFollowingStrategy: initialized=" 
            << Logger::instance().is_initialized() << std::endl;
        INFO("Initializing TrendFollowingStrategy...");
        std::cout << "Strategy capital allocation: $" << tf_config.capital_allocation << std::endl;
        std::cout << "Max leverage: " << tf_config.max_leverage << "x" << std::endl;
        
        // Create a shared_ptr that doesn't own the singleton registry
        auto registry_ptr = std::shared_ptr<InstrumentRegistry>(&registry, [](InstrumentRegistry*){});
        
        auto tf_strategy = std::make_shared<trade_ngin::TrendFollowingStrategy>(
            "TREND_FOLLOWING", tf_config, trend_config, db, registry_ptr);
        
        auto init_result = tf_strategy->initialize();
        if (init_result.is_error()) {
            std::cerr << "Failed to initialize strategy: " << init_result.error()->what() << std::endl;
            return 1;
        }
        INFO("Strategy initialization successful");

        // Start the strategy
        INFO("Starting strategy...");
        auto start_result = tf_strategy->start();
        if (start_result.is_error()) {
            std::cerr << "Failed to start strategy: " << start_result.error()->what() << std::endl;
            return 1;
        }
        INFO("Strategy started successfully");

        // Create portfolio manager and add strategy
        INFO("Creating portfolio manager...");
        auto portfolio = std::make_shared<trade_ngin::PortfolioManager>(portfolio_config);
        auto add_result = portfolio->add_strategy(tf_strategy, 1.0, 
                                config.portfolio_config.use_optimization,
                                config.portfolio_config.use_risk_management);
        if (add_result.is_error()) {
            std::cerr << "Failed to add strategy to portfolio: " << add_result.error()->what() << std::endl;
            return 1;
        }
        INFO("Strategy added to portfolio successfully"); 

        // Run the backtest
        INFO("Running backtest for time period: " + 
            std::to_string(std::chrono::system_clock::to_time_t(config.strategy_config.start_date)) + " to " + 
            std::to_string(std::chrono::system_clock::to_time_t(config.strategy_config.end_date)));
        
        auto result = engine->run_portfolio(portfolio);
        
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
        std::cout << "Total Return: " << (backtest_results.total_return * 100.0) << "%" << std::endl;
        std::cout << "Sharpe Ratio: " << backtest_results.sharpe_ratio << std::endl;
        std::cout << "Sortino Ratio: " << backtest_results.sortino_ratio << std::endl;
        std::cout << "Max Drawdown: " << (backtest_results.max_drawdown * 100.0) << "%" << std::endl;
        std::cout << "Calmar Ratio: " << backtest_results.calmar_ratio << std::endl;
        std::cout << "Volatility: " << (backtest_results.volatility * 100.0) << "%" << std::endl;
        std::cout << "Win Rate: " << (backtest_results.win_rate * 100.0) << "%" << std::endl;
        std::cout << "Total Trades: " << backtest_results.total_trades << std::endl;

        INFO("Backtest application completed successfully");

        std::cerr << "At end of main: initialized=" 
         << Logger::instance().is_initialized() << std::endl;
        
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