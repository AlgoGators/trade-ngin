#include <fstream>
#include <iomanip>
#include <iostream>
#include <unordered_set>
#include <nlohmann/json.hpp>
#include "trade_ngin/backtest/backtest_coordinator.hpp"
#include "trade_ngin/core/config_loader.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/data/conversion_utils.hpp"
#include "trade_ngin/data/database_pooling.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/core/holiday_checker.hpp"
#include "trade_ngin/instruments/equity.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/strategy/mean_reversion.hpp"
#include "trade_ngin/strategy/equity_strategy_builder.hpp"

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
        logger_config.filename_prefix = "bt_equity_mr";
        logger.initialize(logger_config);

        std::atomic_thread_fence(std::memory_order_seq_cst);

        if (!logger.is_initialized()) {
            std::cerr << "ERROR: Logger initialization failed" << std::endl;
            return 1;
        }

        INFO("=== Equity Mean Reversion Backtest ===");

        // ========================================
        // LOAD CONFIGURATION
        // ========================================
        INFO("Loading configuration...");
        auto app_config_result = ConfigLoader::load("./config", "equity_mr");
        if (app_config_result.is_error()) {
            ERROR("Failed to load equity_mr configuration: " +
                  std::string(app_config_result.error()->what()));
            return 1;
        }
        auto app_config = app_config_result.value();
        INFO("Configuration loaded for portfolio: " + app_config.portfolio_id);

        // Wire the shared HolidayChecker into EquityInstrument's static slot so
        // any market-hours queries on equity instruments consult the calendar.
        // Phase 6 §6a: path resolved via HolidayChecker::resolve_holidays_path.
        auto holiday_checker_ptr = std::make_shared<HolidayChecker>(
            HolidayChecker::resolve_holidays_path());
        EquityInstrument::set_holiday_checker(holiday_checker_ptr);

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

        // Load existing instruments (futures from DB)
        auto load_result = registry.load_instruments();
        if (load_result.is_error()) {
            WARN("Could not load instruments from DB (may not have futures metadata): " +
                 std::string(load_result.error()->what()));
        }

        // ========================================
        // COLLECT ENABLED STRATEGIES + LOAD SYMBOLS FROM CONFIG
        // ========================================
        // Iterate strategies_config (validates types, ERRORs on an unknown type
        // instead of silently skipping it -- review §F10) rather than hardcoding
        // the MEAN_REVERSION key.
        auto strat_entries_result = trade_ngin::apps::collect_enabled_equity_strategies(
            app_config.strategies_config, "enabled_backtest");
        if (strat_entries_result.is_error()) {
            ERROR(std::string(strat_entries_result.error()->what()));
            return 1;
        }
        const auto& strat_entries = strat_entries_result.value();

        // Symbols = union (config order, deduped) across all enabled strategies.
        std::vector<std::string> symbols;
        {
            std::unordered_set<std::string> seen;
            for (const auto& entry : strat_entries) {
                if (!entry.def.contains("symbols")) continue;
                for (const auto& sym : entry.def["symbols"]) {
                    std::string s = sym.get<std::string>();
                    if (seen.insert(s).second) symbols.push_back(s);
                }
            }
            if (!symbols.empty()) {
                INFO("Loaded " + std::to_string(symbols.size()) + " symbols from config");
            }
        }
        if (symbols.empty()) {
            WARN("No symbols in strategy config, falling back to database scan (slow)");
            auto symbols_result = db->get_symbols(AssetClass::EQUITIES);
            if (symbols_result.is_error()) {
                ERROR("Failed to get equity symbols: " + std::string(symbols_result.error()->what()));
                return 1;
            }
            symbols = symbols_result.value();
        }

        if (symbols.empty()) {
            ERROR("No equity symbols found");
            return 1;
        }

        // Register equity instruments in the registry. Pass the exchange JSON
        // path so per-symbol exchanges are correctly populated (NASDAQ/NYSE/etc)
        // instead of every symbol silently defaulting to NYSE. Closes audit §1.2.
        auto equity_reg_result = registry.load_equity_instruments(
            symbols, "data/equity_exchanges.json");
        if (equity_reg_result.is_error()) {
            ERROR("Failed to register equity instruments: " +
                  std::string(equity_reg_result.error()->what()));
            return 1;
        }

        // Phase 2 leverage guardrail (audit §3.3): CASH-mode equities cannot
        // be in a portfolio with max_gross_leverage > 1.0 -- cash accounts
        // can't borrow, so a leverage cap above 1.0 is structurally invalid.
        // Fail fast at startup rather than silently producing nonsense margin.
        if (app_config.risk_config.max_gross_leverage > 1.0) {
            for (const auto& symbol : symbols) {
                auto inst = registry.get_equity_instrument(symbol);
                if (inst && inst->get_account_mode() == EquityAccountMode::CASH) {
                    ERROR("Refusing to start: CASH-mode equity " + symbol +
                          " in portfolio with max_gross_leverage=" +
                          std::to_string(app_config.risk_config.max_gross_leverage) +
                          " > 1.0. Set REG_T account_mode (and is_short_allowed "
                          "if needed) or reduce max_gross_leverage to 1.0.");
                    return 1;
                }
            }
        }

        // Print symbols
        std::cout << "Symbols (" << symbols.size() << "): ";
        for (size_t i = 0; i < std::min(symbols.size(), size_t(10)); ++i) {
            std::cout << symbols[i] << " ";
        }
        if (symbols.size() > 10) std::cout << "...";
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
        // CREATE EQUITY STRATEGIES
        // ========================================
        // Normalize allocations across enabled strategies to sum to 1.0 (matches
        // the bt_portfolio convention). With a single enabled strategy this is 1.0.
        double total_allocation = 0.0;
        for (const auto& entry : strat_entries) total_allocation += entry.allocation;
        if (total_allocation <= 0.0) total_allocation = 1.0;

        // Per-strategy StrategyConfig template (symbols, limits). capital_allocation
        // is set per strategy below from the normalized weight.
        StrategyConfig base_strategy_config;
        base_strategy_config.asset_classes = {AssetClass::EQUITIES};
        base_strategy_config.frequencies = {DataFrequency::DAILY};
        base_strategy_config.max_drawdown = app_config.max_drawdown;
        base_strategy_config.max_leverage = app_config.max_leverage;
        for (const auto& symbol : symbols) {
            base_strategy_config.trading_params[symbol] = {};
            base_strategy_config.position_limits[symbol] = app_config.execution.position_limit_backtest;
        }

        auto registry_ptr = std::shared_ptr<InstrumentRegistry>(&registry, [](InstrumentRegistry*) {});

        // Build each enabled strategy. Dispatch on type (collect_enabled_equity_strategies
        // already validated that the type is recognized).
        std::vector<std::pair<std::shared_ptr<StrategyInterface>, double>> strategies;
        for (const auto& entry : strat_entries) {
            double weight = entry.allocation / total_allocation;
            StrategyConfig sc = base_strategy_config;
            sc.capital_allocation = initial_capital * weight;

            std::shared_ptr<StrategyInterface> strategy;
            if (entry.type == "MeanReversionStrategy") {
                auto mr_config = trade_ngin::apps::build_mean_reversion_config(entry.def["config"]);
                strategy = std::make_shared<MeanReversionStrategy>(
                    entry.id, sc, mr_config, db, registry_ptr);
            } else {
                ERROR("Unsupported equity strategy type: " + entry.type + " for " + entry.id);
                return 1;
            }

            auto strat_init_result = strategy->initialize();
            if (strat_init_result.is_error()) {
                ERROR("Failed to initialize strategy " + entry.id + ": " +
                      std::string(strat_init_result.error()->what()));
                return 1;
            }
            auto strat_start_result = strategy->start();
            if (strat_start_result.is_error()) {
                ERROR("Failed to start strategy " + entry.id + ": " +
                      std::string(strat_start_result.error()->what()));
                return 1;
            }
            strategies.emplace_back(std::move(strategy), weight);
            INFO("Equity strategy '" + entry.id + "' initialized and started (allocation " +
                 std::to_string(weight * 100.0) + "%)");
        }

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

        for (const auto& [strategy, weight] : strategies) {
            auto add_result = portfolio->add_strategy(strategy, weight, false,
                                                       portfolio_config.use_risk_management);
            if (add_result.is_error()) {
                ERROR("Failed to add strategy to portfolio: " +
                      std::string(add_result.error()->what()));
                return 1;
            }
        }

        // Register tier-appropriate equity cost configs before the backtest
        // runs. Uses a 30-day warmup window starting at start_date so cost
        // calibration reflects what would have been known at backtest start.
        // Closes audit §1.1: previously unconfigured equities fell through to
        // futures defaults ($1.50/share commission, point_value=100).
        {
            auto warmup_end = start_date + std::chrono::hours(24 * 30);
            auto warmup_result = db->get_market_data(
                symbols, start_date, warmup_end,
                AssetClass::EQUITIES, DataFrequency::DAILY, "ohlcv");
            if (warmup_result.is_ok()) {
                auto warmup_bars_result =
                    trade_ngin::DataConversionUtils::arrow_table_to_bars(warmup_result.value());
                if (warmup_bars_result.is_ok()) {
                    const auto& warmup_bars = warmup_bars_result.value();
                    std::unordered_map<std::string, std::vector<trade_ngin::Bar>> bars_by_symbol;
                    for (const auto& bar : warmup_bars) {
                        bars_by_symbol[bar.symbol].push_back(bar);
                    }
                    coordinator->get_execution_manager()
                        ->get_transaction_cost_manager()
                        .register_equity_costs_from_bars(symbols, bars_by_symbol);
                } else {
                    WARN("Failed to convert warmup bars: " +
                         std::string(warmup_bars_result.error()->what()) +
                         " -- equity cost configs may use unconfigured-symbol fallback");
                }
            } else {
                WARN("Failed to load equity cost warmup data: " +
                     std::string(warmup_result.error()->what()) +
                     " -- equity cost configs may use unconfigured-symbol fallback");
            }
        }

        INFO("Running equity mean reversion backtest...");
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

        std::cout << "\n======= Equity Mean Reversion Backtest Results =======" << std::endl;
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
        std::cout << "====================================================" << std::endl;

        // Save results
        INFO("Saving backtest results to database...");
        try {
            std::vector<std::string> strategy_names;
            std::unordered_map<std::string, double> strategy_allocations;
            for (const auto& entry : strat_entries) {
                strategy_names.push_back(entry.id);
                strategy_allocations[entry.id] = entry.allocation / total_allocation;
            }

            nlohmann::json config_json;
            config_json["strategy_type"] = "MeanReversionStrategy";
            config_json["asset_class"] = "EQUITIES";
            {
                auto mr = trade_ngin::apps::build_mean_reversion_config(
                    strat_entries.front().def["config"]);
                config_json["mean_reversion"] = {
                    {"lookback_period", mr.lookback_period},
                    {"entry_threshold", mr.entry_threshold},
                    {"exit_threshold", mr.exit_threshold},
                    {"risk_target", mr.risk_target},
                    {"position_size", mr.position_size},
                    {"vol_lookback", mr.vol_lookback},
                    {"allow_fractional_shares", mr.allow_fractional_shares}};
            }

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
        INFO("Equity mean reversion backtest completed");

        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Unexpected error: " << e.what() << std::endl;
        return 1;
    }
}
