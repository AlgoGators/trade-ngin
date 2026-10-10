#include <fstream>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include "trade_ngin/backtest/backtest_coordinator.hpp"
#include "trade_ngin/backtest/transaction_cost_analysis.hpp"
#include "trade_ngin/core/config_loader.hpp"
#include "trade_ngin/portfolio/loop_config.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/resolved_sleeves.hpp"
#include "trade_ngin/core/run_id_generator.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/data/database_pooling.hpp"
#include "trade_ngin/data/listing_dates.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/strategy/short_window_log.hpp"
#include "trade_ngin/strategy/sleeve_config.hpp"
#include "trade_ngin/strategy/trend_following.hpp"

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
        logger_config.filename_prefix = "bt_portfolio_conservative";
        logger.initialize(logger_config);

        std::atomic_thread_fence(std::memory_order_seq_cst);

        if (!logger.is_initialized()) {
            std::cerr << "ERROR: Logger initialization failed" << std::endl;
            return 1;
        }

        INFO("Logger initialized successfully");

        // ========================================
        // LOAD CONFIGURATION FROM MODULAR CONFIG FILES
        // ========================================
        INFO("Loading configuration from config/portfolios/conservative...");
        auto app_config_result = ConfigLoader::load("./config", "conservative");
        if (app_config_result.is_error()) {
            ERROR("Failed to load configuration: " + std::string(app_config_result.error()->what()));
            std::cerr << "Failed to load configuration: " << app_config_result.error()->what()
                      << std::endl;
            return 1;
        }
        auto app_config = app_config_result.value();
        // The loop's keys are required on a futures book (LOOP_SPEC section 7.7): a book without
        // one of them does not run.
        if (auto loop_keys = ConfigLoader::require_loop_keys(app_config); loop_keys.is_error()) {
            ERROR("Failed to load configuration: " + std::string(loop_keys.error()->what()));
            std::cerr << "Failed to load configuration: " << loop_keys.error()->what() << std::endl;
            return 1;
        }
        INFO("Configuration loaded successfully for portfolio: " + app_config.portfolio_id);

        // ========================================
        // SETUP DATABASE CONNECTION
        // ========================================
        INFO("Initializing database connection pool...");
        std::string conn_string = app_config.database.get_connection_string();
        size_t num_connections = app_config.database.num_connections;

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

        // ========================================
        // CONFIGURE BACKTEST PARAMETERS
        // ========================================
        trade_ngin::backtest::BacktestConfig config;

        // Set portfolio_id from loaded config
        config.portfolio_id = app_config.portfolio_id;

        // Window resolution lives in ConfigLoader::resolve_backtest_window (M-12).
        // With backtest.frozen_end_date unset -- the deployed state -- it is the
        // same now()/lookback_years arithmetic this block used to do inline.
        bool frozen_window = false;
        auto now = std::chrono::system_clock::now();
        auto window = trade_ngin::ConfigLoader::resolve_backtest_window(
            app_config.backtest, now, &frozen_window);
        config.strategy_config.start_date = window.first;
        config.strategy_config.end_date = window.second;
        if (frozen_window) {
            WARN("M-12 FROZEN BACKTEST WINDOW in force: end_date pinned to "
                 + app_config.backtest.frozen_end_date
                 + ". This is a TEST configuration; a production run must not show this line.");
        }

        config.strategy_config.asset_class = trade_ngin::AssetClass::FUTURES;
        config.strategy_config.data_freq = trade_ngin::DataFrequency::DAILY;
        config.store_trade_details = app_config.backtest.store_trade_details;

        // Load symbols from database
        auto symbols_result = db->get_symbols(trade_ngin::AssetClass::FUTURES);
        auto symbols = symbols_result.value();

        if (symbols_result.is_ok()) {
            // Remove continuous contract variants (.c.0) and full-size ES
            // Using remove_if to avoid undefined behavior from erase-during-iteration
            symbols.erase(
                std::remove_if(symbols.begin(), symbols.end(),
                    [](const std::string& s) {
                        return s.find(".c.0") != std::string::npos ||
                               s == "ES.v.0";
                    }),
                symbols.end());
            // Declared vendor id relabellings (nothing happens without instrument_id_relabels).
            if (!app_config.instrument_id_relabels.empty()) {
                ListingDates::instance().set_relabels(app_config.instrument_id_relabels);
                std::string line = "INSTRUMENT_ID_RELABELS in force (not rolls):";
                for (const auto& r : app_config.instrument_id_relabels) {
                    line += " " + r.symbol + " from " + r.date + " id " + r.to + " is read as " + r.from + ";";
                }
                WARN(line);
            }
            // Listing dates (nothing happens without portfolio.json's listing_dates):
            // a window that starts before a contract's listing date also runs the contract traded
            // before it, on the rows stored under the listed contract's symbol.
            if (!app_config.listing_dates.empty()) {
                ListingDates::instance().set(app_config.listing_dates);
                ListingDates::instance().set_switch_rule(app_config.listing_switch_rule);
                InstrumentRegistry::instance().set_full_size_remap(false);
                // the ratio is the two contracts' sizes, wherever both have a metadata row
                for (const auto& c : app_config.listing_dates) {
                    auto& registry = InstrumentRegistry::instance();
                    if (!registry.has_instrument(c.before) || !registry.has_instrument(c.symbol)) continue;
                    const auto before = registry.get_instrument(c.before);
                    const auto listed = registry.get_instrument(c.symbol);
                    const std::string wrong = ListingDates::ratio_error(
                        c, before ? before->get_multiplier() : 0.0, listed ? listed->get_multiplier() : 0.0);
                    if (!wrong.empty()) throw std::runtime_error(wrong);
                }
                const auto predecessors = ListingDates::instance().predecessor_symbols(
                    symbols, config.strategy_config.start_date);
                std::string line = std::string("LISTING_DATES in force, switch rule ") +
                                   to_string(app_config.listing_switch_rule) + ":";
                for (const auto& c : app_config.listing_dates) {
                    line += " " + c.symbol + " from " + c.listed + " (before it " + c.before + ", 1 = " + std::to_string(c.ratio) + ")";
                }
                line += "; symbols added to this run:";
                for (const auto& p : predecessors) {
                    if (!InstrumentRegistry::instance().has_instrument(p)) {
                        throw std::runtime_error("listing_dates: " + p +
                                                 " has no metadata.contract_metadata row");
                    }
                    line += " " + p;
                    symbols.push_back(p);
                }
                if (predecessors.empty()) line += " none";
                WARN(line);
            }
            config.strategy_config.symbols = symbols;
        } else {
            ERROR("Failed to get symbols: " + std::string(symbols_result.error()->what()));
            throw std::runtime_error("Failed to get symbols: " +
                                     symbols_result.error()->to_string());
        }

        std::cout << "Symbols: ";
        for (const auto& symbol : config.strategy_config.symbols) {
            std::cout << symbol << " ";
        }
        std::cout << std::endl;

        // ========================================
        // APPLY CONFIG VALUES TO BACKTEST CONFIG
        // ========================================
        config.portfolio_config.initial_capital = app_config.initial_capital;
        config.portfolio_config.use_optimization = app_config.use_optimization;
        config.strategy_config.initial_capital = config.portfolio_config.initial_capital;

        std::cout << "Retrieved " << config.strategy_config.symbols.size() << " symbols"
                  << std::endl;
        std::cout << "Initial capital: $" << config.portfolio_config.initial_capital
                  << " (CONSERVATIVE)" << std::endl;

        INFO("Configuration loaded successfully. Testing " +
             std::to_string(config.strategy_config.symbols.size()) + " symbols from " +
             std::to_string(
                 std::chrono::system_clock::to_time_t(config.strategy_config.start_date)) +
             " to " +
             std::to_string(std::chrono::system_clock::to_time_t(config.strategy_config.end_date)));

        // Apply risk configuration from loaded config
        config.portfolio_config.risk_config = app_config.risk_config;
        config.portfolio_config.risk_config.capital = config.portfolio_config.initial_capital;

        // Apply optimization configuration from loaded config
        config.portfolio_config.opt_config = app_config.opt_config;
        config.portfolio_config.opt_config.capital =
            config.portfolio_config.initial_capital.as_double();

        // ========================================
        // INITIALIZE BACKTEST COORDINATOR
        // ========================================
        std::cerr << "Before BacktestCoordinator: initialized="
                  << Logger::instance().is_initialized() << std::endl;
        INFO("Initializing backtest coordinator...");

        trade_ngin::backtest::BacktestCoordinatorConfig coord_config;
        coord_config.initial_capital = static_cast<double>(config.portfolio_config.initial_capital);
        coord_config.use_optimization = config.portfolio_config.use_optimization;
        coord_config.store_trade_details = config.store_trade_details;
        coord_config.portfolio_id = config.portfolio_id;
        coord_config.csv_output_path = config.csv_output_path;

        auto coordinator = std::make_unique<trade_ngin::backtest::BacktestCoordinator>(
            db, &registry, coord_config);

        std::cerr << "After BacktestCoordinator: initialized="
                  << Logger::instance().is_initialized() << std::endl;

        // ========================================
        // SETUP PORTFOLIO CONFIGURATION
        // ========================================
        trade_ngin::PortfolioConfig portfolio_config;
        portfolio_config.total_capital = config.portfolio_config.initial_capital;
        portfolio_config.max_strategy_allocation = app_config.strategy_defaults.max_strategy_allocation;
        portfolio_config.min_strategy_allocation = app_config.strategy_defaults.min_strategy_allocation;
        portfolio_config.use_optimization = app_config.use_optimization;
        portfolio_config.covariance_history_prices = app_config.covariance_history_prices;
        portfolio_config.covariance_stale_dates = app_config.covariance_stale_dates;
        portfolio_config.risk_modules = app_config.risk_schema.portfolio;
        portfolio_config.sleeve_risk_modules = app_config.risk_schema.sleeves;
        portfolio_config.opt_config = config.portfolio_config.opt_config;
        portfolio_config.risk_config = config.portfolio_config.risk_config;
        apply_loop_config(app_config, portfolio_config);

        // ========================================
        // LOAD STRATEGIES FROM CONFIG
        // ========================================
        std::vector<std::shared_ptr<trade_ngin::StrategyInterface>> strategies;
        std::vector<std::string> strategy_names;
        std::unordered_map<std::string, double> strategy_allocations;
        std::unordered_map<std::string, nlohmann::json> strategy_configs_map;
        // Each sleeve's values as resolved here and handed to its strategy: logged once and
        // recorded with the run (core/resolved_sleeves.hpp).
        std::vector<trade_ngin::ResolvedSleeve> resolved_sleeves;

        // Use strategies from loaded config
        auto& strategies_config = app_config.strategies_config;
        if (strategies_config.empty()) {
            ERROR("No strategies found in configuration");
            return 1;
        }

        // Load default allocations from config
        for (const auto& [strategy_id, strategy_def] : strategies_config.items()) {
            if (strategy_def.contains("enabled_backtest") &&
                strategy_def["enabled_backtest"].get<bool>()) {
                double default_allocation = strategy_def.value("default_allocation", 0.5);
                strategy_allocations[strategy_id] = default_allocation;
                strategy_configs_map[strategy_id] = strategy_def;
                strategy_names.push_back(strategy_id);
            }
        }

        if (strategy_names.empty()) {
            ERROR("No enabled strategies found in configuration for backtest");
            return 1;
        }

        // Normalize allocations to sum to 1.0. If configured allocations sum
        // to <1.0 (e.g. 0.6 + 0.3, expecting 10% idle), this loop silently
        // rescales — partial deployment is not supported. Warn so operators
        // can spot a config mistake.
        double total_allocation = 0.0;
        for (const auto& [_, alloc] : strategy_allocations) {
            total_allocation += alloc;
        }
        if (total_allocation > 0.0 && std::abs(total_allocation - 1.0) > 1e-6) {
            WARN("Strategy allocations sum to " + std::to_string(total_allocation) +
                 " (not 1.0); silently rescaling. Partial-capital deployment is not "
                 "supported — adjust default_allocation values or accept full deployment.");
        }
        if (total_allocation > 0.0) {
            for (auto& [_, alloc] : strategy_allocations) {
                alloc /= total_allocation;
            }
        }

        INFO("Loading " + std::to_string(strategy_names.size()) +
             " strategies from conservative config");

        // Create a shared_ptr that doesn't own the singleton registry
        auto registry_ptr =
            std::shared_ptr<InstrumentRegistry>(&registry, [](InstrumentRegistry*) {});

        // Create base strategy configuration using loaded config values
        trade_ngin::StrategyConfig base_strategy_config;
        base_strategy_config.asset_classes = {trade_ngin::AssetClass::FUTURES};
        base_strategy_config.frequencies = {config.strategy_config.data_freq};
        base_strategy_config.max_drawdown = app_config.max_drawdown;
        // The sleeves' own leverage limit is the book's gross leverage limit L_max (risk.json's
        // max_leverage is retired on a futures book).
        base_strategy_config.max_leverage = loop_gross_leverage_limit(app_config);

        // Add position limits from config
        for (const auto& symbol : config.strategy_config.symbols) {
            base_strategy_config.position_limits[symbol] = app_config.execution.position_limit_backtest;
        }

        // Create and initialize each strategy
        for (const auto& strategy_id : strategy_names) {
            const auto& strategy_def = strategy_configs_map[strategy_id];
            std::string strategy_type = strategy_def.value("type", "");

            double allocation = strategy_allocations[strategy_id];
            base_strategy_config.capital_allocation =
                config.portfolio_config.initial_capital.as_double() * allocation;

            INFO("Creating strategy: " + strategy_id + " (type: " + strategy_type +
                 ", allocation: " + std::to_string(allocation * 100.0) + "%)");

            std::shared_ptr<trade_ngin::StrategyInterface> strategy;

            if (strategy_type == "TrendFollowingStrategy") {
                trade_ngin::TrendFollowingConfig trend_config;
                // LOOP_SPEC section 7.7: a futures sleeve's risk_target, idm and vol_lookback_short
                // are required (strategy/sleeve_config.hpp): no in-code default.
                {
                    auto sleeve_keys =
                        trade_ngin::read_required_sleeve_keys(strategy_id, strategy_def, trend_config);
                    if (sleeve_keys.is_error()) {
                        Logger::register_component("SleeveConfig");
                        ERROR(std::string(sleeve_keys.error()->what()));
                        std::cerr << sleeve_keys.error()->what() << std::endl;
                        return 1;
                    }
                }
                if (strategy_def.contains("config")) {
                    const auto& cfg = strategy_def["config"];
                    if (cfg.contains("ema_windows")) {
                        trend_config.ema_windows.clear();
                        for (const auto& window : cfg["ema_windows"]) {
                            trend_config.ema_windows.push_back(
                                {window[0].get<int>(), window[1].get<int>()});
                        }
                    }
                    trend_config.vol_lookback_long = cfg.value("vol_lookback_long", 252);
                }
                // Set FDM from strategy_defaults
                if (trend_config.fdm.empty()) {
                    trend_config.fdm = app_config.strategy_defaults.fdm;
                }

                // The equity slow rule acts on the book's first sleeve only (LOOP_SPEC section 2.5,
                // D40); a first sleeve that does not carry the rule's pairs is refused when built.
                if (strategy_id == strategy_names.front()) {
                    trend_config.equity_slow_symbols = app_config.equity_slow_rule.symbols;
                    trend_config.equity_slow_pairs = app_config.equity_slow_rule.pairs;
                    // Trading rules removed by cost (no value changes without the block).
                    trade_ngin::hand_over_trading_rule_removals(app_config, trend_config);
                    // The first sleeve's own series and its risk target feed the risk overlay
                    // (LOOP_SPEC section 4: the three risk limits are ratios to this tau).
                    portfolio_config.overlay_sleeve = strategy_id;
                    portfolio_config.overlay_tau = trend_config.risk_target;
                }
                resolved_sleeves.push_back(
                    {strategy_id, trend_config.idm, trend_config.risk_target, allocation});
                strategy = std::make_shared<trade_ngin::TrendFollowingStrategy>(
                    strategy_id, base_strategy_config, trend_config, db, registry_ptr);

            } else if (strategy_type == "TrendFollowingFastStrategy") {
                // The FAST sleeve: TrendFollowingStrategy on the fast configuration
                trade_ngin::TrendFollowingConfig trend_config =
                    trade_ngin::fast_trend_following_config();
                // LOOP_SPEC section 7.7: a futures sleeve's risk_target, idm and vol_lookback_short
                // are required (strategy/sleeve_config.hpp): no in-code default.
                {
                    auto sleeve_keys =
                        trade_ngin::read_required_sleeve_keys(strategy_id, strategy_def, trend_config);
                    if (sleeve_keys.is_error()) {
                        Logger::register_component("SleeveConfig");
                        ERROR(std::string(sleeve_keys.error()->what()));
                        std::cerr << sleeve_keys.error()->what() << std::endl;
                        return 1;
                    }
                }
                if (strategy_def.contains("config")) {
                    const auto& cfg = strategy_def["config"];
                    if (cfg.contains("ema_windows")) {
                        trend_config.ema_windows.clear();
                        for (const auto& window : cfg["ema_windows"]) {
                            trend_config.ema_windows.push_back(
                                {window[0].get<int>(), window[1].get<int>()});
                        }
                    }
                    trend_config.vol_lookback_long = cfg.value("vol_lookback_long", 252);
                }
                // Set FDM from strategy_defaults
                if (trend_config.fdm.empty()) {
                    trend_config.fdm = app_config.strategy_defaults.fdm;
                }

                // The equity slow rule acts on the book's first sleeve only (LOOP_SPEC section 2.5,
                // D40); a first sleeve that does not carry the rule's pairs is refused when built.
                if (strategy_id == strategy_names.front()) {
                    trend_config.equity_slow_symbols = app_config.equity_slow_rule.symbols;
                    trend_config.equity_slow_pairs = app_config.equity_slow_rule.pairs;
                    // The first sleeve's own series and its risk target feed the risk overlay
                    // (LOOP_SPEC section 4: the three risk limits are ratios to this tau).
                    portfolio_config.overlay_sleeve = strategy_id;
                    portfolio_config.overlay_tau = trend_config.risk_target;
                }
                resolved_sleeves.push_back(
                    {strategy_id, trend_config.idm, trend_config.risk_target, allocation});
                strategy = std::make_shared<trade_ngin::TrendFollowingStrategy>(
                    strategy_id, base_strategy_config, trend_config, db, registry_ptr);

            } else {
                ERROR("Unknown strategy type: " + strategy_type + " for strategy: " + strategy_id);
                return 1;
            }

            // Initialize strategy
            auto init_result = strategy->initialize();
            if (init_result.is_error()) {
                ERROR("Failed to initialize strategy " + strategy_id + ": " +
                      init_result.error()->what());
                return 1;
            }

            // Start strategy
            auto start_result = strategy->start();
            if (start_result.is_error()) {
                ERROR("Failed to start strategy " + strategy_id + ": " +
                      start_result.error()->what());
                return 1;
            }

            strategies.push_back(strategy);
            INFO("Successfully initialized and started strategy: " + strategy_id);
        }

        // ========================================
        // CREATE PORTFOLIO AND RUN BACKTEST
        // ========================================
        INFO(trade_ngin::resolved_sleeves_log_line(app_config.portfolio_id, resolved_sleeves));
        INFO("Creating portfolio manager with " + std::to_string(strategies.size()) +
             " strategies...");
        auto portfolio = std::make_shared<trade_ngin::PortfolioManager>(portfolio_config);

        for (size_t i = 0; i < strategies.size(); ++i) {
            const auto& strategy = strategies[i];
            const std::string& strategy_id = strategy_names[i];
            double allocation = strategy_allocations[strategy_id];

            auto add_result = portfolio->add_strategy(strategy, allocation,
                                                      config.portfolio_config.use_optimization);

            if (add_result.is_error()) {
                ERROR("Failed to add strategy " + strategy_id +
                      " to portfolio: " + add_result.error()->what());
                return 1;
            }

            INFO("Added strategy " + strategy_id + " with allocation " +
                 std::to_string(allocation * 100.0) + "%");
        }

        // Run the backtest
        INFO("Running conservative portfolio backtest for time period: " +
             std::to_string(
                 std::chrono::system_clock::to_time_t(config.strategy_config.start_date)) +
             " to " +
             std::to_string(std::chrono::system_clock::to_time_t(config.strategy_config.end_date)));

        auto result = coordinator->run_portfolio(
            portfolio, config.strategy_config.symbols, config.strategy_config.start_date,
            config.strategy_config.end_date, config.strategy_config.asset_class,
            config.strategy_config.data_freq);

        if (result.is_error()) {
            std::cerr << "Backtest failed: " << result.error()->what() << std::endl;
            std::cerr << "Error code: " << static_cast<int>(result.error()->code()) << std::endl;
            return 1;
        }

        INFO("Backtest completed successfully");
        INFO(trade_ngin::estimator_short_window_line(strategies));

        // Analyze and display results
        const auto& backtest_results = result.value();

        INFO("Analyzing performance metrics...");

        std::cout << "======= Conservative Portfolio Backtest Results =======" << std::endl;
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
        std::cout << "Transaction Costs: " << backtest_results.transaction_costs << std::endl;
        std::cout << "Roll Costs (upper bound): " << backtest_results.roll_costs << std::endl;
        std::cout << "Roll Fills: " << backtest_results.total_roll_fills << std::endl;

        // Save portfolio results to database
        INFO("Saving conservative portfolio backtest results to database...");
        try {
            nlohmann::json portfolio_config_json = portfolio_config.to_json();
            portfolio_config_json["strategy_allocations"] = strategy_allocations;
            portfolio_config_json["strategy_names"] = strategy_names;
            // T-8D-2 R53: each sleeve's resolved idm and risk_target (its allocation is
            // strategy_allocations above), the window rule, the warm-up and the sessions a year
            // the statistics are annualised with.
            portfolio_config_json[trade_ngin::kSleevesKey] =
                trade_ngin::resolved_sleeves_json(resolved_sleeves, false);
            trade_ngin::add_backtest_run_keys(
                portfolio_config_json, app_config.backtest.lookback_years,
                app_config.backtest.frozen_end_date, backtest_results.warmup_days,
                app_config.statistics.futures_sessions_per_year);
            if (!app_config.instrument_id_relabels.empty()) {
                auto relabels = nlohmann::json::array();
                for (const auto& r : app_config.instrument_id_relabels) {
                    relabels.push_back({{"symbol", r.symbol}, {"date", r.date}, {"from", r.from}, {"to", r.to}});
                }
                portfolio_config_json["instrument_id_relabels"] = relabels;
            }
            if (!app_config.trading_rule_removals.empty()) {
                portfolio_config_json["trading_rule_removals"] = app_config.trading_rule_removals;
            }
            if (!app_config.listing_dates.empty()) {
                auto contracts = nlohmann::json::array();
                for (const auto& c : app_config.listing_dates) {
                    contracts.push_back({{"symbol", c.symbol}, {"listed", c.listed}, {"before", c.before}, {"ratio", c.ratio}});
                }
                portfolio_config_json["listing_dates"] = {
                    {"switch_rule", to_string(app_config.listing_switch_rule)}, {"contracts", contracts}};
            }

            auto save_result = coordinator->save_portfolio_results_to_db(
                backtest_results, strategy_names, strategy_allocations, portfolio,
                portfolio_config_json);

            if (save_result.is_error()) {
                std::cerr << "Failed to save portfolio backtest results to database: "
                          << save_result.error()->what() << std::endl;
                ERROR("Failed to save portfolio backtest results to database: " +
                      std::string(save_result.error()->what()));
            } else {
                INFO("Successfully saved conservative portfolio backtest results to database");
            }
        } catch (const std::exception& e) {
            std::cerr << "Exception during database save: " << e.what() << std::endl;
            ERROR("Exception during database save: " + std::string(e.what()));
        }

        // Cleanup
        INFO("Cleaning up backtest coordinator...");
        coordinator.reset();

        INFO("Conservative portfolio backtest application completed successfully");

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