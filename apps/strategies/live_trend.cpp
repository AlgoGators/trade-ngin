#include <fstream>
#include <iomanip>
#include <iostream>
#include <chrono>
#include <ctime>
#include <set>
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/data/credential_store.hpp"
#include "trade_ngin/data/database_pooling.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/data/conversion_utils.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/strategy/trend_following.hpp"
#include "trade_ngin/core/email_sender.hpp"

using namespace trade_ngin;

int main() {
    try {
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

        // Get current date for daily processing
        auto now = std::chrono::system_clock::now();
        auto now_time_t = std::chrono::system_clock::to_time_t(now);
        std::tm* now_tm = std::localtime(&now_time_t);

        // Set start date to 300 days ago for sufficient historical data
        auto start_date = now - std::chrono::hours(24 * 300);  // 300 days ago

        // Set end date to today
        auto end_date = now;

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
        tf_config.capital_allocation = initial_capital * 0.85;  // Use 85% of capital
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
        
        // Calculate proper PnL based on position changes between days
        INFO("Calculating PnL based on position changes...");
        double total_realized_pnl = 0.0;
        double total_unrealized_pnl = 0.0;
        
        // Get current market prices for all symbols (both current and previous positions)
        std::set<std::string> all_symbols;
        for (const auto& [symbol, position] : positions) {
            if (position.quantity.as_double() != 0.0) {
                all_symbols.insert(symbol);
            }
        }
        for (const auto& [symbol, position] : previous_positions) {
            if (position.quantity.as_double() != 0.0) {
                all_symbols.insert(symbol);
            }
        }
        
        std::vector<std::string> symbols_to_price(all_symbols.begin(), all_symbols.end());
        INFO("Requesting current prices for " + std::to_string(symbols_to_price.size()) + " symbols");
        for (const auto& symbol : symbols_to_price) {
            DEBUG("Requesting price for symbol: " + symbol);
        }
        
        auto current_prices_result = db->get_latest_prices(symbols_to_price, trade_ngin::AssetClass::FUTURES);
        std::unordered_map<std::string, double> current_prices;
        if (current_prices_result.is_ok()) {
            current_prices = current_prices_result.value();
            INFO("Retrieved current prices for " + std::to_string(current_prices.size()) + " symbols");
            for (const auto& [symbol, price] : current_prices) {
                DEBUG("Got price for " + symbol + ": " + std::to_string(price));
            }
        } else {
            ERROR("Failed to get current prices: " + std::string(current_prices_result.error()->what()));
            ERROR("This means unrealized PnL will be calculated using average prices as fallback");
        }
        
        // Process each current position
        for (auto& [symbol, current_position] : positions) {
            double current_qty = current_position.quantity.as_double();
            double current_avg_price = current_position.average_price.as_double();
            
            // Find previous position for this symbol
            auto prev_it = previous_positions.find(symbol);
            double prev_qty = 0.0;
            double prev_avg_price = 0.0;
            double prev_realized_pnl = 0.0;
            
            if (prev_it != previous_positions.end()) {
                prev_qty = prev_it->second.quantity.as_double();
                prev_avg_price = prev_it->second.average_price.as_double();
                prev_realized_pnl = prev_it->second.realized_pnl.as_double();
            }
            
            // Get current market price for this symbol
            double current_market_price = current_avg_price; // Default fallback
            if (current_prices.find(symbol) != current_prices.end()) {
                current_market_price = current_prices[symbol];
            } else {
                WARN("No current market price available for " + symbol + ", using average price as fallback");
            }
            
            // Calculate realized PnL from position changes
            double position_realized_pnl = 0.0;
            
            if (prev_qty != 0.0 && current_qty != 0.0) {
                // Position size changed - calculate realized PnL for the difference
                double qty_change = current_qty - prev_qty;
                if (std::abs(qty_change) > 1e-6) {
                    if (qty_change < 0) {
                        // Position reduced - realize PnL on the closed portion
                        // For futures: realized_pnl = closed_quantity * (current_price - entry_price)
                        position_realized_pnl = -qty_change * (current_market_price - prev_avg_price);
                    } else {
                        // Position increased - no realized PnL, just new average price
                        // The new average price should already be calculated by the strategy
                        position_realized_pnl = 0.0;
                    }
                }
            } else if (prev_qty != 0.0 && current_qty == 0.0) {
                // Position completely closed - realize all PnL
                position_realized_pnl = prev_qty * (current_market_price - prev_avg_price);
            } else if (prev_qty == 0.0 && current_qty != 0.0) {
                // New position - no realized PnL
                position_realized_pnl = 0.0;
            }
            
            // Calculate unrealized PnL for current position
            double position_unrealized_pnl = 0.0;
            if (current_qty != 0.0) {
                // For futures: unrealized_pnl = quantity * (current_price - average_price)
                position_unrealized_pnl = current_qty * (current_market_price - current_avg_price);
            }
            
            // Update position with calculated PnL
            current_position.realized_pnl = Decimal(prev_realized_pnl + position_realized_pnl);
            current_position.unrealized_pnl = Decimal(position_unrealized_pnl);
            
            total_realized_pnl += position_realized_pnl;
            total_unrealized_pnl += position_unrealized_pnl;
            
            DEBUG("Position " + symbol + ": prev_qty=" + std::to_string(prev_qty) + 
                  " current_qty=" + std::to_string(current_qty) + 
                  " prev_avg=" + std::to_string(prev_avg_price) + 
                  " current_avg=" + std::to_string(current_avg_price) + 
                  " market_price=" + std::to_string(current_market_price) + 
                  " realized_pnl=" + std::to_string(position_realized_pnl) + 
                  " unrealized_pnl=" + std::to_string(position_unrealized_pnl));
        }
        
        // Also process any previous positions that are no longer held (for realized PnL)
        for (const auto& [symbol, prev_position] : previous_positions) {
            if (positions.find(symbol) == positions.end() && prev_position.quantity.as_double() != 0.0) {
                // This position was completely closed
                double prev_qty = prev_position.quantity.as_double();
                double prev_avg_price = prev_position.average_price.as_double();
                double prev_realized_pnl = prev_position.realized_pnl.as_double();
                
                // Get current market price for realized PnL calculation
                double current_market_price = prev_avg_price; // Default fallback
                if (current_prices.find(symbol) != current_prices.end()) {
                    current_market_price = current_prices[symbol];
                }
                
                // Calculate realized PnL for completely closed position
                double position_realized_pnl = prev_qty * (current_market_price - prev_avg_price);
                total_realized_pnl += position_realized_pnl;
                
                DEBUG("Closed position " + symbol + ": qty=" + std::to_string(prev_qty) + 
                      " avg_price=" + std::to_string(prev_avg_price) + 
                      " market_price=" + std::to_string(current_market_price) + 
                      " realized_pnl=" + std::to_string(position_realized_pnl));
            }
        }
        
        INFO("Total realized PnL: " + std::to_string(total_realized_pnl));
        INFO("Total unrealized PnL: " + std::to_string(total_unrealized_pnl));

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
                
                // Get current market price
                double market_price = current_position.average_price.as_double();
                if (current_prices.find(symbol) != current_prices.end()) {
                    market_price = current_prices[symbol];
                }
                
                // Create execution report
                ExecutionReport exec;
                exec.order_id = "DAILY_" + symbol + "_" + date_str;  // Replace hyphens with underscores
                exec.exec_id = "EXEC_" + symbol + "_" + std::to_string(daily_executions.size());
                exec.symbol = symbol;
                exec.side = side;
                exec.filled_quantity = std::abs(trade_size);
                exec.fill_price = market_price;
                exec.fill_time = now;
                exec.commission = 0.0;
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
                
                // Get current market price for execution
                double market_price = prev_position.average_price.as_double(); // Default fallback
                if (current_prices.find(symbol) != current_prices.end()) {
                    market_price = current_prices[symbol];
                }
                
                // Create execution report for closing the position
                ExecutionReport exec;
                exec.order_id = "DAILY-" + symbol + "-" + date_str;
                exec.exec_id = "EXEC-" + symbol + "-" + std::to_string(daily_executions.size());
                exec.symbol = symbol;
                exec.side = prev_qty > 0 ? Side::SELL : Side::BUY; // Opposite of original position
                exec.filled_quantity = std::abs(prev_qty);
                exec.fill_price = market_price;
                exec.fill_time = now;
                exec.commission = 0.0;
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

        double total_notional = 0.0;
        int active_positions = 0;

        for (const auto& [symbol, position] : positions) {
            if (position.quantity.as_double() != 0.0) {
                active_positions++;
                double notional = position.quantity.as_double() * position.average_price.as_double();
                total_notional += notional;
                
                std::cout << std::setw(10) << symbol << " | "
                          << std::setw(10) << std::fixed << std::setprecision(2) 
                          << position.quantity.as_double() << " | "
                          << std::setw(10) << std::fixed << std::setprecision(2) 
                          << position.average_price.as_double() << " | "
                          << std::setw(12) << std::fixed << std::setprecision(2) 
                          << notional << " | "
                          << std::setw(10) << std::fixed << std::setprecision(2) 
                          << position.unrealized_pnl.as_double() << std::endl;
            }
        }

        std::cout << std::endl;
        std::cout << "Active Positions: " << active_positions << std::endl;
        std::cout << "Total Notional: $" << std::fixed << std::setprecision(2) << total_notional << std::endl;
        std::cout << "Portfolio Leverage: " << std::fixed << std::setprecision(2) 
                  << (total_notional / initial_capital) << "x" << std::endl;

        // Save positions to database
        INFO("Saving positions to database...");
        std::vector<trade_ngin::Position> positions_to_save;
        positions_to_save.reserve(positions.size());
        
        for (const auto& [symbol, position] : positions) {
            if (position.quantity.as_double() != 0.0) {  // Only save non-zero positions
                // Create a new position with validated values
                trade_ngin::Position validated_position;
                validated_position.symbol = position.symbol;
                validated_position.quantity = position.quantity;
                validated_position.last_update = std::chrono::system_clock::now();  // Use current time
                validated_position.unrealized_pnl = position.unrealized_pnl;
                validated_position.realized_pnl = position.realized_pnl;
                
                // Validate and convert average_price to ensure it's within Decimal limits
                double avg_price_double = static_cast<double>(position.average_price);
                DEBUG("Validating position " + symbol + " with average_price: " + std::to_string(avg_price_double));
                
                // Decimal limit is approximately 92,233,720,368,547.75807
                const double DECIMAL_MAX = 9.223372036854775807e13;  // INT64_MAX / SCALE
                if (avg_price_double > DECIMAL_MAX || avg_price_double < -DECIMAL_MAX) {
                    WARN("Position " + symbol + " has average_price " + std::to_string(avg_price_double) + 
                         " which exceeds Decimal limit (" + std::to_string(DECIMAL_MAX) + "), using 1.0 instead");
                    validated_position.average_price = trade_ngin::Decimal(1.0);
                } else {
                    try {
                        validated_position.average_price = position.average_price;
                        DEBUG("Successfully validated average_price for " + symbol);
                    } catch (const std::exception& e) {
                        ERROR("Failed to validate average_price for " + symbol + ": " + std::string(e.what()));
                        validated_position.average_price = trade_ngin::Decimal(1.0);
                    }
                }
                
                positions_to_save.push_back(validated_position);
                DEBUG("Position to save: " + symbol + " qty=" + std::to_string(position.quantity.as_double()) + 
                      " price=" + std::to_string(static_cast<double>(validated_position.average_price)));
            }
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
        // Live trading metrics (calculated from actual position changes)
        double total_pnl = total_realized_pnl + total_unrealized_pnl;
        double current_portfolio_value = initial_capital + total_pnl;
        double daily_return = 0.0;
        
        // Calculate daily return if we have previous day data
        double previous_portfolio_value = initial_capital; // Default to initial capital
        if (!previous_positions.empty()) {
            // Try to load previous day's portfolio value from live_results table
            try {
                std::stringstream prev_date_ss;
                auto prev_time_t = std::chrono::system_clock::to_time_t(previous_date);
                prev_date_ss << std::put_time(std::gmtime(&prev_time_t), "%Y-%m-%d %H:%M:%S");
                
                std::string prev_query = "SELECT current_portfolio_value FROM trading.live_results WHERE strategy_id = 'LIVE_TREND_FOLLOWING' AND date = '" + prev_date_ss.str() + "'";
                auto prev_result = db->execute_query(prev_query);
                if (prev_result.is_ok() && prev_result.value()->num_rows() > 0) {
                    // Extract portfolio value from result (simplified - would need proper Arrow table parsing)
                    // For now, use initial capital as fallback
                    previous_portfolio_value = initial_capital;
                }
            } catch (const std::exception& e) {
                INFO("Could not load previous day portfolio value: " + std::string(e.what()));
            }
            
            if (previous_portfolio_value > 0) {
                daily_return = (current_portfolio_value - previous_portfolio_value) / previous_portfolio_value * 100.0;
            }
        }
        
        std::cout << "Total P&L: $" << std::fixed << std::setprecision(2) << total_pnl << std::endl;
        std::cout << "Realized P&L: $" << std::fixed << std::setprecision(2) << total_realized_pnl << std::endl;
        std::cout << "Unrealized P&L: $" << std::fixed << std::setprecision(2) << total_unrealized_pnl << std::endl;
        std::cout << "Current Portfolio Value: $" << std::fixed << std::setprecision(2) << current_portfolio_value << std::endl;
        std::cout << "Daily Return: " << std::fixed << std::setprecision(2) << daily_return << "%" << std::endl;
        std::cout << "Portfolio Leverage: " << std::fixed << std::setprecision(2) 
                  << (total_notional / current_portfolio_value) << "x" << std::endl;

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
            // Calculate current date for results
            auto current_date = std::chrono::system_clock::now();
            
            // Calculate portfolio metrics
            double total_return = 0.0;  // For daily runs, this would be calculated from previous day
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
            config_json["total_notional"] = total_notional;
            config_json["portfolio_leverage"] = total_notional / initial_capital;
            
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
            double portfolio_leverage = total_notional / current_portfolio_value;
            
            // First delete existing results for this strategy and date
            std::string delete_query = "DELETE FROM trading.live_results WHERE strategy_id = 'LIVE_TREND_FOLLOWING' AND date = '" + date_ss.str() + "'";
            auto delete_result = db->execute_direct_query(delete_query);
            if (delete_result.is_error()) {
                WARN("Failed to delete existing live results: " + std::string(delete_result.error()->what()));
            }
            
            // Then insert new results
            std::string query = "INSERT INTO trading.live_results "
                               "(strategy_id, date, total_return, volatility, total_pnl, unrealized_pnl, "
                               "realized_pnl, current_portfolio_value, portfolio_var, gross_leverage, "
                               "net_leverage, portfolio_leverage, max_correlation, jump_risk, risk_scale, "
                               "total_notional, active_positions, config) "
                               "VALUES ('LIVE_TREND_FOLLOWING', '" + date_ss.str() + "', " +
                               std::to_string(total_return) + ", " + std::to_string(volatility) + ", " +
                               std::to_string(total_pnl) + ", " + std::to_string(total_unrealized_pnl) + ", " +
                               std::to_string(total_realized_pnl) + ", " + std::to_string(current_portfolio_value) + ", " +
                               std::to_string(portfolio_var) + ", " + std::to_string(gross_leverage) + ", " +
                               std::to_string(net_leverage) + ", " + std::to_string(portfolio_leverage) + ", " +
                               std::to_string(max_correlation) + ", " + std::to_string(jump_risk) + ", " +
                               std::to_string(risk_scale) + ", " + std::to_string(total_notional) + ", " +
                               std::to_string(active_positions) + ", '" + config_json.dump() + "')";
            
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
        INFO("Saving positions to file...");
        std::string filename = "daily_positions_" + 
                              std::to_string(now_tm->tm_year + 1900) + 
                              std::string(2 - std::to_string(now_tm->tm_mon + 1).length(), '0') + 
                              std::to_string(now_tm->tm_mon + 1) + 
                              std::string(2 - std::to_string(now_tm->tm_mday).length(), '0') + 
                              std::to_string(now_tm->tm_mday) + ".csv";
        
        std::ofstream position_file(filename);
        if (position_file.is_open()) {
            position_file << "symbol,quantity,avg_price,market_price,notional,unrealized_pnl,realized_pnl,forecast\n";
            for (const auto& [symbol, position] : positions) {
                double notional = position.quantity.as_double() * position.average_price.as_double();
                double forecast = tf_strategy->get_forecast(symbol);
                double market_price = position.average_price.as_double(); // Default fallback
                if (current_prices.find(symbol) != current_prices.end()) {
                    market_price = current_prices[symbol];
                }
                position_file << symbol << ","
                             << position.quantity.as_double() << ","
                             << position.average_price.as_double() << ","
                             << market_price << ","
                             << notional << ","
                             << position.unrealized_pnl.as_double() << ","
                             << position.realized_pnl.as_double() << ","
                             << forecast << "\n";
            }
            position_file.close();
            INFO("Positions saved to " + filename);
        } else {
            ERROR("Failed to open position file for writing");
        }

        // Store equity curve in database
        INFO("Storing equity curve in database...");
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
        std::cout << "Positions file: " << filename << std::endl;
        std::cout << "Total processing time: " << std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now() - now).count() << "ms" << std::endl;

        INFO("Daily trend following position generation completed successfully");

        // Send email report with trading results
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
                
                // Create strategy metrics map (no duplicates with risk metrics)
                std::map<std::string, double> strategy_metrics;
                
                // Add live trading metrics to strategy_metrics
                strategy_metrics["Current Portfolio Value"] = current_portfolio_value;
                strategy_metrics["Total P&L"] = total_pnl;
                strategy_metrics["Realized P&L"] = total_realized_pnl;
                strategy_metrics["Unrealized P&L"] = total_unrealized_pnl;
                strategy_metrics["Daily Return"] = daily_return;
                strategy_metrics["Gross Leverage"] = total_notional / current_portfolio_value;
                strategy_metrics["Net Leverage"] = total_notional / current_portfolio_value; // Same as gross for this strategy
                strategy_metrics["Active Positions"] = active_positions;
                strategy_metrics["Total Notional"] = total_notional;
                
                // Note: Risk metrics (Volatility, Jump Risk, Risk Scale) are shown in Risk Metrics section
                // to avoid duplication
                
                // Generate email body
                std::string email_body = email_sender->generate_trading_report_body(
                    positions, 
                    risk_eval.is_ok() ? std::make_optional(risk_eval.value()) : std::nullopt,
                    strategy_metrics,
                    date_str
                );
                
                // Send email
                auto send_result = email_sender->send_email(subject, email_body, true);
                if (send_result.is_error()) {
                    ERROR("Failed to send email: " + std::string(send_result.error()->what()));
                } else {
                    INFO("Email report sent successfully");
                }
            }
        } catch (const std::exception& e) {
            ERROR("Exception during email sending: " + std::string(e.what()));
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
