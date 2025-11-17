#include <fstream>
#include <iomanip>
#include <iostream>
#include "trade_ngin/backtest/backtest_engine.hpp"
#include "trade_ngin/backtest/transaction_cost_analysis.hpp"
#include "trade_ngin/core/logger.hpp"
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
        logger_config.min_level = LogLevel::INFO;
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

        // Set start/end dates - Use a date range with sufficient history
        std::tm start_tm = {};
        start_tm.tm_year = 2020 - 1900;
        start_tm.tm_mon = 0;
        start_tm.tm_mday = 1;
        auto start_time_t = std::mktime(&start_tm);
        config.strategy_config.start_date = std::chrono::system_clock::from_time_t(start_time_t);

        auto now = std::chrono::system_clock::now();
        config.strategy_config.end_date = now;

        config.strategy_config.asset_class = trade_ngin::AssetClass::FUTURES;
        config.strategy_config.data_freq = trade_ngin::DataFrequency::DAILY;
        config.strategy_config.commission_rate = 0.0002;
        config.strategy_config.slippage_model = 0.5;

        // Strategy symbols - 7 major FX futures
        config.strategy_config.symbols = {"6C.v.0", "6A.v.0", "6J.v.0", "6B.v.0", "6E.v.0", "6M.v.0", "6N.v.0"};

        std::cout << "\n=== Backtest Configuration ===" << std::endl;
        std::cout << "Symbols: ";
        for (const auto& symbol : config.strategy_config.symbols) {
            std::cout << symbol << " ";
        }
        std::cout << std::endl;

        // Portfolio settings
        config.portfolio_config.initial_capital = 1000000.0;
        config.portfolio_config.use_risk_management = false;
        config.portfolio_config.use_optimization = false;

        std::cout << "Initial capital: $" << std::fixed << std::setprecision(0)
                  << config.portfolio_config.initial_capital << std::endl;
        std::cout << "================================\n" << std::endl;

        INFO("Configuration loaded successfully.");

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

        fx_config.capital_allocation = config.portfolio_config.initial_capital.as_double();
        fx_config.symbols = config.strategy_config.symbols;
        fx_config.max_leverage = 5.0;

        // Calculation windows
        fx_config.volatility_window = 30;
        fx_config.momentum_lookback = 120;
        fx_config.ewmac_short_lookback = 8;
        fx_config.ewmac_long_lookback = 32;
        fx_config.zscore_lookback = 60;
        fx_config.regime_threshold = 0.5;

        // Position settings
        fx_config.num_long_positions = 2;
        fx_config.num_short_positions = 2;
        fx_config.use_volatility_scaling = true;

        // Rebalancing settings
        fx_config.momentum_rebalance_days = 20;
        fx_config.mean_reversion_rebalance_days = 5;

        // Risk settings
        fx_config.stop_loss_pct = 0.10;

        // Persistence settings
        fx_config.save_positions = false;
        fx_config.save_signals = false;
        fx_config.save_executions = false;

        // Add position limits and costs
        for (const auto& symbol : config.strategy_config.symbols) {
            fx_config.position_limits[symbol] = 100.0;
            fx_config.costs[symbol] = config.strategy_config.commission_rate.as_double();
        }

        // Display strategy parameters
        std::cout << "\n=== Strategy Parameters ===" << std::endl;
        std::cout << "Capital: $" << std::fixed << std::setprecision(0)
                  << fx_config.capital_allocation << std::endl;
        std::cout << "Volatility window: " << fx_config.volatility_window << " days" << std::endl;
        std::cout << "Z-score lookback: " << fx_config.zscore_lookback << " days" << std::endl;
        std::cout << "Momentum lookback: " << fx_config.momentum_lookback << " days" << std::endl;
        std::cout << "EWMAC: " << fx_config.ewmac_short_lookback << "/"
                  << fx_config.ewmac_long_lookback << " days" << std::endl;
        std::cout << "Positions: " << fx_config.num_long_positions << " long, "
                  << fx_config.num_short_positions << " short" << std::endl;
        std::cout << "============================\n" << std::endl;

        // Create and initialize strategy
        std::string strategy_id = "REGIME_SWITCHING_FX";
        auto fx_strategy = std::make_shared<trade_ngin::RegimeSwitchingFXStrategy>(
            strategy_id, fx_config, db);

        auto init_result = fx_strategy->initialize();
        if (init_result.is_error()) {
            std::cerr << "Failed to initialize strategy: " << init_result.error()->what()
                      << std::endl;
            return 1;
        }

        auto start_result = fx_strategy->start();
        if (start_result.is_error()) {
            std::cerr << "Failed to start strategy: " << start_result.error()->what() << std::endl;
            return 1;
        }

        // Create portfolio and add strategy
        auto portfolio = std::make_shared<trade_ngin::PortfolioManager>(portfolio_config);
        auto add_result =
            portfolio->add_strategy(fx_strategy, 1.0, config.portfolio_config.use_optimization,
                                    config.portfolio_config.use_risk_management);
        if (add_result.is_error()) {
            std::cerr << "Failed to add strategy: " << add_result.error()->what() << std::endl;
            return 1;
        }

        // Run backtest
        std::cout << "=== Running Backtest ===" << std::endl;
        std::cout << "NOTE: ~90 day warm-up period required" << std::endl;
        std::cout << "========================\n" << std::endl;

        auto result = engine->run_portfolio(portfolio);

        if (result.is_error()) {
            std::cerr << "Backtest failed: " << result.error()->what() << std::endl;
            return 1;
        }

        // Display results
        const auto& backtest_results = result.value();

        std::cout << "\n=== Backtest Results ===" << std::endl;
        std::cout << "Total Return:    " << std::fixed << std::setprecision(2)
                  << (backtest_results.total_return * 100.0) << "%" << std::endl;
        std::cout << "Sharpe Ratio:    " << std::fixed << std::setprecision(3)
                  << backtest_results.sharpe_ratio << std::endl;
        std::cout << "Max Drawdown:    " << std::fixed << std::setprecision(2)
                  << (backtest_results.max_drawdown * 100.0) << "%" << std::endl;
        std::cout << "Win Rate:        " << std::fixed << std::setprecision(2)
                  << (backtest_results.win_rate * 100.0) << "%" << std::endl;
        std::cout << "Total Trades:    " << backtest_results.total_trades << std::endl;
        std::cout << "========================\n" << std::endl;

        // Save results
        auto save_result = engine->save_results_to_db(backtest_results);
        if (save_result.is_error()) {
            WARN("Failed to save results: " + std::string(save_result.error()->what()));
        }

        engine.reset();
        INFO("Backtest completed successfully");
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}