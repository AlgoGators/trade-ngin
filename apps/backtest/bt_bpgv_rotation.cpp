#include <fstream>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include "trade_ngin/backtest/backtest_coordinator.hpp"
#include "trade_ngin/core/config_loader.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/data/database_pooling.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/instruments/equity.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/strategy/bpgv_rotation.hpp"

using namespace trade_ngin;
using namespace trade_ngin::backtest;

int main() {
    try {
        StateManager::reset_instance();
        Logger::reset_for_tests();

        auto& logger = Logger::instance();
        LoggerConfig logger_config;
        logger_config.min_level = LogLevel::INFO;
        logger_config.destination = LogDestination::BOTH;
        logger_config.log_directory = "logs";
        logger_config.filename_prefix = "bt_bpgv_rotation";
        logger.initialize(logger_config);

        std::atomic_thread_fence(std::memory_order_seq_cst);

        if (!logger.is_initialized()) {
            std::cerr << "ERROR: Logger initialization failed" << std::endl;
            return 1;
        }

        INFO("=== BPGV Macro Regime Rotation Backtest ===");

        // ========================================
        // LOAD CONFIGURATION
        // ========================================
        INFO("Loading configuration...");
        auto app_config_result = ConfigLoader::load("./config", "bpgv_rotation");
        if (app_config_result.is_error()) {
            ERROR("Failed to load bpgv_rotation configuration: " +
                  std::string(app_config_result.error()->what()));
            return 1;
        }
        auto app_config = app_config_result.value();
        INFO("Configuration loaded for portfolio: " + app_config.portfolio_id);

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

        auto db_guard = DatabasePool::instance().acquire_connection();
        auto db = db_guard.get();

        if (!db || !db->is_connected()) {
            std::cerr << "Failed to acquire database connection" << std::endl;
            return 1;
        }
        INFO("Database connection established");

        // ========================================
        // INITIALIZE INSTRUMENT REGISTRY
        // ========================================
        auto& registry = InstrumentRegistry::instance();
        auto init_result = registry.initialize(db);
        if (init_result.is_error()) {
            std::cerr << "Failed to initialize instrument registry: "
                      << init_result.error()->what() << std::endl;
            return 1;
        }

        auto load_result = registry.load_instruments();
        if (load_result.is_error()) {
            WARN("Could not load instruments from DB (may not have futures metadata): " +
                 std::string(load_result.error()->what()));
        }

        // ========================================
        // LOAD SYMBOLS FROM CONFIG
        // ========================================
        std::vector<std::string> symbols;
        const auto& bpgv_strategy_def = app_config.strategies_config["BPGV_ROTATION"];
        if (bpgv_strategy_def.contains("symbols")) {
            for (const auto& sym : bpgv_strategy_def["symbols"]) {
                symbols.push_back(sym.get<std::string>());
            }
            INFO("Loaded " + std::to_string(symbols.size()) + " symbols from config");
        }

        if (symbols.empty()) {
            ERROR("No symbols found in BPGV_ROTATION config");
            return 1;
        }

        // Register equity instruments
        auto equity_reg_result = registry.load_equity_instruments(symbols);
        if (equity_reg_result.is_error()) {
            ERROR("Failed to register equity instruments: " +
                  std::string(equity_reg_result.error()->what()));
            return 1;
        }

        std::cout << "Symbols (" << symbols.size() << "): ";
        for (const auto& sym : symbols) {
            std::cout << sym << " ";
        }
        std::cout << std::endl;

        // ========================================
        // CONFIGURE BACKTEST PARAMETERS
        // ========================================
        auto now = std::chrono::system_clock::now();
        auto now_time_t = std::chrono::system_clock::to_time_t(now);
        std::tm* now_tm = std::localtime(&now_time_t);

        std::tm start_tm = *now_tm;
        start_tm.tm_year -= app_config.backtest.lookback_years;
        auto start_time_t = std::mktime(&start_tm);
        Timestamp start_date = std::chrono::system_clock::from_time_t(start_time_t);
        Timestamp end_date = now;

        double initial_capital = app_config.initial_capital;

        INFO("Backtest period: " + std::to_string(app_config.backtest.lookback_years) + " years");
        std::cout << "Initial capital: $" << std::fixed << std::setprecision(0) << initial_capital
                  << std::endl;

        // ========================================
        // INITIALIZE BACKTEST COORDINATOR
        // ========================================
        BacktestCoordinatorConfig coord_config;
        coord_config.initial_capital = initial_capital;
        coord_config.use_risk_management = app_config.strategy_defaults.use_risk_management;
        coord_config.use_optimization = app_config.strategy_defaults.use_optimization;
        coord_config.store_trade_details = app_config.backtest.store_trade_details;
        coord_config.portfolio_id = app_config.portfolio_id;

        auto coordinator = std::make_unique<BacktestCoordinator>(db, &registry, coord_config);

        auto coord_init_result = coordinator->initialize();
        if (coord_init_result.is_error()) {
            ERROR("Failed to initialize backtest coordinator: " +
                  std::string(coord_init_result.error()->what()));
            return 1;
        }

        // ========================================
        // CREATE BPGV ROTATION STRATEGY
        // ========================================
        if (!app_config.strategies_config.contains("BPGV_ROTATION")) {
            ERROR("BPGV_ROTATION strategy not found in config");
            return 1;
        }
        const auto& strategy_def = app_config.strategies_config["BPGV_ROTATION"];
        if (!strategy_def.contains("config")) {
            ERROR("BPGV_ROTATION strategy missing 'config' section");
            return 1;
        }
        const auto& bpgv_cfg = strategy_def["config"];

        BPGVRotationConfig bpgv_config;
        bpgv_config.macro_csv_path = bpgv_cfg.value("macro_csv_path", std::string("data/macro/bpgv_regime.csv"));
        bpgv_config.rebalance_day_of_month = bpgv_cfg.value("rebalance_day_of_month", 18);
        bpgv_config.momentum_lookback_days = bpgv_cfg.value("momentum_lookback_days", 63);
        bpgv_config.momentum_tilt_scale = bpgv_cfg.value("momentum_tilt_scale", 0.40);
        bpgv_config.homebuilder_tilt_scale = bpgv_cfg.value("homebuilder_tilt_scale", 0.20);
        bpgv_config.homebuilder_symbol = bpgv_cfg.value("homebuilder_symbol", std::string("XHB"));
        bpgv_config.breakout_sma_window = bpgv_cfg.value("breakout_sma_window", 50);
        bpgv_config.crash_threshold = bpgv_cfg.value("crash_threshold", -0.07);
        bpgv_config.crash_lookback_days = bpgv_cfg.value("crash_lookback_days", 5);
        bpgv_config.crash_override_calendar_days = bpgv_cfg.value("crash_override_calendar_days", 14);
        bpgv_config.crash_defensive_weight = bpgv_cfg.value("crash_defensive_weight", 0.45);
        bpgv_config.allow_fractional_shares = bpgv_cfg.value("allow_fractional_shares", true);

        // Load custom symbol lists if provided
        if (bpgv_cfg.contains("risk_on_symbols")) {
            bpgv_config.risk_on_symbols.clear();
            for (const auto& sym : bpgv_cfg["risk_on_symbols"]) {
                bpgv_config.risk_on_symbols.push_back(sym.get<std::string>());
            }
        }
        if (bpgv_cfg.contains("risk_off_symbols")) {
            bpgv_config.risk_off_symbols.clear();
            for (const auto& sym : bpgv_cfg["risk_off_symbols"]) {
                bpgv_config.risk_off_symbols.push_back(sym.get<std::string>());
            }
        }
        if (bpgv_cfg.contains("cash_symbols")) {
            bpgv_config.cash_symbols.clear();
            for (const auto& sym : bpgv_cfg["cash_symbols"]) {
                bpgv_config.cash_symbols.push_back(sym.get<std::string>());
            }
        }

        // Tier-1 remediation (Fix 3): seed warmup_start_date with the backtest
        // start so initialize() can pre-load ~520 days of history into each
        // symbol's in-memory deque before the first live bar arrives.
        bpgv_config.warmup_start_date = start_date;

        // Tier-1 nested configs
        if (bpgv_cfg.contains("crash_override")) {
            bpgv_config.crash_override.from_json(bpgv_cfg.at("crash_override"));
        }
        if (bpgv_cfg.contains("rebalance")) {
            bpgv_config.rebalance.from_json(bpgv_cfg.at("rebalance"));
        }
        if (bpgv_cfg.contains("momentum")) {
            bpgv_config.momentum.from_json(bpgv_cfg.at("momentum"));
        }
        if (bpgv_cfg.contains("breakout")) {
            bpgv_config.breakout.from_json(bpgv_cfg.at("breakout"));
        }

        StrategyConfig strategy_config;
        strategy_config.asset_classes = {AssetClass::EQUITIES};
        strategy_config.frequencies = {DataFrequency::DAILY};
        strategy_config.capital_allocation = initial_capital;
        strategy_config.max_drawdown = app_config.max_drawdown;
        strategy_config.max_leverage = app_config.max_leverage;

        for (const auto& symbol : symbols) {
            strategy_config.trading_params[symbol] = {};
            strategy_config.position_limits[symbol] = app_config.execution.position_limit_backtest;
        }

        auto registry_ptr = std::shared_ptr<InstrumentRegistry>(&registry, [](InstrumentRegistry*) {});

        auto strategy = std::make_shared<BPGVRotationStrategy>(
            "BPGV_ROTATION", strategy_config, bpgv_config, db, registry_ptr);

        auto strat_init_result = strategy->initialize();
        if (strat_init_result.is_error()) {
            ERROR("Failed to initialize BPGV rotation strategy: " +
                  std::string(strat_init_result.error()->what()));
            return 1;
        }

        auto strat_start_result = strategy->start();
        if (strat_start_result.is_error()) {
            ERROR("Failed to start BPGV rotation strategy: " +
                  std::string(strat_start_result.error()->what()));
            return 1;
        }

        INFO("BPGV rotation strategy initialized and started");

        // ========================================
        // CREATE PORTFOLIO AND RUN BACKTEST
        // ========================================
        PortfolioConfig portfolio_config;
        portfolio_config.total_capital = Decimal(initial_capital);
        portfolio_config.reserve_capital = Decimal(initial_capital * app_config.reserve_capital_pct);
        portfolio_config.use_optimization = false;
        portfolio_config.use_risk_management = app_config.strategy_defaults.use_risk_management;
        portfolio_config.risk_config = app_config.risk_config;

        auto portfolio = std::make_shared<PortfolioManager>(portfolio_config);

        auto add_result = portfolio->add_strategy(strategy, 1.0, false,
                                                   portfolio_config.use_risk_management);
        if (add_result.is_error()) {
            ERROR("Failed to add strategy to portfolio: " +
                  std::string(add_result.error()->what()));
            return 1;
        }

        INFO("Running BPGV macro regime rotation backtest...");
        auto result = coordinator->run_portfolio(
            portfolio, symbols, start_date, end_date,
            AssetClass::EQUITIES, DataFrequency::DAILY);

        if (result.is_error()) {
            std::cerr << "Backtest failed: " << result.error()->what() << std::endl;
            return 1;
        }

        INFO("Backtest completed successfully");

        // ========================================
        // DISPLAY RESULTS
        // ========================================
        const auto& backtest_results = result.value();

        std::cout << "\n======= BPGV Macro Regime Rotation Backtest Results =======" << std::endl;
        std::cout << "Total Return: " << std::fixed << std::setprecision(2)
                  << (backtest_results.total_return * 100.0) << "%" << std::endl;
        std::cout << "Sharpe Ratio: " << std::setprecision(3) << backtest_results.sharpe_ratio
                  << std::endl;
        std::cout << "Sortino Ratio: " << backtest_results.sortino_ratio << std::endl;
        std::cout << "Max Drawdown: " << std::setprecision(2)
                  << (backtest_results.max_drawdown * 100.0) << "%" << std::endl;
        std::cout << "Calmar Ratio: " << std::setprecision(3) << backtest_results.calmar_ratio
                  << std::endl;
        std::cout << "Volatility: " << std::setprecision(2)
                  << (backtest_results.volatility * 100.0) << "%" << std::endl;
        std::cout << "Win Rate: " << (backtest_results.win_rate * 100.0) << "%" << std::endl;
        std::cout << "Total Trades: " << backtest_results.total_trades << std::endl;
        std::cout << "Crash Overrides: " << strategy->get_crash_override_count() << std::endl;
        std::cout << "==========================================================" << std::endl;

        // Save results
        INFO("Saving backtest results to database...");
        try {
            std::vector<std::string> strategy_names = {"BPGV_ROTATION"};
            std::unordered_map<std::string, double> strategy_allocations = {
                {"BPGV_ROTATION", 1.0}};

            nlohmann::json config_json;
            config_json["strategy_type"] = "BPGVRotationStrategy";
            config_json["asset_class"] = "EQUITIES";
            config_json["bpgv_rotation"] = {
                {"macro_csv_path", bpgv_config.macro_csv_path},
                {"rebalance_day_of_month", bpgv_config.rebalance_day_of_month},
                {"momentum_lookback_days", bpgv_config.momentum_lookback_days},
                {"momentum_tilt_scale", bpgv_config.momentum_tilt_scale},
                {"homebuilder_tilt_scale", bpgv_config.homebuilder_tilt_scale},
                {"breakout_sma_window", bpgv_config.breakout_sma_window},
                {"crash_threshold", bpgv_config.crash_threshold},
                {"crash_lookback_days", bpgv_config.crash_lookback_days},
                {"crash_override_calendar_days", bpgv_config.crash_override_calendar_days},
                {"crash_defensive_weight", bpgv_config.crash_defensive_weight},
                {"allow_fractional_shares", bpgv_config.allow_fractional_shares}};

            auto save_result = coordinator->save_portfolio_results_to_db(
                backtest_results, strategy_names, strategy_allocations, portfolio, config_json);

            if (save_result.is_error()) {
                WARN("Failed to save results to database: " +
                     std::string(save_result.error()->what()));
            } else {
                INFO("Results saved to database");
            }
        } catch (const std::exception& e) {
            WARN("Exception during database save: " + std::string(e.what()));
        }

        coordinator.reset();
        INFO("BPGV macro regime rotation backtest completed");

        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Unexpected error: " << e.what() << std::endl;
        return 1;
    }
}
