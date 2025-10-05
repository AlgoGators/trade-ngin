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
#include "trade_ngin/instruments/futures.hpp"
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
        
        // Get current market prices for PnL calculations
        INFO("Getting current market prices for PnL calculations...");
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
        INFO("Requesting current prices for " + std::to_string(symbols_to_price.size()) + " symbols");

        auto current_prices_result = db->get_latest_prices(symbols_to_price, trade_ngin::AssetClass::FUTURES);
        std::unordered_map<std::string, double> current_prices;
        if (current_prices_result.is_ok()) {
            current_prices = current_prices_result.value();
            INFO("Retrieved current prices for " + std::to_string(current_prices.size()) + " symbols");
        } else {
            ERROR("Failed to get current prices: " + std::string(current_prices_result.error()->what()));
        }

        // Calculate Daily PnL for each position
        INFO("Calculating daily PnL for positions...");
        double daily_realized_pnl = 0.0;
        double daily_unrealized_pnl = 0.0;  // For futures, this will be 0 as all PnL is realized
        double total_daily_commissions = 0.0;

        // Track PnL by position for database storage
        std::unordered_map<std::string, double> position_daily_pnl;

        // Calculate PnL for each current position
        for (auto& [symbol, current_position] : positions) {
            double current_qty = current_position.quantity.as_double();
            double current_price = current_position.average_price.as_double();

            // Get actual market price if available
            if (current_prices.find(symbol) != current_prices.end()) {
                current_price = current_prices[symbol];
            }

            // Find previous position
            double prev_qty = 0.0;
            double prev_price = current_price;  // Default to current if no previous
            auto prev_it = previous_positions.find(symbol);
            if (prev_it != previous_positions.end()) {
                prev_qty = prev_it->second.quantity.as_double();
                prev_price = prev_it->second.average_price.as_double();
            }

            // Calculate daily PnL for this position
            double daily_position_pnl = 0.0;

            if (prev_qty != 0.0 && current_qty != 0.0) {
                // Position held overnight and still open
                // PnL = quantity * price_change (for futures, this is mark-to-market)
                daily_position_pnl = prev_qty * (current_price - prev_price);

                // If position size changed, add PnL from the change
                if (std::abs(current_qty - prev_qty) > 1e-6) {
                    // Position changed during the day
                    // Additional quantity traded at today's price
                    // This PnL is already reflected in executions
                }
            } else if (prev_qty != 0.0 && current_qty == 0.0) {
                // Position was closed today
                daily_position_pnl = prev_qty * (current_price - prev_price);
            } else if (prev_qty == 0.0 && current_qty != 0.0) {
                // New position opened today
                // No PnL from overnight hold, only from intraday if price moved
                // For new positions, PnL is 0 on the first day (just opened at current price)
                daily_position_pnl = 0.0;
            }

            // Store position daily PnL (before commissions)
            position_daily_pnl[symbol] = daily_position_pnl;

            // For futures, all PnL is realized (mark-to-market)
            daily_realized_pnl += daily_position_pnl;

            // Update position with daily PnL
            current_position.realized_pnl = Decimal(daily_position_pnl);
            current_position.unrealized_pnl = Decimal(0.0);  // Always 0 for futures

            INFO("Position " + symbol + " daily PnL: prev_qty=" + std::to_string(prev_qty) +
                 " curr_qty=" + std::to_string(current_qty) +
                 " prev_price=" + std::to_string(prev_price) +
                 " curr_price=" + std::to_string(current_price) +
                 " daily_pnl=" + std::to_string(daily_position_pnl));
        }

        // Check for positions that were closed (in previous but not in current)
        for (const auto& [symbol, prev_position] : previous_positions) {
            if (positions.find(symbol) == positions.end() && prev_position.quantity.as_double() != 0.0) {
                // Position was completely closed
                double prev_qty = prev_position.quantity.as_double();
                double prev_price = prev_position.average_price.as_double();
                double current_price = prev_price;  // Default

                if (current_prices.find(symbol) != current_prices.end()) {
                    current_price = current_prices[symbol];
                }

                double daily_position_pnl = prev_qty * (current_price - prev_price);
                position_daily_pnl[symbol] = daily_position_pnl;
                daily_realized_pnl += daily_position_pnl;

                // Add a zero-quantity position to track the closed position's PnL
                Position closed_pos;
                closed_pos.symbol = symbol;
                closed_pos.quantity = Decimal(0.0);
                closed_pos.average_price = Decimal(current_price);
                closed_pos.realized_pnl = Decimal(daily_position_pnl);
                closed_pos.unrealized_pnl = Decimal(0.0);
                closed_pos.last_update = now;
                positions[symbol] = closed_pos;

                INFO("Closed position " + symbol + " daily PnL: qty=" + std::to_string(prev_qty) +
                     " prev_price=" + std::to_string(prev_price) +
                     " curr_price=" + std::to_string(current_price) +
                     " daily_pnl=" + std::to_string(daily_position_pnl));
            }
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
                
                // Get current market price
                double market_price = current_position.average_price.as_double();
                if (current_prices.find(symbol) != current_prices.end()) {
                    market_price = current_prices[symbol];
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
                exec.fill_price = market_price;
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
                
                // Get current market price for execution
                double market_price = prev_position.average_price.as_double(); // Default fallback
                if (current_prices.find(symbol) != current_prices.end()) {
                    market_price = current_prices[symbol];
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
                exec.fill_price = market_price;
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

        double gross_notional = 0.0;
        double net_notional = 0.0;
        double total_posted_margin = 0.0;  // Sum of per-contract initial margins times contracts
        double maintenance_requirement_today = 0.0;  // Sum of per-contract maintenance margins times contracts
        int active_positions = 0;

        for (const auto& [symbol, position] : positions) {
            if (position.quantity.as_double() != 0.0) {
                active_positions++;
                // Use current market price if available
                double market_price = position.average_price.as_double();
                auto itp = current_prices.find(symbol);
                if (itp != current_prices.end()) {
                    market_price = itp->second;
                }
                double signed_notional = position.quantity.as_double() * market_price;
                net_notional += signed_notional;
                gross_notional += std::abs(signed_notional);

                // Compute posted margin per instrument (per-contract initial margin × contracts)
                try {
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

                    auto instrument_ptr = registry.get_instrument(lookup_sym);
                    if (instrument_ptr) {
                        double contracts_abs = std::abs(position.quantity.as_double());
                        double initial_margin_per_contract = instrument_ptr->get_margin_requirement();
                        total_posted_margin += contracts_abs * initial_margin_per_contract;

                        // Try to get maintenance margin if available (e.g., futures)
                        // If not available, fall back to initial margin
                        double maintenance_margin_per_contract = initial_margin_per_contract;
                        // FuturesInstrument has get_maintenance_margin(); detect via dynamic_cast
                        if (auto futures_ptr = std::dynamic_pointer_cast<trade_ngin::FuturesInstrument>(instrument_ptr)) {
                            maintenance_margin_per_contract = futures_ptr->get_maintenance_margin();
                        }
                        maintenance_requirement_today += contracts_abs * maintenance_margin_per_contract;
                    }
                } catch (const std::exception& e) {
                    WARN("Failed to compute posted margin for " + symbol + ": " + std::string(e.what()));
                }
                
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
        double margin_leverage = (total_posted_margin > 0.0) ? (gross_notional / total_posted_margin) : 0.0;
        if (margin_leverage <= 1.0 && active_positions > 0) {
            WARN("Implied margin leverage (gross_notional / posted_margin) is <= 1.0; verify margins.");
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
                     " which exceeds Decimal limit, using current market price instead");
                // Use current market price if available
                if (current_prices.find(symbol) != current_prices.end()) {
                    validated_position.average_price = trade_ngin::Decimal(current_prices[symbol]);
                } else {
                    validated_position.average_price = trade_ngin::Decimal(1.0);
                }
            } else {
                try {
                    validated_position.average_price = position.average_price;
                } catch (const std::exception& e) {
                    ERROR("Failed to validate average_price for " + symbol + ": " + std::string(e.what()));
                    if (current_prices.find(symbol) != current_prices.end()) {
                        validated_position.average_price = trade_ngin::Decimal(current_prices[symbol]);
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
        // Note: PnL calculations have been moved above for proper sequencing
        // The variables are already calculated:
        // - total_pnl, daily_pnl, current_portfolio_value
        // - daily_return, total_return
        // - total_daily_commissions (renamed from total_commissions)

        // Calculate and deduct commissions from executions
        for (const auto& exec : daily_executions) {
            total_daily_commissions += exec.commission.as_double();
        }

        // Deduct commissions from daily PnL
        daily_realized_pnl -= total_daily_commissions;
        INFO("Total daily commissions: $" + std::to_string(total_daily_commissions));
        INFO("Daily PnL after commissions: $" + std::to_string(daily_realized_pnl));
        
        // Load previous day's aggregates (portfolio value, total pnl, total commissions)
        double previous_portfolio_value = initial_capital; // Default to initial capital
        double previous_total_pnl = 0.0;
        double previous_total_commissions = 0.0;

        try {
            auto db_ptr = std::dynamic_pointer_cast<PostgresDatabase>(db);
            if (db_ptr) {
                auto prev_agg = db_ptr->get_previous_live_aggregates("LIVE_TREND_FOLLOWING", now, "trading.live_results");
                if (prev_agg.is_ok()) {
                    std::tie(previous_portfolio_value, previous_total_pnl, previous_total_commissions) = prev_agg.value();
                    INFO("Loaded previous aggregates - portfolio_value: $" + std::to_string(previous_portfolio_value) +
                         ", total_pnl: $" + std::to_string(previous_total_pnl) +
                         ", total_commissions: $" + std::to_string(previous_total_commissions));
                } else {
                    INFO("No previous aggregates found: " + std::string(prev_agg.error()->what()));
                }
            }
        } catch (const std::exception& e) {
            INFO("Could not load previous day aggregates: " + std::string(e.what()));
        }

        // Calculate cumulative values
        double total_pnl = previous_total_pnl + daily_realized_pnl;
        double current_portfolio_value = previous_portfolio_value + daily_realized_pnl;
        double daily_pnl = daily_realized_pnl;  // Already calculated above
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
        std::cout << "Implied Margin Leverage: " << std::fixed << std::setprecision(2)
                  << margin_leverage << "x" << std::endl;
        double margin_cushion = 0.0;
        if (current_portfolio_value > 0.0) {
            margin_cushion = (current_portfolio_value - maintenance_requirement_today) / current_portfolio_value;
        } else {
            margin_cushion = -1.0;
        }

        // Warnings per thresholds
        if (total_posted_margin > current_portfolio_value) {
            WARN("Posted margin exceeds current portfolio value; check sizing and risk limits.");
        }
        if (margin_cushion < 0.20) {
            WARN("Margin cushion below 20%.");
        }
        if (margin_leverage > 4.0) {
            WARN("Implied margin leverage above 4x.");
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
            // Calculate current date for results
            auto current_date = std::chrono::system_clock::now();
            
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
            // margin_leverage and margin_cushion already computed above
            
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
                               "net_leverage, portfolio_leverage, margin_leverage, margin_cushion, max_correlation, jump_risk, risk_scale, "
                               "gross_notional, net_notional, active_positions, config, daily_return, daily_pnl, "
                               "total_commissions, daily_realized_pnl, daily_unrealized_pnl, daily_commissions, margin_posted, cash_available) "
                               "VALUES ('LIVE_TREND_FOLLOWING', '" + date_ss.str() + "', " +
                                std::to_string(total_return_annualized) + ", " + std::to_string(volatility) + ", " +
                               std::to_string(total_pnl) + ", " + std::to_string(total_unrealized_pnl) + ", " +
                               std::to_string(total_realized_pnl) + ", " + std::to_string(current_portfolio_value) + ", " +
                               std::to_string(portfolio_var) + ", " + std::to_string(gross_leverage) + ", " +
                               std::to_string(net_leverage) + ", " + std::to_string(portfolio_leverage) + ", " +
                               std::to_string(margin_leverage) + ", " + std::to_string(margin_cushion) + ", " +
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
        INFO("Saving positions to file...");
        std::string filename = "daily_positions_" + 
                              std::to_string(now_tm->tm_year + 1900) + 
                              std::string(2 - std::to_string(now_tm->tm_mon + 1).length(), '0') + 
                              std::to_string(now_tm->tm_mon + 1) + 
                              std::string(2 - std::to_string(now_tm->tm_mday).length(), '0') + 
                              std::to_string(now_tm->tm_mday) + ".csv";
        
        std::ofstream position_file(filename);
        if (position_file.is_open()) {
            position_file << "symbol,quantity,quantity_change,market_price,price_change,price_change_pct,notional,pct_of_gross_notional,pct_of_portfolio_value,unrealized_pnl,realized_pnl,forecast\n";
            for (const auto& [symbol, position] : positions) {
                double current_qty = position.quantity.as_double();
                double forecast = tf_strategy->get_forecast(symbol);
                double market_price = position.average_price.as_double(); // Default fallback
                if (current_prices.find(symbol) != current_prices.end()) {
                    market_price = current_prices[symbol];
                }
                double notional = current_qty * market_price;

                // Get previous position data for calculations
                double prev_qty = 0.0;
                double prev_price = market_price; // Default to current if no previous
                auto prev_it = previous_positions.find(symbol);
                if (prev_it != previous_positions.end()) {
                    prev_qty = prev_it->second.quantity.as_double();
                    prev_price = prev_it->second.average_price.as_double();
                }

                // Calculate position-level metrics
                double quantity_change = current_qty - prev_qty;
                double price_change = market_price - prev_price;
                double price_change_pct = (prev_price != 0.0) ? (price_change / prev_price) * 100.0 : 0.0;
                double pct_of_gross_notional = (gross_notional != 0.0) ? (std::abs(notional) / gross_notional) * 100.0 : 0.0;
                double pct_of_portfolio_value = (current_portfolio_value != 0.0) ? (std::abs(notional) / std::abs(current_portfolio_value)) * 100.0 : 0.0;

                position_file << symbol << ","
                             << current_qty << ","
                             << quantity_change << ","
                             << market_price << ","
                             << price_change << ","
                             << price_change_pct << ","
                             << notional << ","
                             << pct_of_gross_notional << ","
                             << pct_of_portfolio_value << ","
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

                // Leverage Metrics
                strategy_metrics["Gross Leverage"] = gross_notional / current_portfolio_value;
                strategy_metrics["Net Leverage"] = net_notional / current_portfolio_value;
                strategy_metrics["Portfolio Leverage (Gross)"] = gross_notional / current_portfolio_value;
                strategy_metrics["Margin Leverage"] = margin_leverage;

                // Risk & Liquidity Metrics
                strategy_metrics["Margin Cushion"] = margin_cushion * 100.0; // Convert to percentage
                strategy_metrics["Margin Posted"] = total_posted_margin;
                strategy_metrics["Cash Available"] = current_portfolio_value - total_posted_margin;

                // Generate email body with is_daily_strategy flag set to true
                std::string email_body = email_sender->generate_trading_report_body(
                    positions,
                    risk_eval.is_ok() ? std::make_optional(risk_eval.value()) : std::nullopt,
                    strategy_metrics,
                    daily_executions,
                    date_str,
                    true  // is_daily_strategy
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
