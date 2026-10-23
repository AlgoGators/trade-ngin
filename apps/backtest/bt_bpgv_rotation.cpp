// apps/backtest/bt_bpgv_rotation.cpp
//
// BPGV macro-regime rotation backtest. Follows bt_equity_mean_reversion.cpp (window, cost warm-up,
// statistics, persistence) with what a NAV-sized monthly rotation needs on top:
//   * warmup through the coordinator: the load starts required_warmup_days() bars before the
//     trading start, so every long lookback fills from one adjusted series and the metrics
//     exclude those bars;
//   * the NAV feed (StrategyInterface::on_portfolio_value), so the book compounds;
//   * a backtest-only bar source (strategies.<id>.backtest_data) for ETFs the database does not
//     carry yet (SMH XHB IYR BIL DBMF).
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <unordered_set>
#include <nlohmann/json.hpp>
#include "trade_ngin/backtest/backtest_coordinator.hpp"
#include "trade_ngin/backtest/backtest_data_loader.hpp"
#include "trade_ngin/backtest/equity_cost_warmup.hpp"
#include "trade_ngin/core/config_loader.hpp"
#include "trade_ngin/core/holiday_checker.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/no_value.hpp"
#include "trade_ngin/core/resolved_sleeves.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/data/database_pooling.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/instruments/equity.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/strategy/bpgv_rotation.hpp"
#include "trade_ngin/strategy/equity_strategy_builder.hpp"

using namespace trade_ngin;
using namespace trade_ngin::backtest;

namespace {

// Resolve the strategy's "backtest_data" block into the coordinator's data options.
// Missing block = database, no strictness (exactly what bt_equity_mr does).
Result<void> apply_backtest_data_block(const nlohmann::json& block, BacktestCoordinatorConfig& cfg,
                                       std::string& source_label) {
    const std::string source = block.value("bar_source", std::string("db"));
    if (source == "db") {
        cfg.data_source = DataSource::REAL;
    } else if (source == "csv") {
        cfg.data_source = DataSource::CSV;
        cfg.csv_bar_dir = block.value("bar_dir", std::string("data/equity_bars"));
    } else {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "backtest_data.bar_source must be \"db\" or \"csv\", got \"" +
                                    source + "\"",
                                "bt_bpgv_rotation");
    }
    cfg.require_all_symbols = block.value("require_all_symbols", false);
    cfg.require_aligned_calendar = block.value("require_aligned_calendar", false);
    source_label = source;
    return Result<void>();
}

}  // namespace

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

        auto load_result = registry.load_instruments();
        if (load_result.is_error()) {
            WARN("Could not load instruments from DB (may not have futures metadata): " +
                 std::string(load_result.error()->what()));
        }

        // ========================================
        // COLLECT THE ENABLED STRATEGY + LOAD SYMBOLS FROM CONFIG
        // ========================================
        auto strat_entries_result = trade_ngin::apps::collect_enabled_equity_strategies(
            app_config.strategies_config, "enabled_backtest");
        if (strat_entries_result.is_error()) {
            ERROR(std::string(strat_entries_result.error()->what()));
            return 1;
        }
        const auto& strat_entries = strat_entries_result.value();

        // BPGV sizes every target to the prior-close NAV of the WHOLE portfolio
        // (on_portfolio_value), so a run is BPGV alone: next to another strategy it would be
        // over-sized by 1/allocation.
        if (strat_entries.size() != 1 || strat_entries.front().type != "BPGVRotationStrategy") {
            ERROR("bt_bpgv_rotation runs exactly one enabled BPGVRotationStrategy; found " +
                  std::to_string(strat_entries.size()) + " enabled strategies");
            return 1;
        }
        const auto& entry = strat_entries.front();

        std::vector<std::string> symbols;
        {
            std::unordered_set<std::string> seen;
            for (const auto& sym : entry.def.value("symbols", nlohmann::json::array())) {
                std::string s = sym.get<std::string>();
                if (seen.insert(s).second) symbols.push_back(s);
            }
        }
        if (symbols.empty()) {
            ERROR("No symbols in strategies_config; BPGV needs its ETF universe listed");
            return 1;
        }
        INFO("Loaded " + std::to_string(symbols.size()) + " symbols from config");

        auto equity_reg_result = registry.load_equity_instruments(
            symbols, "data/equity_exchanges.json");
        if (equity_reg_result.is_error()) {
            ERROR("Failed to register equity instruments: " +
                  std::string(equity_reg_result.error()->what()));
            return 1;
        }

        // The optimizer is hard-coded off below (HD 2026-09-01, as bt_equity_mean_reversion).
        if (auto optimizer_guard =
                apps::refuse_if_optimizer_requested(app_config.use_optimization);
            optimizer_guard.is_error()) {
            ERROR(std::string(optimizer_guard.error()->what()));
            return 1;
        }

        // BPGV sizes on the prior-close NAV and nothing here moves a gate's capital with it,
        // so a Carver module on the book is refused (its ruled assignment is `none`).
        if (auto gate = apps::refuse_if_nav_sized_book_has_a_capital_gate(app_config.risk_schema);
            gate.is_error()) {
            ERROR(std::string(gate.error()->what()));
            return 1;
        }

        // CASH-mode equities cannot sit in a portfolio with max_gross_leverage > 1.0
        // (same guardrail as bt_equity_mean_reversion.cpp, audit §3.3).
        if (app_config.risk_config.max_gross_leverage > 1.0) {
            for (const auto& symbol : symbols) {
                auto inst = registry.get_equity_instrument(symbol);
                if (inst && inst->get_account_mode() == EquityAccountMode::CASH) {
                    ERROR("Refusing to start: CASH-mode equity " + symbol +
                          " in portfolio with max_gross_leverage=" +
                          std::to_string(app_config.risk_config.max_gross_leverage) +
                          " > 1.0. Set REG_T account_mode or reduce max_gross_leverage to 1.0.");
                    return 1;
                }
            }
        }

        std::cout << "Symbols (" << symbols.size() << "): ";
        for (const auto& sym : symbols) std::cout << sym << " ";
        std::cout << std::endl;

        // ========================================
        // CONFIGURE BACKTEST PARAMETERS
        // ========================================
        // Window resolution lives in ConfigLoader::resolve_backtest_window (M-12), as in the
        // other backtest apps: backtest.frozen_end_date pins the end date.
        bool frozen_window = false;
        auto now = std::chrono::system_clock::now();
        auto window = trade_ngin::ConfigLoader::resolve_backtest_window(
            app_config.backtest, now, &frozen_window);
        const Timestamp trading_start = window.first;
        const Timestamp end_date = window.second;
        if (frozen_window) {
            WARN("M-12 FROZEN BACKTEST WINDOW in force: end_date pinned to "
                 + app_config.backtest.frozen_end_date
                 + ". This is a TEST configuration; a production run must not show this line.");
        }
        const double initial_capital = app_config.initial_capital;

        INFO("Backtest period: " + std::to_string(app_config.backtest.lookback_years) + " years");
        std::cout << "Initial capital: $" << std::fixed << std::setprecision(0) << initial_capital
                  << std::endl;

        // ========================================
        // INITIALIZE BACKTEST COORDINATOR
        // ========================================
        BacktestCoordinatorConfig coord_config;
        coord_config.initial_capital = initial_capital;
        coord_config.use_optimization = app_config.use_optimization;
        coord_config.store_trade_details = app_config.backtest.store_trade_details;
        coord_config.portfolio_id = app_config.portfolio_id;
        // T-8D R2, T-8D-2 R76 (as bt_equity_mean_reversion): K of the equity statistics grid
        // (NYSE sessions), the one factor the run's annualised statistics use.
        coord_config.statistics_sessions_per_year = app_config.statistics.equity_sessions_per_year;
        std::string bar_source_label;
        auto data_block_result = apply_backtest_data_block(
            entry.def.value("backtest_data", nlohmann::json::object()), coord_config,
            bar_source_label);
        if (data_block_result.is_error()) {
            ERROR(std::string(data_block_result.error()->what()));
            return 1;
        }
        INFO("Bar source: " + bar_source_label +
             (coord_config.data_source == DataSource::CSV ? " (" + coord_config.csv_bar_dir + ")"
                                                          : "") +
             ", require_all_symbols=" + (coord_config.require_all_symbols ? "yes" : "no") +
             ", require_aligned_calendar=" +
             (coord_config.require_aligned_calendar ? "yes" : "no"));

        auto coordinator = std::make_unique<BacktestCoordinator>(db, &registry, coord_config);

        auto coord_init_result = coordinator->initialize();
        if (coord_init_result.is_error()) {
            ERROR("Failed to initialize backtest coordinator: " +
                  std::string(coord_init_result.error()->what()));
            return 1;
        }

        // ========================================
        // CREATE THE STRATEGY
        // ========================================
        StrategyConfig strategy_config;
        strategy_config.asset_classes = {AssetClass::EQUITIES};
        strategy_config.frequencies = {DataFrequency::DAILY};
        strategy_config.max_drawdown = app_config.max_drawdown;
        strategy_config.max_leverage = app_config.max_leverage;
        strategy_config.capital_allocation = initial_capital;
        for (const auto& symbol : symbols) {
            strategy_config.trading_params[symbol] = {};
            strategy_config.position_limits[symbol] = app_config.execution.position_limit_backtest;
        }

        auto registry_ptr =
            std::shared_ptr<InstrumentRegistry>(&registry, [](InstrumentRegistry*) {});

        const auto bpgv_config = trade_ngin::apps::build_bpgv_rotation_config(entry.def["config"]);
        auto strategy = std::make_shared<BPGVRotationStrategy>(entry.id, strategy_config,
                                                               bpgv_config, db, registry_ptr);
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
        const int required_warmup_bars = strategy->required_warmup_days();
        INFO("Equity strategy '" + entry.id + "' initialized and started");

        // ========================================
        // WARMUP WINDOW
        // ========================================
        // The coordinator consumes exactly calculate_warmup_days() leading bars as warmup, so the
        // load starts that many TRADING days (plus slack) before the trading start; evaluation
        // may begin a few bars early, and the run reports its actual first trading bar.
        const int warmup_calendar_days =
            static_cast<int>(std::ceil(required_warmup_bars * 366.0 / 252.0)) + 14;
        // One hour later on purpose: BacktestCoordinator seeds the equity curve with a synthetic
        // point AT start_date, and a start on the first bar's midnight timestamp would give two
        // curve points one key (equity_curve PK).
        const Timestamp loaded_start =
            trading_start - std::chrono::hours(24LL * warmup_calendar_days) + std::chrono::hours(1);
        INFO("Warmup: " + std::to_string(required_warmup_bars) + " bars required; loading from " +
             core::format_utc_date(loaded_start));

        // ========================================
        // CREATE PORTFOLIO AND RUN BACKTEST
        // ========================================
        PortfolioConfig portfolio_config;
        portfolio_config.total_capital = Decimal(initial_capital);
        portfolio_config.use_optimization = false;
        portfolio_config.covariance_history_prices = app_config.covariance_history_prices;
        portfolio_config.covariance_stale_dates = app_config.covariance_stale_dates;
        portfolio_config.risk_modules = app_config.risk_schema.portfolio;
        portfolio_config.sleeve_risk_modules = app_config.risk_schema.sleeves;
        portfolio_config.allow_fractional_positions = bpgv_config.allow_fractional_shares;
        portfolio_config.risk_config = app_config.risk_config;
        INFO(std::string("Fractional positions permitted: ") +
             (bpgv_config.allow_fractional_shares ? "yes (from strategy allow_fractional_shares)"
                                                  : "no"));

        auto portfolio = std::make_shared<PortfolioManager>(portfolio_config);
        auto add_result = portfolio->add_strategy(strategy, 1.0, false);
        if (add_result.is_error()) {
            ERROR("Failed to add strategy to portfolio: " +
                  std::string(add_result.error()->what()));
            return 1;
        }

        // H-13 (HD 2026-09-21), as bt_equity_mean_reversion: the tiers come from the 30 calendar
        // days before the first loaded bar, loaded with the bus off, on both cost managers. The
        // bars come through the run's own source, so the CSV-only ETFs are tiered too.
        backtest::register_equity_cost_warmup_from_bars(
            symbols, loaded_start,
            [&](const Timestamp& from, const Timestamp& to) {
                DataLoadConfig cost_config;
                cost_config.symbols = symbols;
                cost_config.start_date = from;
                cost_config.end_date = to;
                cost_config.asset_class = AssetClass::EQUITIES;
                cost_config.data_freq = DataFrequency::DAILY;
                cost_config.data_source = coord_config.data_source;
                cost_config.csv_bar_dir = coord_config.csv_bar_dir;
                return BacktestDataLoader(db).load_market_data(cost_config);
            },
            coordinator->get_execution_manager()->get_transaction_cost_manager(), *portfolio);

        INFO("Running BPGV macro regime rotation backtest...");
        auto result = coordinator->run_portfolio(
            portfolio, symbols, loaded_start, end_date,
            AssetClass::EQUITIES, DataFrequency::DAILY);

        if (result.is_error()) {
            std::cerr << "Backtest failed: " << result.error()->what() << std::endl;
            return 1;
        }
        INFO("Backtest completed successfully");

        const auto& backtest_results = result.value();

        // Warmup contract: the coordinator must have excluded at least the required bars.
        if (backtest_results.warmup_days < required_warmup_bars) {
            ERROR("Warmup contract violated: coordinator excluded " +
                  std::to_string(backtest_results.warmup_days) + " bars, strategy requires " +
                  std::to_string(required_warmup_bars));
            return 1;
        }
        std::string first_trading_bar = "n/a";
        {
            const auto& curve = backtest_results.equity_curve;
            const size_t idx = static_cast<size_t>(backtest_results.warmup_days) + 1;
            if (idx < curve.size()) first_trading_bar = core::format_utc_date(curve[idx].first);
        }

        // ========================================
        // DISPLAY RESULTS
        // ========================================
        std::cout << "\n======= BPGV Macro Regime Rotation Backtest Results =======" << std::endl;
        std::cout << "Total Return: " << std::fixed << std::setprecision(2)
                  << (backtest_results.total_return * 100.0) << "%" << std::endl;
        std::cout << "Sharpe Ratio: " << std::setprecision(3) << ratio_or_na(backtest_results.sharpe_ratio)
                  << std::endl;
        std::cout << "Sortino Ratio: " << ratio_or_na(backtest_results.sortino_ratio) << std::endl;
        std::cout << "Max Drawdown: " << std::setprecision(2)
                  << (backtest_results.max_drawdown * 100.0) << "%" << std::endl;
        std::cout << "Calmar Ratio: " << std::setprecision(3) << ratio_or_na(backtest_results.calmar_ratio)
                  << std::endl;
        std::cout << "Volatility (annualised): " << std::setprecision(2)
                  << (backtest_results.volatility * 100.0) << "%" << std::endl;
        std::cout << "Trade win rate (closing fills): " << (backtest_results.win_rate * 100.0)
                  << "%" << std::endl;
        std::cout << "Closing fills: " << backtest_results.total_trades << std::endl;
        std::cout << "Warmup bars excluded: " << backtest_results.warmup_days
                  << " (first evaluated bar " << first_trading_bar << ")" << std::endl;
        std::cout << "==========================================================" << std::endl;

        // Save results
        INFO("Saving backtest results to database...");
        try {
            nlohmann::json config_json;
            config_json["strategy_type"] = "BPGVRotationStrategy";
            config_json["asset_class"] = "EQUITIES";
            config_json["trading_start"] = core::format_utc_date(trading_start);
            config_json["end_date"] = core::format_utc_date(end_date);
            config_json["loaded_start"] = core::format_utc_date(loaded_start);
            config_json["warmup_bars_required"] = required_warmup_bars;
            config_json["warmup_bars_excluded"] = backtest_results.warmup_days;
            config_json["bar_source"] = bar_source_label;
            config_json["allow_fractional_positions"] = bpgv_config.allow_fractional_shares;
            config_json["strategies"] = {{entry.id, bpgv_config.to_json()}};
            // T-8D-2 R53 (as bt_equity_mean_reversion): the window rule, the warm-up and the
            // sessions a year the statistics used.
            trade_ngin::add_backtest_run_keys(
                config_json, app_config.backtest.lookback_years,
                app_config.backtest.frozen_end_date, backtest_results.warmup_days,
                backtest_results.statistics_sessions_per_year);

            auto save_result = coordinator->save_portfolio_results_to_db(
                backtest_results, {entry.id}, {{entry.id, 1.0}}, portfolio, config_json);

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
