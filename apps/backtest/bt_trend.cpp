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

/*
TO-DO:
    - Check that risk management is working
    - Check that optimization is working
    - Visualize results (matplotlib?)
    - Check that slippage model is working
    - Fix data access for strategies & TCA
    - Update all the configs to save / load to a file
    - Remove wait times in tests (if possible)
*/

using namespace trade_ngin;
using namespace trade_ngin::backtest;

int main() {
    std::cout << "=== Starting Backtest Setup ===" << std::endl;
    INFO("Starting trend following backtest application");

    try {
        // Initialize the logger
        LoggerConfig logger_config;
        logger_config.min_level = LogLevel::DEBUG;
        logger_config.destination = LogDestination::BOTH;
        logger_config.log_directory = "logs";
        logger_config.filename_prefix = "bt_trend";
        Logger::instance().initialize(logger_config);
        INFO("Logger initialized successfully");
        
        // Setup database connection pool
        INFO("Initializing database connection pool...");
        auto credentials = std::make_shared<trade_ngin::CredentialStore>("./config.json");
        std::string username = credentials->get<std::string>("database", "username");
        std::string password = credentials->get<std::string>("database", "password");
        std::string host = credentials->get<std::string>("database", "host");
        std::string port = credentials->get<std::string>("database", "port");
        std::string db_name = credentials->get<std::string>("database", "name");

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
        auto load_result = registry.load_instruments(AssetClass::FUTURES);
        if (load_result.is_error()) {
            std::cerr << "Warning: Failed to load futures instruments: " << load_result.error()->what() << std::endl;
            std::cerr << "Continuing with configuration-based contract specifications." << std::endl;
        } else {
            INFO("Successfully loaded futures instruments from database");
        }
        
        // Configure backtest parameters
        INFO("Loading configuration...");

        trade_ngin::backtest::BacktestConfig config;

        // Convert timestamps to proper format
        auto now = std::chrono::system_clock::now();
        auto now_time_t = std::chrono::system_clock::to_time_t(now);
        std::tm* now_tm = std::localtime(&now_time_t);

        // Set start date to 3 years ago
        std::tm start_tm = *now_tm;
        start_tm.tm_year -= 3; // 3 years ago
        auto start_time_t = std::mktime(&start_tm);
        config.strategy_config.start_date = std::chrono::system_clock::from_time_t(start_time_t);

        // Set end date to today
        config.strategy_config.end_date = now;

        config.strategy_config.asset_class = trade_ngin::AssetClass::FUTURES;
        config.strategy_config.data_freq = trade_ngin::DataFrequency::DAILY;
        config.strategy_config.commission_rate = 0.0005; // 5 basis points
        config.strategy_config.slippage_model = 1.0; // 1 basis point
        
        // Set only one symbol for testing (comment this and uncomment below to load all symbols)
        auto symbols = std::vector<std::string>{"6B.v.0"};
        config.strategy_config.symbols = symbols;

        // UNCOMMENT TO LOAD SYMBOLS FROM DATABASE
        /* 
        auto symbols = db->get_symbols(trade_ngin::AssetClass::FUTURES);
        if (symbols.is_ok()) {
            config.strategy_config.symbols = symbols.value();
        } else {
            // Detailed error logging
            ERROR("Failed to get symbols: " + std::string(symbols.error()->what()));
            throw std::runtime_error("Failed to get symbols: " + symbols.error()->to_string());
        } */
        
        std::cout << "Symbols: ";
        for (const auto& symbol : config.strategy_config.symbols) {
            std::cout << symbol << " ";
        }
        std::cout << std::endl;

        // Configure portfolio settings
        config.portfolio_config.initial_capital = 1000000.0;  // $1M
        config.portfolio_config.use_risk_management = true;
        config.portfolio_config.use_optimization = true;
        
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
        config.portfolio_config.opt_config.capital = config.portfolio_config.initial_capital;
        config.portfolio_config.opt_config.asymmetric_risk_buffer = 0.1;
        config.portfolio_config.opt_config.cost_penalty_scalar = 10;
        config.portfolio_config.opt_config.max_iterations = 100;
        config.portfolio_config.opt_config.convergence_threshold = 1e-6;
        
        // Initialize backtest engine
        INFO("Initializing backtest engine...");
        auto engine = std::make_unique<trade_ngin::backtest::BacktestEngine>(config, db);

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
        tf_config.capital_allocation = config.portfolio_config.initial_capital;
        tf_config.max_leverage = 4.0;
        tf_config.save_positions = false;
        tf_config.save_signals = false;
        tf_config.save_executions = false;
        
        // Add position limits and contract sizes
        for (const auto& symbol : config.strategy_config.symbols) {
            tf_config.position_limits[symbol] = 1000.0;  // Max 1000 units per symbol
            tf_config.trading_params[symbol] = 1.0;      // *CHANGE*: Contract size multiplier
            tf_config.costs[symbol] = config.strategy_config.commission_rate;
        }
        
        // Configure trend following parameters
        trade_ngin::TrendFollowingConfig trend_config;
        trend_config.weight = 1.0 / (config.strategy_config.symbols.size());  // Equal weight for each symbol
        trend_config.risk_target = 0.2;       // Target 20% annualized risk
        trend_config.idm = 2.5;               // Instrument diversification multiplier
        trend_config.use_position_buffering = true;
        trend_config.ema_windows = {
            {2, 8}, {4, 16}, {8, 32}, {16, 64}, {32, 128}, {64, 256}
        };
        trend_config.vol_lookback_short = 32;  // Short vol lookback
        trend_config.vol_lookback_long = 252;  // Long vol lookback
        trend_config.fdm = {
            {1, 1.0}, {2, 1.03}, {3, 1.08}, {4, 1.13}, {5, 1.19}, {6, 1.26}
        };
        
        // Create and initialize the strategies
        INFO("Initializing TrendFollowingStrategy...");
        std::cout << "Strategy capital allocation: $" << tf_config.capital_allocation << std::endl;
        std::cout << "Max leverage: " << tf_config.max_leverage << "x" << std::endl;
        
        auto tf_strategy = std::make_shared<trade_ngin::TrendFollowingStrategy>(
            "TREND_FOLLOWING", tf_config, trend_config, db);
        
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
        INFO("Backtest engine initialized, starting backtest run...");
        std::cout << "\n=== Starting Backtest Execution ===" << std::endl;
        std::cout << "Time period: " << 
            std::chrono::system_clock::to_time_t(config.strategy_config.start_date) << " to " <<
            std::chrono::system_clock::to_time_t(config.strategy_config.end_date) << std::endl;
        
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

        // Perform transaction cost analysis
        trade_ngin::backtest::TCAConfig tca_config;
        tca_config.pre_trade_window = std::chrono::minutes(5);
        tca_config.post_trade_window = std::chrono::minutes(5);
        tca_config.spread_factor = 1.0;
        tca_config.market_impact_coefficient = 1.0;
        tca_config.volatility_multiplier = 1.5;
        tca_config.use_arrival_price = true;
        tca_config.use_vwap = true;
        tca_config.use_twap = true;
        tca_config.calculate_opportunity_costs = true;
        tca_config.analyze_timing_costs = true;
        
        auto tca = std::make_unique<trade_ngin::backtest::TransactionCostAnalyzer>(tca_config);
        
        std::cout << "\n======= Transaction Cost Analysis =======" << std::endl;
        
        // Calculate total transaction costs
        double total_commission = 0.0;
        double total_market_impact = 0.0;
        double total_spread_cost = 0.0;
        double total_timing_cost = 0.0;
        
        // Get another database connection for loading market data for TCA
        auto tca_db_guard = DatabasePool::instance().acquire_connection();
        auto tca_db = tca_db_guard.get();
        
        if (!tca_db || !tca_db->is_connected()) {
            WARN("Failed to acquire database connection for TCA. Skipping detailed transaction cost analysis.");
        } else {
            // Load market data for TCA - simplified for this example
            std::vector<trade_ngin::Bar> market_data;
            
            // Group executions by symbol for analysis
            std::unordered_map<std::string, std::vector<trade_ngin::ExecutionReport>> executions_by_symbol;
            for (const auto& exec : backtest_results.executions) {
                executions_by_symbol[exec.symbol].push_back(exec);
            }
            
            // Analyze executions by symbol
            for (const auto& [symbol, executions] : executions_by_symbol) {
                auto tca_result = tca->analyze_trade_sequence(executions, market_data);
                if (tca_result.is_ok()) {
                    const auto& metrics = tca_result.value();
                    std::cout << "Symbol: " << symbol << std::endl;
                    std::cout << "  Commission: $" << metrics.commission << std::endl;
                    std::cout << "  Spread Cost: $" << metrics.spread_cost << std::endl;
                    std::cout << "  Market Impact: $" << metrics.market_impact << std::endl;
                    std::cout << "  Timing Cost: $" << metrics.timing_cost << std::endl;
                    std::cout << "  Participation Rate: " << metrics.participation_rate * 100.0 << "%" << std::endl;
                    std::cout << "  Execution Time: " << metrics.execution_time.count() << "ms" << std::endl;
                    
                    total_commission += metrics.commission;
                    total_market_impact += metrics.market_impact;
                    total_spread_cost += metrics.spread_cost;
                    total_timing_cost += metrics.timing_cost;
                }
            }
        }
        
        // Display total transaction costs
        std::cout << "\nTotal Transaction Costs:" << std::endl;
        std::cout << "  Total Commission: $" << total_commission << std::endl;
        std::cout << "  Total Spread Cost: $" << total_spread_cost << std::endl;
        std::cout << "  Total Market Impact: $" << total_market_impact << std::endl;
        std::cout << "  Total Timing Cost: $" << total_timing_cost << std::endl;
        std::cout << "  Total Costs: $" << (total_commission + total_spread_cost + total_market_impact + total_timing_cost) << std::endl;
        std::cout << "  % of Total Return: " << ((total_commission + total_spread_cost + total_market_impact + total_timing_cost) / 
                                               (backtest_results.total_return * config.portfolio_config.initial_capital)) * 100.0 << "%" << std::endl;
        
        // Analyze portfolio performance
        std::cout << "\n======= Portfolio Analysis =======" << std::endl;
        
        // Get portfolio positions
        auto portfolio_positions = portfolio->get_portfolio_positions();
        
        // Calculate portfolio metrics
        double portfolio_value = config.portfolio_config.initial_capital;
        for (const auto& [symbol, pos] : portfolio_positions) {
            // Assume we use the last price from backtest results
            double last_price = 0.0;
            if (!backtest_results.executions.empty()) {
                for (auto it = backtest_results.executions.rbegin(); it != backtest_results.executions.rend(); ++it) {
                    if (it->symbol == symbol) {
                        last_price = it->fill_price;
                        break;
                    }
                }
            }
            
            if (last_price > 0.0) {
                portfolio_value += pos.quantity * last_price;
                
                std::cout << "Symbol: " << symbol << std::endl;
                std::cout << "  Position: " << pos.quantity << " shares" << std::endl;
                std::cout << "  Average Price: $" << pos.average_price << std::endl;
                std::cout << "  Last Price: $" << last_price << std::endl;
                std::cout << "  P&L: $" << (last_price - pos.average_price) * pos.quantity << std::endl;
                std::cout << "  Weight: " << (pos.quantity * last_price / portfolio_value) * 100.0 << "%" << std::endl;
            }
        }
        
        std::cout << "\nFinal Portfolio Value: $" << portfolio_value << std::endl;
        std::cout << "Total Return: " << ((portfolio_value / config.portfolio_config.initial_capital) - 1.0) * 100.0 << "%" << std::endl;
        
        std::string results_dir = "apps/backtest/results";

        // Create the directory if it doesn't exist (platform-specific)
        #ifdef _WIN32
            std::string mkdir_command = "mkdir " + results_dir + " 2> nul";
        #else
            std::string mkdir_command = "mkdir -p " + results_dir + " 2> /dev/null";
        #endif
        system(mkdir_command.c_str());

        // Save results to database and CSV
        INFO("Writing results to file...");
        std::string run_id = "TF_PORTFOLIO_" + std::to_string(
            std::chrono::system_clock::now().time_since_epoch().count());
            
        auto save_result = engine->save_results(backtest_results, run_id);
        if (save_result.is_error()) {
            std::cerr << "Warning: Failed to save results to database: " << save_result.error()->what() << std::endl;
        } else {
            std::cout << "Results saved to database with ID: " << run_id << std::endl;
        }
        
        // Save equity curve to CSV
        std::string equity_curve_filename = results_dir + "/equity_curve_" + run_id + ".csv";
        std::ofstream equity_curve_file(equity_curve_filename);
        if (equity_curve_file.is_open()) {
            equity_curve_file << "Date,Equity\n";
            for (const auto& [timestamp, equity] : backtest_results.equity_curve) {
                // Convert timestamp to string
                auto time_t = std::chrono::system_clock::to_time_t(timestamp);
                std::tm tm;
                trade_ngin::core::safe_localtime(&time_t, &tm);
                std::stringstream ss;
                ss << std::put_time(&tm, "%Y-%m-%d");
                
                equity_curve_file << ss.str() << "," << equity << "\n";
            }
            equity_curve_file.close();
            std::cout << "Equity curve saved to " << equity_curve_filename << std::endl;
        }
        
        // Save trade list to CSV
        std::string trades_filename = results_dir + "/trades_" + run_id + ".csv";
        std::ofstream trades_file(trades_filename);
        if (trades_file.is_open()) {
            trades_file << "Symbol,Side,Quantity,Price,DateTime,Commission\n";
            for (const auto& exec : backtest_results.executions) {
                auto time_t = std::chrono::system_clock::to_time_t(exec.fill_time);
                std::tm tm;
                trade_ngin::core::safe_localtime(&time_t, &tm);
                std::stringstream ss;
                ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
                
                trades_file << exec.symbol << ","
                          << (exec.side == trade_ngin::Side::BUY ? "BUY" : "SELL") << ","
                          << exec.filled_quantity << ","
                          << exec.fill_price << ","
                          << ss.str() << ","
                          << exec.commission << "\n";
            }
            trades_file.close();
            std::cout << "Trade list saved to " << trades_filename << std::endl;
        }

        // Save performance metrics to CSV
        std::string metrics_filename = results_dir + "/metrics_" + run_id + ".csv";
        std::ofstream metrics_file(metrics_filename);
        if (metrics_file.is_open()) {
            metrics_file << "Metric,Value\n";
            metrics_file << "Total Return," << backtest_results.total_return << "\n";
            metrics_file << "Sharpe Ratio," << backtest_results.sharpe_ratio << "\n";
            metrics_file << "Sortino Ratio," << backtest_results.sortino_ratio << "\n";
            metrics_file << "Max Drawdown," << backtest_results.max_drawdown << "\n";
            metrics_file << "Calmar Ratio," << backtest_results.calmar_ratio << "\n";
            metrics_file << "Volatility," << backtest_results.volatility << "\n";
            metrics_file << "Win Rate," << backtest_results.win_rate << "\n";
            metrics_file << "Total Trades," << backtest_results.total_trades << "\n";
            metrics_file.close();
            std::cout << "Performance metrics saved to " << metrics_filename << std::endl;
        }
        
        INFO("Backtest application completed successfully");
        
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "Unexpected error: " << e.what() << std::endl;
        return 1;
    }
}