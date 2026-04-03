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
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/strategy/copper_gold_ip.hpp"

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
        logger_config.filename_prefix = "bt_copper_gold_ip";
        logger.initialize(logger_config);

        std::atomic_thread_fence(std::memory_order_seq_cst);

        if (!logger.is_initialized()) {
            std::cerr << "ERROR: Logger initialization failed" << std::endl;
            return 1;
        }

        INFO("=== Copper-Gold IP Macro Regime Strategy Backtest ===");

        // ========================================
        // LOAD CONFIGURATION
        // ========================================
        INFO("Loading configuration...");
        auto app_config_result = ConfigLoader::load("./config", "copper_gold_ip");
        if (app_config_result.is_error()) {
            ERROR("Failed to load copper_gold_ip configuration: " +
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
            WARN("Could not load instruments from DB: " +
                 std::string(load_result.error()->what()));
        }

        // ========================================
        // LOAD SYMBOLS FROM CONFIG
        // ========================================
        std::vector<std::string> symbols;
        const auto& cg_strategy_def = app_config.strategies_config["COPPER_GOLD_IP"];
        if (cg_strategy_def.contains("symbols")) {
            for (const auto& sym : cg_strategy_def["symbols"]) {
                symbols.push_back(sym.get<std::string>());
            }
            INFO("Loaded " + std::to_string(symbols.size()) + " symbols from config");
        }

        if (symbols.empty()) {
            ERROR("No symbols found in COPPER_GOLD_IP config");
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
        coord_config.warmup_days = 300;  // Need 252 days for vol, 120 for z-score

        auto coordinator = std::make_unique<BacktestCoordinator>(db, &registry, coord_config);

        auto coord_init_result = coordinator->initialize();
        if (coord_init_result.is_error()) {
            ERROR("Failed to initialize backtest coordinator: " +
                  std::string(coord_init_result.error()->what()));
            return 1;
        }

        // ========================================
        // CREATE COPPER-GOLD IP STRATEGY
        // ========================================
        if (!app_config.strategies_config.contains("COPPER_GOLD_IP")) {
            ERROR("COPPER_GOLD_IP strategy not found in config");
            return 1;
        }
        const auto& strategy_def = app_config.strategies_config["COPPER_GOLD_IP"];
        if (!strategy_def.contains("config")) {
            ERROR("COPPER_GOLD_IP strategy missing 'config' section");
            return 1;
        }
        const auto& cg_cfg = strategy_def["config"];

        CopperGoldIPConfig cg_config;
        cg_config.macro_csv_path = cg_cfg.value("macro_csv_path", std::string("data/macro/copper_gold_daily.csv"));
        cg_config.roc_window = cg_cfg.value("roc_window", 20);
        cg_config.ma_fast = cg_cfg.value("ma_fast", 10);
        cg_config.ma_slow = cg_cfg.value("ma_slow", 50);
        cg_config.zscore_window = cg_cfg.value("zscore_window", 120);
        cg_config.zscore_threshold = cg_cfg.value("zscore_threshold", 0.5);
        cg_config.w1 = cg_cfg.value("w1", 0.33);
        cg_config.w2 = cg_cfg.value("w2", 0.33);
        cg_config.w3 = cg_cfg.value("w3", 0.34);
        cg_config.min_holding_period = cg_cfg.value("min_holding_period", 5);
        cg_config.spx_momentum_lookback = cg_cfg.value("spx_momentum_lookback", 60);
        cg_config.breakeven_lookback = cg_cfg.value("breakeven_lookback", 20);
        cg_config.liquidity_zscore_window = cg_cfg.value("liquidity_zscore_window", 60);
        cg_config.liquidity_threshold = cg_cfg.value("liquidity_threshold", -1.5);
        cg_config.dxy_momentum_lookback = cg_cfg.value("dxy_momentum_lookback", 20);
        cg_config.dxy_momentum_threshold = cg_cfg.value("dxy_momentum_threshold", 0.03);
        cg_config.china_cli_avg_window = cg_cfg.value("china_cli_avg_window", 65);
        cg_config.china_cli_threshold = cg_cfg.value("china_cli_threshold", -2.0);
        cg_config.correlation_window = cg_cfg.value("correlation_window", 20);
        cg_config.correlation_threshold = cg_cfg.value("correlation_threshold", 0.70);
        cg_config.leverage_target = cg_cfg.value("leverage_target", 2.0);
        cg_config.max_margin_utilization = cg_cfg.value("max_margin_utilization", 0.50);
        cg_config.drawdown_warning_pct = cg_cfg.value("drawdown_warning_pct", 0.10);
        cg_config.drawdown_stop_pct = cg_cfg.value("drawdown_stop_pct", 0.15);
        cg_config.max_single_equity_notional = cg_cfg.value("max_single_equity_notional", 0.20);
        cg_config.max_single_commodity_notional = cg_cfg.value("max_single_commodity_notional", 0.15);
        cg_config.max_total_equity_notional = cg_cfg.value("max_total_equity_notional", 0.35);
        cg_config.max_total_commodity_notional = cg_cfg.value("max_total_commodity_notional", 0.40);

        // Load custom symbol list if provided
        if (cg_cfg.contains("futures_symbols")) {
            cg_config.futures_symbols.clear();
            for (const auto& sym : cg_cfg["futures_symbols"]) {
                cg_config.futures_symbols.push_back(sym.get<std::string>());
            }
        }

        StrategyConfig strategy_config;
        strategy_config.asset_classes = {AssetClass::FUTURES};
        strategy_config.frequencies = {DataFrequency::DAILY};
        strategy_config.capital_allocation = initial_capital;
        strategy_config.max_drawdown = app_config.max_drawdown;
        strategy_config.max_leverage = app_config.max_leverage;

        for (const auto& symbol : symbols) {
            strategy_config.trading_params[symbol] = {};
            strategy_config.position_limits[symbol] = app_config.execution.position_limit_backtest;
        }

        auto registry_ptr = std::shared_ptr<InstrumentRegistry>(&registry, [](InstrumentRegistry*) {});

        auto strategy = std::make_shared<CopperGoldIPStrategy>(
            "COPPER_GOLD_IP", strategy_config, cg_config, db, registry_ptr);

        auto strat_init_result = strategy->initialize();
        if (strat_init_result.is_error()) {
            ERROR("Failed to initialize Copper-Gold IP strategy: " +
                  std::string(strat_init_result.error()->what()));
            return 1;
        }

        auto strat_start_result = strategy->start();
        if (strat_start_result.is_error()) {
            ERROR("Failed to start Copper-Gold IP strategy: " +
                  std::string(strat_start_result.error()->what()));
            return 1;
        }

        INFO("Copper-Gold IP strategy initialized and started");

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

        INFO("Running Copper-Gold IP macro regime strategy backtest...");
        auto result = coordinator->run_portfolio(
            portfolio, symbols, start_date, end_date,
            AssetClass::FUTURES, DataFrequency::DAILY);

        if (result.is_error()) {
            std::cerr << "Backtest failed: " << result.error()->what() << std::endl;
            return 1;
        }

        INFO("Backtest completed successfully");

        // ========================================
        // DISPLAY RESULTS
        // ========================================
        const auto& backtest_results = result.value();

        std::cout << "\n======= Copper-Gold IP Strategy Backtest Results =======" << std::endl;
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
        std::cout << "Final Equity: $" << std::setprecision(0) << strategy->get_current_equity()
                  << std::endl;
        std::cout << "========================================================" << std::endl;

        // Save results
        INFO("Saving backtest results to database...");
        try {
            std::vector<std::string> strategy_names = {"COPPER_GOLD_IP"};
            std::unordered_map<std::string, double> strategy_allocations = {
                {"COPPER_GOLD_IP", 1.0}};

            nlohmann::json config_json;
            config_json["strategy_type"] = "CopperGoldIPStrategy";
            config_json["asset_class"] = "FUTURES";
            config_json["copper_gold_ip"] = {
                {"macro_csv_path", cg_config.macro_csv_path},
                {"roc_window", cg_config.roc_window},
                {"ma_fast", cg_config.ma_fast},
                {"ma_slow", cg_config.ma_slow},
                {"zscore_window", cg_config.zscore_window},
                {"zscore_threshold", cg_config.zscore_threshold},
                {"min_holding_period", cg_config.min_holding_period},
                {"leverage_target", cg_config.leverage_target},
                {"drawdown_warning_pct", cg_config.drawdown_warning_pct},
                {"drawdown_stop_pct", cg_config.drawdown_stop_pct}};

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
        INFO("Copper-Gold IP strategy backtest completed");

        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Unexpected error: " << e.what() << std::endl;
        return 1;
    }
}
