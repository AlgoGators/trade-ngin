#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <chrono>
#include <ctime>
#include <set>
#include <algorithm>
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/data/credential_store.hpp"
#include "trade_ngin/data/database_pooling.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/data/conversion_utils.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/instruments/futures.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/strategy/trend_following.hpp"
#include "trade_ngin/core/email_sender.hpp"

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
        logger_config.min_level = LogLevel::DEBUG;
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
        INFO("DEBUG: Start date: " + std::to_string(std::chrono::system_clock::to_time_t(start_date)));
        INFO("DEBUG: End date: " + std::to_string(std::chrono::system_clock::to_time_t(end_date)));
        INFO("DEBUG: Target date (now): " + std::to_string(std::chrono::system_clock::to_time_t(now)));

        double initial_capital = 500000.0;  // $500k
        double commission_rate = 0.0005;    // 5 basis points
        double slippage_model = 1.0;       // 1 basis point

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
        std::cout << "Commission rate: " << (commission_rate * 100) << " bps" << std::endl;
        std::cout << "Slippage model: " << slippage_model << " bps" << std::endl;

        INFO("Configuration loaded successfully. Processing " +
             std::to_string(symbols.size()) + " symbols from " +
             std::to_string(std::chrono::system_clock::to_time_t(start_date)) +
             " to " +
             std::to_string(std::chrono::system_clock::to_time_t(end_date)));

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
            ERROR("Margin metadata validation failed for one or more futures instruments. Aborting run.");
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
        portfolio_config.max_strategy_allocation = 1.0;     // Only have one strategy currently
        portfolio_config.min_strategy_allocation = 0.1;
        portfolio_config.use_optimization = true;
        portfolio_config.use_risk_management = true;
        portfolio_config.opt_config = opt_config;
        portfolio_config.risk_config = risk_config;

        // Create trend following strategy configuration
        trade_ngin::StrategyConfig tf_config;
        tf_config.capital_allocation = initial_capital;  // Match backtest (reserve handled separately)
        tf_config.asset_classes = {trade_ngin::AssetClass::FUTURES};
        tf_config.frequencies = {trade_ngin::DataFrequency::DAILY};
        tf_config.max_drawdown = 0.4;   // Match backtest defaults
        tf_config.max_leverage = 4.0;
        tf_config.save_positions = false;  // Disable automatic position saving (we'll do it manually)
        tf_config.save_signals = false;
        tf_config.save_executions = false;  // No executions in daily mode

        // Add position limits and contract sizes
        for (const auto& symbol : symbols) {
            tf_config.position_limits[symbol] = 500.0;  // Conservative limits
            tf_config.costs[symbol] = commission_rate;
        }

        // Configure trend following parameters
        trade_ngin::TrendFollowingConfig trend_config;
        trend_config.weight = 0.03;       // Match backtest defaults
        trend_config.risk_target = 0.2;
        trend_config.idm = 2.5;           // Instrument diversification multiplier
        trend_config.use_position_buffering = true;  // Use buffering for daily trading
        trend_config.ema_windows = {{2, 8}, {4, 16}, {8, 32}, {16, 64}, {32, 128}, {64, 256}};
        trend_config.vol_lookback_short = 32;
        trend_config.vol_lookback_long = 252;
        trend_config.fdm = {{1, 1.0}, {2, 1.03}, {3, 1.08}, {4, 1.13}, {5, 1.19}, {6, 1.26}};

        // Create and initialize the strategies
        // Before TrendFollowingStrategy
        std::cerr << "Before TrendFollowingStrategy: initialized="
                  << Logger::instance().is_initialized() << std::endl;
        INFO("Initializing TrendFollowingStrategy...");
        std::cout << "Strategy capital allocation: $" << tf_config.capital_allocation << std::endl;
        std::cout << "Max leverage: " << tf_config.max_leverage << "x" << std::endl;

        // Create a shared_ptr that doesn't own the singleton registry
        auto registry_ptr =
            std::shared_ptr<InstrumentRegistry>(&registry, [](InstrumentRegistry*) {});

        auto tf_strategy = std::make_shared<trade_ngin::TrendFollowingStrategy>(
            "LIVE_TREND_FOLLOWING", tf_config, trend_config, db, registry_ptr);

        auto init_result = tf_strategy->initialize();
        if (init_result.is_error()) {
            std::cerr << "Failed to initialize strategy: " << init_result.error()->what()
                      << std::endl;
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
        auto add_result =
            portfolio->add_strategy(tf_strategy, 1.0, portfolio_config.use_optimization,
                                    portfolio_config.use_risk_management);
        if (add_result.is_error()) {
            std::cerr << "Failed to add strategy to portfolio: " << add_result.error()->what()
                      << std::endl;
            return 1;
        }
        INFO("Strategy added to portfolio successfully");

        // Load market data for daily processing
        INFO("Loading market data for daily processing...");
        auto market_data_result = db->get_market_data(
            symbols, start_date, end_date, 
            trade_ngin::AssetClass::FUTURES,
            trade_ngin::DataFrequency::DAILY, "ohlcv");
        
        if (market_data_result.is_error()) {
            ERROR("Failed to load market data: " + std::string(market_data_result.error()->what()));
            return 1;
        }
        
        // Convert Arrow table to Bars using the same conversion as backtest
        auto conversion_result = trade_ngin::DataConversionUtils::arrow_table_to_bars(market_data_result.value());
        if (conversion_result.is_error()) {
            ERROR("Failed to convert market data to bars: " + std::string(conversion_result.error()->what()));
            return 1;
        }
        
        auto all_bars = conversion_result.value();
        INFO("Loaded " + std::to_string(all_bars.size()) + " total bars");

        if (all_bars.empty()) {
            ERROR("No historical data loaded. Cannot calculate positions.");
            ERROR("This may be due to missing market data for the requested date.");
            ERROR("Please check if market data exists for " + std::to_string(std::chrono::system_clock::to_time_t(now)) + 
                  " and the 300 days prior.");
            return 1;
        }

        // Pre-warm strategy state so portfolio can pull price history for optimization/risk
        INFO("Preprocessing data in strategy to populate price history...");
        auto strat_prewarm = tf_strategy->on_data(all_bars);
        if (strat_prewarm.is_error()) {
            std::cerr << "Failed to preprocess data in strategy: "
                      << strat_prewarm.error()->what() << std::endl;
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

        // Get optimized portfolio positions (integer-rounded after optimization/risk)
        INFO("Retrieving optimized portfolio positions...");
        auto positions = portfolio->get_portfolio_positions();
        
        // Load previous day positions for PnL calculation
        INFO("Loading previous day positions for PnL calculation...");
        auto previous_date = now - std::chrono::hours(24);
        auto previous_positions_result = db->load_positions_by_date("LIVE_TREND_FOLLOWING", previous_date, "trading.positions");
        std::unordered_map<std::string, Position> previous_positions;
        
        if (previous_positions_result.is_ok()) {
            previous_positions = previous_positions_result.value();
            INFO("Loaded " + std::to_string(previous_positions.size()) + " previous day positions");
        } else {
            INFO("No previous day positions found (first run or no data): " + std::string(previous_positions_result.error()->what()));
        }

        INFO("DEBUG: Previous date used for lookup: " + std::to_string(std::chrono::system_clock::to_time_t(previous_date)));
        INFO("DEBUG: Current date: " + std::to_string(std::chrono::system_clock::to_time_t(now)));
        INFO("DEBUG: Previous positions loaded: " + std::to_string(previous_positions.size()));
        for (const auto& [symbol, pos] : previous_positions) {
            INFO("DEBUG: Previous position - " + symbol + ": " + std::to_string(pos.quantity.as_double()));
}
        
        // Get market prices - TWO sets needed for PnL lag model
        INFO("Getting market prices for PnL lag model...");
        std::set<std::string> all_symbols;
        for (const auto& [symbol, position] : positions) {
            if (position.quantity.as_double() != 0.0) {
                all_symbols.insert(symbol);
            }
        }
        // Also add symbols from previous positions that might have been closed
        for (const auto& [symbol, position] : previous_positions) {
            all_symbols.insert(symbol);
        }

        std::vector<std::string> symbols_to_price(all_symbols.begin(), all_symbols.end());
        INFO("Requesting prices for " + std::to_string(symbols_to_price.size()) + " symbols");

        // For historical runs, use the close prices from the bars data for consistency
        // For live runs, get real-time prices
        // We need TWO price sets:
        // 1. previous_day_close_prices (Day T-1 close) - for execution and market_price on Day T
        // 2. two_days_ago_close_prices (Day T-2 close) - for finalizing Day T-1 PnL
        std::unordered_map<std::string, double> previous_day_close_prices;  // Day T-1 close
        std::unordered_map<std::string, double> two_days_ago_close_prices;  // Day T-2 close

        if (use_override_date) {
            // Historical run: Extract close prices from the bars data
            INFO("Historical run detected - using close prices from bars data for date: " +
                 std::to_string(std::chrono::system_clock::to_time_t(target_date)));

            // Group bars by symbol and sort by timestamp
            std::unordered_map<std::string, std::vector<Bar>> bars_by_symbol;
            for (const auto& bar : all_bars) {
                bars_by_symbol[bar.symbol].push_back(bar);
            }

            for (auto& [symbol, symbol_bars] : bars_by_symbol) {
                if (!symbol_bars.empty()) {
                    // Sort by timestamp
                    std::sort(symbol_bars.begin(), symbol_bars.end(),
                             [](const Bar& a, const Bar& b) { return a.timestamp < b.timestamp; });

                    // Last bar is Day T-1 (yesterday's close) - automatically falls back to last available trading day
                    double yesterday_close = static_cast<double>(symbol_bars.back().close);
                    previous_day_close_prices[symbol] = yesterday_close;
                    
                    // Log the actual date of the last bar for debugging
                    auto last_bar_time = std::chrono::system_clock::to_time_t(symbol_bars.back().timestamp);
                    DEBUG("Day T-1 close for " + symbol + ": " + std::to_string(yesterday_close) + 
                          " (from " + std::to_string(last_bar_time) + ")");

                    // Second-to-last bar is Day T-2 (two days ago close) - for finalizing Day T-1
                    if (symbol_bars.size() >= 2) {
                        double two_days_ago_close = static_cast<double>(symbol_bars[symbol_bars.size() - 2].close);
                        two_days_ago_close_prices[symbol] = two_days_ago_close;
                        auto second_last_bar_time = std::chrono::system_clock::to_time_t(symbol_bars[symbol_bars.size() - 2].timestamp);
                        DEBUG("Day T-2 close for " + symbol + ": " + std::to_string(two_days_ago_close) + 
                              " (from " + std::to_string(second_last_bar_time) + ")");
                    }
                } else {
                    WARN("No bars data available for symbol: " + symbol);
                }
            }
            INFO("Using historical close prices: " + std::to_string(previous_day_close_prices.size()) + " Day T-1, " +
                 std::to_string(two_days_ago_close_prices.size()) + " Day T-2");
            INFO("Note: For weekends/holidays, Day T-1 automatically falls back to last available trading day");
        } else {
            // Live run: Get yesterday's and two-days-ago close prices from database
            // Query for yesterday's close
            auto yesterday_date = now - std::chrono::hours(24);
            auto yesterday_prices_result = db->get_latest_prices(symbols_to_price, trade_ngin::AssetClass::FUTURES);
            if (yesterday_prices_result.is_ok()) {
                previous_day_close_prices = yesterday_prices_result.value();
                INFO("Retrieved Day T-1 (yesterday) close prices for " + std::to_string(previous_day_close_prices.size()) + " symbols");
                
                // Check if we have prices for all required symbols
                if (previous_day_close_prices.size() < symbols_to_price.size()) {
                    WARN("Missing prices for some symbols. Required: " + std::to_string(symbols_to_price.size()) + 
                         ", Got: " + std::to_string(previous_day_close_prices.size()));
                    for (const auto& symbol : symbols_to_price) {
                        if (previous_day_close_prices.find(symbol) == previous_day_close_prices.end()) {
                            WARN("Missing price for symbol: " + symbol);
                        }
                    }
                }
            } else {
                ERROR("Failed to get yesterday's close prices: " + std::string(yesterday_prices_result.error()->what()));
                ERROR("This may be due to missing market data for yesterday (" + std::to_string(std::chrono::system_clock::to_time_t(yesterday_date)) + ")");
                ERROR("Continuing with empty price data - this may cause PnL calculation issues");
            }

            // Query for two-days-ago close (for finalizing yesterday's PnL)
            auto two_days_ago_date = now - std::chrono::hours(48);
            // TODO: Need database method to get historical close by specific date
            // For now, we'll use the same approach as historical mode
            WARN("Live mode two-days-ago close price retrieval not yet implemented - will skip Day T-1 finalization");
        }

        // ========================================
        // STEP 1: FINALIZE YESTERDAY'S (Day T-1) PnL
        // ========================================
        INFO("STEP 1: Finalizing Day T-1 PnL using Day T-1 close prices...");
        
        // Check if we have sufficient data for PnL finalization
        if (previous_day_close_prices.empty() && !previous_positions.empty()) {
            ERROR("CRITICAL: Cannot finalize yesterday's PnL - no close prices available!");
            ERROR("This indicates missing market data for yesterday (" + std::to_string(std::chrono::system_clock::to_time_t(previous_date)) + ")");
            ERROR("Cannot proceed safely. Please ensure market data is available before running.");
            return 1;
        }

        // We need to UPDATE yesterday's positions with actual PnL
        // Yesterday's positions were stored with realized_pnl = 0
        // Now we have yesterday's close (Day T-1 close), so we can calculate actual PnL

        double yesterday_total_pnl = 0.0;

        if (!two_days_ago_close_prices.empty() && !previous_positions.empty()) {
            INFO("Calculating Day T-1 finalized PnL...");

            for (const auto& [symbol, prev_position] : previous_positions) {
                double prev_qty = prev_position.quantity.as_double();
                if (prev_qty == 0.0) continue;

                double prev_entry_price = prev_position.average_price.as_double();

                // Get Day T-2 close (entry price for Day T-1 position) - NEVER change the original entry price
                double day_t2_close = prev_entry_price;  // Use the original entry price from database
                
                // Get Day T-1 close (current market price for Day T-1 finalization)
                double day_t1_close = prev_entry_price;  // Default fallback
                if (previous_day_close_prices.find(symbol) != previous_day_close_prices.end()) {
                    day_t1_close = previous_day_close_prices[symbol];
                } else {
                    WARN("No Day T-1 close price available for " + symbol + ", using entry price as fallback");
                }

                // Get point value multiplier
                double point_value = 0.0;
                try {
                    point_value = tf_strategy->get_point_value_multiplier(symbol);
                } catch (const std::exception& e) {
                    ERROR("CRITICAL: Cannot get point value for " + symbol + ": " + e.what());
                    throw;
                }

                // Calculate Day T-1 PnL: qty * (Day T-1 close - Day T-2 close) * point_value
                double yesterday_position_pnl = prev_qty * (day_t1_close - day_t2_close) * point_value;
                yesterday_total_pnl += yesterday_position_pnl;

                INFO("Day T-1 finalization for " + symbol + ": qty=" + std::to_string(prev_qty) +
                     " Entry price (Day T-2)=" + std::to_string(day_t2_close) +
                     " Day T-1 close=" + std::to_string(day_t1_close) +
                     " point_value=" + std::to_string(point_value) +
                     " Day T-1 PnL=" + std::to_string(yesterday_position_pnl));
            }

            // UPDATE yesterday's positions in database with finalized PnL
            INFO("Updating Day T-1 positions with finalized PnL: $" + std::to_string(yesterday_total_pnl));

            std::vector<trade_ngin::Position> yesterday_finalized_positions;
            for (const auto& [symbol, prev_position] : previous_positions) {
                double prev_qty = prev_position.quantity.as_double();

                // Use the ORIGINAL entry price from the database - NEVER change it
                double day_t2_close = prev_position.average_price.as_double();

                // Get Day T-1 close for PnL calculation
                double day_t1_close = day_t2_close;  // Default fallback
                if (previous_day_close_prices.find(symbol) != previous_day_close_prices.end()) {
                    day_t1_close = previous_day_close_prices[symbol];
                } else {
                    WARN("No Day T-1 close price available for " + symbol + ", using entry price as fallback");
                }

                // Get point value
                double point_value = 0.0;
                try {
                    point_value = tf_strategy->get_point_value_multiplier(symbol);
                } catch (const std::exception& e) {
                    ERROR("CRITICAL: Cannot get point value for " + symbol + ": " + e.what());
                    throw;
                }

                // Calculate finalized PnL
                double yesterday_position_pnl = prev_qty * (day_t1_close - day_t2_close) * point_value;

                // Create updated position for Day T-1 - PRESERVE original entry price
                trade_ngin::Position updated_pos;
                updated_pos.symbol = symbol;
                updated_pos.quantity = prev_position.quantity;
                updated_pos.average_price = prev_position.average_price;  // KEEP original entry price
                updated_pos.realized_pnl = Decimal(yesterday_position_pnl);  // FINALIZED PnL
                updated_pos.unrealized_pnl = Decimal(0.0);
                updated_pos.last_update = previous_date;  // Keep yesterday's date

                yesterday_finalized_positions.push_back(updated_pos);
            }

            // Store updated positions for yesterday (Day T-1) in database
            if (!yesterday_finalized_positions.empty()) {
                auto update_result = db->store_positions(yesterday_finalized_positions, "LIVE_TREND_FOLLOWING", "trading.positions");
                if (update_result.is_error()) {
                    ERROR("Failed to update Day T-1 positions: " + std::string(update_result.error()->what()));
                } else {
                    INFO("Successfully updated " + std::to_string(yesterday_finalized_positions.size()) + " Day T-1 positions with finalized PnL");
                }
            }

            // Also UPDATE yesterday's live_results with finalized PnL
            INFO("Updating Day T-1 live_results with finalized PnL...");
            // We'll do this later after loading previous aggregates
        } else {
            INFO("Skipping Day T-1 finalization (no two_days_ago prices or no previous positions)");
        }

        // ========================================
        // STEP 2: CREATE TODAY'S (Day T) POSITIONS WITH ZERO PnL
        // ========================================
        INFO("STEP 2: Creating Day T positions with zero PnL (placeholders)...");

        double total_daily_commissions = 0.0;  // Will be calculated from executions

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
            current_position.realized_pnl = Decimal(0.0);  // PLACEHOLDER - will be finalized tomorrow
            current_position.unrealized_pnl = Decimal(0.0);  // Always 0 for futures
            current_position.last_update = now;  // Today's timestamp

            INFO("Day T position for " + symbol + ": qty=" + std::to_string(current_position.quantity.as_double()) +
                 " entry_price=" + std::to_string(yesterday_close) +
                 " realized_pnl=0 (placeholder)");
        }

        // ADD THESE DEBUG STATEMENTS:
        INFO("DEBUG: About to start execution generation");
        INFO("DEBUG: Previous positions size: " + std::to_string(previous_positions.size()));
        INFO("DEBUG: Current positions size: " + std::to_string(positions.size()));

        // Generate execution reports for position changes
        INFO("Generating execution reports for position changes...");
        std::vector<ExecutionReport> daily_executions;

        // Create date string for order/exec IDs
        std::stringstream date_ss;
        date_ss << std::setfill('0') << std::setw(4) << (now_tm->tm_year + 1900)
                << std::setw(2) << (now_tm->tm_mon + 1)
                << std::setw(2) << now_tm->tm_mday;
        std::string date_str = date_ss.str();

        // Handle existing positions that changed
        for (const auto& [symbol, current_position] : positions) {
            double current_qty = current_position.quantity.as_double();
            double prev_qty = 0.0;
            
            // Get previous quantity
            auto prev_it = previous_positions.find(symbol);
            if (prev_it != previous_positions.end()) {
                prev_qty = prev_it->second.quantity.as_double();
            }
            INFO("DEBUG: Checking " + symbol + " - Current: " + std::to_string(current_qty) + 
            ", Previous: " + std::to_string(prev_qty) + ", Diff: " + std::to_string(std::abs(current_qty - prev_qty)));

            // Check if position changed
            if (std::abs(current_qty - prev_qty) > 1e-6) {
                double trade_size = current_qty - prev_qty;
                Side side = trade_size > 0 ? Side::BUY : Side::SELL;
                
                // Get Day T-1 close price (execution price for Day T)
                double market_price = current_position.average_price.as_double();
                if (previous_day_close_prices.find(symbol) != previous_day_close_prices.end()) {
                    market_price = previous_day_close_prices[symbol];
                }
                
                // Create execution report
                ExecutionReport exec;
                exec.order_id = "DAILY_" + symbol + "_" + date_str;
                // Make exec_id unique with timestamp to avoid duplicates
                auto timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
                exec.exec_id = "EXEC_" + symbol + "_" + std::to_string(timestamp_ms) + "_" + std::to_string(daily_executions.size());
                exec.symbol = symbol;
                exec.side = side;
                exec.filled_quantity = std::abs(trade_size);

                // Apply slippage to match backtest behavior
                double slip_factor = slippage_model / 10000.0;  // Convert bps to decimal (1 bp = 0.0001)
                double fill_price = side == Side::BUY ? market_price * (1.0 + slip_factor)
                                                      : market_price * (1.0 - slip_factor);
                exec.fill_price = fill_price;
                exec.fill_time = now;
                // Calculate transaction costs using the same model as backtesting
                // Base commission: 5 basis points * quantity
                double commission_cost = std::abs(trade_size) * commission_rate;
                // Market impact: 5 basis points * quantity * price
                double market_impact = std::abs(trade_size) * market_price * 0.0005;
                // Fixed cost per trade
                double fixed_cost = 1.0;
                exec.commission = commission_cost + market_impact + fixed_cost;
                exec.is_partial = false;
                
                daily_executions.push_back(exec);
                
                INFO("Generated execution: " + symbol + " " + 
                     (side == Side::BUY ? "BUY" : "SELL") + " " +
                     std::to_string(std::abs(trade_size)) + " at " + 
                     std::to_string(market_price));
            }
        }

        // Handle completely closed positions
        for (const auto& [symbol, prev_position] : previous_positions) {
            if (positions.find(symbol) == positions.end() && prev_position.quantity.as_double() != 0.0) {
                // This position was completely closed
                double prev_qty = prev_position.quantity.as_double();
                
                // Get Day T-1 close price (execution price for closing on Day T)
                double market_price = prev_position.average_price.as_double(); // Default fallback
                if (previous_day_close_prices.find(symbol) != previous_day_close_prices.end()) {
                    market_price = previous_day_close_prices[symbol];
                }
                
                // Create execution report for closing the position
                ExecutionReport exec;
                exec.order_id = "DAILY_" + symbol + "_" + date_str;
                // Make exec_id unique with timestamp to avoid duplicates
                auto timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
                exec.exec_id = "EXEC_" + symbol + "_" + std::to_string(timestamp_ms) + "_" + std::to_string(daily_executions.size());
                exec.symbol = symbol;
                exec.side = prev_qty > 0 ? Side::SELL : Side::BUY; // Opposite of original position
                exec.filled_quantity = std::abs(prev_qty);

                // Apply slippage to match backtest behavior
                double slip_factor = slippage_model / 10000.0;  // Convert bps to decimal (1 bp = 0.0001)
                Side close_side = prev_qty > 0 ? Side::SELL : Side::BUY;
                double fill_price = close_side == Side::BUY ? market_price * (1.0 + slip_factor)
                                                            : market_price * (1.0 - slip_factor);
                exec.fill_price = fill_price;
                exec.fill_time = now;
                // Calculate transaction costs using the same model as backtesting
                // Base commission: 5 basis points * quantity
                double commission_cost = std::abs(prev_qty) * commission_rate;
                // Market impact: 5 basis points * quantity * price
                double market_impact = std::abs(prev_qty) * market_price * 0.0005;
                // Fixed cost per trade
                double fixed_cost = 1.0;
                exec.commission = commission_cost + market_impact + fixed_cost;
                exec.is_partial = false;
                
                daily_executions.push_back(exec);
                
                INFO("Generated execution for closed position: " + symbol + " " + 
                     (exec.side == Side::BUY ? "BUY" : "SELL") + " " +
                     std::to_string(std::abs(prev_qty)) + " at " + 
                     std::to_string(market_price));
            }
        }

        // Store executions in database
        // Store executions in database
        if (!daily_executions.empty()) {
            INFO("Storing " + std::to_string(daily_executions.size()) + " executions to database...");
            
            // ADD THIS DEBUG SECTION:
            for (const auto& exec : daily_executions) {
                INFO("DEBUG: Execution data - order_id: " + exec.order_id);
                INFO("DEBUG: Execution data - exec_id: " + exec.exec_id);
                INFO("DEBUG: Execution data - symbol: " + exec.symbol);
                INFO("DEBUG: Execution data - side: " + std::to_string(static_cast<int>(exec.side)));
                INFO("DEBUG: Execution data - quantity: " + std::to_string(exec.filled_quantity));
                INFO("DEBUG: Execution data - price: " + std::to_string(exec.fill_price));
                INFO("DEBUG: Execution data - commission: " + std::to_string(exec.commission));
                INFO("DEBUG: Execution data - is_partial: " + std::to_string(exec.is_partial));
            }
            // Before inserting, delete any stale executions for today with the same order_ids
            try {
                // Build unique order_id list
                std::set<std::string> unique_order_ids;
                for (const auto& exec : daily_executions) {
                    unique_order_ids.insert(exec.order_id);
                }

                if (!unique_order_ids.empty()) {
                    // Build comma-separated quoted list for SQL IN clause
                    std::ostringstream ids_ss;
                    bool first = true;
                    for (const auto& oid : unique_order_ids) {
                        if (!first) ids_ss << ", ";
                        ids_ss << "'" << oid << "'";
                        first = false;
                    }

                    // Create YYYY-MM-DD for date filter to match execution_time
                    std::stringstream date_only_ss;
                    auto now_time_t_for_del = std::chrono::system_clock::to_time_t(now);
                    date_only_ss << std::put_time(std::gmtime(&now_time_t_for_del), "%Y-%m-%d");

                    std::string delete_execs_query =
                        "DELETE FROM trading.executions "
                        "WHERE DATE(execution_time) = '" + date_only_ss.str() + "' "
                        "AND order_id IN (" + ids_ss.str() + ")";

                    INFO("Deleting stale executions for today with matching order_ids: " + std::to_string(unique_order_ids.size()));
                    auto del_res = db->execute_direct_query(delete_execs_query);
                    if (del_res.is_error()) {
                        WARN("Failed to delete stale executions: " + std::string(del_res.error()->what()));
                    } else {
                        INFO("Stale executions (if any) deleted successfully");
                    }
                }
            } catch (const std::exception& e) {
                WARN("Exception while deleting stale executions: " + std::string(e.what()));
            }

            auto exec_result = db->store_executions(daily_executions, "trading.executions");
            if (exec_result.is_error()) {
                ERROR("Failed to store executions: " + std::string(exec_result.error()->what()));
            } else {
                INFO("Successfully stored " + std::to_string(daily_executions.size()) + " executions to database");
            }
        } else {
            INFO("No executions to store (no position changes detected)");
        }
        
        std::cout << "\n======= Daily Position Report =======" << std::endl;
        std::cout << "Date: " << (now_tm->tm_year + 1900) << "-"
                  << std::setfill('0') << std::setw(2) << (now_tm->tm_mon + 1) << "-"
                  << std::setfill('0') << std::setw(2) << now_tm->tm_mday << std::endl;
        std::cout << "Total Positions: " << positions.size() << std::endl;
        std::cout << std::endl;

        // Add header for position table
        std::cout << std::setw(10) << "Symbol" << " | "
                  << std::setw(10) << "Quantity" << " | "
                  << std::setw(10) << "Mkt Price" << " | "
                  << std::setw(12) << "Notional" << " | "
                  << std::setw(10) << "Unreal PnL" << std::endl;
        std::cout << std::string(60, '-') << std::endl;

        double gross_notional = 0.0;
        double net_notional = 0.0;
        double total_posted_margin = 0.0;  // Sum of per-contract initial margins times contracts
        double maintenance_requirement_today = 0.0;  // Sum of per-contract maintenance margins times contracts
        int active_positions = 0;

        for (const auto& [symbol, position] : positions) {
            if (position.quantity.as_double() != 0.0) {
                active_positions++;
                // Use Day T-1 close (market price for Day T positions)
                double market_price = position.average_price.as_double();
                auto itp = previous_day_close_prices.find(symbol);
                if (itp != previous_day_close_prices.end()) {
                    market_price = itp->second;
                }
                // Normalize variant suffixes for lookup only; keep original symbol for logging/DB
                std::string lookup_sym = symbol;
                auto dotpos = lookup_sym.find(".v.");
                if (dotpos != std::string::npos) {
                    lookup_sym = lookup_sym.substr(0, dotpos);
                }
                dotpos = lookup_sym.find(".c.");
                if (dotpos != std::string::npos) {
                    lookup_sym = lookup_sym.substr(0, dotpos);
                }

                // Get contract multiplier for proper notional calculation
                double contract_multiplier = 1.0;

                // ONLY use registry - no fallbacks allowed
                try {
                    auto instrument_ptr = registry.get_instrument(lookup_sym);
                    if (!instrument_ptr) {
                        ERROR("CRITICAL: Instrument " + lookup_sym + " not found in registry!");
                        ERROR("Available instruments in registry:");
                        auto all_instruments = registry.get_all_instruments();
                        for (const auto& inst : all_instruments) {
                            ERROR("  - " + inst.first);
                        }
                        ERROR("Cannot calculate margin or notional without proper data.");
                        ERROR("This may cause segmentation faults. Exiting gracefully.");
                        return 1;  // Exit gracefully instead of throwing
                    }

                    // Get the contract multiplier for notional calculation from registry (ONLY source)
                    contract_multiplier = instrument_ptr->get_multiplier();
                    if (contract_multiplier <= 0) {
                        ERROR("CRITICAL: Invalid multiplier " + std::to_string(contract_multiplier) +
                              " for " + lookup_sym);
                        throw std::runtime_error("Invalid multiplier for: " + lookup_sym);
                    }

                    double contracts_abs = std::abs(position.quantity.as_double());
                    double initial_margin_per_contract = instrument_ptr->get_margin_requirement();
                    if (initial_margin_per_contract <= 0) {
                        ERROR("CRITICAL: Invalid initial margin " + std::to_string(initial_margin_per_contract) +
                              " for " + lookup_sym);
                        throw std::runtime_error("Invalid initial margin for: " + lookup_sym);
                    }
                    total_posted_margin += contracts_abs * initial_margin_per_contract;

                    // Try to get maintenance margin if available (e.g., futures)
                    // If not available, use initial margin (conservative approach)
                    double maintenance_margin_per_contract = initial_margin_per_contract;
                    // FuturesInstrument has get_maintenance_margin(); detect via dynamic_cast
                    if (auto futures_ptr = std::dynamic_pointer_cast<trade_ngin::FuturesInstrument>(instrument_ptr)) {
                        maintenance_margin_per_contract = futures_ptr->get_maintenance_margin();
                        if (maintenance_margin_per_contract <= 0) {
                            ERROR("CRITICAL: Invalid maintenance margin " +
                                  std::to_string(maintenance_margin_per_contract) + " for " + lookup_sym);
                            throw std::runtime_error("Invalid maintenance margin for: " + lookup_sym);
                        }
                    }
                    maintenance_requirement_today += contracts_abs * maintenance_margin_per_contract;
                } catch (const std::exception& e) {
                    ERROR("CRITICAL: Failed to get instrument data for " + symbol + ": " + e.what());
                    throw;  // Re-throw the exception - don't hide the error
                }

                // Calculate notional with proper contract multiplier
                double signed_notional = position.quantity.as_double() * market_price * contract_multiplier;
                net_notional += signed_notional;
                gross_notional += std::abs(signed_notional);

                std::cout << std::setw(10) << symbol << " | "
                          << std::setw(10) << std::fixed << std::setprecision(2) 
                          << position.quantity.as_double() << " | "
                          << std::setw(10) << std::fixed << std::setprecision(2) 
                          << market_price << " | "
                          << std::setw(12) << std::fixed << std::setprecision(2) 
                          << signed_notional << " | "
                          << std::setw(10) << std::fixed << std::setprecision(2) 
                          << position.unrealized_pnl.as_double() << std::endl;
            }
        }

        std::cout << std::endl;
        std::cout << "Active Positions: " << active_positions << std::endl;
        std::cout << "Gross Notional: $" << std::fixed << std::setprecision(2) << gross_notional << std::endl;
        std::cout << "Net Notional: $" << std::fixed << std::setprecision(2) << net_notional << std::endl;
        std::cout << "Portfolio Leverage (gross/current): " << std::fixed << std::setprecision(2) 
                  << (gross_notional / initial_capital) << "x" << std::endl;
        // Posted margin should never be zero if there are active positions; enforce and warn
        if (active_positions > 0 && total_posted_margin <= 0.0) {
            ERROR("Computed posted margin is non-positive while positions are active. Check instrument metadata.");
        }
        // Equity-to-Margin Ratio = gross_notional / total_posted_margin
        // This metric shows how many times the gross notional exposure is covered by posted margin
        // Higher values indicate more leverage relative to margin requirements
        double equity_to_margin_ratio = (total_posted_margin > 0.0) ? (gross_notional / total_posted_margin) : 0.0;
        if (equity_to_margin_ratio <= 1.0 && active_positions > 0) {
            WARN("Equity-to-Margin Ratio (gross_notional / posted_margin) is <= 1.0; verify margins.");
        }

        // Save positions to database with daily PnL values
        INFO("Saving positions to database with daily PnL...");
        std::vector<trade_ngin::Position> positions_to_save;
        positions_to_save.reserve(positions.size());

        for (const auto& [symbol, position] : positions) {
            // Save positions if they have non-zero quantity OR if they have PnL (closed positions today)
            bool has_quantity = position.quantity.as_double() != 0.0;
            bool has_pnl = (position.realized_pnl.as_double() != 0.0 || position.unrealized_pnl.as_double() != 0.0);

            // Don't save positions with zero quantity and zero PnL
            if (!has_quantity && !has_pnl) {
                continue;
            }

            // Create a new position with validated values
            trade_ngin::Position validated_position;
            validated_position.symbol = position.symbol;
            validated_position.quantity = position.quantity;
            validated_position.last_update = now;  // Use current timestamp

            // For futures, daily PnL is all realized (mark-to-market)
            // The realized_pnl field contains the daily PnL we calculated
            validated_position.realized_pnl = position.realized_pnl;  // Daily realized PnL
            validated_position.unrealized_pnl = Decimal(0.0);  // Always 0 for futures

            // Validate and convert average_price to ensure it's within Decimal limits
            double avg_price_double = static_cast<double>(position.average_price);

            // Decimal limit is approximately 92,233,720,368,547.75807
            const double DECIMAL_MAX = 9.223372036854775807e13;  // INT64_MAX / SCALE
            if (avg_price_double > DECIMAL_MAX || avg_price_double < -DECIMAL_MAX) {
                WARN("Position " + symbol + " has average_price " + std::to_string(avg_price_double) +
                     " which exceeds Decimal limit, using Day T-1 close instead");
                // Use Day T-1 close if available
                if (previous_day_close_prices.find(symbol) != previous_day_close_prices.end()) {
                    validated_position.average_price = trade_ngin::Decimal(previous_day_close_prices[symbol]);
                } else {
                    validated_position.average_price = trade_ngin::Decimal(1.0);
                }
            } else {
                try {
                    validated_position.average_price = position.average_price;
                } catch (const std::exception& e) {
                    ERROR("Failed to validate average_price for " + symbol + ": " + std::string(e.what()));
                    if (previous_day_close_prices.find(symbol) != previous_day_close_prices.end()) {
                        validated_position.average_price = trade_ngin::Decimal(previous_day_close_prices[symbol]);
                    } else {
                        validated_position.average_price = trade_ngin::Decimal(1.0);
                    }
                }
            }

            positions_to_save.push_back(validated_position);
            INFO("Position to save: " + symbol +
                 " qty=" + std::to_string(validated_position.quantity.as_double()) +
                 " price=" + std::to_string(static_cast<double>(validated_position.average_price)) +
                 " daily_realized_pnl=" + std::to_string(static_cast<double>(validated_position.realized_pnl)) +
                 " daily_unrealized_pnl=" + std::to_string(static_cast<double>(validated_position.unrealized_pnl)));
        }
        
        if (!positions_to_save.empty()) {
            INFO("Attempting to save " + std::to_string(positions_to_save.size()) + " positions to database");
            DEBUG("Database connection status: " + std::string(db->is_connected() ? "connected" : "disconnected"));
            
            auto save_result = db->store_positions(positions_to_save, "LIVE_TREND_FOLLOWING", "trading.positions");
            if (save_result.is_error()) {
                ERROR("Failed to save positions to database: " + std::string(save_result.error()->what()));
                ERROR("Error code: " + std::to_string(static_cast<int>(save_result.error()->code())));
            } else {
                INFO("Successfully saved " + std::to_string(positions_to_save.size()) + " positions to database");
            }
        } else {
            INFO("No positions to save (all positions are zero)");
        }

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
            std::cout << "Net Leverage: " << std::fixed << std::setprecision(2)
                      << r.net_leverage << std::endl;
            std::cout << "Max Correlation: " << std::fixed << std::setprecision(2)
                      << r.correlation_risk << std::endl;
            std::cout << "Jump Risk (99th): " << std::fixed << std::setprecision(2)
                      << r.jump_risk << std::endl;
            std::cout << "Risk Scale: " << std::fixed << std::setprecision(2)
                      << r.recommended_scale << std::endl;
        } else {
            std::cout << "Volatility: N/A" << std::endl;
            std::cout << "Gross Leverage: N/A" << std::endl;
            std::cout << "Net Leverage: N/A" << std::endl;
            std::cout << "Max Correlation: N/A" << std::endl;
            std::cout << "Jump Risk (99th): N/A" << std::endl;
            std::cout << "Risk Scale: N/A" << std::endl;
        }
        // ========================================
        // STEP 3: CALCULATE COMMISSIONS AND Day T PnL (ZERO)
        // ========================================
        INFO("STEP 3: Calculating commissions and Day T PnL...");

        // Calculate commissions from executions
        for (const auto& exec : daily_executions) {
            total_daily_commissions += exec.commission.as_double();
        }
        INFO("Total daily commissions: $" + std::to_string(total_daily_commissions));

        // Day T PnL is ZERO (placeholder) - positions were just opened at Day T-1 close
        double daily_realized_pnl = 0.0;
        double daily_unrealized_pnl = 0.0;
        double daily_pnl_for_today = -total_daily_commissions;  // Only commissions on Day T

        INFO("Day T PnL (placeholder): $0.00");
        INFO("Day T commissions: $" + std::to_string(total_daily_commissions));
        INFO("Day T total impact: $" + std::to_string(daily_pnl_for_today));

        // ========================================
        // STEP 4: UPDATE Day T-1 live_results AND equity_curve WITH FINALIZED PnL
        // ========================================
        // Skip if this is the first trading day (no previous positions to finalize)
        bool is_first_trading_day = previous_positions.empty() ||
                                     (previous_positions.size() > 0 &&
                                      std::all_of(previous_positions.begin(), previous_positions.end(),
                                                 [](const auto& p) { return p.second.quantity.as_double() == 0.0; }));

        // Declare yesterday's daily metrics outside the block so they're available for email
        double yesterday_daily_return_for_email = 0.0;
        double yesterday_daily_pnl_for_email = 0.0;
        double yesterday_realized_pnl_for_email = 0.0;
        double yesterday_unrealized_pnl_for_email = 0.0;

        if (!two_days_ago_close_prices.empty() && yesterday_total_pnl != 0.0 && !is_first_trading_day) {
            INFO("STEP 4: Updating Day T-1 live_results with finalized PnL: $" + std::to_string(yesterday_total_pnl));

            // Get yesterday's commissions and other existing metrics from database
            double yesterday_commissions = 0.0;
            double yesterday_total_commissions = 0.0;
            double yesterday_gross_notional = 0.0;
            double yesterday_net_notional = 0.0;
            int yesterday_active_positions = 0;
            double yesterday_margin_posted = 0.0;

            std::stringstream yesterday_date_ss;
            auto yesterday_time_t = std::chrono::system_clock::to_time_t(previous_date);
            yesterday_date_ss << std::put_time(std::gmtime(&yesterday_time_t), "%Y-%m-%d");

            try {
                std::string query =
                    "SELECT COALESCE(daily_commissions, 0.0) as daily_commissions, "
                    "COALESCE(total_commissions, 0.0) as total_commissions, "
                    "COALESCE(gross_notional, 0.0) as gross_notional, "
                    "COALESCE(net_notional, 0.0) as net_notional, "
                    "COALESCE(active_positions, 0) as active_positions, "
                    "COALESCE(margin_posted, 0.0) as margin_posted "
                    "FROM trading.live_results "
                    "WHERE strategy_id = 'LIVE_TREND_FOLLOWING' AND DATE(date) = '" + yesterday_date_ss.str() + "'";
                INFO("Querying yesterday's metrics for date: " + yesterday_date_ss.str());
                auto result = db->execute_query(query);
                if (result.is_error()) {
                    WARN("Query failed: " + std::string(result.error()->what()));
                    INFO("Commission query succeeded, but parsing not implemented - using 0");
                } else if (result.value()->num_rows() == 0) {
                    WARN("Query returned 0 rows for date: " + yesterday_date_ss.str());
                }
                if (result.is_ok() && result.value()->num_rows() > 0) {
                    INFO("Found " + std::to_string(result.value()->num_rows()) + " rows for yesterday");
                    auto table = result.value();
                    INFO("Table has " + std::to_string(table->num_columns()) + " columns");
                    if (table->num_columns() >= 6) {
                        auto comm_arr = std::static_pointer_cast<arrow::DoubleArray>(table->column(0)->chunk(0));
                        auto total_comm_arr = std::static_pointer_cast<arrow::DoubleArray>(table->column(1)->chunk(0));
                        auto gross_arr = std::static_pointer_cast<arrow::DoubleArray>(table->column(2)->chunk(0));
                        auto net_arr = std::static_pointer_cast<arrow::DoubleArray>(table->column(3)->chunk(0));
                        auto pos_arr = std::static_pointer_cast<arrow::Int64Array>(table->column(4)->chunk(0));
                        auto margin_arr = std::static_pointer_cast<arrow::DoubleArray>(table->column(5)->chunk(0));

                        if (comm_arr && comm_arr->length() > 0 && !comm_arr->IsNull(0)) {
                            yesterday_commissions = comm_arr->Value(0);
                            INFO("Successfully loaded yesterday_commissions: $" + std::to_string(yesterday_commissions));
                        } else {
                            INFO("Failed to load commissions from Arrow array");
                        }
                        if (total_comm_arr && total_comm_arr->length() > 0 && !total_comm_arr->IsNull(0)) {
                            yesterday_total_commissions = total_comm_arr->Value(0);
                        }
                        if (gross_arr && gross_arr->length() > 0 && !gross_arr->IsNull(0)) {
                            yesterday_gross_notional = gross_arr->Value(0);
                            INFO("Loaded yesterday_gross_notional: $" + std::to_string(yesterday_gross_notional));
                        }
                        if (net_arr && net_arr->length() > 0 && !net_arr->IsNull(0)) {
                            yesterday_net_notional = net_arr->Value(0);
                        }
                        if (pos_arr && pos_arr->length() > 0 && !pos_arr->IsNull(0)) {
                            yesterday_active_positions = static_cast<int>(pos_arr->Value(0));
                        }
                        if (margin_arr && margin_arr->length() > 0 && !margin_arr->IsNull(0)) {
                            yesterday_margin_posted = margin_arr->Value(0);
                            INFO("Loaded yesterday_margin_posted: $" + std::to_string(yesterday_margin_posted));
                        }
                    }
                }
            } catch (const std::exception& e) {
                WARN("Failed to get yesterday's metrics: " + std::string(e.what()));
            }

            // Since we couldn't query commissions, we'll calculate daily_pnl in SQL
            // For cumulative calculations, we need to query the actual commission value
            double yesterday_commissions_for_calc = 0.0;
            try {
                std::string comm_query = "SELECT COALESCE(daily_commissions, 0.0) FROM trading.live_results "
                                        "WHERE strategy_id = 'LIVE_TREND_FOLLOWING' AND DATE(date) = '" + yesterday_date_ss.str() + "'";
                auto comm_result = db->execute_direct_query(comm_query);
                if (comm_result.is_ok()) {
                    // Parse the result - it should be a simple value
                    // For now, we'll just use yesterday_commissions which defaults to 0
                    INFO("Commission query succeeded, but parsing not implemented - using 0");
                }
            } catch (const std::exception& e) {
                INFO("Could not query commissions: " + std::string(e.what()));
            }

            // Use the queried value from earlier (which may be 0 if query failed)
            double yesterday_daily_pnl_finalized = yesterday_total_pnl - yesterday_commissions;

            INFO("Day T-1 PnL breakdown:");
            INFO("  Position PnL (yesterday_total_pnl): $" + std::to_string(yesterday_total_pnl));
            INFO("  Commissions (yesterday_commissions): $" + std::to_string(yesterday_commissions));
            INFO("  Net PnL (yesterday_daily_pnl_finalized): $" + std::to_string(yesterday_daily_pnl_finalized));

            // Get the day BEFORE yesterday's portfolio value and total_pnl
            double day_before_yesterday_portfolio_value = initial_capital;
            double day_before_yesterday_total_pnl = 0.0;
            try {
                auto db_ptr = std::dynamic_pointer_cast<PostgresDatabase>(db);
                if (db_ptr) {
                    auto prev_agg = db_ptr->get_previous_live_aggregates("LIVE_TREND_FOLLOWING", previous_date, "trading.live_results");
                    if (prev_agg.is_ok()) {
                        std::tie(day_before_yesterday_portfolio_value, day_before_yesterday_total_pnl, std::ignore) = prev_agg.value();
                        INFO("Loaded day-before-yesterday aggregates: portfolio=$" + std::to_string(day_before_yesterday_portfolio_value) +
                             ", total_pnl=$" + std::to_string(day_before_yesterday_total_pnl));
                    }
                }
            } catch (const std::exception& e) {
                INFO("Could not load day-before-yesterday aggregates: " + std::string(e.what()));
            }

            // Calculate yesterday's cumulative values
            // NOTE: Since we may not have correct commissions, the cumulative values will be recalculated by SQL
            // using the daily_pnl formula (daily_realized_pnl - daily_commissions)
            double yesterday_total_pnl_cumulative = day_before_yesterday_total_pnl + yesterday_daily_pnl_finalized;
            double yesterday_portfolio_value_finalized = day_before_yesterday_portfolio_value + yesterday_daily_pnl_finalized;

            // Calculate yesterday's returns
            double yesterday_daily_return = 0.0;
            if (day_before_yesterday_portfolio_value > 0) {
                yesterday_daily_return = (yesterday_daily_pnl_finalized / day_before_yesterday_portfolio_value) * 100.0;
            }

            // Note: Yesterday's metrics for email will be loaded from database after update

            // Calculate yesterday's annualized return
            double yesterday_total_return_annualized = 0.0;
            double yesterday_total_return_decimal = 0.0;
            if (initial_capital > 0.0) {
                yesterday_total_return_decimal = (yesterday_portfolio_value_finalized - initial_capital) / initial_capital;
            }

            // Get trading days count for annualization
            int trading_days_count = 1;
            try {
                auto count_result = db->execute_query(
                    "SELECT COUNT(*) AS cnt FROM trading.live_results WHERE strategy_id = 'LIVE_TREND_FOLLOWING'");
                if (count_result.is_ok()) {
                    auto table = count_result.value();
                    if (table && table->num_rows() > 0 && table->num_columns() > 0) {
                        auto arr = std::static_pointer_cast<arrow::Int64Array>(table->column(0)->chunk(0));
                        if (arr && arr->length() > 0 && !arr->IsNull(0)) {
                            trading_days_count = std::max<int>(1, static_cast<int>(arr->Value(0)));
                        }
                    }
                }
            } catch (const std::exception& e) {
                WARN("Failed to count trading days: " + std::string(e.what()));
            }

            double rdaily = std::pow(1.0 + yesterday_total_return_decimal, 1.0 / static_cast<double>(trading_days_count)) - 1.0;
            double annualized_decimal = std::pow(1.0 + rdaily, 252.0) - 1.0;
            yesterday_total_return_annualized = annualized_decimal * 100.0;

            // Calculate yesterday's leverage and risk metrics
            // IMPORTANT: We MUST preserve existing values from the database
            // These were calculated correctly when Day T-1 was originally processed
            double yesterday_portfolio_leverage = 0.0;
            double yesterday_equity_to_margin_ratio = 0.0;

            // Load existing values from database - DO NOT RECALCULATE
            try {
                std::string existing_metrics_query =
                    "SELECT portfolio_leverage, equity_to_margin_ratio, gross_notional, margin_posted "
                    "FROM trading.live_results "
                    "WHERE strategy_id = 'LIVE_TREND_FOLLOWING' AND DATE(date) = '" + yesterday_date_ss.str() + "'";
                auto existing_result = db->execute_query(existing_metrics_query);
                if (existing_result.is_ok() && existing_result.value()->num_rows() > 0) {
                    auto table = existing_result.value();
                    auto leverage_arr = std::static_pointer_cast<arrow::DoubleArray>(table->column(0)->chunk(0));
                    auto equity_margin_arr = std::static_pointer_cast<arrow::DoubleArray>(table->column(1)->chunk(0));
                    auto gross_notional_arr = std::static_pointer_cast<arrow::DoubleArray>(table->column(2)->chunk(0));
                    auto margin_posted_arr = std::static_pointer_cast<arrow::DoubleArray>(table->column(3)->chunk(0));

                    if (leverage_arr && !leverage_arr->IsNull(0)) {
                        yesterday_portfolio_leverage = leverage_arr->Value(0);
                    }
                    if (equity_margin_arr && !equity_margin_arr->IsNull(0)) {
                        yesterday_equity_to_margin_ratio = equity_margin_arr->Value(0);
                    }

                    // Also update the gross_notional and margin_posted if available
                    if (gross_notional_arr && !gross_notional_arr->IsNull(0)) {
                        yesterday_gross_notional = gross_notional_arr->Value(0);
                    }
                    if (margin_posted_arr && !margin_posted_arr->IsNull(0)) {
                        yesterday_margin_posted = margin_posted_arr->Value(0);
                    }

                    INFO("Preserved existing metrics from database: leverage=" +
                         std::to_string(yesterday_portfolio_leverage) + ", equity_to_margin=" +
                         std::to_string(yesterday_equity_to_margin_ratio) + ", gross_notional=" +
                         std::to_string(yesterday_gross_notional) + ", margin_posted=" +
                         std::to_string(yesterday_margin_posted));
                }
            } catch (const std::exception& e) {
                WARN("Failed to load existing metrics: " + std::string(e.what()));
            }

            // DO NOT recalculate these values - they should remain as loaded from database
            // These values were correctly calculated when the day was originally processed
            double yesterday_cash_available = yesterday_portfolio_value_finalized - yesterday_margin_posted;

            // UPDATE yesterday's live_results with ALL recalculated metrics
            // Note: We calculate daily_pnl, total_pnl, and current_portfolio_value in SQL
            // to properly incorporate the EXISTING daily_commissions value
            // IMPORTANT: Only update portfolio_leverage and equity_to_margin_ratio if they are NULL or 0
            std::string update_query =
                "WITH day_before AS ("
                "  SELECT COALESCE(current_portfolio_value, " + std::to_string(initial_capital) + ") as portfolio, "
                "         COALESCE(total_pnl, 0.0) as total_pnl "
                "  FROM trading.live_results "
                "  WHERE strategy_id = 'LIVE_TREND_FOLLOWING' AND DATE(date) < '" + yesterday_date_ss.str() + "' "
                "  ORDER BY date DESC LIMIT 1"
                ") "
                "UPDATE trading.live_results SET "
                "daily_realized_pnl = " + std::to_string(yesterday_total_pnl) + ", "
                "daily_pnl = " + std::to_string(yesterday_total_pnl) + " - COALESCE(daily_commissions, 0.0), "
                "total_pnl = COALESCE((SELECT total_pnl FROM day_before), 0.0) + (" + std::to_string(yesterday_total_pnl) + " - COALESCE(daily_commissions, 0.0)), "
                "total_realized_pnl = COALESCE((SELECT total_pnl FROM day_before), 0.0) + (" + std::to_string(yesterday_total_pnl) + " - COALESCE(daily_commissions, 0.0)), "
                "current_portfolio_value = COALESCE((SELECT portfolio FROM day_before), " + std::to_string(initial_capital) + ") + (" + std::to_string(yesterday_total_pnl) + " - COALESCE(daily_commissions, 0.0)), "
                "daily_return = CASE WHEN COALESCE((SELECT portfolio FROM day_before), " + std::to_string(initial_capital) + ") > 0 "
                "               THEN ((" + std::to_string(yesterday_total_pnl) + " - COALESCE(daily_commissions, 0.0)) / COALESCE((SELECT portfolio FROM day_before), " + std::to_string(initial_capital) + ")) * 100.0 "
                "               ELSE 0.0 END, "
                "total_return = " + std::to_string(yesterday_total_return_annualized) + ", "
                "portfolio_leverage = CASE WHEN portfolio_leverage IS NULL OR portfolio_leverage = 0 THEN " + std::to_string(yesterday_portfolio_leverage) + " ELSE portfolio_leverage END, "
                "equity_to_margin_ratio = CASE WHEN equity_to_margin_ratio IS NULL OR equity_to_margin_ratio = 0 THEN " + std::to_string(yesterday_equity_to_margin_ratio) + " ELSE equity_to_margin_ratio END, "
                "cash_available = COALESCE((SELECT portfolio FROM day_before), " + std::to_string(initial_capital) + ") + (" + std::to_string(yesterday_total_pnl) + " - COALESCE(daily_commissions, 0.0)) - COALESCE(margin_posted, 0.0) "
                "WHERE strategy_id = 'LIVE_TREND_FOLLOWING' AND DATE(date) = '" + yesterday_date_ss.str() + "'";

            auto update_result = db->execute_direct_query(update_query);
            if (update_result.is_error()) {
                ERROR("Failed to update Day T-1 live_results: " + std::string(update_result.error()->what()));
            } else {
                INFO("Successfully updated Day T-1 live_results with finalized PnL and all metrics");
            }

            // UPDATE yesterday's equity_curve
            // Use current_portfolio_value from the updated live_results
            INFO("Updating Day T-1 equity_curve...");
            std::string update_equity_query =
                "UPDATE trading.equity_curve SET "
                "equity = (SELECT current_portfolio_value FROM trading.live_results "
                "          WHERE strategy_id = 'LIVE_TREND_FOLLOWING' AND DATE(date) = '" + yesterday_date_ss.str() + "') "
                "WHERE strategy_id = 'LIVE_TREND_FOLLOWING' AND DATE(timestamp) = '" + yesterday_date_ss.str() + "'";

            auto update_equity_result = db->execute_direct_query(update_equity_query);
            if (update_equity_result.is_error()) {
                ERROR("Failed to update Day T-1 equity_curve: " + std::string(update_equity_result.error()->what()));
            } else {
                INFO("Successfully updated Day T-1 equity_curve");
            }

            // Load updated metrics from database for email - MUST do this AFTER the UPDATE
            try {
                std::string metrics_query =
                    "SELECT daily_return, daily_pnl, daily_realized_pnl, daily_unrealized_pnl, "
                    "portfolio_leverage, equity_to_margin_ratio "
                    "FROM trading.live_results "
                    "WHERE strategy_id = 'LIVE_TREND_FOLLOWING' AND DATE(date) = '" + yesterday_date_ss.str() + "'";

                INFO("Loading yesterday's metrics from database with query: " + metrics_query);
                auto metrics_result = db->execute_query(metrics_query);

                if (metrics_result.is_ok() && metrics_result.value()->num_rows() > 0) {
                    auto table = metrics_result.value();
                    if (table->num_columns() >= 4) {
                        auto daily_return_arr = std::static_pointer_cast<arrow::DoubleArray>(table->column(0)->chunk(0));
                        auto daily_pnl_arr = std::static_pointer_cast<arrow::DoubleArray>(table->column(1)->chunk(0));
                        auto daily_realized_arr = std::static_pointer_cast<arrow::DoubleArray>(table->column(2)->chunk(0));
                        auto daily_unrealized_arr = std::static_pointer_cast<arrow::DoubleArray>(table->column(3)->chunk(0));

                        if (daily_return_arr && daily_return_arr->length() > 0 && !daily_return_arr->IsNull(0)) {
                            yesterday_daily_return_for_email = daily_return_arr->Value(0);
                            INFO("Loaded yesterday's daily_return: " + std::to_string(yesterday_daily_return_for_email));
                        }
                        if (daily_pnl_arr && daily_pnl_arr->length() > 0 && !daily_pnl_arr->IsNull(0)) {
                            yesterday_daily_pnl_for_email = daily_pnl_arr->Value(0);
                            INFO("Loaded yesterday's daily_pnl: " + std::to_string(yesterday_daily_pnl_for_email));
                        }
                        if (daily_realized_arr && daily_realized_arr->length() > 0 && !daily_realized_arr->IsNull(0)) {
                            yesterday_realized_pnl_for_email = daily_realized_arr->Value(0);
                            INFO("Loaded yesterday's daily_realized_pnl: " + std::to_string(yesterday_realized_pnl_for_email));
                        } else {
                            // If daily_realized_pnl is null or 0, use yesterday_total_pnl as fallback
                            yesterday_realized_pnl_for_email = yesterday_total_pnl;
                            INFO("Using calculated yesterday_total_pnl as realized PnL: " + std::to_string(yesterday_realized_pnl_for_email));
                        }
                        if (daily_unrealized_arr && daily_unrealized_arr->length() > 0 && !daily_unrealized_arr->IsNull(0)) {
                            yesterday_unrealized_pnl_for_email = daily_unrealized_arr->Value(0);
                            INFO("Loaded yesterday's daily_unrealized_pnl: " + std::to_string(yesterday_unrealized_pnl_for_email));
                        }

                        // For futures, unrealized PnL should always be 0, realized PnL is the total daily PnL
                        yesterday_unrealized_pnl_for_email = 0.0;  // Futures have no unrealized PnL

                        INFO("Successfully loaded yesterday's metrics from database for email");
                    }
                } else {
                    WARN("No metrics found in database for yesterday, using calculated values");
                    // Use the calculated values as fallback
                    yesterday_realized_pnl_for_email = yesterday_total_pnl;
                    yesterday_daily_pnl_for_email = yesterday_total_pnl;  // For futures, daily PnL = realized PnL
                    yesterday_unrealized_pnl_for_email = 0.0;  // No unrealized for futures
                }
            } catch (const std::exception& e) {
                WARN("Failed to load updated yesterday's metrics: " + std::string(e.what()));
                // Use calculated values as fallback
                yesterday_realized_pnl_for_email = yesterday_total_pnl;
                yesterday_daily_pnl_for_email = yesterday_total_pnl;
                yesterday_unrealized_pnl_for_email = 0.0;
            }
        } else {
            if (is_first_trading_day) {
                INFO("Skipping Day T-1 update (first trading day - no previous positions to finalize)");
            } else {
                INFO("Skipping Day T-1 live_results update (no two_days_ago prices or zero PnL)");
            }
        }

        // ========================================
        // STEP 5: LOAD UPDATED PREVIOUS DAY AGGREGATES AND CALCULATE Day T CUMULATIVE VALUES
        // ========================================
        INFO("STEP 5: Loading updated previous day aggregates and calculating Day T cumulative values...");

        // Load previous day's aggregates (portfolio value, total pnl, total commissions)
        // This is done AFTER updating Day T-1 live_results to ensure we get the finalized values
        double previous_portfolio_value = initial_capital; // Default to initial capital
        double previous_total_pnl = 0.0;
        double previous_total_commissions = 0.0;

        try {
            auto db_ptr = std::dynamic_pointer_cast<PostgresDatabase>(db);
            if (db_ptr) {
                auto prev_agg = db_ptr->get_previous_live_aggregates("LIVE_TREND_FOLLOWING", now, "trading.live_results");
                if (prev_agg.is_ok()) {
                    std::tie(previous_portfolio_value, previous_total_pnl, previous_total_commissions) = prev_agg.value();
                    INFO("Loaded updated previous aggregates - portfolio_value: $" + std::to_string(previous_portfolio_value) +
                         ", total_pnl: $" + std::to_string(previous_total_pnl) +
                         ", total_commissions: $" + std::to_string(previous_total_commissions));
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
        double daily_pnl = daily_pnl_for_today;  // Only commissions on Day T
        double total_commissions_cumulative = previous_total_commissions + total_daily_commissions;

        // Since it's futures, all PnL is realized
        double total_realized_pnl = total_pnl;
        double total_unrealized_pnl = 0.0;

        // Calculate returns
        double daily_return = 0.0;                 // in percent
        double total_return_annualized = 0.0;      // in percent

        if (previous_portfolio_value > 0) {
            daily_return = (daily_pnl / previous_portfolio_value) * 100.0;
        }

        // Annualize using geometric method based on cumulative total return over n days
        // R_total_decimal = (current_value - initial_capital) / initial_capital
        double total_return_decimal = 0.0;
        if (initial_capital > 0.0) {
            total_return_decimal = (current_portfolio_value - initial_capital) / initial_capital;
        }

        // Get n = number of trading days (rows in live_results for this strategy)
        int trading_days_count = 1; // Default to 1 to avoid division by zero on first day
        try {
            auto count_result = db->execute_query(
                "SELECT COUNT(*) AS cnt FROM trading.live_results WHERE strategy_id = 'LIVE_TREND_FOLLOWING'");
            if (count_result.is_ok()) {
                auto table = count_result.value();
                if (table && table->num_rows() > 0 && table->num_columns() > 0) {
                    auto col = table->column(0);
                    if (col->num_chunks() > 0) {
                        auto arr = std::static_pointer_cast<arrow::Int64Array>(col->chunk(0));
                        if (arr && arr->length() > 0 && !arr->IsNull(0)) {
                            trading_days_count = std::max<int>(1, static_cast<int>(arr->Value(0)));
                        }
                    }
                }
            }
        } catch (const std::exception& e) {
            WARN(std::string("Failed to count live_results rows: ") + e.what());
        }

        // Rdaily from total return across n days (in decimal)
        double rdaily = std::pow(1.0 + total_return_decimal, 1.0 / static_cast<double>(trading_days_count)) - 1.0;
        // Annualize: (1 + Rdaily)^252 - 1, then convert to percent
        double annualized_decimal = std::pow(1.0 + rdaily, 252.0) - 1.0;
        total_return_annualized = annualized_decimal * 100.0;

        INFO("Portfolio value calculation:");
        INFO("  Previous portfolio value: $" + std::to_string(previous_portfolio_value));
        INFO("  Daily PnL: $" + std::to_string(daily_pnl));
        INFO("  Current portfolio value: $" + std::to_string(current_portfolio_value));
        INFO("  Total PnL: $" + std::to_string(total_pnl));
        INFO("  Daily return: " + std::to_string(daily_return) + "%");
        INFO("  Annualized return: " + std::to_string(total_return_annualized) + "%");
        
        std::cout << "Total P&L: $" << std::fixed << std::setprecision(2) << total_pnl << std::endl;
        std::cout << "Realized P&L: $" << std::fixed << std::setprecision(2) << total_realized_pnl << std::endl;
        std::cout << "Unrealized P&L: $" << std::fixed << std::setprecision(2) << total_unrealized_pnl << std::endl;
        std::cout << "Current Portfolio Value: $" << std::fixed << std::setprecision(2) << current_portfolio_value << std::endl;
        std::cout << "Total Return (Annualized): " << std::fixed << std::setprecision(2) << total_return_annualized << "%" << std::endl;
        std::cout << "Daily Return: " << std::fixed << std::setprecision(2) << daily_return << "%" << std::endl;
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
            margin_cushion = (current_portfolio_value - maintenance_requirement_today) / current_portfolio_value;
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
        std::cout << std::setw(10) << "Symbol" << " | "
                  << std::setw(12) << "Forecast" << " | "
                  << std::setw(12) << "Position" << std::endl;
        std::cout << std::string(40, '-') << std::endl;

        // Collect signals for database storage
        std::unordered_map<std::string, double> signals_to_store;

        for (const auto& symbol : symbols) {
            double forecast = tf_strategy->get_forecast(symbol);
            double position = tf_strategy->get_position(symbol);

            signals_to_store[symbol] = forecast;

            std::cout << std::setw(10) << symbol << " | "
                      << std::setw(12) << std::fixed << std::setprecision(4) << forecast << " | "
                      << std::setw(12) << std::fixed << std::setprecision(2) << position << std::endl;
        }

        // Store signals in database
        if (!signals_to_store.empty()) {
            INFO("Storing " + std::to_string(signals_to_store.size()) + " signals to database...");
            auto signals_result = db->store_signals(signals_to_store, "LIVE_TREND_FOLLOWING", now, "trading.signals");
            if (signals_result.is_error()) {
                ERROR("Failed to store signals: " + std::string(signals_result.error()->what()));
            } else {
                INFO("Successfully stored " + std::to_string(signals_to_store.size()) + " signals to database");
            }
        } else {
            INFO("No signals to store (all forecasts are zero)");
        }

        // Save trading results to results table
        INFO("Saving trading results to database...");
        try {
            // Calculate current date for results (use override date if specified)
            auto current_date = now;
            
            // Use the calculated returns from above
            double sharpe_ratio = 0.0;  // Would need historical data to calculate
            double sortino_ratio = 0.0; // Would need historical data to calculate
            double max_drawdown = 0.0;  // Would need historical data to calculate
            double calmar_ratio = 0.0;  // Would need historical data to calculate
            double volatility = 0.0;
            int total_trades = 0;       // No trades in daily position generation
            double win_rate = 0.0;      // No trades in daily position generation
            double profit_factor = 0.0; // No trades in daily position generation
            double avg_win = 0.0;       // No trades in daily position generation
            double avg_loss = 0.0;      // No trades in daily position generation
            double max_win = 0.0;       // No trades in daily position generation
            double max_loss = 0.0;      // No trades in daily position generation
            double avg_holding_period = 0.0; // No trades in daily position generation
            double var_95 = 0.0;
            double cvar_95 = 0.0;
            double beta = 0.0;
            double correlation = 0.0;
            double downside_volatility = 0.0;
            
            // Get volatility from risk evaluation if available
            if (risk_eval.is_ok()) {
                const auto& r = risk_eval.value();
                volatility = r.portfolio_var * 100.0; // Convert to percentage
                var_95 = r.portfolio_var * 100.0;     // Use portfolio VaR as proxy
                cvar_95 = r.portfolio_var * 100.0;    // Use portfolio VaR as proxy (no CVaR available)
                beta = 0.0;                           // No beta available in RiskResult
                correlation = r.correlation_risk;     // Use correlation risk
            }
            
            // Create configuration JSON
            nlohmann::json config_json;
            config_json["strategy_type"] = "LIVE_TREND_FOLLOWING";
            config_json["capital_allocation"] = tf_config.capital_allocation;
            config_json["max_leverage"] = tf_config.max_leverage;
            config_json["weight"] = trend_config.weight;
            config_json["risk_target"] = trend_config.risk_target;
            config_json["idm"] = trend_config.idm;
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

            // Use the calculated PnL values from position analysis
            double portfolio_leverage = (current_portfolio_value != 0.0) ? (gross_notional / current_portfolio_value) : 0.0;
            // equity_to_margin_ratio and margin_cushion already computed above
            
            // First delete existing results for this strategy and date
            // Validate table name before using it in DELETE query
            auto live_results_table_validation = db->validate_table_name("trading.live_results");
            if (live_results_table_validation.is_error()) {
                ERROR("Invalid live results table name: " + std::string(live_results_table_validation.error()->what()));
            } else {
                std::string delete_query = "DELETE FROM trading.live_results WHERE strategy_id = 'LIVE_TREND_FOLLOWING' AND date = '" + date_ss.str() + "'";
                auto delete_result = db->execute_direct_query(delete_query);
                if (delete_result.is_error()) {
                    WARN("Failed to delete existing live results: " + std::string(delete_result.error()->what()));
                }
            }
            
            // Then insert new results with all required columns
            std::string query = "INSERT INTO trading.live_results "
                               "(strategy_id, date, total_return, volatility, total_pnl, total_unrealized_pnl, "
                               "total_realized_pnl, current_portfolio_value, portfolio_var, gross_leverage, "
                               "net_leverage, portfolio_leverage, equity_to_margin_ratio, margin_cushion, max_correlation, jump_risk, risk_scale, "
                               "gross_notional, net_notional, active_positions, config, daily_return, daily_pnl, "
                               "total_commissions, daily_realized_pnl, daily_unrealized_pnl, daily_commissions, margin_posted, cash_available) "
                               "VALUES ('LIVE_TREND_FOLLOWING', '" + date_ss.str() + "', " +
                                std::to_string(total_return_annualized) + ", " + std::to_string(volatility) + ", " +
                               std::to_string(total_pnl) + ", " + std::to_string(total_unrealized_pnl) + ", " +
                               std::to_string(total_realized_pnl) + ", " + std::to_string(current_portfolio_value) + ", " +
                               std::to_string(portfolio_var) + ", " + std::to_string(gross_leverage) + ", " +
                               std::to_string(net_leverage) + ", " + std::to_string(portfolio_leverage) + ", " +
                               std::to_string(equity_to_margin_ratio) + ", " + std::to_string(margin_cushion) + ", " +
                               std::to_string(max_correlation) + ", " + std::to_string(jump_risk) + ", " +
                                std::to_string(risk_scale) + ", " + std::to_string(gross_notional) + ", " +
                                std::to_string(net_notional) + ", " +
                               std::to_string(active_positions) + ", '" + config_json.dump() + "', " +
                               std::to_string(daily_return) + ", " + std::to_string(daily_pnl) + ", " + std::to_string(total_commissions_cumulative) + ", " +
                               std::to_string(daily_realized_pnl) + ", " + std::to_string(daily_unrealized_pnl) + ", " +
                               std::to_string(total_daily_commissions) + ", " + std::to_string(total_posted_margin) + ", " + std::to_string(current_portfolio_value - total_posted_margin) + ")";
            
            auto results_save_result = db->execute_direct_query(query);
            
            if (results_save_result.is_error()) {
                ERROR("Failed to save trading results: " + std::string(results_save_result.error()->what()));
            } else {
                INFO("Successfully saved trading results to database");
            }
        } catch (const std::exception& e) {
            ERROR("Exception while saving trading results: " + std::string(e.what()));
        }

        // Save positions to file for external consumption
        INFO("Saving today's positions to file...");
        // Format: DD-MM-YYYY_positions.csv
        std::string day_str = std::string(2 - std::to_string(now_tm->tm_mday).length(), '0') + std::to_string(now_tm->tm_mday);
        std::string month_str = std::string(2 - std::to_string(now_tm->tm_mon + 1).length(), '0') + std::to_string(now_tm->tm_mon + 1);
        std::string year_str = std::to_string(now_tm->tm_year + 1900);
        std::string today_filename = day_str + "-" + month_str + "-" + year_str + "_positions.csv";
        
        std::ofstream position_file(today_filename);
        if (position_file.is_open()) {
            // Query daily commissions per symbol from executions table
            std::unordered_map<std::string, double> symbol_commissions;
            try {
                std::string commission_query =
                    "SELECT symbol, COALESCE(SUM(commission), 0.0) as total_commission "
                    "FROM trading.executions "
                    "WHERE DATE(execution_time) = '" + date_ss.str() + "' "
                    "GROUP BY symbol";
                auto commission_result = db->execute_query(commission_query);
                if (commission_result.is_ok()) {
                    auto table = commission_result.value();
                    if (table && table->num_rows() > 0) {
                        auto symbol_col = table->column(0);
                        auto commission_col = table->column(1);
                        for (int64_t i = 0; i < table->num_rows(); ++i) {
                            auto sym_chunk = symbol_col->chunk(0);
                            auto comm_chunk = commission_col->chunk(0);
                            if (sym_chunk && comm_chunk) {
                                auto sym_array = std::static_pointer_cast<arrow::StringArray>(sym_chunk);
                                auto comm_array = std::static_pointer_cast<arrow::DoubleArray>(comm_chunk);
                                if (i < sym_array->length() && i < comm_array->length()) {
                                    std::string sym = sym_array->GetString(i);
                                    double comm = comm_array->Value(i);
                                    symbol_commissions[sym] = comm;
                                }
                            }
                        }
                    }
                } else {
                    WARN("Failed to query commissions: " + std::string(commission_result.error()->what()));
                }
            } catch (const std::exception& e) {
                WARN("Exception querying commissions: " + std::string(e.what()));
            }

            // Portfolio-level metrics as header comment
            position_file << "# Portfolio Value: " << std::fixed << std::setprecision(2) << current_portfolio_value
                         << ", Gross Notional: " << gross_notional
                         << ", Net Notional: " << net_notional
                         << ", Date: " << date_ss.str() << "\n";

            // CSV header for today's positions (forward-looking, no PnL)
            position_file << "symbol,quantity,market_price,notional,pct_of_gross_notional,pct_of_portfolio_value,forecast,volatility,ema_8,ema_32,ema_64,ema_256\n";
            for (const auto& [symbol, position] : positions) {
                double current_qty = position.quantity.as_double();

                // Get contract multiplier for proper notional calculation (YOUR FIX)
                std::string lookup_sym = symbol;
                auto dotpos = lookup_sym.find(".v.");
                if (dotpos != std::string::npos) {
                    lookup_sym = lookup_sym.substr(0, dotpos);
                }
                dotpos = lookup_sym.find(".c.");
                if (dotpos != std::string::npos) {
                    lookup_sym = lookup_sym.substr(0, dotpos);
                }

                double contract_multiplier = 0.0;
                try {
                    auto& registry = InstrumentRegistry::instance();
                    auto instrument_ptr = registry.get_instrument(lookup_sym);
                    if (!instrument_ptr) {
                        ERROR("CRITICAL: Instrument " + lookup_sym + " not found in registry for CSV export!");
                        throw std::runtime_error("Missing instrument in registry: " + lookup_sym);
                    }
                    // Get multiplier from registry (ONLY source)
                    contract_multiplier = instrument_ptr->get_multiplier();
                    if (contract_multiplier <= 0) {
                        ERROR("CRITICAL: Invalid multiplier " + std::to_string(contract_multiplier) +
                              " for " + lookup_sym);
                        throw std::runtime_error("Invalid multiplier for: " + lookup_sym);
                    }
                } catch (const std::exception& e) {
                    ERROR("CRITICAL: Failed to get multiplier for " + symbol + ": " + e.what());
                    throw;  // Re-throw - cannot export without proper multiplier
                }
                double forecast = tf_strategy->get_forecast(symbol);
                // Get Day T-1 close (market price for Day T positions)
                double market_price = position.average_price.as_double(); // Default fallback
                if (previous_day_close_prices.find(symbol) != previous_day_close_prices.end()) {
                    market_price = previous_day_close_prices[symbol];
                }

                // Notional with proper contract multiplier (YOUR FIX)
                double notional = current_qty * market_price * contract_multiplier;

                // Calculate price difference percentage (YOUR VERSION)
                double price_diff_pct = 0.0;
                if (position.average_price.as_double() != 0.0) {
                    price_diff_pct = ((market_price - position.average_price.as_double()) /
                                      position.average_price.as_double()) * 100.0;
                }

                // Get previous position data for calculations (THEIR VERSION)
                double prev_qty = 0.0;
                double prev_price = market_price; // Default to current if no previous
                auto prev_it = previous_positions.find(symbol);
                if (prev_it != previous_positions.end()) {
                    prev_qty = prev_it->second.quantity.as_double();
                    prev_price = prev_it->second.average_price.as_double();
                }

                // Calculate position-level metrics (THEIR VERSION)
                double quantity_change = current_qty - prev_qty;
                double price_change = market_price - prev_price;
                double price_change_pct = (prev_price != 0.0) ? (price_change / prev_price) * 100.0 : 0.0;
                double pct_of_gross_notional = (gross_notional != 0.0) ? (std::abs(notional) / gross_notional) * 100.0 : 0.0;
                double pct_of_portfolio_value = (current_portfolio_value != 0.0) ? (std::abs(notional) / std::abs(current_portfolio_value)) * 100.0 : 0.0;

                // Get volatility from strategy's instrument data
                double volatility = 0.0;
                auto instrument_data = tf_strategy->get_instrument_data(symbol);
                if (instrument_data != nullptr) {
                    volatility = instrument_data->current_volatility;
                }

                // Get EMA values (8, 32, 64, 256)
                auto ema_values = tf_strategy->get_ema_values(symbol, {8, 32, 64, 256});
                double ema_8 = ema_values.count(8) ? ema_values[8] : 0.0;
                double ema_32 = ema_values.count(32) ? ema_values[32] : 0.0;
                double ema_64 = ema_values.count(64) ? ema_values[64] : 0.0;
                double ema_256 = ema_values.count(256) ? ema_values[256] : 0.0;

                // Write row for today's positions (forward-looking, no PnL/commission)
                position_file << symbol << ","
                             << current_qty << ","
                             << market_price << ","
                             << notional << ","
                             << pct_of_gross_notional << ","
                             << pct_of_portfolio_value << ","
                             << forecast << ","
                             << std::fixed << std::setprecision(6) << volatility << ","
                             << ema_8 << ","
                             << ema_32 << ","
                             << ema_64 << ","
                             << ema_256 << "\n";
            }
            position_file.close();
            INFO("Today's positions saved to " + today_filename);
        } else {
            ERROR("Failed to open today's position file for writing");
        }

        // Save yesterday's finalized positions (if not first trading day)
        std::string yesterday_filename;
        if (!is_first_trading_day && !previous_positions.empty()) {
            INFO("Saving yesterday's finalized positions to file...");
            // Calculate yesterday's date
            auto yesterday_time = now - std::chrono::hours(24);
            auto yesterday_time_t = std::chrono::system_clock::to_time_t(yesterday_time);
            std::tm* yesterday_tm = std::gmtime(&yesterday_time_t);

            std::string yesterday_day_str = std::string(2 - std::to_string(yesterday_tm->tm_mday).length(), '0') + std::to_string(yesterday_tm->tm_mday);
            std::string yesterday_month_str = std::string(2 - std::to_string(yesterday_tm->tm_mon + 1).length(), '0') + std::to_string(yesterday_tm->tm_mon + 1);
            std::string yesterday_year_str = std::to_string(yesterday_tm->tm_year + 1900);

            // Format: DD-1-MM-YYYY_positions_asof_DD-MM-YYYY.csv
            yesterday_filename = yesterday_day_str + "-" + yesterday_month_str + "-" + yesterday_year_str +
                                "_positions_asof_" + day_str + "-" + month_str + "-" + year_str + ".csv";

            std::ofstream yesterday_file(yesterday_filename);
            if (yesterday_file.is_open()) {
                // CSV header for yesterday's finalized positions
                yesterday_file << "symbol,quantity,entry_price,exit_price,realized_pnl\n";

                // Load finalized positions from database for CSV
                std::string yesterday_date_ss_csv;
                std::ostringstream yss_csv;
                auto yesterday_time_t_csv = std::chrono::system_clock::to_time_t(yesterday_time);
                yss_csv << std::put_time(std::gmtime(&yesterday_time_t_csv), "%Y-%m-%d");
                yesterday_date_ss_csv = yss_csv.str();

                INFO("Loading finalized positions for date: " + yesterday_date_ss_csv);

                std::string positions_query_csv = "SELECT symbol, quantity, average_price, daily_realized_pnl, daily_unrealized_pnl, last_update "
                                                 "FROM trading.positions "
                                                 "WHERE strategy_id = 'LIVE_TREND_FOLLOWING' AND DATE(last_update) = '" + yesterday_date_ss_csv + "'";

                INFO("Executing query: " + positions_query_csv);
                auto positions_result_csv = db->execute_query(positions_query_csv);

                if (!positions_result_csv.is_ok()) {
                    ERROR("Query failed: " + std::string(positions_result_csv.error()->what()));
                } else if (positions_result_csv.value()->num_rows() == 0) {
                    WARN("Query returned 0 rows for date: " + yesterday_date_ss_csv);
                }

                if (positions_result_csv.is_ok() && positions_result_csv.value()->num_rows() > 0) {
                    auto table_csv = positions_result_csv.value();
                    // All columns are StringArrays from generic converter
                    auto symbol_arr_csv = std::static_pointer_cast<arrow::StringArray>(table_csv->column(0)->chunk(0));
                    auto quantity_arr_csv = std::static_pointer_cast<arrow::StringArray>(table_csv->column(1)->chunk(0));
                    auto avg_price_arr_csv = std::static_pointer_cast<arrow::StringArray>(table_csv->column(2)->chunk(0));
                    auto realized_pnl_arr_csv = std::static_pointer_cast<arrow::StringArray>(table_csv->column(3)->chunk(0));

                    for (int64_t i = 0; i < table_csv->num_rows(); ++i) {
                        if (!symbol_arr_csv->IsNull(i) && !quantity_arr_csv->IsNull(i)) {
                            std::string symbol = symbol_arr_csv->GetString(i);
                            double quantity = std::stod(quantity_arr_csv->GetString(i));
                            double avg_price = std::stod(avg_price_arr_csv->GetString(i));
                            double realized_pnl = std::stod(realized_pnl_arr_csv->GetString(i));
                            
                            // Skip positions with zero quantity
                            if (std::abs(quantity) < 0.0001) continue;
                            
                            // Entry price: Day T-2 close
                            double entry_price = 0.0;
                            if (two_days_ago_close_prices.find(symbol) != two_days_ago_close_prices.end()) {
                                entry_price = two_days_ago_close_prices[symbol];
                            }
                            
                            // Exit price: Day T-1 close
                            double exit_price = 0.0;
                            if (previous_day_close_prices.find(symbol) != previous_day_close_prices.end()) {
                                exit_price = previous_day_close_prices[symbol];
                            }
                            
                            // Write row with finalized PnL from database
                            yesterday_file << symbol << ","
                                          << quantity << ","
                                          << entry_price << ","
                                          << exit_price << ","
                                          << realized_pnl << "\n";
                        }
                    }
                    INFO("Wrote " + std::to_string(table_csv->num_rows()) + " finalized positions to CSV");
                } else {
                    WARN("Failed to load finalized positions for CSV, using previous_positions");
                    // Fallback to previous_positions
                    for (const auto& [symbol, prev_position] : previous_positions) {
                        double prev_qty = prev_position.quantity.as_double();
                        if (std::abs(prev_qty) < 0.0001) continue;
                        
                        // Entry price: Day T-2 close
                        double entry_price = 0.0;
                        if (two_days_ago_close_prices.find(symbol) != two_days_ago_close_prices.end()) {
                            entry_price = two_days_ago_close_prices[symbol];
                        }
                        
                        // Exit price: Day T-1 close
                        double exit_price = 0.0;
                        if (previous_day_close_prices.find(symbol) != previous_day_close_prices.end()) {
                            exit_price = previous_day_close_prices[symbol];
                        }
                        
                        // Realized PnL (GROSS from positions table)
                        double realized_pnl = prev_position.realized_pnl.as_double();
                        
                        yesterday_file << symbol << ","
                                      << prev_qty << ","
                                      << entry_price << ","
                                      << exit_price << ","
                                      << realized_pnl << "\n";
                    }
                }

                yesterday_file.close();
                INFO("Yesterday's finalized positions saved to " + yesterday_filename);
            } else {
                ERROR("Failed to open yesterday's position file for writing");
            }
        }

        // Store equity curve in database
        INFO("Storing equity curve in database...");
        
        // First delete existing equity curve entry for this strategy and date
        // Use the same timestamp formatting as the database function to ensure consistency
        auto db_ptr = std::dynamic_pointer_cast<PostgresDatabase>(db);
        if (db_ptr) {
            // Validate table name before using it in DELETE query
            auto equity_table_validation = db_ptr->validate_table_name("trading.equity_curve");
            if (equity_table_validation.is_error()) {
                ERROR("Invalid equity curve table name: " + std::string(equity_table_validation.error()->what()));
            } else {
                // Use date-based deletion to handle any timestamp precision issues
                std::stringstream date_ss;
                auto time_t = std::chrono::system_clock::to_time_t(now);
                date_ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%d");
                
                std::string delete_equity_query = "DELETE FROM trading.equity_curve WHERE strategy_id = 'LIVE_TREND_FOLLOWING' AND DATE(timestamp) = '" + date_ss.str() + "'";
                auto delete_equity_result = db->execute_direct_query(delete_equity_query);
                if (delete_equity_result.is_error()) {
                    WARN("Failed to delete existing equity curve entry: " + std::string(delete_equity_result.error()->what()));
                }
            }
        }
        
        auto store_equity_result = db->store_trading_equity_curve("LIVE_TREND_FOLLOWING", now, current_portfolio_value, "trading.equity_curve");
        if (store_equity_result.is_error()) {
            ERROR("Failed to store equity curve: " + std::string(store_equity_result.error()->what()));
        } else {
            INFO("Equity curve stored successfully");
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
            std::cout << "Total processing time: " << std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now() - now).count() << "ms" << std::endl;
        }

        INFO("Daily trend following position generation completed successfully");

        // Send email report with trading results (based on send_email flag)
        if (send_email) {
            INFO("Sending email report...");
            try {
                auto email_sender = std::make_shared<EmailSender>(credentials);
                auto email_init_result = email_sender->initialize();
                if (email_init_result.is_error()) {
                    ERROR("Failed to initialize email sender: " + std::string(email_init_result.error()->what()));
                } else {
                // Prepare email data
                std::string date_str = std::to_string(now_tm->tm_year + 1900) + "-"
                                     + std::string(2 - std::to_string(now_tm->tm_mon + 1).length(), '0')
                                     + std::to_string(now_tm->tm_mon + 1) + "-"
                                     + std::string(2 - std::to_string(now_tm->tm_mday).length(), '0')
                                     + std::to_string(now_tm->tm_mday);

                std::string subject = "Daily Trading Report - " + date_str;

                // Load yesterday's finalized positions for email display
                std::unordered_map<std::string, Position> yesterday_positions_finalized;
                std::map<std::string, double> yesterday_daily_metrics_final;
                std::unordered_map<std::string, double> yesterday_entry_prices;  // Day T-2 close
                std::unordered_map<std::string, double> yesterday_exit_prices;   // Day T-1 close

                // Calculate yesterday's date for email
                auto yesterday_time_email = now - std::chrono::hours(24);
                auto yesterday_time_t_email = std::chrono::system_clock::to_time_t(yesterday_time_email);

                // Load finalized positions from database for email
                std::string yesterday_date_for_email;
                std::ostringstream yss_email;
                yss_email << std::put_time(std::gmtime(&yesterday_time_t_email), "%Y-%m-%d");
                yesterday_date_for_email = yss_email.str();

                INFO("Loading yesterday's finalized positions for email: " + yesterday_date_for_email);

                std::string positions_query_email = "SELECT symbol, quantity, average_price, daily_realized_pnl, daily_unrealized_pnl, last_update "
                                                   "FROM trading.positions "
                                                   "WHERE strategy_id = 'LIVE_TREND_FOLLOWING' AND DATE(last_update) = '" + yesterday_date_for_email + "'";

                auto positions_result_email = db->execute_query(positions_query_email);

                if (positions_result_email.is_ok() && positions_result_email.value()->num_rows() > 0) {
                    auto table_email = positions_result_email.value();
                    // All columns are StringArrays from generic converter
                    auto symbol_arr = std::static_pointer_cast<arrow::StringArray>(table_email->column(0)->chunk(0));
                    auto quantity_arr = std::static_pointer_cast<arrow::StringArray>(table_email->column(1)->chunk(0));
                    auto avg_price_arr = std::static_pointer_cast<arrow::StringArray>(table_email->column(2)->chunk(0));
                    auto realized_pnl_arr = std::static_pointer_cast<arrow::StringArray>(table_email->column(3)->chunk(0));

                    for (int64_t i = 0; i < table_email->num_rows(); ++i) {
                        if (!symbol_arr->IsNull(i) && !quantity_arr->IsNull(i)) {
                            std::string symbol = symbol_arr->GetString(i);
                            double quantity = std::stod(quantity_arr->GetString(i));
                            double avg_price = std::stod(avg_price_arr->GetString(i));
                            double realized_pnl = std::stod(realized_pnl_arr->GetString(i));

                            // Skip positions with zero quantity
                            if (std::abs(quantity) < 0.0001) continue;

                            // Create Position object for yesterday's finalized position
                            Position pos;
                            pos.symbol = symbol;
                            pos.quantity = Decimal(quantity);
                            pos.average_price = Decimal(avg_price);
                            pos.realized_pnl = Decimal(realized_pnl);

                            yesterday_positions_finalized[symbol] = pos;

                            // Populate entry and exit prices
                            if (two_days_ago_close_prices.find(symbol) != two_days_ago_close_prices.end()) {
                                yesterday_entry_prices[symbol] = two_days_ago_close_prices[symbol];
                            }
                            if (previous_day_close_prices.find(symbol) != previous_day_close_prices.end()) {
                                yesterday_exit_prices[symbol] = previous_day_close_prices[symbol];
                            }
                        }
                    }
                    INFO("Loaded " + std::to_string(yesterday_positions_finalized.size()) + " finalized positions for email");

                    // Load yesterday's daily metrics from database for accurate display
                    std::string yesterday_metrics_query =
                        "SELECT daily_return, daily_unrealized_pnl, daily_realized_pnl, daily_pnl "
                        "FROM trading.live_results "
                        "WHERE strategy_id = 'LIVE_TREND_FOLLOWING' AND date = '" + yesterday_date_for_email + "' "
                        "ORDER BY date DESC LIMIT 1";

                    INFO("Loading yesterday's daily metrics from live_results: " + yesterday_metrics_query);
                    auto yesterday_metrics_result = db->execute_query(yesterday_metrics_query);

                    if (yesterday_metrics_result.is_ok() && yesterday_metrics_result.value()->num_rows() > 0) {
                        auto metrics_table = yesterday_metrics_result.value();
                        INFO("Retrieved " + std::to_string(metrics_table->num_rows()) + " rows from live_results");

                        auto daily_return_arr = std::static_pointer_cast<arrow::StringArray>(metrics_table->column(0)->chunk(0));
                        auto daily_unrealized_arr = std::static_pointer_cast<arrow::StringArray>(metrics_table->column(1)->chunk(0));
                        auto daily_realized_arr = std::static_pointer_cast<arrow::StringArray>(metrics_table->column(2)->chunk(0));
                        auto daily_total_arr = std::static_pointer_cast<arrow::StringArray>(metrics_table->column(3)->chunk(0));

                        if (!daily_return_arr->IsNull(0)) {
                            yesterday_daily_metrics_final["Daily Return"] = std::stod(daily_return_arr->GetString(0));
                            INFO("Daily Return: " + daily_return_arr->GetString(0));
                        }
                        if (!daily_unrealized_arr->IsNull(0)) {
                            yesterday_daily_metrics_final["Daily Unrealized PnL"] = std::stod(daily_unrealized_arr->GetString(0));
                            INFO("Daily Unrealized PnL: " + daily_unrealized_arr->GetString(0));
                        }
                        if (!daily_realized_arr->IsNull(0)) {
                            yesterday_daily_metrics_final["Daily Realized PnL"] = std::stod(daily_realized_arr->GetString(0));
                            INFO("Daily Realized PnL: " + daily_realized_arr->GetString(0));
                        }
                        if (!daily_total_arr->IsNull(0)) {
                            yesterday_daily_metrics_final["Daily Total PnL"] = std::stod(daily_total_arr->GetString(0));
                            INFO("Daily Total PnL: " + daily_total_arr->GetString(0));
                        }

                        INFO("Successfully loaded yesterday's daily metrics from live_results");
                    } else {
                        if (yesterday_metrics_result.is_error()) {
                            ERROR("Failed to query live_results: " + std::string(yesterday_metrics_result.error()->what()));
                        } else {
                            WARN("No rows found in live_results for date: " + yesterday_date_for_email);
                        }
                        // Fallback: calculate from positions if database query fails
                        double yesterday_daily_realized = 0.0;
                        for (const auto& [symbol, pos] : yesterday_positions_finalized) {
                            yesterday_daily_realized += pos.realized_pnl.as_double();
                        }
                        yesterday_daily_metrics_final["Daily Realized PnL"] = yesterday_daily_realized;
                        INFO("Calculated yesterday's metrics from positions (fallback) - Daily Realized PnL: " + std::to_string(yesterday_daily_realized));
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
                strategy_metrics["Total Annualized Return"] = total_return_annualized;
                strategy_metrics["Total Unrealized PnL"] = total_unrealized_pnl;
                strategy_metrics["Total Realized PnL"] = total_realized_pnl;
                strategy_metrics["Total PnL"] = total_pnl;
                if (risk_eval.is_ok()) {
                    strategy_metrics["Volatility"] = risk_eval.value().portfolio_var * 100.0;
                }
                strategy_metrics["Total Commissions"] = total_commissions_cumulative;
                strategy_metrics["Current Portfolio Value"] = current_portfolio_value;

                // Leverage Metrics - Calculate values from position analysis
                double gross_leverage_calc = (current_portfolio_value != 0.0) ? (gross_notional / current_portfolio_value) : 0.0;
                double net_leverage_calc = (current_portfolio_value != 0.0) ? (net_notional / current_portfolio_value) : 0.0;
                double portfolio_leverage_calc = (current_portfolio_value != 0.0) ? (gross_notional / current_portfolio_value) : 0.0;
                
                strategy_metrics["Gross Leverage"] = gross_leverage_calc;
                strategy_metrics["Net Leverage"] = net_leverage_calc;
                strategy_metrics["Portfolio Leverage"] = portfolio_leverage_calc;
                strategy_metrics["Equity-to-Margin Ratio"] = equity_to_margin_ratio;

                // Risk & Liquidity Metrics
                strategy_metrics["Margin Cushion"] = margin_cushion * 100.0; // Convert to percentage
                strategy_metrics["Margin Posted"] = total_posted_margin;
                strategy_metrics["Cash Available"] = current_portfolio_value - total_posted_margin;

                // Note: yesterday_daily_metrics_final is now loaded AFTER database updates above
                // So we don't need to create it here anymore

                // Generate email body with is_daily_strategy flag set to true and current prices
                std::string email_body = email_sender->generate_trading_report_body(
                    positions,
                    risk_eval.is_ok() ? std::make_optional(risk_eval.value()) : std::nullopt,
                    strategy_metrics,
                    daily_executions,
                    date_str,
                    true,  // is_daily_strategy
                    previous_day_close_prices,  // Pass Day T-1 close prices for today's positions
                    db,  // Pass database for symbols reference table
                    yesterday_positions_finalized,  // Now populated with yesterday's finalized positions
                    yesterday_exit_prices,  // Day T-1 close prices for yesterday's positions
                    yesterday_entry_prices,  // Day T-2 close prices for yesterday's positions
                    yesterday_daily_metrics_final  // Yesterday's metrics
                );
                
                // Send email with CSV attachments: today's positions and yesterday's finalized (if available)
                std::vector<std::string> attachments = {today_filename};
                if (!yesterday_filename.empty()) {
                    attachments.push_back(yesterday_filename);
                }

                auto send_result = email_sender->send_email(subject, email_body, true, attachments);
                if (send_result.is_error()) {
                    ERROR("Failed to send email: " + std::string(send_result.error()->what()));
                } else {
                    std::string attachment_list = today_filename;
                    if (!yesterday_filename.empty()) {
                        attachment_list += ", " + yesterday_filename;
                    }
                    INFO("Email report sent successfully with CSV attachments: " + attachment_list);
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
