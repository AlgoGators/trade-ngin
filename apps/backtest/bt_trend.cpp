#include <iostream>
#include <ctime>
#include <memory>
#include <chrono>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/database.hpp"
#include "trade_ngin/core/credential_store.hpp"
#include "trade_ngin/backtest/backtest_engine.hpp"
#include "trade_ngin/backtest/backtest_config_manager.hpp"
#include "trade_ngin/strategy/trend_following.hpp"
#include "trade_ngin/risk/portfolio_manager.hpp"
#include "trade_ngin/markets/instrument_registry.hpp"
#include "trade_ngin/core/log_manager.hpp"

/*
TO-DO:
    - Check that risk management is working
    - Check that optimization is working
        - Need to fix / check in backtest_engine.cpp (run_portfolio())
    - Visualize results (matplotlib?)
    - Check that slippage model is working
    - Fix data access for strategies & TCA
    - Update all the configs to save / load to a file
    - Remove wait times in tests (if possible)
    - Fix Arrow no discard attributes`
    - Fix weighting in position sizing
        - Currently, the position sizing is based on the number of symbols in the strategy
        - Need to change it to come from dyn opt
    - Fix logging across system. For some reason, some of the logger files do not align
    with their respective components. (i.e. the positions populate in the risk manager log file
    but not in the strategy log file)
        - Use a single logger instance across the system
        - Use a single log file for each run
*/

using namespace trade_ngin;
using namespace trade_ngin::backtest;

// Function to display usage information
void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [options]" << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  -h, --help                     Display this help message" << std::endl;
    std::cout << "  -c, --config <filename>        Load configuration from file" << std::endl;
    std::cout << "  -s, --save <filename>          Save configuration to file" << std::endl;
    std::cout << "  -d, --config-dir <directory>   Specify the configuration directory (default: config)" << std::endl;
    std::cout << "  -o, --output-dir <directory>   Specify the output directory for results (default: apps/backtest/results)" << std::endl;
    std::cout << "  --start-date <YYYY-MM-DD>      Start date for backtest" << std::endl;
    std::cout << "  --end-date <YYYY-MM-DD>        End date for backtest" << std::endl;
    std::cout << "  --capital <amount>             Initial capital amount" << std::endl;
    std::cout << "  --symbols <sym1,sym2,...>      Comma-separated list of symbols" << std::endl;
    std::cout << "  --run-id <id>                  Specify a run ID for the backtest" << std::endl;
    std::cout << "  --debug                        Enable debug logging" << std::endl;
}

// Function to parse a date string in YYYY-MM-DD format to a time_point
std::chrono::system_clock::time_point parse_date(const std::string& date_str) {
    std::tm tm = {};
    std::stringstream ss(date_str);
    
    // Parse YYYY-MM-DD format
    ss >> std::get_time(&tm, "%Y-%m-%d");
    
    if (ss.fail()) {
        throw std::runtime_error("Failed to parse date: " + date_str + ". Expected format: YYYY-MM-DD");
    }
    
    // Convert to time_t
    auto time = std::mktime(&tm);
    return std::chrono::system_clock::from_time_t(time);
}

// Function to generate a timestamp-based run ID if none is provided
std::string generate_run_id() {
    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm* now_tm = std::localtime(&now_time_t);
    
    std::stringstream ss;
    ss << "BT_" << std::put_time(now_tm, "%Y%m%d%H%M%S");
    return ss.str();
}

int main(int argc, char* argv[]) {
    std::cout << "=== Starting Backtest Setup ===" << std::endl;
    
    // Default configuration values
    std::string config_file = "";
    std::string save_file = "";
    std::string config_dir = "config";
    std::string output_dir = "apps/backtest/results";
    std::string start_date = "";
    std::string end_date = "";
    std::string symbols_list = "";
    std::string run_id = "";
    double initial_capital = 0.0;  // 0 means use default from config
    bool debug_mode = false;
    
    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "-c" || arg == "--config") {
            if (i + 1 < argc) {
                config_file = argv[++i];
            } else {
                std::cerr << "Error: --config requires a filename argument." << std::endl;
                return 1;
            }
        } else if (arg == "-s" || arg == "--save") {
            if (i + 1 < argc) {
                save_file = argv[++i];
            } else {
                std::cerr << "Error: --save requires a filename argument." << std::endl;
                return 1;
            }
        } else if (arg == "-d" || arg == "--config-dir") {
            if (i + 1 < argc) {
                config_dir = argv[++i];
            } else {
                std::cerr << "Error: --config-dir requires a directory argument." << std::endl;
                return 1;
            }
        } else if (arg == "-o" || arg == "--output-dir") {
            if (i + 1 < argc) {
                output_dir = argv[++i];
            } else {
                std::cerr << "Error: --output-dir requires a directory argument." << std::endl;
                return 1;
            }
        } else if (arg == "--start-date") {
            if (i + 1 < argc) {
                start_date = argv[++i];
            } else {
                std::cerr << "Error: --start-date requires a date argument (YYYY-MM-DD)." << std::endl;
                return 1;
            }
        } else if (arg == "--end-date") {
            if (i + 1 < argc) {
                end_date = argv[++i];
            } else {
                std::cerr << "Error: --end-date requires a date argument (YYYY-MM-DD)." << std::endl;
                return 1;
            }
        } else if (arg == "--capital") {
            if (i + 1 < argc) {
                try {
                    initial_capital = std::stod(argv[++i]);
                } catch (const std::exception& e) {
                    std::cerr << "Error: --capital requires a numeric argument." << std::endl;
                    return 1;
                }
            } else {
                std::cerr << "Error: --capital requires a numeric argument." << std::endl;
                return 1;
            }
        } else if (arg == "--symbols") {
            if (i + 1 < argc) {
                symbols_list = argv[++i];
            } else {
                std::cerr << "Error: --symbols requires a comma-separated list." << std::endl;
                return 1;
            }
        } else if (arg == "--run-id") {
            if (i + 1 < argc) {
                run_id = argv[++i];
            } else {
                std::cerr << "Error: --run-id requires an identifier argument." << std::endl;
                return 1;
            }
        } else if (arg == "--debug") {
            debug_mode = true;
        } else {
            std::cerr << "Unknown option: " << arg << std::endl;
            print_usage(argv[0]);
            return 1;
        }
    }
    
    // Generate a run ID if not provided
    if (run_id.empty()) {
        run_id = generate_run_id();
    }
    
    // Create output directory for this run
    std::string run_output_dir = output_dir + "/" + run_id;
    std::filesystem::create_directories(run_output_dir);
    
    try {
        // Initialize the logging system
        LoggerConfig logger_config;
        logger_config.min_level = debug_mode ? LogLevel::DEBUG : LogLevel::INFO;
        logger_config.destination = LogDestination::BOTH;
        logger_config.log_directory = "logs";
        logger_config.filename_prefix = run_id;
        logger_config.allow_reinitialize = true;
        
        // Use the LogManager to initialize logging system-wide
        LogManager::instance().initialize(logger_config);
        
        // Configure the backtest component logger
        LogManager::instance().configure_component_logger("backtest_engine");
        
        INFO("Logging system initialized successfully with run ID: " + run_id);
        
        // Create or load config manager
        BacktestConfigManager config_manager(config_dir);
        
        // If a config file was specified, load it
        if (!config_file.empty()) {
            INFO("Loading configuration from file: " + config_file);
            auto load_result = config_manager.load(config_file);
            
            if (load_result.is_error()) {
                std::cerr << "Failed to load configuration: " << load_result.error()->what() << std::endl;
                return 1;
            }
            INFO("Configuration loaded successfully from: " + config_file);
        } else {
            // Create default configuration
            INFO("Creating default configuration...");
            auto default_config_result = BacktestConfigManager::create_default();
            
            if (default_config_result.is_error()) {
                std::cerr << "Failed to create default configuration: " << default_config_result.error()->what() << std::endl;
                return 1;
            }
            
            config_manager = default_config_result.value();
            INFO("Default configuration created successfully");
        }
        
        // Get the configurations
        auto& backtest_config = config_manager.backtest_config();
        auto& strategy_config = config_manager.strategy_config();
        auto& trend_config = config_manager.trend_config();
        
        // Override with command line arguments if provided
        if (!output_dir.empty()) {
            backtest_config.csv_output_path = run_output_dir;
        }
        
        // Set run ID
        backtest_config.run_id = run_id;
        
        // Parse date overrides if provided
        if (!start_date.empty()) {
            try {
                backtest_config.strategy_config.start_date = parse_date(start_date);
                INFO("Start date set to: " + start_date);
            } catch (const std::exception& e) {
                std::cerr << "Error parsing start date: " << e.what() << std::endl;
                return 1;
            }
        }
        
        if (!end_date.empty()) {
            try {
                backtest_config.strategy_config.end_date = parse_date(end_date);
                INFO("End date set to: " + end_date);
            } catch (const std::exception& e) {
                std::cerr << "Error parsing end date: " << e.what() << std::endl;
                return 1;
            }
        }
        
        // Set initial capital if provided
        if (initial_capital > 0) {
            backtest_config.portfolio_config.initial_capital = initial_capital;
            strategy_config.capital_allocation = initial_capital;
            INFO("Initial capital set to: $" + std::to_string(initial_capital));
        }
        
        // Parse symbols if provided
        if (!symbols_list.empty()) {
            std::vector<std::string> symbols;
            std::stringstream ss(symbols_list);
            std::string symbol;
            
            while (std::getline(ss, symbol, ',')) {
                symbols.push_back(symbol);
            }
            
            if (!symbols.empty()) {
                backtest_config.strategy_config.symbols = symbols;
                INFO("Using provided symbols: " + symbols_list);
            }
        }
        
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
        
        // If symbols not provided and not in config, get them from database
        if (backtest_config.strategy_config.symbols.empty()) {
            auto symbols = db->get_symbols(trade_ngin::AssetClass::FUTURES);

            if (symbols.is_ok()) {
                backtest_config.strategy_config.symbols = symbols.value();
                strategy_config.position_limits.clear();
                strategy_config.costs.clear();
                
                // Update strategy config with symbols
                for (const auto& symbol : backtest_config.strategy_config.symbols) {
                    strategy_config.position_limits[symbol] = 1000.0;  // Max 1000 units per symbol
                    strategy_config.costs[symbol] = backtest_config.strategy_config.commission_rate;
                }
            } else {
                // Detailed error logging
                ERROR("Failed to get symbols: " + std::string(symbols.error()->what()));
                throw std::runtime_error("Failed to get symbols: " + symbols.error()->to_string());
            }
        }
        
        // Display configuration summary
        std::cout << "=== Backtest Configuration Summary ===" << std::endl;
        std::cout << "Run ID: " << run_id << std::endl;
        std::cout << "Symbols: " << backtest_config.strategy_config.symbols.size() << " total" << std::endl;
        
        for (size_t i = 0; i < std::min(size_t(10), backtest_config.strategy_config.symbols.size()); ++i) {
            std::cout << backtest_config.strategy_config.symbols[i] << " ";
        }
        if (backtest_config.strategy_config.symbols.size() > 10) {
            std::cout << "... (and " << (backtest_config.strategy_config.symbols.size() - 10) << " more)";
        }
        std::cout << std::endl;
        
        std::cout << "Initial capital: $" << backtest_config.portfolio_config.initial_capital << std::endl;
        std::cout << "Commission rate: " << (backtest_config.strategy_config.commission_rate * 100) << " bps" << std::endl;
        std::cout << "Slippage model: " << backtest_config.strategy_config.slippage_model << " bps" << std::endl;
        
        auto start_time_t = std::chrono::system_clock::to_time_t(backtest_config.strategy_config.start_date);
        auto end_time_t = std::chrono::system_clock::to_time_t(backtest_config.strategy_config.end_date);
        
        std::cout << "Backtest period: " << std::put_time(std::localtime(&start_time_t), "%Y-%m-%d") 
                 << " to " << std::put_time(std::localtime(&end_time_t), "%Y-%m-%d") << std::endl;
                 
        std::cout << "Risk target: " << (trend_config.risk_target * 100) << "%" << std::endl;
        std::cout << "IDM: " << trend_config.idm << std::endl;
        
        // Save configuration if requested
        if (!save_file.empty()) {
            INFO("Saving configuration to file: " + save_file);
            auto save_result = config_manager.save(save_file);
            
            if (save_result.is_error()) {
                std::cerr << "Failed to save configuration: " << save_result.error()->what() << std::endl;
                // Continue with backtest anyway
            } else {
                INFO("Configuration saved successfully to: " + save_file);
            }
        }
        
        // Initialize backtest engine
        INFO("Initializing backtest engine...");
        auto engine = std::make_unique<trade_ngin::backtest::BacktestEngine>(backtest_config, db);

        // Create and initialize the trend following strategy
        INFO("Initializing TrendFollowingStrategy...");
        std::cout << "Strategy capital allocation: $" << strategy_config.capital_allocation << std::endl;
        std::cout << "Max leverage: " << strategy_config.max_leverage << "x" << std::endl;
        
        auto tf_strategy = std::make_shared<trade_ngin::TrendFollowingStrategy>(
            "TREND_FOLLOWING", strategy_config, trend_config, db, &registry);
        
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
        auto portfolio = std::make_shared<trade_ngin::PortfolioManager>(backtest_config.portfolio_config);
        auto add_result = portfolio->add_strategy(tf_strategy, 1.0, 
                                backtest_config.portfolio_config.use_optimization,
                                backtest_config.portfolio_config.use_risk_management);
        if (add_result.is_error()) {
            std::cerr << "Failed to add strategy to portfolio: " << add_result.error()->what() << std::endl;
            return 1;
        }
        INFO("Strategy added to portfolio successfully"); 

        // Run the backtest
        INFO("Running backtest...");
        std::cout << "Running backtest..." << std::endl;
        
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
        
        // Suggest visualizing results
        std::cout << std::endl;
        std::cout << "Results saved to: " << run_output_dir << std::endl;
        std::cout << "To visualize results, run: ./visualize_results.sh " << run_output_dir << std::endl;
        
        INFO("Backtest application completed successfully");
        
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "Unexpected error: " << e.what() << std::endl;
        ERROR("Unexpected error: " + std::string(e.what()));
        return 1;
    }
}