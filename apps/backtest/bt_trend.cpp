#include <fstream>
#include <iomanip>
#include <iostream>
#include "trade_ngin/backtest/backtest_engine.hpp"
#include "trade_ngin/backtest/transaction_cost_analysis.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/state_manager.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/data/credential_store.hpp"
#include "trade_ngin/data/database_pooling.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/strategy/regime_switching_fx_strategy.hpp"

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
        logger_config.filename_prefix = "bt_regime_fx";
        logger.initialize(logger_config);

        std::atomic_thread_fence(std::memory_order_seq_cst);

        if (!logger.is_initialized()) {
            std::cerr << "ERROR: Logger initialization failed" << std::endl;
            return 1;
        }

        INFO("Logger initialized successfully for Regime Switching FX backtest");

        // Setup database connection pool
        INFO("Initializing database connection pool...");
        auto credentials = std::make_shared<trade_ngin::CredentialStore>(
            "./config.json");

        auto username_result = credentials->get<std::string>("database", "username");
        auto password_result = credentials->get<std::string>("database", "password");
        auto host_result = credentials->get<std::string>("database", "host");
        auto port_result = credentials->get<std::string>("database", "port");
        auto db_name_result = credentials->get<std::string>("database", "name");

        if (username_result.is_error() || password_result.is_error() ||
            host_result.is_error() || port_result.is_error() || db_name_result.is_error()) {
            std::cerr << "Failed to get database credentials from config.json" << std::endl;
            return 1;
        }

        std::string conn_string =
            "postgresql://" + username_result.value() + ":" + password_result.value() + "@" +
            host_result.value() + ":" + port_result.value() + "/" + db_name_result.value();

        // Initialize connection pool
        size_t num_connections = 5;
        auto pool_result = DatabasePool::instance().initialize(conn_string, num_connections);
        if (pool_result.is_error()) {
            std::cerr << "Failed to initialize connection pool: " << pool_result.error()->what()
                      << std::endl;
            return 1;
        }
        INFO("Database connection pool initialized");

        // Get database connection
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

        auto load_result = registry.load_instruments();
        if (load_result.is_error()) {
            WARN("Failed to load futures instruments: " +
                 std::string(load_result.error()->what()));
        } else {
            INFO("Successfully loaded futures instruments from database");
        }

        // Configure backtest parameters
        INFO("Loading configuration...");
        trade_ngin::backtest::BacktestConfig config;

        // Set start/end dates
        auto now = std::chrono::system_clock::now();
        auto now_time_t = std::chrono::system_clock::to_time_t(now);
        std::tm* now_tm = std::localtime(&now_time_t);

        // Start from 6 months ago for faster testing (can change back to 2 years after testing)
        std::tm start_tm = *now_tm;
        start_tm.tm_mon -= 6;  // 6 months instead of 2 years for faster testing
        auto start_time_t = std::mktime(&start_tm);
        config.strategy_config.start_date = std::chrono::system_clock::from_time_t(start_time_t);
        config.strategy_config.end_date = now;

        config.strategy_config.asset_class = trade_ngin::AssetClass::FUTURES;
        config.strategy_config.data_freq = trade_ngin::DataFrequency::DAILY;
        config.strategy_config.commission_rate = Decimal(0.0002);
        config.strategy_config.slippage_model = Decimal(0.5);

        // Strategy symbols - use .v.0 suffix format as database stores them
        // FX futures symbols: AUD, GBP, CAD, EUR, JPY, BRL, MXN, NZD, CHF
        config.strategy_config.symbols = {
            "6A.v.0",  // AUD/USD
            "6B.v.0",  // GBP/USD
            "6C.v.0",  // CAD/USD
            "6E.v.0",  // EUR/USD
            "6J.v.0",  // JPY/USD
            "6L.v.0",  // BRL/USD
            "6M.v.0",  // MXN/USD
            "6N.v.0",  // NZD/USD
            "6S.v.0"   // CHF/USD
        };

        std::cout << "Symbols: ";
        for (const auto& symbol : config.strategy_config.symbols) {
            std::cout << symbol << " ";
        }
        std::cout << std::endl;

        // Portfolio settings
        config.portfolio_config.initial_capital = Decimal(1000000.0);
        config.portfolio_config.use_risk_management = true;
        config.portfolio_config.use_optimization = false;  // Disable for initial test

        std::cout << "Retrieved " << config.strategy_config.symbols.size() << " symbols"
                  << std::endl;
        std::cout << "Initial capital: $" << config.portfolio_config.initial_capital << std::endl;

        INFO("Configuration loaded successfully. Testing " +
             std::to_string(config.strategy_config.symbols.size()) + " symbols.");

        // Configure risk management
        config.portfolio_config.risk_config.capital = config.portfolio_config.initial_capital;
        config.portfolio_config.risk_config.var_limit = 0.15;
        config.portfolio_config.risk_config.max_gross_leverage = 5.0;
        config.portfolio_config.risk_config.max_net_leverage = 5.0;

        // Configure optimization
        config.portfolio_config.opt_config.capital =
            config.portfolio_config.initial_capital.as_double();
        config.portfolio_config.opt_config.tau = 1.0;

        // Initialize backtest engine
        INFO("Initializing backtest engine...");
        auto engine = std::make_unique<trade_ngin::backtest::BacktestEngine>(config, db);

        // Setup portfolio configuration
        trade_ngin::PortfolioConfig portfolio_config;
        portfolio_config.total_capital = config.portfolio_config.initial_capital;
        portfolio_config.use_optimization = config.portfolio_config.use_optimization;
        portfolio_config.use_risk_management = config.portfolio_config.use_risk_management;
        portfolio_config.opt_config = config.portfolio_config.opt_config;
        portfolio_config.risk_config = config.portfolio_config.risk_config;

        // Create Regime Switching FX Strategy Configuration
        INFO("Configuring RegimeSwitchingFXStrategy...");
        trade_ngin::RegimeSwitchingFXConfig fx_config;

        // Basic settings
        fx_config.capital_allocation = config.portfolio_config.initial_capital.as_double();
        fx_config.symbols = config.strategy_config.symbols;
        fx_config.max_leverage = 5.0;

        fx_config.volatility_window = 30;           // 30-day rolling volatility
        fx_config.performance_lookback = 5;         // 5-day return for ranking
        fx_config.zscore_lookback = 60;             // 60-day z-score window
        fx_config.low_dispersion_threshold = -0.5;  // Momentum threshold
        fx_config.high_dispersion_threshold = 0.5;  // Mean reversion threshold

        // Position settings
        fx_config.num_long_positions = 2;
        fx_config.num_short_positions = 2;
        fx_config.use_volatility_scaling = true;
        fx_config.stop_loss_pct = 0.10;

        // Persistence settings - disable strategy-level persistence for backtests
        // The BacktestEngine will handle saving all results to backtest.* tables at the end
        // Strategy-level persistence (save_positions, save_executions) saves to trading.* tables
        // which is only needed for live trading, not backtests
        fx_config.save_positions = false;  // Disabled - BacktestEngine handles saving to backtest.final_positions
        fx_config.save_signals = false;    // Disabled to prevent stalling with large datasets
        fx_config.save_executions = false; // Disabled - BacktestEngine handles saving to backtest.executions

        // Add position limits and costs
        for (const auto& symbol : config.strategy_config.symbols) {
            fx_config.position_limits[symbol] = 300.0;  // Increased to allow volatility scaling (base 100 * 3x max)
            fx_config.costs[symbol] = config.strategy_config.commission_rate.as_double();
        }

        // Create and initialize the strategy
        INFO("Initializing RegimeSwitchingFXStrategy...");
        std::cout << "Strategy capital allocation: $" << fx_config.capital_allocation << std::endl;
        std::cout << "Volatility window: " << fx_config.volatility_window << " days" << std::endl;
        std::cout << "Z-score lookback: " << fx_config.zscore_lookback << " days" << std::endl;
        std::cout << "Performance lookback: " << fx_config.performance_lookback << " days" << std::endl;

        std::string strategy_id = "REGIME_SWITCHING_FX";
        auto fx_strategy = std::make_shared<trade_ngin::RegimeSwitchingFXStrategy>(
            strategy_id, fx_config, db);

        auto init_result = fx_strategy->initialize();
        if (init_result.is_error()) {
            std::cerr << "Failed to initialize strategy: " << init_result.error()->what()
                      << std::endl;
            return 1;
        }
        INFO("Strategy initialization successful");

        // Start the strategy
        INFO("Starting strategy...");
        auto start_result = fx_strategy->start();
        if (start_result.is_error()) {
            std::cerr << "Failed to start strategy: " << start_result.error()->what() << std::endl;
            return 1;
        }
        INFO("Strategy started successfully");

        // Create portfolio manager and add strategy
        INFO("Creating portfolio manager...");
        auto portfolio = std::make_shared<trade_ngin::PortfolioManager>(portfolio_config);
        auto add_result =
            portfolio->add_strategy(fx_strategy, 1.0, config.portfolio_config.use_optimization,
                                    config.portfolio_config.use_risk_management);
        if (add_result.is_error()) {
            std::cerr << "Failed to add strategy to portfolio: " << add_result.error()->what()
                      << std::endl;
            return 1;
        }
        INFO("Strategy added to portfolio successfully");

        // Run the backtest
        INFO("Running backtest for time period: " +
             std::to_string(
                 std::chrono::system_clock::to_time_t(config.strategy_config.start_date)) +
             " to " +
             std::to_string(std::chrono::system_clock::to_time_t(config.strategy_config.end_date)));

        INFO("NOTE: Strategy requires warm-up period (volatility_window + zscore_lookback = ~90 days)");

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

        std::cout << "\n======= Backtest Results (Regime Switching FX) =======" << std::endl;
        std::cout << "Total Return: " << std::fixed << std::setprecision(2)
                  << (backtest_results.total_return * 100.0) << "%" << std::endl;
        std::cout << "Sharpe Ratio: " << std::fixed << std::setprecision(3)
                  << backtest_results.sharpe_ratio << std::endl;
        std::cout << "Sortino Ratio: " << std::fixed << std::setprecision(3)
                  << backtest_results.sortino_ratio << std::endl;
        std::cout << "Max Drawdown: " << std::fixed << std::setprecision(2)
                  << (backtest_results.max_drawdown * 100.0) << "%" << std::endl;
        std::cout << "Calmar Ratio: " << std::fixed << std::setprecision(3)
                  << backtest_results.calmar_ratio << std::endl;
        std::cout << "Volatility: " << std::fixed << std::setprecision(2)
                  << (backtest_results.volatility * 100.0) << "%" << std::endl;
        std::cout << "Win Rate: " << std::fixed << std::setprecision(2)
                  << (backtest_results.win_rate * 100.0) << "%" << std::endl;
        std::cout << "Total Trades: " << backtest_results.total_trades << std::endl;
        std::cout << "========================================================\n" << std::endl;

        // Save results to database
        INFO("Saving backtest results to database...");
        try {
            auto save_result = engine->save_results_to_db(backtest_results);
            if (save_result.is_error()) {
                WARN("Failed to save backtest results to database: " +
                     std::string(save_result.error()->what()));
            } else {
                INFO("Successfully saved backtest results to database");
            }
        } catch (const std::exception& e) {
            WARN("Exception during database save: " + std::string(e.what()));
        }

        // Cleanup
        INFO("Cleaning up backtest engine...");
        engine.reset();

        INFO("Backtest application completed successfully");
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