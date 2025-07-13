#include <signal.h>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <thread>
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/data/credential_store.hpp"
#include "trade_ngin/data/database_pooling.hpp"
#include "trade_ngin/data/market_data_bus.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/execution/execution_engine.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/order/order_manager.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/risk/risk_manager.hpp"
#include "trade_ngin/strategy/trend_following.hpp"

using namespace trade_ngin;

// Global flag for graceful shutdown
std::atomic<bool> shutdown_requested{false};

// Signal handler for graceful shutdown
void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        INFO("Received shutdown signal, initiating graceful shutdown...");
        shutdown_requested = true;
    }
}

int main() {
    try {
        // Set up signal handlers for graceful shutdown
        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);

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
        INFO("Starting Live Trend Following Trading System");

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

        // Initialize connection pool with sufficient connections for live trading
        size_t num_connections = 10;  // More connections for live trading
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

        // Get trading symbols
        auto symbols_result = db->get_symbols(trade_ngin::AssetClass::FUTURES);
        auto symbols = symbols_result.value();

        if (symbols_result.is_ok()) {
            // Filter out unwanted symbols for live trading
            for (const auto& symbol : symbols) {
                if (symbol.find(".c.0") != std::string::npos ||
                    symbol.find("MES.c.0") != std::string::npos ||
                    symbol.find("ES.v.0") != std::string::npos) {
                    symbols.erase(std::remove(symbols.begin(), symbols.end(), symbol),
                                  symbols.end());
                }
            }
        } else {
            ERROR("Failed to get symbols: " + std::string(symbols_result.error()->what()));
            throw std::runtime_error("Failed to get symbols: " +
                                     symbols_result.error()->to_string());
        }

        std::cout << "Live Trading Symbols: ";
        for (const auto& symbol : symbols) {
            std::cout << symbol << " ";
        }
        std::cout << std::endl;

        // Live trading configuration
        double initial_capital = 500000.0;  // $500k
        double commission_rate = 0.0005;    // 5 basis points
        double slippage_model = 1.0;        // 1 basis point

        INFO("Live Trading Configuration:");
        INFO("Initial Capital: $" + std::to_string(initial_capital));
        INFO("Commission Rate: " + std::to_string(commission_rate * 100) + " bps");
        INFO("Slippage Model: " + std::to_string(slippage_model) + " bps");
        INFO("Number of Symbols: " + std::to_string(symbols.size()));

        // Initialize Order Manager for live trading
        INFO("Initializing Order Manager...");
        OrderManagerConfig order_config;
        order_config.max_orders_per_second = 50;  // Conservative for live trading
        order_config.max_pending_orders = 500;
        order_config.max_order_size = 10000.0;  // Smaller max order size for safety
        order_config.max_notional_value = 500000.0;
        order_config.simulate_fills = false;  // Real fills for live trading
        order_config.retry_attempts = 3;
        order_config.retry_delay_ms = 200.0;  // Longer delay for live trading

        auto order_manager = std::make_shared<OrderManager>(order_config, "LIVE_ORDER_MANAGER");
        auto order_init_result = order_manager->initialize();
        if (order_init_result.is_error()) {
            std::cerr << "Failed to initialize order manager: " << order_init_result.error()->what()
                      << std::endl;
            return 1;
        }
        INFO("Order Manager initialized successfully");

        // Initialize Execution Engine
        INFO("Initializing Execution Engine...");
        auto execution_engine = std::make_shared<ExecutionEngine>(order_manager);
        auto exec_init_result = execution_engine->initialize();
        if (exec_init_result.is_error()) {
            std::cerr << "Failed to initialize execution engine: "
                      << exec_init_result.error()->what() << std::endl;
            return 1;
        }
        INFO("Execution Engine initialized successfully");

        // Initialize Risk Manager
        INFO("Initializing Risk Manager...");
        RiskConfig risk_config;
        risk_config.capital = Decimal(initial_capital);
        risk_config.confidence_level = 0.99;
        risk_config.lookback_period = 252;
        risk_config.var_limit = 0.15;
        risk_config.jump_risk_limit = 0.10;
        risk_config.max_correlation = 0.7;
        risk_config.max_gross_leverage = 4.0;
        risk_config.max_net_leverage = 2.0;

        auto risk_manager = std::make_shared<RiskManager>(risk_config);
        INFO("Risk Manager initialized successfully");

        // Setup portfolio configuration for live trading
        INFO("Setting up Portfolio Manager...");
        trade_ngin::PortfolioConfig portfolio_config;
        portfolio_config.total_capital = initial_capital;
        portfolio_config.reserve_capital = initial_capital * 0.15;  // 15% reserve for live trading
        portfolio_config.max_strategy_allocation = 1.0;
        portfolio_config.min_strategy_allocation = 0.1;
        portfolio_config.use_optimization = true;
        portfolio_config.use_risk_management = true;

        // Configure portfolio optimization
        portfolio_config.opt_config.tau = 1.0;
        portfolio_config.opt_config.capital = initial_capital;
        portfolio_config.opt_config.cost_penalty_scalar = 50.0;
        portfolio_config.opt_config.asymmetric_risk_buffer = 0.1;
        portfolio_config.opt_config.max_iterations = 100;
        portfolio_config.opt_config.convergence_threshold = 1e-6;
        portfolio_config.opt_config.use_buffering = true;
        portfolio_config.opt_config.buffer_size_factor = 0.05;

        // Configure portfolio risk management
        portfolio_config.risk_config = risk_config;

        auto portfolio = std::make_shared<trade_ngin::PortfolioManager>(portfolio_config);
        INFO("Portfolio Manager configured successfully");

        // Create trend following strategy configuration for live trading
        INFO("Configuring Trend Following Strategy for live trading...");
        trade_ngin::StrategyConfig tf_config;
        tf_config.capital_allocation = initial_capital * 0.85;  // Use 85% of capital
        tf_config.asset_classes = {trade_ngin::AssetClass::FUTURES};
        tf_config.frequencies = {trade_ngin::DataFrequency::DAILY};
        tf_config.max_drawdown = 0.35;  // More conservative for live trading
        tf_config.max_leverage = 3.0;   // Lower leverage for live trading
        tf_config.save_positions = true;
        tf_config.save_signals = true;
        tf_config.save_executions = true;

        // Add position limits and contract sizes for live trading
        for (const auto& symbol : symbols) {
            tf_config.position_limits[symbol] = 500.0;  // More conservative limits
            tf_config.costs[symbol] = commission_rate;
        }

        // Configure trend following parameters for live trading
        trade_ngin::TrendFollowingConfig trend_config;
        trend_config.weight = 0.025;      // 2.5% weight per symbol (more conservative)
        trend_config.risk_target = 0.15;  // Target 15% annualized risk (more conservative)
        trend_config.idm = 2.5;           // Instrument diversification multiplier
        trend_config.use_position_buffering = true;  // Use buffering for live trading
        trend_config.ema_windows = {{2, 8}, {4, 16}, {8, 32}, {16, 64}, {32, 128}, {64, 256}};
        trend_config.vol_lookback_short = 32;
        trend_config.vol_lookback_long = 252;
        trend_config.fdm = {{1, 1.0}, {2, 1.03}, {3, 1.08}, {4, 1.13}, {5, 1.19}, {6, 1.26}};

        // Create and initialize the trend following strategy
        INFO("Initializing Trend Following Strategy...");
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

        // Add strategy to portfolio
        INFO("Adding strategy to portfolio...");
        auto add_result =
            portfolio->add_strategy(tf_strategy, 1.0, portfolio_config.use_optimization,
                                    portfolio_config.use_risk_management);
        if (add_result.is_error()) {
            std::cerr << "Failed to add strategy to portfolio: " << add_result.error()->what()
                      << std::endl;
            return 1;
        }
        INFO("Strategy added to portfolio successfully");

        // Set up market data bus subscription for live data
        INFO("Setting up market data subscriptions...");
        auto& market_data_bus = MarketDataBus::instance();

        // Subscribe to market data events for all symbols
        SubscriberInfo strategy_subscriber;
        strategy_subscriber.id = "LIVE_TREND_STRATEGY";
        strategy_subscriber.event_types = {MarketDataEventType::TRADE, MarketDataEventType::QUOTE,
                                           MarketDataEventType::BAR};
        strategy_subscriber.symbols = symbols;
        strategy_subscriber.callback = [&tf_strategy](const MarketDataEvent& event) {
            // Convert market data event to strategy data format
            if (event.type == MarketDataEventType::BAR) {
                // Process bar data
                Bar bar;
                bar.symbol = event.symbol;
                bar.timestamp = event.timestamp;

                auto open_it = event.numeric_fields.find("open");
                auto high_it = event.numeric_fields.find("high");
                auto low_it = event.numeric_fields.find("low");
                auto close_it = event.numeric_fields.find("close");
                auto volume_it = event.numeric_fields.find("volume");

                if (open_it != event.numeric_fields.end())
                    bar.open = open_it->second;
                if (high_it != event.numeric_fields.end())
                    bar.high = high_it->second;
                if (low_it != event.numeric_fields.end())
                    bar.low = low_it->second;
                if (close_it != event.numeric_fields.end())
                    bar.close = close_it->second;
                if (volume_it != event.numeric_fields.end())
                    bar.volume = volume_it->second;

                std::vector<Bar> bars{bar};
                auto result = tf_strategy->on_data(bars);
                if (result.is_error()) {
                    ERROR("Failed to process market data for " + event.symbol + ": " +
                          result.error()->what());
                }
            }
        };

        auto subscribe_result = market_data_bus.subscribe(strategy_subscriber);
        if (subscribe_result.is_error()) {
            std::cerr << "Failed to subscribe to market data: " << subscribe_result.error()->what()
                      << std::endl;
            return 1;
        }
        INFO("Market data subscription successful");

        // Subscribe to order updates
        SubscriberInfo order_subscriber;
        order_subscriber.id = "LIVE_ORDER_UPDATES";
        order_subscriber.event_types = {MarketDataEventType::ORDER_UPDATE};
        order_subscriber.symbols = symbols;
        order_subscriber.callback = [&tf_strategy](const MarketDataEvent& event) {
            // Process order updates
            auto order_id_it = event.string_fields.find("order_id");
            auto status_it = event.string_fields.find("status");
            auto filled_qty_it = event.numeric_fields.find("filled_quantity");
            auto avg_price_it = event.numeric_fields.find("average_price");

            if (order_id_it != event.string_fields.end() &&
                status_it != event.string_fields.end()) {
                ExecutionReport report;
                report.order_id = order_id_it->second;
                // Set basic execution report fields
                if (filled_qty_it != event.numeric_fields.end()) {
                    report.filled_quantity = Quantity(filled_qty_it->second);
                }
                if (avg_price_it != event.numeric_fields.end()) {
                    report.fill_price = Price(avg_price_it->second);
                }

                auto result = tf_strategy->on_execution(report);
                if (result.is_error()) {
                    ERROR("Failed to process execution report: " +
                          std::string(result.error()->what()));
                }
            }
        };

        auto order_subscribe_result = market_data_bus.subscribe(order_subscriber);
        if (order_subscribe_result.is_error()) {
            std::cerr << "Failed to subscribe to order updates: "
                      << order_subscribe_result.error()->what() << std::endl;
            return 1;
        }
        INFO("Order update subscription successful");

        // Main live trading loop
        INFO("Entering main live trading loop...");
        std::cout << "Live trading system is now running. Press Ctrl+C to stop." << std::endl;

        auto last_risk_check = std::chrono::steady_clock::now();
        auto last_portfolio_update = std::chrono::steady_clock::now();
        auto last_metrics_log = std::chrono::steady_clock::now();

        while (!shutdown_requested) {
            auto now = std::chrono::steady_clock::now();

            // Periodic risk checks (every 5 minutes)
            if (now - last_risk_check > std::chrono::minutes(5)) {
                INFO("Performing periodic risk check...");
                // Process current positions for risk check
                auto current_positions = tf_strategy->get_positions();
                auto market_data =
                    risk_manager->create_market_data(std::vector<Bar>{});  // Empty for now
                auto risk_result = risk_manager->process_positions(current_positions, market_data);
                if (risk_result.is_error()) {
                    ERROR("Risk check failed: " + std::string(risk_result.error()->what()));
                }
                last_risk_check = now;
            }

            // Periodic portfolio updates (every 15 minutes)
            if (now - last_portfolio_update > std::chrono::minutes(15)) {
                INFO("Performing periodic portfolio update...");
                // Update portfolio allocations and risk metrics
                // Get portfolio positions and calculate value
                auto portfolio_positions = portfolio->get_portfolio_positions();
                double total_value = 0.0;
                double total_pnl = 0.0;

                // Calculate total value and P&L (simplified)
                for (const auto& [symbol, position] : portfolio_positions) {
                    total_value +=
                        position.quantity.as_double() * position.average_price.as_double();
                    total_pnl +=
                        position.realized_pnl.as_double() + position.unrealized_pnl.as_double();
                }

                INFO("Portfolio State - Total Value: $" + std::to_string(total_value) + ", P&L: $" +
                     std::to_string(total_pnl));
                last_portfolio_update = now;
            }

            // Periodic metrics logging (every hour)
            if (now - last_metrics_log > std::chrono::hours(1)) {
                INFO("Logging periodic metrics...");
                auto strategy_metrics = tf_strategy->get_metrics();
                INFO(
                    "Strategy Metrics - Total P&L: $" + std::to_string(strategy_metrics.total_pnl) +
                    ", Sharpe Ratio: " + std::to_string(strategy_metrics.sharpe_ratio) +
                    ", Max Drawdown: " + std::to_string(strategy_metrics.max_drawdown * 100) + "%");
                last_metrics_log = now;
            }

            // Sleep for a short interval to prevent excessive CPU usage
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // Graceful shutdown
        INFO("Initiating graceful shutdown...");

        // Stop the strategy
        INFO("Stopping strategy...");
        auto stop_result = tf_strategy->stop();
        if (stop_result.is_error()) {
            ERROR("Failed to stop strategy: " + std::string(stop_result.error()->what()));
        } else {
            INFO("Strategy stopped successfully");
        }

        // Unsubscribe from market data
        INFO("Unsubscribing from market data...");
        market_data_bus.unsubscribe("LIVE_TREND_STRATEGY");
        market_data_bus.unsubscribe("LIVE_ORDER_UPDATES");
        INFO("Market data unsubscription complete");

        // Final portfolio state
        auto final_portfolio_positions = portfolio->get_portfolio_positions();
        auto final_strategy_metrics = tf_strategy->get_metrics();

        double final_total_value = 0.0;
        double final_total_pnl = 0.0;

        for (const auto& [symbol, position] : final_portfolio_positions) {
            final_total_value += position.quantity.as_double() * position.average_price.as_double();
            final_total_pnl +=
                position.realized_pnl.as_double() + position.unrealized_pnl.as_double();
        }

        std::cout << "\n======= Live Trading Session Summary =======" << std::endl;
        std::cout << "Final Portfolio Value: $" << final_total_value << std::endl;
        std::cout << "Total P&L: $" << final_total_pnl << std::endl;
        std::cout << "Strategy P&L: $" << final_strategy_metrics.total_pnl << std::endl;
        std::cout << "Strategy Sharpe Ratio: " << final_strategy_metrics.sharpe_ratio << std::endl;
        std::cout << "Strategy Max Drawdown: " << (final_strategy_metrics.max_drawdown * 100.0)
                  << "%" << std::endl;
        std::cout << "Total Trades: " << final_strategy_metrics.total_trades << std::endl;

        INFO("Live trading session completed successfully");

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
