#include "trade_ngin/api/backtest_api.hpp"

#include <iostream>
#include <nlohmann/json.hpp>
#include "trade_ngin/backtest/backtest_coordinator.hpp"
#include "trade_ngin/core/config_loader.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/resolved_sleeves.hpp"
#include "trade_ngin/data/listing_dates.hpp"
#include "trade_ngin/portfolio/loop_config.hpp"
#include "trade_ngin/strategy/sleeve_config.hpp"
#include "trade_ngin/strategy/trend_following.hpp"
#include "trade_ngin/data/database_pooling.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"

namespace trade_ngin::api {

namespace {

/// Initialize file logging for a run.
void init_logging(const std::string& portfolio_name) {
    Logger::reset_for_tests();
    auto& logger = Logger::instance();
    LoggerConfig logger_config;
    logger_config.min_level = LogLevel::DEBUG;
    logger_config.destination = LogDestination::BOTH;
    logger_config.log_directory = "logs";
    logger_config.filename_prefix = "bt_portfolio_" + portfolio_name;
    logger.initialize(logger_config);
}

}  // namespace

void BacktestRunner::initialize(std::string portfolio_name) {
    portfolio_name_ = std::move(portfolio_name);
    run_config_.reset();

    // TODO move database and other initialization here?

    init_logging(portfolio_name_);
}

void BacktestRunner::initialize_with_config(std::string portfolio_name, BacktestRunConfig config) {
    portfolio_name_ = std::move(portfolio_name);
    run_config_ = std::move(config);

    init_logging(portfolio_name_);
}

void BacktestRunner::set_data_source(std::shared_ptr<MarketDataSource> source) {
    if (!run_config_) {
        run_config_ = BacktestRunConfig{};
    }
    run_config_->data_source = std::move(source);
}

Result<backtest::BacktestResults> BacktestRunner::run_backtest() {
    // Argument-driven path: skip ConfigLoader and the database entirely.
    if (run_config_ && run_config_->data_source) {
        return run_backtest_from_config();
    }

    try {
        // Reset all singletons to ensure clean state between runs
        StateManager::reset_instance();
        auto& logger = Logger::instance();

        std::atomic_thread_fence(std::memory_order_seq_cst);

        if (!logger.is_initialized()) {
            std::cerr << "ERROR: Logger initialization failed" << std::endl;
            return make_error<backtest::BacktestResults>(
                ErrorCode::NOT_INITIALIZED, "Logger initialization failed", "BacktestAPI");
        }

        INFO("Logger initialized successfully");

        // ========================================
        // LOAD CONFIGURATION FROM MODULAR CONFIG FILES
        // ========================================
        INFO("Loading configuration from config/portfolios/" + portfolio_name_ + "...");
        auto app_config_result = ConfigLoader::load("config", portfolio_name_);
        if (app_config_result.is_error()) {
            ERROR("Failed to load configuration: " +
                  std::string(app_config_result.error()->what()));
            std::cerr << "Failed to load configuration: " << app_config_result.error()->what()
                      << std::endl;
            return make_error<backtest::BacktestResults>(
                app_config_result.error()->code(),
                "Failed to load configuration: " + std::string(app_config_result.error()->what()),
                "BacktestAPI");
        }
        auto app_config = app_config_result.value();
        // The loop's keys are required on a futures book (LOOP_SPEC section 7.7): a book
        // without one of them does not run.
        if (auto loop_keys = ConfigLoader::require_loop_keys(app_config);
            loop_keys.is_error()) {
            ERROR("Failed to load configuration: " + std::string(loop_keys.error()->what()));
            return make_error<backtest::BacktestResults>(
                loop_keys.error()->code(),
                "Failed to load configuration: " + std::string(loop_keys.error()->what()),
                "BacktestAPI");
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
            return make_error<backtest::BacktestResults>(
                ErrorCode::DATABASE_ERROR,
                "Failed to initialize connection pool: " + std::string(pool_result.error()->what()),
                "BacktestAPI");
        }
        INFO("Database connection pool initialized with " + std::to_string(num_connections) +
             " connections");

        // Get a database connection from the pool
        auto db_guard = DatabasePool::instance().acquire_connection();
        auto db = db_guard.get();

        if (!db || !db->is_connected()) {
            std::cerr << "Failed to acquire database connection from pool" << std::endl;
            return make_error<backtest::BacktestResults>(
                ErrorCode::DATABASE_ERROR, "Failed to acquire database connection from pool",
                "BacktestAPI");
        }
        INFO("Successfully acquired database connection from pool");

        // Initialize instrument registry
        INFO("Initializing instrument registry...");
        auto& registry = InstrumentRegistry::instance();

        auto instrument_registry_init_result = registry.initialize(db);
        if (instrument_registry_init_result.is_error()) {
            std::cerr << "Failed to initialize instrument registry: "
                      << instrument_registry_init_result.error()->what() << std::endl;
            return make_error<backtest::BacktestResults>(
                ErrorCode::DATABASE_ERROR,
                "Failed to initialize instrument registry: " +
                    std::string(instrument_registry_init_result.error()->what()),
                "BacktestAPI");
        }

        // Load instruments
        auto load_result = registry.load_instruments();
        if (load_result.is_error() || registry.get_all_instruments().empty()) {
            std::cerr << "Failed to load instruments: " << load_result.error()->what() << std::endl;
            ERROR("Failed to load instruments: " + std::string(load_result.error()->what()));
            return make_error<backtest::BacktestResults>(
                ErrorCode::DATABASE_ERROR,
                "Failed to load instruments: " + std::string(load_result.error()->what()),
                "BacktestAPI");
        } else {
            INFO("Successfully loaded instruments from database");
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

        // Window resolution lives in ConfigLoader::resolve_backtest_window (M-12), as the
        // engine's own backtest runners use it.
        bool frozen_window = false;
        auto now = std::chrono::system_clock::now();
        auto window = ConfigLoader::resolve_backtest_window(app_config.backtest, now,
                                                            &frozen_window);
        config.strategy_config.start_date = window.first;
        config.strategy_config.end_date = window.second;
        if (frozen_window) {
            WARN("M-12 FROZEN BACKTEST WINDOW in force: end_date pinned to " +
                 app_config.backtest.frozen_end_date +
                 ". This is a TEST configuration; a production run must not show this line.");
        }

        config.strategy_config.asset_class = app_config.asset_class;
        config.strategy_config.data_freq = trade_ngin::DataFrequency::DAILY;
        config.store_trade_details = app_config.backtest.store_trade_details;

        // Load symbols from database
        auto symbols_result = db->get_symbols(config.strategy_config.asset_class);
        auto symbols = symbols_result.value();

        if (symbols_result.is_ok()) {
            // Remove continuous contract variants (.c.0) and full-size ES
            // Using remove_if to avoid undefined behavior from erase-during-iteration
            symbols.erase(std::remove_if(symbols.begin(), symbols.end(),
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
                    line += " " + r.symbol + " from " + r.date + " id " + r.to + " is read as " +
                            r.from + ";";
                }
                WARN(line);
            }
            // Listing dates (nothing happens without portfolio.json's listing_dates): a window
            // that starts before a contract's listing date also runs the contract traded
            // before it, on the rows stored under the listed contract's symbol.
            if (!app_config.listing_dates.empty()) {
                ListingDates::instance().set(app_config.listing_dates);
                ListingDates::instance().set_switch_rule(app_config.listing_switch_rule);
                InstrumentRegistry::instance().set_full_size_remap(false);
                for (const auto& c : app_config.listing_dates) {
                    if (!registry.has_instrument(c.before) || !registry.has_instrument(c.symbol))
                        continue;
                    const auto before = registry.get_instrument(c.before);
                    const auto listed = registry.get_instrument(c.symbol);
                    const std::string wrong = ListingDates::ratio_error(
                        c, before ? before->get_multiplier() : 0.0,
                        listed ? listed->get_multiplier() : 0.0);
                    if (!wrong.empty())
                        return make_error<backtest::BacktestResults>(
                            ErrorCode::INVALID_DATA, wrong, "BacktestAPI");
                }
                const auto predecessors = ListingDates::instance().predecessor_symbols(
                    symbols, config.strategy_config.start_date);
                std::string line = std::string("LISTING_DATES in force, switch rule ") +
                                   to_string(app_config.listing_switch_rule) + ":";
                for (const auto& c : app_config.listing_dates) {
                    line += " " + c.symbol + " from " + c.listed + " (before it " + c.before +
                            ", 1 = " + std::to_string(c.ratio) + ")";
                }
                line += "; symbols added to this run:";
                for (const auto& p : predecessors) {
                    if (!registry.has_instrument(p)) {
                        return make_error<backtest::BacktestResults>(
                            ErrorCode::INVALID_DATA,
                            "listing_dates: " + p + " has no metadata.contract_metadata row",
                            "BacktestAPI");
                    }
                    line += " " + p;
                    symbols.push_back(p);
                }
                if (predecessors.empty())
                    line += " none";
                WARN(line);
            }
            config.strategy_config.symbols = symbols;
        } else {
            ERROR("Failed to get symbols: " + std::string(symbols_result.error()->what()));
            throw std::runtime_error("Failed to get symbols: " +
                                     symbols_result.error()->to_string());
        }

        for (const auto& symbol : config.strategy_config.symbols) {
            INFO("Loaded symbol: " + symbol);
        }

        // ========================================
        // APPLY CONFIG VALUES TO BACKTEST CONFIG
        // ========================================
        config.portfolio_config.initial_capital = app_config.initial_capital;
        config.portfolio_config.use_optimization = app_config.use_optimization;
        config.strategy_config.initial_capital = config.portfolio_config.initial_capital;

        std::cout << "Retrieved " << config.strategy_config.symbols.size() << " symbols"
                  << std::endl;
        std::cout << "Initial capital: $" << config.portfolio_config.initial_capital << std::endl;

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
        // T-8D R2, T-8D-2 R76: K of the futures statistics grid, the one factor the run's
        // annualised statistics use.
        coord_config.statistics_sessions_per_year = app_config.statistics.futures_sessions_per_year;

        auto coordinator = std::make_unique<trade_ngin::backtest::BacktestCoordinator>(
            db, &registry, coord_config);

        std::cerr << "After BacktestCoordinator: initialized="
                  << Logger::instance().is_initialized() << std::endl;

        // ========================================
        // SETUP PORTFOLIO CONFIGURATION
        // ========================================
        trade_ngin::PortfolioConfig portfolio_config;
        portfolio_config.total_capital = config.portfolio_config.initial_capital;
        portfolio_config.max_strategy_allocation =
            app_config.strategy_defaults.max_strategy_allocation;
        portfolio_config.min_strategy_allocation =
            app_config.strategy_defaults.min_strategy_allocation;
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
        std::vector<std::shared_ptr<trade_ngin::BaseStrategy>> strategies;
        std::vector<std::string> strategy_names;
        std::unordered_map<std::string, double> strategy_allocations;
        std::unordered_map<std::string, nlohmann::json> strategy_configs_map;
        // Each sleeve's values as resolved here, logged once and recorded with the run.
        std::vector<ResolvedSleeve> resolved_sleeves;

        // Use strategies from loaded config
        auto& strategies_config = app_config.strategies_config;
        if (strategies_config.empty()) {
            ERROR("No strategies found in configuration");
            return make_error<backtest::BacktestResults>(
                ErrorCode::INVALID_DATA, "No strategies found in configuration", "BacktestAPI");
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
            return make_error<backtest::BacktestResults>(
                ErrorCode::INVALID_DATA,
                "No enabled strategies found in configuration for backtest", "BacktestAPI");
        }

        // Normalize allocations to sum to 1.0. A sum other than 1.0 is rescaled; partial
        // deployment is not supported, so say so.
        double total_allocation = 0.0;
        for (const auto& [_, alloc] : strategy_allocations) {
            total_allocation += alloc;
        }
        if (total_allocation > 0.0 && std::abs(total_allocation - 1.0) > 1e-6) {
            WARN("Strategy allocations sum to " + std::to_string(total_allocation) +
                 " (not 1.0); silently rescaling.");
        }
        if (total_allocation > 0.0) {
            for (auto& [_, alloc] : strategy_allocations) {
                alloc /= total_allocation;
            }
        }

        INFO("Loading " + std::to_string(strategy_names.size()) + " strategies from config");

        // Create a shared_ptr that doesn't own the singleton registry
        auto registry_ptr =
            std::shared_ptr<InstrumentRegistry>(&registry, [](InstrumentRegistry*) {});

        // Create base strategy configuration using loaded config values
        trade_ngin::StrategyConfig base_strategy_config;
        base_strategy_config.asset_classes = {config.strategy_config.asset_class};
        base_strategy_config.frequencies = {config.strategy_config.data_freq};
        base_strategy_config.max_drawdown = app_config.max_drawdown;
        // The sleeves' own leverage limit is the book's gross leverage limit L_max.
        base_strategy_config.max_leverage = loop_gross_leverage_limit(app_config);

        // Add position limits from config
        for (const auto& symbol : config.strategy_config.symbols) {
            base_strategy_config.position_limits[symbol] =
                app_config.execution.position_limit_backtest;
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

            if (registered_strategies_.find(strategy_id) == registered_strategies_.end()) {
                ERROR("Strategy " + strategy_id + " is not registered");
                return make_error<backtest::BacktestResults>(
                    ErrorCode::INVALID_DATA, "Strategy " + strategy_id + " is not registered",
                    "BacktestAPI");
            }

            // TODO remove this - keeping it as an example
            // This should be outside and done by the user - for future reference
            // bt.register_strategy(
            //     "TrendFollowingStrategy",  // strategy ID
            //     [](const StrategyContext& ctx, const nlohmann::json& cfg) ->
            //     std::shared_ptr<BaseStrategy> {
            //
            //         // Build the trend following config from JSON or defaults
            //         TrendFollowingConfig trend_config;
            //         trend_config.vol_lookback_short = cfg.value("vol_lookback_short", 22);
            //         trend_config.vol_lookback_long =
            //             cfg.value("vol_lookback_long", trend_config.vol_lookback_short * 4);
            //
            //         // Construct the strategy with context and config
            //         return std::make_shared<TrendFollowingStrategy>(
            //             "TF1",                 // strategy instance ID
            //             ctx.portfolio_config,   // base StrategyConfig (capital, symbols, etc.)
            //             trend_config,           // TrendFollowingConfig
            //             ctx.db,                 // database connection
            //             ctx.registry            // instrument registry
            //         );
            //     });

            // LOOP_SPEC section 7.7: a futures sleeve's risk_target, idm and vol_lookback_short
            // are required (strategy/sleeve_config.hpp): no in-code default. The first sleeve's
            // risk target feeds the risk overlay (section 4).
            TrendFollowingConfig sleeve_keys;
            if (auto read = read_required_sleeve_keys(strategy_id, strategy_def, sleeve_keys);
                read.is_error()) {
                ERROR(std::string(read.error()->what()));
                return make_error<backtest::BacktestResults>(
                    read.error()->code(), std::string(read.error()->what()), "BacktestAPI");
            }
            if (strategy_id == strategy_names.front()) {
                portfolio_config.overlay_sleeve = strategy_id;
                portfolio_config.overlay_tau = sleeve_keys.risk_target;
            }
            resolved_sleeves.push_back(
                {strategy_id, sleeve_keys.idm, sleeve_keys.risk_target, allocation});

            auto factory = registered_strategies_[strategy_id];
            StrategyContext strategy_ctx{base_strategy_config, db, registry_ptr};
            auto strategy = factory(strategy_ctx, strategy_def);

            // Initialize strategy
            auto init_result = strategy->initialize();
            if (init_result.is_error()) {
                ERROR("Failed to initialize strategy " + strategy_id + ": " +
                      init_result.error()->what());
                return make_error<backtest::BacktestResults>(
                    ErrorCode::STRATEGY_ERROR,
                    "Failed to initialize strategy " + strategy_id + ": " +
                        std::string(init_result.error()->what()),
                    "BacktestAPI");
            }

            // Start strategy
            auto start_result = strategy->start();
            if (start_result.is_error()) {
                ERROR("Failed to start strategy " + strategy_id + ": " +
                      start_result.error()->what());
                return make_error<backtest::BacktestResults>(
                    ErrorCode::STRATEGY_ERROR,
                    "Failed to start strategy " + strategy_id + ": " +
                        std::string(start_result.error()->what()),
                    "BacktestAPI");
            }

            strategies.push_back(strategy);
            INFO("Successfully initialized and started strategy: " + strategy_id);
        }

        // ========================================
        // CREATE PORTFOLIO AND RUN BACKTEST
        // ========================================
        INFO(resolved_sleeves_log_line(app_config.portfolio_id, resolved_sleeves));
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
                return make_error<backtest::BacktestResults>(
                    ErrorCode::STRATEGY_ERROR,
                    "Failed to add strategy " + strategy_id +
                        " to portfolio: " + std::string(add_result.error()->what()),
                    "BacktestAPI");
            }

            INFO("Added strategy " + strategy_id + " with allocation " +
                 std::to_string(allocation * 100.0) + "%");
        }

        // Run the backtest
        INFO("Running backtest for time period: " +
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
            return make_error<backtest::BacktestResults>(result.error()->code(),
                                                         result.error()->what(), "BacktestAPI");
        }

        INFO("Backtest completed successfully");

        // Analyze and display results
        const auto& backtest_results = result.value();

        INFO("Analyzing performance metrics...");

        std::cout << "======= Backtest Results =======" << std::endl;
        std::cout << "Portfolio: " << portfolio_name_ << std::endl;
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

        // Save portfolio results to database
        INFO("Saving portfolio backtest results to database...");
        try {
            nlohmann::json portfolio_config_json = portfolio_config.to_json();
            portfolio_config_json["strategy_allocations"] = strategy_allocations;
            portfolio_config_json["strategy_names"] = strategy_names;
            // T-8D-2 R53: each sleeve's resolved idm and risk_target, the window rule, the
            // warm-up and the sessions a year the statistics were annualised with.
            portfolio_config_json[kSleevesKey] = resolved_sleeves_json(resolved_sleeves, false);
            add_backtest_run_keys(portfolio_config_json, app_config.backtest.lookback_years,
                                  app_config.backtest.frozen_end_date,
                                  backtest_results.warmup_days,
                                  backtest_results.statistics_sessions_per_year);
            if (!app_config.instrument_id_relabels.empty()) {
                auto relabels = nlohmann::json::array();
                for (const auto& r : app_config.instrument_id_relabels) {
                    relabels.push_back({{"symbol", r.symbol},
                                        {"date", r.date},
                                        {"from", r.from},
                                        {"to", r.to}});
                }
                portfolio_config_json["instrument_id_relabels"] = relabels;
            }
            if (!app_config.listing_dates.empty()) {
                auto contracts = nlohmann::json::array();
                for (const auto& c : app_config.listing_dates) {
                    contracts.push_back({{"symbol", c.symbol},
                                         {"listed", c.listed},
                                         {"before", c.before},
                                         {"ratio", c.ratio}});
                }
                portfolio_config_json["listing_dates"] = {
                    {"switch_rule", to_string(app_config.listing_switch_rule)},
                    {"contracts", contracts}};
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
                INFO("Successfully saved portfolio backtest results to database");
            }
        } catch (const std::exception& e) {
            std::cerr << "Exception during database save: " << e.what() << std::endl;
            ERROR("Exception during database save: " + std::string(e.what()));
        }

        // Cleanup
        INFO("Cleaning up backtest coordinator...");
        coordinator.reset();

        INFO("Backtest application completed successfully");

        std::cerr << "At end of main: initialized=" << Logger::instance().is_initialized()
                  << std::endl;

        return backtest_results;

    } catch (const std::exception& e) {
        std::cerr << "Unexpected error: " << e.what() << std::endl;
        ERROR("Unexpected error: " + std::string(e.what()));
        return make_error<backtest::BacktestResults>(ErrorCode::UNKNOWN_ERROR, e.what(),
                                                     "BacktestAPI");
    } catch (...) {
        std::cerr << "Unknown error occurred" << std::endl;
        ERROR("Unknown error occurred");
        return make_error<backtest::BacktestResults>(ErrorCode::UNKNOWN_ERROR, "Unknown error",
                                                     "BacktestAPI");
    }
}

Result<backtest::BacktestResults> BacktestRunner::run_backtest_from_config() {
    try {
        StateManager::reset_instance();

        const auto& cfg = *run_config_;

        if (!Logger::instance().is_initialized()) {
            return make_error<backtest::BacktestResults>(
                ErrorCode::NOT_INITIALIZED, "Logger initialization failed", "BacktestAPI");
        }

        INFO("Running argument-driven backtest (no config files, no database)");

        // ========================================
        // VALIDATE INPUTS
        // ========================================
        if (cfg.symbols.empty()) {
            return make_error<backtest::BacktestResults>(
                ErrorCode::INVALID_DATA, "No symbols provided in BacktestRunConfig", "BacktestAPI");
        }

        if (cfg.strategies.empty()) {
            return make_error<backtest::BacktestResults>(
                ErrorCode::INVALID_DATA, "No strategies provided in BacktestRunConfig",
                "BacktestAPI");
        }

        if (cfg.start_date >= cfg.end_date) {
            return make_error<backtest::BacktestResults>(
                ErrorCode::INVALID_ARGUMENT, "start_date must be before end_date", "BacktestAPI");
        }

        for (const auto& spec : cfg.strategies) {
            if (registered_strategies_.find(spec.strategy_id) == registered_strategies_.end()) {
                return make_error<backtest::BacktestResults>(
                    ErrorCode::INVALID_DATA, "Strategy " + spec.strategy_id + " is not registered",
                    "BacktestAPI");
            }
        }

        // ========================================
        // INSTRUMENT REGISTRY
        // ========================================
        // Instruments must have been registered by the caller. The registry is
        // a singleton, so verify it actually holds the symbols we need rather
        // than silently reusing a previous run's contents.
        auto& registry = InstrumentRegistry::instance();
        auto all_instruments = registry.get_all_instruments();
        if (all_instruments.empty()) {
            return make_error<backtest::BacktestResults>(
                ErrorCode::NOT_INITIALIZED,
                "InstrumentRegistry is empty; call register_instrument() for each symbol before "
                "running a database-free backtest",
                "BacktestAPI");
        }

        for (const auto& symbol : cfg.symbols) {
            if (!registry.has_instrument(symbol)) {
                return make_error<backtest::BacktestResults>(
                    ErrorCode::INVALID_DATA,
                    "No instrument registered for symbol '" + symbol +
                        "'; register one so contract multiplier and tick size are known",
                    "BacktestAPI");
            }
        }

        INFO("Registry contains " + std::to_string(all_instruments.size()) + " instruments");

        auto registry_ptr =
            std::shared_ptr<InstrumentRegistry>(&registry, [](InstrumentRegistry*) {});

        // ========================================
        // BUILD CONFIGURATION
        // ========================================
        backtest::BacktestCoordinatorConfig coord_config;
        coord_config.initial_capital = cfg.initial_capital;
        coord_config.use_optimization = cfg.use_optimization;
        coord_config.statistics_sessions_per_year = cfg.statistics_sessions_per_year;
        // Results storage requires a database connection, which this path does
        // not have. store_trade_details also gates in-run position saving.
        coord_config.store_trade_details = false;
        coord_config.store_results = false;
        coord_config.portfolio_id = cfg.portfolio_id;
        coord_config.export_csv = cfg.export_csv;
        coord_config.csv_output_path = cfg.csv_output_path;

        RiskConfig risk_config = cfg.risk_config.value_or(RiskConfig{});
        risk_config.capital = cfg.initial_capital;

        DynamicOptConfig opt_config = cfg.opt_config.value_or(DynamicOptConfig{});
        opt_config.capital = cfg.initial_capital;

        PortfolioConfig portfolio_config;
        portfolio_config.total_capital = cfg.initial_capital;
        portfolio_config.risk_modules = cfg.risk_modules;
        if (portfolio_config.risk_modules.empty() && !cfg.no_risk_reason.empty()) {
            auto none = make_none_module(cfg.no_risk_reason, cfg.no_risk_ruled_by,
                                         cfg.no_risk_ruled_on);
            if (none.is_error()) {
                return make_error<backtest::BacktestResults>(
                    none.error()->code(), std::string(none.error()->what()), "BacktestAPI");
            }
            portfolio_config.risk_modules.push_back(none.value());
        }
        portfolio_config.max_strategy_allocation = cfg.max_strategy_allocation;
        portfolio_config.min_strategy_allocation = cfg.min_strategy_allocation;
        portfolio_config.use_optimization = cfg.use_optimization;
        portfolio_config.opt_config = opt_config;
        portfolio_config.risk_config = risk_config;

        StrategyConfig base_strategy_config;
        base_strategy_config.asset_classes = {cfg.asset_class};
        base_strategy_config.frequencies = {cfg.data_freq};
        base_strategy_config.max_drawdown = cfg.max_drawdown;
        base_strategy_config.max_leverage = cfg.max_leverage;
        for (const auto& symbol : cfg.symbols) {
            base_strategy_config.position_limits[symbol] = cfg.position_limit;
        }

        return execute(cfg.symbols, cfg.start_date, cfg.end_date, cfg.asset_class, cfg.data_freq,
                       cfg.strategies, base_strategy_config, portfolio_config, coord_config,
                       /*db=*/nullptr, cfg.data_source, registry_ptr);

    } catch (const std::exception& e) {
        ERROR("Unexpected error: " + std::string(e.what()));
        return make_error<backtest::BacktestResults>(ErrorCode::UNKNOWN_ERROR, e.what(),
                                                     "BacktestAPI");
    } catch (...) {
        ERROR("Unknown error occurred");
        return make_error<backtest::BacktestResults>(ErrorCode::UNKNOWN_ERROR, "Unknown error",
                                                     "BacktestAPI");
    }
}

Result<backtest::BacktestResults> BacktestRunner::execute(
    const std::vector<std::string>& symbols, const Timestamp& start_date, const Timestamp& end_date,
    AssetClass asset_class, DataFrequency data_freq,
    const std::vector<StrategySpec>& strategy_specs, const StrategyConfig& base_strategy_config,
    const PortfolioConfig& portfolio_config,
    const backtest::BacktestCoordinatorConfig& coord_config, std::shared_ptr<PostgresDatabase> db,
    std::shared_ptr<MarketDataSource> data_source, std::shared_ptr<InstrumentRegistry> registry) {
    // ========================================
    // NORMALIZE ALLOCATIONS
    // ========================================
    double total_allocation = 0.0;
    for (const auto& spec : strategy_specs) {
        total_allocation += spec.allocation;
    }
    if (total_allocation <= 0.0) {
        return make_error<backtest::BacktestResults>(
            ErrorCode::INVALID_DATA, "Total strategy allocation must be greater than zero",
            "BacktestAPI");
    }

    // ========================================
    // CREATE COORDINATOR
    // ========================================
    std::unique_ptr<backtest::BacktestCoordinator> coordinator;
    if (db) {
        coordinator =
            std::make_unique<backtest::BacktestCoordinator>(db, registry.get(), coord_config);
    } else {
        coordinator = std::make_unique<backtest::BacktestCoordinator>(data_source, registry.get(),
                                                                      coord_config);
    }

    // ========================================
    // BUILD STRATEGIES
    // ========================================
    std::vector<std::shared_ptr<BaseStrategy>> strategies;
    std::vector<std::string> strategy_names;
    std::unordered_map<std::string, double> strategy_allocations;

    for (const auto& spec : strategy_specs) {
        double allocation = spec.allocation / total_allocation;
        strategy_allocations[spec.strategy_id] = allocation;
        strategy_names.push_back(spec.strategy_id);

        StrategyConfig strategy_config = base_strategy_config;
        strategy_config.capital_allocation = coord_config.initial_capital * allocation;

        INFO("Creating strategy: " + spec.strategy_id +
             " (allocation: " + std::to_string(allocation * 100.0) + "%)");

        auto factory = registered_strategies_[spec.strategy_id];
        StrategyContext strategy_ctx{strategy_config, db, registry};
        auto strategy = factory(strategy_ctx, spec.config);

        if (!strategy) {
            return make_error<backtest::BacktestResults>(
                ErrorCode::STRATEGY_ERROR, "Strategy factory returned null for " + spec.strategy_id,
                "BacktestAPI");
        }

        auto init_result = strategy->initialize();
        if (init_result.is_error()) {
            return make_error<backtest::BacktestResults>(
                ErrorCode::STRATEGY_ERROR,
                "Failed to initialize strategy " + spec.strategy_id + ": " +
                    std::string(init_result.error()->what()),
                "BacktestAPI");
        }

        auto start_result = strategy->start();
        if (start_result.is_error()) {
            return make_error<backtest::BacktestResults>(
                ErrorCode::STRATEGY_ERROR,
                "Failed to start strategy " + spec.strategy_id + ": " +
                    std::string(start_result.error()->what()),
                "BacktestAPI");
        }

        strategies.push_back(strategy);
        INFO("Successfully initialized and started strategy: " + spec.strategy_id);
    }

    // ========================================
    // CREATE PORTFOLIO
    // ========================================
    auto portfolio = std::make_shared<PortfolioManager>(portfolio_config);

    for (size_t i = 0; i < strategies.size(); ++i) {
        const std::string& strategy_id = strategy_names[i];
        auto add_result = portfolio->add_strategy(
            strategies[i], strategy_allocations[strategy_id], portfolio_config.use_optimization);

        if (add_result.is_error()) {
            return make_error<backtest::BacktestResults>(
                ErrorCode::STRATEGY_ERROR,
                "Failed to add strategy " + strategy_id +
                    " to portfolio: " + std::string(add_result.error()->what()),
                "BacktestAPI");
        }

        INFO("Added strategy " + strategy_id + " with allocation " +
             std::to_string(strategy_allocations[strategy_id] * 100.0) + "%");
    }

    // ========================================
    // RUN
    // ========================================
    auto result = coordinator->run_portfolio(portfolio, symbols, start_date, end_date, asset_class,
                                             data_freq);

    if (result.is_error()) {
        return make_error<backtest::BacktestResults>(result.error()->code(), result.error()->what(),
                                                     "BacktestAPI");
    }

    const auto& backtest_results = result.value();

    std::cout << "======= Backtest Results =======" << std::endl;
    std::cout << "Portfolio: " << portfolio_name_ << std::endl;
    std::cout << "Total Return: " << (backtest_results.total_return * 100.0) << "%" << std::endl;
    std::cout << "Sharpe Ratio: " << backtest_results.sharpe_ratio << std::endl;
    std::cout << "Max Drawdown: " << (backtest_results.max_drawdown * 100.0) << "%" << std::endl;
    std::cout << "Total Trades: " << backtest_results.total_trades << std::endl;

    // Persist results only when a database is available and storage is enabled.
    if (db && coord_config.store_trade_details) {
        try {
            nlohmann::json portfolio_config_json = portfolio_config.to_json();
            portfolio_config_json["strategy_allocations"] = strategy_allocations;
            portfolio_config_json["strategy_names"] = strategy_names;

            auto save_result = coordinator->save_portfolio_results_to_db(
                backtest_results, strategy_names, strategy_allocations, portfolio,
                portfolio_config_json);

            if (save_result.is_error()) {
                ERROR("Failed to save portfolio backtest results to database: " +
                      std::string(save_result.error()->what()));
            }
        } catch (const std::exception& e) {
            ERROR("Exception during database save: " + std::string(e.what()));
        }
    } else {
        INFO("Results storage disabled; returning results in memory only");
    }

    coordinator.reset();
    INFO("Backtest completed successfully");

    return backtest_results;
}

Result<void> BacktestRunner::register_strategy(const std::string& strategy_id,
                                               StrategyFactory factory) {
    if (registered_strategies_.find(strategy_id) != registered_strategies_.end()) {
        return make_error<void>(ErrorCode::INVALID_DATA,
                                "Strategy ID already registered: " + strategy_id, "BacktestAPI");
    }

    registered_strategies_[strategy_id] = factory;
    INFO("Registered strategy factory for strategy ID: " + strategy_id);
    return {};
}

}  // namespace trade_ngin::api
