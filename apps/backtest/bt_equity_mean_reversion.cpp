#include <fstream>
#include <iomanip>
#include <iostream>
#include "trade_ngin/backtest/backtest_engine.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/data/credential_store.hpp"
#include "trade_ngin/data/database_pooling.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/strategy/mean_reversion.hpp"

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
        logger_config.filename_prefix = "bt_equity_mr";
        logger.initialize(logger_config);

        if (!logger.is_initialized()) {
            std::cerr << "ERROR: Logger initialization failed" << std::endl;
            return 1;
        }

        INFO("Logger initialized successfully");

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

        // Initialize connection pool
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
        INFO("Instrument registry initialized");

        // Configure backtest parameters for EQUITIES
        INFO("Loading configuration...");
        trade_ngin::backtest::BacktestConfig config;

        // Set date range - last 2 years
        auto now = std::chrono::system_clock::now();
        auto now_time_t = std::chrono::system_clock::to_time_t(now);
        std::tm* now_tm = std::localtime(&now_time_t);

        // Start date: 2 years ago
        std::tm start_tm = *now_tm;
        start_tm.tm_year -= 2;
        auto start_time_t = std::mktime(&start_tm);
        config.strategy_config.start_date = std::chrono::system_clock::from_time_t(start_time_t);
        config.strategy_config.end_date = now;

        // CRITICAL: Set asset class to EQUITIES
        config.strategy_config.asset_class = AssetClass::EQUITIES;
        config.strategy_config.data_freq = DataFrequency::DAILY;
        config.strategy_config.data_type = "ohlcv";
        config.strategy_config.commission_rate = 0.001;  // 10 basis points for equities
        config.strategy_config.slippage_model = 0.5;     // 0.5 basis point
        config.strategy_config.warmup_days = 20;         // 20-day warmup for mean reversion

        // Get equity symbols from database
        INFO("Loading equity symbols from database...");
        auto symbols_result = db->get_symbols(AssetClass::EQUITIES);
        if (symbols_result.is_error()) {
            std::cerr << "Failed to get symbols: " << symbols_result.error()->what() << std::endl;
            return 1;
        }

        auto symbols = symbols_result.value();
        std::cout << "Found " << symbols.size() << " equity symbols in database" << std::endl;

        // For testing, you can limit to specific symbols or use all
        // Uncomment to limit to specific tickers:
        symbols = {"AAPL", "MSFT", "GOOGL", "AMZN", "TSLA"};

        config.strategy_config.symbols = symbols;

        std::cout << "Testing with symbols: ";
        for (const auto& symbol : config.strategy_config.symbols) {
            std::cout << symbol << " ";
        }
        std::cout << std::endl;

        // Configure portfolio settings
        config.portfolio_config.initial_capital = 100000.0;  // $100K
        config.portfolio_config.use_risk_management = false; // Disable for simple backtest
        config.portfolio_config.use_optimization = false;

        std::cout << "Initial capital: $" << config.portfolio_config.initial_capital << std::endl;
        std::cout << "Commission rate: " << (config.strategy_config.commission_rate * 100) << " bps"
                  << std::endl;
        std::cout << "Slippage model: " << config.strategy_config.slippage_model << " bps"
                  << std::endl;

        INFO("Configuration loaded. Testing " +
             std::to_string(config.strategy_config.symbols.size()) + " symbols from " +
             std::to_string(std::chrono::system_clock::to_time_t(config.strategy_config.start_date)) +
             " to " +
             std::to_string(std::chrono::system_clock::to_time_t(config.strategy_config.end_date)));

        // Initialize backtest engine
        INFO("Initializing backtest engine...");
        auto engine = std::make_unique<BacktestEngine>(config, db);

        // Create mean reversion strategy configuration
        StrategyConfig mr_strategy_config;
        mr_strategy_config.capital_allocation = config.portfolio_config.initial_capital.as_double();
        mr_strategy_config.asset_classes = {AssetClass::EQUITIES};
        mr_strategy_config.frequencies = {DataFrequency::DAILY};
        mr_strategy_config.max_drawdown = 0.3;   // 30% max drawdown for equities
        mr_strategy_config.max_leverage = 2.0;   // Lower leverage for equities
        mr_strategy_config.save_positions = false;
        mr_strategy_config.save_signals = false;
        mr_strategy_config.save_executions = false;

        // Add position limits for equities (whole shares)
        for (const auto& symbol : config.strategy_config.symbols) {
            mr_strategy_config.position_limits[symbol] = 10000.0;  // Max 10,000 shares per symbol
            mr_strategy_config.trading_params[symbol] = 1.0;       // Price per share (not multiplier)
            mr_strategy_config.costs[symbol] = config.strategy_config.commission_rate.as_double();
        }

        // Configure mean reversion parameters
        MeanReversionConfig mr_config;
        mr_config.lookback_period = 20;        // 20-day moving average
        mr_config.entry_threshold = 2.0;       // Enter at 2 standard deviations
        mr_config.exit_threshold = 0.5;        // Exit at 0.5 standard deviations
        mr_config.risk_target = 0.15;          // 15% annualized risk
        mr_config.position_size = 0.1;         // 10% of capital per position
        mr_config.vol_lookback = 20;           // 20-day volatility
        mr_config.use_stop_loss = true;
        mr_config.stop_loss_pct = 0.05;        // 5% stop loss

        // Create and initialize the strategy
        INFO("Initializing MeanReversionStrategy for equities...");
        std::cout << "Strategy capital allocation: $" << mr_strategy_config.capital_allocation << std::endl;
        std::cout << "Max leverage: " << mr_strategy_config.max_leverage << "x" << std::endl;
        std::cout << "Lookback period: " << mr_config.lookback_period << " days" << std::endl;
        std::cout << "Entry threshold: " << mr_config.entry_threshold << " std devs" << std::endl;

        // Create registry pointer
        auto registry_ptr = std::shared_ptr<InstrumentRegistry>(&registry, [](InstrumentRegistry*) {});

        auto mr_strategy = std::make_shared<MeanReversionStrategy>(
            "EQUITY_MEAN_REVERSION", mr_strategy_config, mr_config, db, registry_ptr);

        // The backtest engine will call initialize() and start() internally
        // Run the backtest
        INFO("Running backtest...");
        auto result = engine->run(mr_strategy);

        if (result.is_error()) {
            std::cerr << "Backtest failed: " << result.error()->what() << std::endl;
            std::cerr << "Error code: " << static_cast<int>(result.error()->code()) << std::endl;
            return 1;
        }

        INFO("Backtest completed successfully");

        // Analyze and display results
        const auto& backtest_results = result.value();

        std::cout << "\n======= Equity Mean Reversion Backtest Results =======" << std::endl;
        std::cout << std::fixed << std::setprecision(2);
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
        std::cout << "Profit Factor: " << backtest_results.profit_factor << std::endl;
        std::cout << "Average Win: $" << backtest_results.avg_win << std::endl;
        std::cout << "Average Loss: $" << backtest_results.avg_loss << std::endl;
        std::cout << "Max Win: $" << backtest_results.max_win << std::endl;
        std::cout << "Max Loss: $" << backtest_results.max_loss << std::endl;

        // Save results to database
        INFO("Saving backtest results to database...");
        try {
            auto save_result = engine->save_results_to_db(backtest_results, "equity_mean_reversion");
            if (save_result.is_error()) {
                WARN("Failed to save results to database: " + std::string(save_result.error()->what()));
            } else {
                INFO("Successfully saved backtest results to database");
            }
        } catch (const std::exception& e) {
            WARN("Exception during database save: " + std::string(e.what()));
        }

        // Save to CSV
        INFO("Saving results to CSV...");
        try {
            auto csv_result = engine->save_results_to_csv(backtest_results, "equity_mean_reversion");
            if (csv_result.is_error()) {
                WARN("Failed to save CSV: " + std::string(csv_result.error()->what()));
            } else {
                INFO("Successfully saved results to CSV");
            }
        } catch (const std::exception& e) {
            WARN("Exception during CSV save: " + std::string(e.what()));
        }

        // Cleanup
        INFO("Cleaning up...");
        engine.reset();

        INFO("Equity mean reversion backtest completed successfully");

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
