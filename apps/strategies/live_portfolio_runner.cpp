// Shared implementation behind both live portfolio executables.
//
// Derived mechanically from live_portfolio_conservative.cpp: the two apps were
// 98% identical (121 differing lines of ~6550), which meant every fix had to be
// made twice and, in practice, sometimes was not -- the MarketDataBus
// duplicate-processing guard below existed only in the conservative copy.
//
// Behaviour is parameterised through LivePortfolioConfig; everything else is
// common to both portfolios.

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include "trade_ngin/apps/live_portfolio_helpers.hpp"
#include "trade_ngin/apps/qt_processed_report.hpp"
#include "trade_ngin/apps/live_runtime_invocation.hpp"
#include "trade_ngin/apps/consumption_projection.hpp"
#include "trade_ngin/apps/run_consumption.hpp"
#include "trade_ngin/core/config_loader.hpp"
#include "trade_ngin/core/email_sender.hpp"
#include "trade_ngin/core/holiday_checker.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/data/conversion_utils.hpp"
#include "trade_ngin/data/database_pooling.hpp"
#include "trade_ngin/data/market_data_bus.hpp"
#include "live_portfolio_runner.hpp"
#include "trade_ngin/apps/book_tail.hpp"
#include "trade_ngin/git_version.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/instruments/futures.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/live/csv_exporter.hpp"
#include "trade_ngin/live/execution_manager.hpp"
#include "trade_ngin/live/live_data_loader.hpp"
#include "trade_ngin/live/live_historical_metrics.hpp"
#include "trade_ngin/live/live_metrics_calculator.hpp"
#include "trade_ngin/live/live_pnl_manager.hpp"
#include "trade_ngin/live/live_price_manager.hpp"
#include "trade_ngin/live/live_trading_coordinator.hpp"
#include "trade_ngin/live/margin_manager.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/storage/live_results_manager.hpp"
#include "trade_ngin/strategy/trend_following.hpp"
#include "trade_ngin/strategy/trend_following_fast.hpp"
#include "trade_ngin/strategy/trend_following_slow.hpp"

using namespace trade_ngin;



// F1 dual portfolio: the operational chain reads what QT actually decided.
// 'What did we hold yesterday' is the qt stream, not the raw signal output --
// position buffering and execution sizing both key off it, so reading the
// system stream here would compute today's trades against a book we do not hold.
// Migration 002 backfills qt for all prior history, so this is populated from
// the first run onward.
static constexpr const char* QT_STREAM = "qt";
// The untouched counterfactual: compounds only off its own prior positions, so it
// never sees a QT edit. Requires migration 003.
static constexpr const char* BENCHMARK_STREAM = "benchmark";

// latest_bar_by_symbol(), compute_mark_to_market_equity(), and
// build_run_inputs_row() live in trade_ngin/apps/live_portfolio_helpers.hpp
// (src/apps/live_portfolio_helpers.cpp) -- extracted so they're unit-tested
// (tests/apps/test_live_portfolio_helpers.cpp) instead of living
// uncompiled-by-anything-but-this-binary inside an apps/ entrypoint.

namespace {
// Typed caller dependencies: no std::function, void context or hidden lambda.
class LivePortfolioTailCallbacks final : public BookTailCallbacks {
public:
    LivePortfolioTailCallbacks(BookTailInputs& book, const StrategySelection& selection,
        const StrategyConfig& base_strategy_config, const LivePortfolioConfig& portfolio_cfg,
        std::shared_ptr<InstrumentRegistry>& registry_ptr,
        std::shared_ptr<StrategyInterface>& primary_strategy,
        std::shared_ptr<TrendFollowingStrategy>& primary_trend,
        const std::unordered_map<std::string, Bar>& latest_bars)
        : book_(book), selection_(selection), base_strategy_config_(base_strategy_config),
          portfolio_cfg_(portfolio_cfg), registry_ptr_(registry_ptr),
          primary_strategy_(primary_strategy), primary_trend_(primary_trend), latest_bars_(latest_bars) {}

    void run_counterfactual_book() override {
        auto& run_consumption = book_.run_consumption;
        auto& portfolio_config = book_.portfolio_config;
        auto& db = book_.db;
        auto& strategy_names = book_.strategy_names;
        auto& strategy_allocations = book_.strategy_allocations;
        auto& now = book_.now;
        auto& combined_strategy_id = book_.combined_strategy_id;
        auto& coordinator_config = book_.coordinator_config;
        auto& all_bars = book_.all_bars;
        auto& registry_ptr = registry_ptr_;
        const auto& latest_bars = latest_bars_;
        // ========================================
        // BENCHMARK PASS: the untouched counterfactual portfolio
        // ========================================
        // Answers "has QT added value over time?", which neither other stream can.
        //
        // qt        = what we actually held (QT edits it)
        // system    = what the algorithm said TODAY given the real book -- scores each
        //             individual decision, but drifts along with QT because position
        //             buffering anchors on yesterday's actual position
        // benchmark = what the algorithm would have compounded to having never seen a
        //             single QT edit -- the only genuinely independent comparison
        //
        // Everything here is deliberately isolated from the operational chain: its own
        // strategy instances, its own PortfolioManager, and priors read from its own
        // stream. It reuses the already-fetched bars rather than re-querying.
        //
        // A benchmark failure is never fatal. The real book is already stored by this
        // point, and a missing day of counterfactual is a reporting gap, not a trading
        // problem.
        run_consumption.observe_benchmark_mode(portfolio_config.benchmark_mode);
        if (portfolio_config.benchmark_mode == "live") {
            try {
                INFO("BENCHMARK: computing untouched counterfactual portfolio");

            auto bench_strategies = create_benchmark_strategy_set();

            // Distinct component id: PortfolioManager registers with the global
            // StateManager, which REJECTS duplicate ids, and its constructor throws on
            // that failure. The default id would abort the run here.
            auto bench_portfolio = std::make_shared<trade_ngin::PortfolioManager>(
                portfolio_config, "PORTFOLIO_MANAGER_BENCHMARK", registry_ptr);

            for (size_t i = 0; i < bench_strategies.size(); ++i) {
                const std::string& strat_name = strategy_names[i];
                auto add_result = bench_portfolio->add_strategy(
                    bench_strategies[i], strategy_allocations[strat_name],
                    portfolio_config.use_optimization, portfolio_config.use_risk_management);
                if (add_result.is_error()) {
                    run_consumption.note_benchmark_error();
                    WARN("BENCHMARK: failed to add strategy " + strat_name + ": " +
                         std::string(add_result.error()->what()));
                }
            }

            // Seed from the BENCHMARK's own prior positions -- never qt. This is the
            // whole point: the counterfactual must compound on itself.
            {
                auto bench_prev_date = now - std::chrono::hours(24);
                std::string bench_seed_name =
                    strategy_names.empty() ? std::string() : strategy_names[0];
                auto bench_seed = db->load_positions_by_date(
                    combined_strategy_id, bench_seed_name, coordinator_config.portfolio_id,
                    bench_prev_date, "trading.positions", BENCHMARK_STREAM);
                if (bench_seed.is_ok() && !bench_seed.value().empty()) {
                    auto bench_tf_seeded = bench_strategies.empty()
                                               ? Result<void>()
                                               : bench_strategies[0]->seed_positions(
                                                     bench_seed.value());
                    if (bench_tf_seeded.is_error()) {
                        WARN("BENCHMARK: failed to seed strategy positions: " +
                             std::string(bench_tf_seeded.error()->what()));
                    }
                    int bench_seeded = 0;
                    for (const auto& [sym, pos] : bench_seed.value()) {
                        if (bench_portfolio->update_strategy_position(bench_seed_name, sym, pos)
                                .is_ok()) {
                            bench_seeded++;
                        }
                    }
                    INFO("BENCHMARK: seeded " + std::to_string(bench_seeded) +
                         " prior position(s) from its own stream");
                } else {
                    INFO("BENCHMARK: no prior benchmark positions; starting flat "
                         "(expected on the first run after migration 003)");
                }
            }

            // Same bars as the operational chain -- only the starting positions differ.
            auto bench_process = bench_portfolio->process_market_data(all_bars);
            if (bench_process.is_error()) {
                run_consumption.note_benchmark_error();
                WARN("BENCHMARK: process_market_data failed: " +
                     std::string(bench_process.error()->what()));
            } else {
                auto bench_positions_map = bench_portfolio->get_strategy_positions();
                int bench_stored = 0;
                for (const auto& [strat_name, pos_map] : bench_positions_map) {
                    std::vector<trade_ngin::Position> bench_vec;
                    bench_vec.reserve(pos_map.size());
                    for (const auto& [sym, pos] : pos_map) {
                        if (pos.quantity.as_double() != 0.0) {
                            bench_vec.push_back(pos);
                        }
                    }
                    if (bench_vec.empty()) {
                        continue;
                    }
                    auto bench_save = db->store_positions(
                        bench_vec, combined_strategy_id, strat_name,
                        coordinator_config.portfolio_id, "trading.positions", BENCHMARK_STREAM);
                    if (bench_save.is_error()) {
                        run_consumption.note_benchmark_error();
                        WARN("BENCHMARK: failed to store positions for " + strat_name + ": " +
                             std::string(bench_save.error()->what()));
                    } else {
                        bench_stored += static_cast<int>(bench_vec.size());
                    }
                }
                INFO("BENCHMARK: stored " + std::to_string(bench_stored) +
                     " counterfactual position(s)");

                // Compute and store mark-to-market equity for benchmark stream (ADR-005
                // D-4): sum(quantity * close_price) per strategy's book, never derived
                // from the fills/PnL machinery -- see compute_mark_to_market_equity().
                try {
                    double bench_equity = 0.0;
                    for (const auto& [strat_name, pos_map] : bench_positions_map) {
                        bench_equity += compute_mark_to_market_equity(pos_map, latest_bars);
                    }
                    auto equity_store = db->store_trading_equity_curve(
                        combined_strategy_id, now, bench_equity,
                        coordinator_config.portfolio_id, "trading.equity_curve",
                        BENCHMARK_STREAM);
                    if (equity_store.is_error()) {
                        run_consumption.note_benchmark_error();
                        WARN("BENCHMARK: failed to store equity curve: " +
                             std::string(equity_store.error()->what()));
                    } else {
                        INFO("BENCHMARK: stored mark-to-market equity: $" +
                             std::to_string(bench_equity));
                    }
                } catch (const std::exception& equity_e) {
                    run_consumption.note_benchmark_error();
                    WARN("BENCHMARK: failed to compute/store equity (non-fatal): " +
                         std::string(equity_e.what()));
                }
            }
            run_consumption.note_benchmark_success();
            } catch (const std::exception& e) {
                run_consumption.note_benchmark_error();
                WARN("BENCHMARK pass failed (non-fatal, real book already stored): " +
                     std::string(e.what()));
            }
        } else {
            INFO("BENCHMARK mode is deferred; skipping live benchmark pass");
        }

    }

    BookTailForecastDisplay get_primary_forecast_display(const std::string& symbol) override {
        const auto& tf_strategy_typed = primary_trend_;
        double forecast = tf_strategy_typed ? tf_strategy_typed->get_forecast(symbol) : 0.0;
        double position = tf_strategy_typed ? tf_strategy_typed->get_position(symbol) : 0.0;
        return {forecast, position};
    }

    void stop_primary_strategy() override {
        auto& tf_strategy = primary_strategy_;
        // Stop the strategy
        INFO("Stopping strategy...");
        auto stop_result = tf_strategy->stop();
        if (stop_result.is_error()) {
            ERROR("Failed to stop strategy: " + std::string(stop_result.error()->what()));
        } else {
            INFO("Strategy stopped successfully");
        }

    }

private:
    std::vector<std::shared_ptr<StrategyInterface>> create_benchmark_strategy_set() {
        return build_strategy_instances(selection_, base_strategy_config_, book_.initial_capital,
            book_.app_config.strategy_defaults, portfolio_cfg_.slow_max_symbol_concentration,
            book_.db, registry_ptr_, nullptr);
    }
    BookTailInputs& book_;
    const StrategySelection& selection_;
    const StrategyConfig& base_strategy_config_;
    const LivePortfolioConfig& portfolio_cfg_;
    std::shared_ptr<InstrumentRegistry>& registry_ptr_;
    std::shared_ptr<StrategyInterface>& primary_strategy_;
    std::shared_ptr<TrendFollowingStrategy>& primary_trend_;
    const std::unordered_map<std::string, Bar>& latest_bars_;
};
}  // namespace

int trade_ngin::run_live_portfolio(const LivePortfolioConfig& portfolio_cfg, int argc, char* argv[]) {
    try {
        std::vector<std::string> raw_arguments;
        for (int index = 1; index < argc; ++index) raw_arguments.emplace_back(argv[index]);
        std::optional<std::string> environment_portfolio;
        if (const char* value = std::getenv("TRADE_NGIN_PORTFOLIO")) {
            environment_portfolio = value;
        }
        auto portfolio_selection = resolve_portfolio_selection(
            raw_arguments, environment_portfolio, portfolio_cfg.config_name);
        if (portfolio_selection.is_error()) {
            std::cerr << portfolio_selection.error()->what() << std::endl;
            std::cerr << "Usage: " << argv[0]
                      << " [YYYY-MM-DD] [--send-email] [--portfolio NAME]" << std::endl;
            return 1;
        }

        // Parse command-line arguments for date override and email flag
        std::chrono::system_clock::time_point target_date;
        bool use_override_date = false;
        bool send_email = false;  // Default to false for historical runs

        // Parse command-line arguments
        for (const auto& arg : portfolio_selection.value().runner_arguments) {

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
                std::cerr << "Usage: " << argv[0]
                          << " [YYYY-MM-DD] [--send-email] [--portfolio NAME]" << std::endl;
                std::cerr << "Example: " << argv[0]
                          << " 2025-01-01 --send-email --portfolio base" << std::endl;
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
        logger_config.min_level = LogLevel::INFO;
        logger_config.destination = LogDestination::BOTH;
        logger_config.log_directory = "logs";
        logger_config.filename_prefix = portfolio_cfg.log_prefix;
        logger.initialize(logger_config);

        std::atomic_thread_fence(std::memory_order_seq_cst);

        if (!logger.is_initialized()) {
            std::cerr << "ERROR: Logger initialization failed" << std::endl;
            return 1;
        }

        INFO("Logger initialized successfully");

        std::cerr << "After Logger initialization: initialized="
                  << Logger::instance().is_initialized() << std::endl;

        // ========================================
        // LOAD CONFIGURATION FROM MODULAR CONFIG FILES
        // ========================================
        const auto& config_name = portfolio_selection.value().config_name;
        INFO("Loading configuration from config/portfolios/" + config_name +
             "...");
        auto app_config_result = ConfigLoader::load("./config", config_name);
        if (app_config_result.is_error()) {
            ERROR("Failed to load configuration: " +
                  std::string(app_config_result.error()->what()));
            std::cerr << "Failed to load configuration: " << app_config_result.error()->what()
                      << std::endl;
            return 1;
        }
        auto app_config = app_config_result.value();
        INFO("Configuration loaded successfully for portfolio: " + app_config.portfolio_id);

        // Setup database connection pool
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

        // Configure daily position generation parameters
        INFO("Loading configuration...");

        // ========================================
        // PHASE 1: CONFIG-DRIVEN STRATEGY LOADING
        // Load strategies from modular config using enabled_live flag
        // ========================================
        std::string portfolio_id = app_config.portfolio_id;
        INFO("Using portfolio_id: " + portfolio_id);

        // Load strategies from config (mirror bt_portfolio.cpp pattern).
        // select_enabled_live_strategies is shared with benchmark_replay
        // (ADR-005) so a replay derives the identical selection from a
        // recorded run_inputs.config_snapshot rather than today's config.
        // The scheduler normally passes an explicit date. It never bypasses
        // runtime approval; historical benchmark replay is a separate program.
        const auto invocation_clock = std::chrono::system_clock::now();
        auto invocation = trade_ngin::resolve_live_runtime_control(
            std::getenv("QT_RUNTIME_CONTROL_ENABLED"),
            use_override_date ? target_date : invocation_clock, invocation_clock);
        if (invocation.is_error()) {
            ERROR("Controlled operational historical-date runs are unsupported");
            return 1;
        }
        const bool controlled_runtime = invocation.value();
        RunConsumption run_consumption;
        run_consumption.controlled_selection = controlled_runtime;
        run_consumption.selection.emplace();
        auto selection_result = invoke_run_result(&run_consumption.selection.value(),
            [&](SelectionConsumption* output) {
                return controlled_runtime
                    ? trade_ngin::select_controlled_live_strategies(app_config.strategies_config, output)
                    : trade_ngin::select_enabled_live_strategies(app_config.strategies_config, output);
            });
        run_consumption.observe_native_limits();
        if (selection_result.is_error()) {
            ERROR("Failed to select enabled_live strategies: " +
                  std::string(selection_result.error()->what()));
            return 1;
        }
        auto selection = selection_result.value();
        std::vector<std::string>& strategy_names = selection.names;
        std::unordered_map<std::string, double>& strategy_allocations = selection.allocations;
        std::unordered_map<std::string, nlohmann::json>& strategy_configs = selection.configs;
        for (const auto& name : strategy_names) {
            if (!run_consumption.admit_strategy(name)) break;
        }
        run_consumption.discard_failed_evidence();

        if (selection.allocation_sum_before_normalization > 0.0 &&
            std::abs(selection.allocation_sum_before_normalization - 1.0) > 1e-6) {
            WARN("Strategy allocations sum to " +
                 std::to_string(selection.allocation_sum_before_normalization) +
                 " (not 1.0); silently rescaling. Partial-capital deployment is not "
                 "supported — adjust default_allocation values or accept full deployment.");
        }

        std::string combined_strategy_id = trade_ngin::build_combined_strategy_id(strategy_names);
        INFO("Combined strategy_id (Tier 2): " + combined_strategy_id);

        // Get current date for daily processing (or use override date)
        auto now = use_override_date ? target_date : std::chrono::system_clock::now();
        auto now_time_t = std::chrono::system_clock::to_time_t(now);
        std::tm* now_tm = std::localtime(&now_time_t);

        // Set start date based on configured historical window
        const int historical_days = app_config.live.historical_days;
        run_consumption.historical_days = historical_days;
        run_consumption.historical_days_reached = true;
        auto start_date = now - std::chrono::hours(24 * historical_days);

        // Set end date based on run type to avoid lookahead bias
        auto end_date = use_override_date ? (now - std::chrono::hours(24)) : now;
        // For historical runs: exclude current day's data (use previous day)
        // For live runs: include current day's data (use current day)

        INFO("DEBUG: Run type: " + std::string(use_override_date ? "HISTORICAL" : "LIVE"));
        INFO("DEBUG: Start date: " +
             std::to_string(std::chrono::system_clock::to_time_t(start_date)));
        INFO("DEBUG: End date: " + std::to_string(std::chrono::system_clock::to_time_t(end_date)));
        INFO("DEBUG: Target date (now): " +
             std::to_string(std::chrono::system_clock::to_time_t(now)));

        double initial_capital = app_config.initial_capital;
        auto trading_snapshot = build_runtime_trading_snapshot(app_config);
        if (trading_snapshot.is_error()) {
            ERROR("Runtime configuration snapshot refused");
            return 1;
        }
        PublicationEvidenceToken evidence_token;
        auto publication_start = db->begin_live_publication(combined_strategy_id, portfolio_id,
            now, trading_snapshot.value(), controlled_runtime, TRADE_NGIN_GIT_SHA,
            PublicationEvidenceRequirement::RequiredFinalObservations, &evidence_token);
        if (publication_start.is_error()) {
            ERROR("Runtime scope or approved configuration refused");
            return 1;
        }
        if (publication_start.value()) {
            INFO("Approved retired-scope stop acknowledged; no positions published");
            return 0;
        }
        // Every early return leaves an honest failed/unacknowledged attempt.
        auto publication_guard = std::shared_ptr<void>(nullptr,
            [db](void*) { db->abandon_live_publication(); });

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
        } else {
            // Detailed error logging
            ERROR("Failed to get symbols: " + std::string(symbols_result.error()->what()));
            throw std::runtime_error("Failed to get symbols: " +
                                     symbols_result.error()->to_string());
        }
        for (const auto& symbol : symbols) {
            if (!run_consumption.admit_symbol(symbol)) break;
        }
        run_consumption.discard_failed_evidence();

        std::cout << "Symbols: ";
        for (const auto& symbol : symbols) {
            std::cout << symbol << " ";
        }
        std::cout << std::endl;

        std::cout << "Retrieved " << symbols.size() << " symbols" << std::endl;
        std::cout << "Initial capital: $" << initial_capital << std::endl;

        INFO("Configuration loaded successfully. Processing " + std::to_string(symbols.size()) +
             " symbols from " + std::to_string(std::chrono::system_clock::to_time_t(start_date)) +
             " to " + std::to_string(std::chrono::system_clock::to_time_t(end_date)));

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
            ERROR(
                "Margin metadata validation failed for one or more futures instruments. Aborting "
                "run.");
            return 1;
        }

        // Configure portfolio risk management
        RiskConfig risk_config = app_config.risk_config;
        risk_config.capital = Decimal(initial_capital);

        // Configure portfolio optimization
        DynamicOptConfig opt_config = app_config.opt_config;
        opt_config.capital = initial_capital;

        // Setup portfolio configuration
        trade_ngin::PortfolioConfig portfolio_config;
        portfolio_config.total_capital = initial_capital;
        portfolio_config.reserve_capital = initial_capital * app_config.reserve_capital_pct;
        portfolio_config.max_strategy_allocation =
            app_config.strategy_defaults.max_strategy_allocation;
        portfolio_config.min_strategy_allocation =
            app_config.strategy_defaults.min_strategy_allocation;
        portfolio_config.use_optimization = app_config.strategy_defaults.use_optimization;
        portfolio_config.use_risk_management = app_config.strategy_defaults.use_risk_management;
        portfolio_config.benchmark_mode = app_config.benchmark_mode;
        portfolio_config.opt_config = opt_config;
        portfolio_config.risk_config = risk_config;

        // ========================================
        // PHASE 2: STRATEGY INSTANCE FACTORY
        // Create strategies based on type from modular config
        // ========================================

        // Base strategy configuration (used by all strategies)
        trade_ngin::StrategyConfig base_strategy_config;
        base_strategy_config.asset_classes = {trade_ngin::AssetClass::FUTURES};
        base_strategy_config.frequencies = {trade_ngin::DataFrequency::DAILY};
        base_strategy_config.max_drawdown = app_config.max_drawdown;
        base_strategy_config.max_leverage = app_config.max_leverage;

        // Add position limits and costs for all symbols
        for (const auto& symbol : symbols) {
            base_strategy_config.position_limits[symbol] = app_config.execution.position_limit_live;
        }

        // Create a shared_ptr that doesn't own the singleton registry
        auto registry_ptr =
            std::shared_ptr<InstrumentRegistry>(&registry, [](InstrumentRegistry*) {});

        // Vector to hold all strategy instances
        // (strategies is produced by build_strategy_set below)

        INFO("Creating " + std::to_string(strategy_names.size()) + " strategies from config");

        // Factory: create each strategy based on type. A lambda so it can be
        // invoked twice: once for the operational chain, once for the
        // untouched benchmark. The benchmark MUST get its own instances --
        // positions_ holds the Carver buffer anchor, so a shared set would
        // let the real book leak into the counterfactual.
        //
        // The construction itself lives in build_strategy_instances()
        // (live_portfolio_helpers), shared with benchmark_replay (ADR-005)
        // so the 7 parity gate compares positions built by identical code
        // on both sides -- a hand-duplicated copy here would risk silent
        // drift as this logic evolves.
        auto build_strategy_set =
            [&](FactoryConsumption* output = nullptr) -> std::vector<std::shared_ptr<trade_ngin::StrategyInterface>> {
            return trade_ngin::build_strategy_instances(
                selection, base_strategy_config, initial_capital, app_config.strategy_defaults,
                portfolio_cfg.slow_max_symbol_concentration, db, registry_ptr, output);
        };

        if (run_consumption.collecting()) run_consumption.primary_factory.emplace();
        auto strategies = invoke_run_value(
            run_consumption.primary_factory ? &run_consumption.primary_factory.value() : nullptr,
            [&](FactoryConsumption* output) { return build_strategy_set(output); });
        run_consumption.observe_native_limits();

        INFO("Successfully created " + std::to_string(strategies.size()) + " strategies");

        // Create map from strategy name to strategy instance for CSV export
        trade_ngin::StrategyInstancesMap strategy_instances_map;
        for (size_t i = 0; i < strategies.size(); ++i) {
            auto* base_strategy = dynamic_cast<trade_ngin::BaseStrategy*>(strategies[i].get());
            if (base_strategy != nullptr) {
                strategy_instances_map[strategy_names[i]] = base_strategy;
            }
        }
        INFO("Created strategy instances map with " +
             std::to_string(strategy_instances_map.size()) + " entries");

        // Get reference to first strategy for single-strategy compatibility (Phase 3 will fix this)
        auto tf_strategy = strategies[0];

        // Cast to TrendFollowingStrategy for methods like get_forecast/get_position (Phase 3 will
        // iterate all) Note: This works for both TrendFollowingStrategy and its subclasses
        // (Slow/Fast)
        auto tf_strategy_typed =
            std::dynamic_pointer_cast<trade_ngin::TrendFollowingStrategy>(tf_strategy);

        // ========================================
        // PHASE 3: PORTFOLIO MANAGER LOOP
        // Add all strategies to portfolio with normalized allocations
        // ========================================
        INFO("Creating portfolio manager with " + std::to_string(strategies.size()) +
             " strategies...");
        auto portfolio = std::make_shared<trade_ngin::PortfolioManager>(portfolio_config);

        for (size_t i = 0; i < strategies.size(); ++i) {
            const auto& strategy = strategies[i];
            const std::string& strat_name = strategy_names[i];
            double allocation = strategy_allocations[strat_name];

            INFO("Adding strategy " + strat_name + " with allocation " +
                 std::to_string(allocation * 100.0) + "%");

            auto* registration = run_consumption.append_strategy(
                run_consumption.registrations, strat_name);
            auto add_result = invoke_run_result(registration ? &registration->call : nullptr,
                [&](PortfolioRegistrationTrace* output) {
                    return portfolio->add_strategy(strategy, allocation,
                        portfolio_config.use_optimization,
                        portfolio_config.use_risk_management, output);
                });

            if (add_result.is_error()) {
                ERROR("Failed to add strategy " + strat_name +
                      " to portfolio: " + add_result.error()->what());
                return 1;
            }
            INFO("Strategy " + strat_name + " added to portfolio successfully");
        }

        INFO("All " + std::to_string(strategies.size()) + " strategies added to portfolio");

        // ========================================
        // STORE LIVE RUN METADATA
        // Save run metadata (allocations, configs) for this trading day
        // ========================================
        INFO("Storing live run metadata for this trading day...");
        {
            // Build portfolio config JSON
            nlohmann::json portfolio_config_json;
            portfolio_config_json["total_capital"] =
                static_cast<double>(portfolio_config.total_capital);
            portfolio_config_json["reserve_capital"] =
                static_cast<double>(portfolio_config.reserve_capital);
            portfolio_config_json["use_optimization"] = portfolio_config.use_optimization;
            portfolio_config_json["use_risk_management"] = portfolio_config.use_risk_management;

            const auto captured_now = std::chrono::system_clock::now();
            const auto captured_seconds = std::chrono::system_clock::to_time_t(captured_now);
            std::tm captured_utc{};
            trade_ngin::core::safe_gmtime(&captured_seconds, &captured_utc);
            const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                captured_now.time_since_epoch()).count() % 1000000;
            std::ostringstream captured_time;
            captured_time << std::put_time(&captured_utc, "%Y-%m-%dT%H:%M:%S")
                          << '.' << std::setfill('0') << std::setw(6) << micros << 'Z';
            auto inspection = trade_ngin::build_live_config_inspection_capture(
                app_config, selection, portfolio_cfg.slow_max_symbol_concentration);
            inspection["capture_schema_version"] = 1;
            inspection["captured_at"] = captured_time.str();
            if (inspection.dump().size() > 2u * 1024u * 1024u) {
                inspection = {{"capture_schema_version", 1},
                              {"captured_at", captured_time.str()},
                              {"status", "unavailable"}, {"reason", "capture_failed"},
                              {"supplied", nullptr}, {"selected_trend", nullptr}};
            }
            portfolio_config_json["config_inspection"] = std::move(inspection);

            // Convert strategy_allocations to JSON
            nlohmann::json strategy_alloc_json(strategy_allocations);

            // strategy_configs is already nlohmann::json
            auto metadata_result = db->store_live_run_metadata(
                now, combined_strategy_id, portfolio_id, strategy_alloc_json, portfolio_config_json,
                strategy_configs  // already nlohmann::json
            );

            if (metadata_result.is_error()) {
                WARN("Failed to store live run metadata: " +
                     std::string(metadata_result.error()->what()));
            } else {
                INFO("Successfully stored live run metadata for date");
            }
        }

        // Create LiveTradingCoordinator to manage all live trading components
        INFO("Creating LiveTradingCoordinator for centralized component management");
        LiveTradingConfig coordinator_config;
        coordinator_config.strategy_id = combined_strategy_id;  // From config (Phase 1)
        coordinator_config.portfolio_id = portfolio_id;         // From loaded config
        coordinator_config.schema = "trading";
        coordinator_config.initial_capital = initial_capital;
        coordinator_config.store_results = true;
        coordinator_config.calculate_risk_metrics = true;

        auto coordinator =
            std::make_unique<LiveTradingCoordinator>(db, registry, coordinator_config);

        // Initialize the coordinator
        auto init_coord_result = coordinator->initialize();
        if (init_coord_result.is_error()) {
            ERROR("Failed to initialize LiveTradingCoordinator: " +
                  std::string(init_coord_result.error()->what()));
            return 1;
        }
        INFO("LiveTradingCoordinator initialized successfully");

        // Get component references from coordinator
        auto* data_loader = coordinator->get_data_loader();
        auto* metrics_calculator = coordinator->get_metrics_calculator();
        auto* results_manager = coordinator->get_results_manager();
        auto* price_manager = coordinator->get_price_manager();
        auto* pnl_manager = coordinator->get_pnl_manager();

        // Create Phase 3 managers
        INFO("Creating ExecutionManager and MarginManager for Phase 3");
        auto execution_manager = std::make_unique<ExecutionManager>();
        auto margin_manager = std::make_unique<MarginManager>(registry);

        // Create Phase 4 CSV exporter with portfolio-specific directory
        INFO("Creating CSVExporter for Phase 4");
        std::string csv_output_dir = "apps/strategies/results/" + portfolio_id;
        std::filesystem::create_directories(csv_output_dir);
        INFO("CSV output directory: " + csv_output_dir);
        auto csv_exporter = std::make_unique<CSVExporter>(csv_output_dir);

        // ========================================
        // PUBLISH RISK LIMITS FOR ALGOLENS VALIDATION
        // Stage 1b: engine publishes limits to database so AlgoLens can validate
        // manual position edits before committing them.
        // ========================================
        {
            // Build the limits envelope from engine config.
            // Honest publication: only include limits that the engine actually enforces.
            nlohmann::json limits;

            // max_symbol_position_contracts: per-symbol position caps in CONTRACT
            // UNITS (base_strategy_config.position_limits fans out
            // app_config.execution.position_limit_live). Deliberately NOT named
            // "notional": these are not dollars, and a gate reading them as
            // dollars would wave through enormous edits.
            nlohmann::json symbol_contracts;
            for (const auto& [symbol, limit] : base_strategy_config.position_limits) {
                symbol_contracts[symbol] = limit;
            }
            limits["max_symbol_position_contracts"] = symbol_contracts;

            // max_gross_leverage: enforced by RiskManager in process_positions()
            // (see src/risk/risk_manager.cpp: calculate_leverage_multiplier)
            limits["max_gross_leverage"] = risk_config.max_gross_leverage;

            // max_net_leverage: also enforced by RiskManager
            limits["max_net_leverage"] = risk_config.max_net_leverage;

            // NOTE: max_gross_notional and max_position_count are not enforced by the engine.
            // NOT including them is honest; omitting a limit that doesn't exist is better than
            // publishing a number that nobody chose and will drift from reality.

            INFO("Publishing risk limits for strategy=" + combined_strategy_id +
                 " portfolio=" + portfolio_id);
            auto publish_result = db->store_risk_limits(combined_strategy_id, portfolio_id, limits);
            if (publish_result.is_error()) {
                WARN("Failed to publish risk limits (trading run continues): " +
                     std::string(publish_result.error()->what()));
            }
        }

        // Load market data for daily processing
        INFO("Loading market data for daily processing...");
        // Disable MarketDataBus auto-publish to prevent duplicate processing
        MarketDataBus::instance().set_publish_enabled(false);
        INFO("MarketDataBus publishing DISABLED before get_market_data");
        auto market_data_result = invoke_run_result(
            run_consumption.collecting() ? &run_consumption.market_fetch : nullptr,
            [&](RunEmptyObservation*) {
                return db->get_market_data(symbols, start_date, end_date,
                    trade_ngin::AssetClass::FUTURES, trade_ngin::DataFrequency::DAILY, "ohlcv");
            });
        INFO("MarketDataBus publishing RE-ENABLED after get_market_data");
        MarketDataBus::instance().set_publish_enabled(true);

        if (market_data_result.is_error()) {
            ERROR("Failed to load market data: " + std::string(market_data_result.error()->what()));
            return 1;
        }

        // Convert Arrow table to Bars using the same conversion as backtest
        auto conversion_result = invoke_run_result(
            run_consumption.collecting() ? &run_consumption.arrow_conversion : nullptr,
            [&](RunEmptyObservation*) {
                return trade_ngin::DataConversionUtils::arrow_table_to_bars(market_data_result.value());
            });
        if (conversion_result.is_error()) {
            ERROR("Failed to convert market data to bars: " +
                  std::string(conversion_result.error()->what()));
            return 1;
        }

        auto all_bars = conversion_result.value();
        INFO("Loaded " + std::to_string(all_bars.size()) + " total bars");
        // Latest close per symbol, built once -- used by compute_mark_to_market_equity()
        // in the benchmark pass below, rather than re-scanning all_bars per position.
        auto latest_bars = latest_bar_by_symbol(all_bars);

        // Update price manager with bars to extract T-1 and T-2 prices
        if (price_manager) {
            auto price_update_result = price_manager->update_from_bars(all_bars, now);
            if (price_update_result.is_error()) {
                ERROR("Failed to update price manager with bar data: " +
                      std::string(price_update_result.error()->what()));
                return 1;
            }
            INFO("Price manager updated - extracted T-1 and T-2 prices from bars");
        } else {
            ERROR("Price manager not initialized");
            return 1;
        }

        if (all_bars.empty()) {
            ERROR("No historical data loaded. Cannot calculate positions.");
            ERROR("This may be due to missing market data for the requested date.");
            ERROR("Please check if market data exists for " +
                  std::to_string(std::chrono::system_clock::to_time_t(now)) +
                  " and the 300 days prior.");
            return 1;
        }
        run_consumption.market_input_completed = true;

        // ========================================
        // NON-TRADING DAY DETECTION
        // Check if yesterday was a non-trading day (weekend or holiday)
        // If so, reuse previous positions unchanged to avoid phantom executions
        // Uses same logic as email system for robustness
        // ========================================
        auto early_previous_day_close_prices = price_manager->get_all_previous_day_prices();

        // Determine if yesterday was a non-trading day
        int day_of_week = now_tm->tm_wday;  // 0=Sunday, 6=Saturday
        bool is_sunday = (day_of_week == 0);

        // Check if yesterday was a holiday using HolidayChecker
        HolidayChecker holiday_checker("include/trade_ngin/core/holidays.json");
        auto yesterday_for_check = now - std::chrono::hours(24);
        auto yesterday_time_t_check = std::chrono::system_clock::to_time_t(yesterday_for_check);
        std::tm yesterday_tm_check = *std::gmtime(&yesterday_time_t_check);
        std::ostringstream yesterday_oss_check;
        yesterday_oss_check << std::put_time(&yesterday_tm_check, "%Y-%m-%d");
        std::string yesterday_date_str_check = yesterday_oss_check.str();
        bool is_yesterday_holiday = holiday_checker.is_holiday(yesterday_date_str_check);

        // Yesterday was non-trading if: today is Sunday (Sat was non-trading) OR yesterday was
        // holiday
        bool is_non_trading_day = is_sunday || is_yesterday_holiday;

        // Flag to track if we should skip strategy processing
        bool skip_strategy_processing = false;

        // Data structures for non-trading day case
        std::unordered_map<std::string, std::unordered_map<std::string, Position>>
            strategy_positions_map;
        std::unordered_map<std::string, Position> positions;

        if (early_previous_day_close_prices.empty()) {
            if (is_non_trading_day) {
                // ========================================
                // EXPECTED: Non-trading day detected
                // Reuse previous positions, skip strategy processing
                // ========================================
                INFO("═══════════════════════════════════════════════════════════════");
                INFO("NON-TRADING DAY DETECTED - POSITIONS UNCHANGED");
                INFO("═══════════════════════════════════════════════════════════════");

                if (is_sunday) {
                    INFO("Today is Sunday - Saturday was not a trading day");
                } else if (is_yesterday_holiday) {
                    INFO("Yesterday (" + yesterday_date_str_check + ") was a holiday: " +
                         holiday_checker.get_holiday_name(yesterday_date_str_check));
                }

                INFO("No new market data available - positions will remain unchanged");
                INFO("Loading previous trading day positions to carry forward...");

                // Calculate previous date for position loading
                auto previous_date_nontrade = now - std::chrono::hours(24);

                // Load previous positions for each strategy and use as current
                for (const auto& [strategy_name, allocation] : strategy_allocations) {
                    auto prev_result = db->load_positions_by_date(
                        combined_strategy_id, strategy_name, coordinator_config.portfolio_id,
                        previous_date_nontrade, "trading.positions", QT_STREAM);

                    if (prev_result.is_ok() && !prev_result.value().empty()) {
                        strategy_positions_map[strategy_name] = prev_result.value();
                        INFO("Loaded " + std::to_string(prev_result.value().size()) +
                             " positions for strategy: " + strategy_name);

                        // Also add to combined positions map
                        for (const auto& [symbol, pos] : prev_result.value()) {
                            positions[symbol] = pos;
                        }
                    } else {
                        INFO("No previous positions found for strategy: " + strategy_name);
                        strategy_positions_map[strategy_name] = {};
                    }
                }

                INFO("Total positions carried forward: " + std::to_string(positions.size()));
                INFO("═══════════════════════════════════════════════════════════════");
                INFO("Skipping strategy calculations - proceeding to storage phase");
                INFO("═══════════════════════════════════════════════════════════════");

                skip_strategy_processing = true;

            } else {
                // ========================================
                // UNEXPECTED: No prices on a trading day
                // This indicates a data pipeline issue
                // ========================================
                ERROR("═══════════════════════════════════════════════════════════════");
                ERROR("DATA ISSUE DETECTED - ABORTING");
                ERROR("═══════════════════════════════════════════════════════════════");
                ERROR("No T-1 close prices available, but today appears to be a trading day!");
                ERROR("Today: " + std::string(use_override_date ? "HISTORICAL RUN" : "LIVE RUN"));
                ERROR("Day of week: " + std::to_string(day_of_week) + " (0=Sun, 6=Sat)");
                ERROR("Yesterday: " + yesterday_date_str_check);
                ERROR("Is yesterday a holiday? " +
                      std::string(is_yesterday_holiday ? "YES" : "NO"));
                ERROR("");
                ERROR("This indicates missing market data in the database.");
                ERROR("Please investigate the data pipeline before re-running.");
                ERROR("═══════════════════════════════════════════════════════════════");
                return 1;  // Fail fast on data issues
            }
        }
        run_consumption.observe_non_trading_decision(skip_strategy_processing);

        // ========================================
        // UPDATE TRANSACTION COST MANAGER WITH MARKET DATA
        // Feed rolling ADV and volatility for accurate cost calculations
        // ========================================
        INFO("Updating execution manager with market data for transaction cost tracking...");

        // Build map of latest bars per symbol (T-1 data)
        std::unordered_map<std::string, Bar> latest_bars_per_symbol;
        std::unordered_map<std::string, Bar> previous_bars_per_symbol;

        for (const auto& bar : all_bars) {
            auto it = latest_bars_per_symbol.find(bar.symbol);
            if (it == latest_bars_per_symbol.end() || bar.timestamp > it->second.timestamp) {
                // Save previous latest as "previous" before updating
                if (it != latest_bars_per_symbol.end()) {
                    previous_bars_per_symbol[bar.symbol] = it->second;
                }
                latest_bars_per_symbol[bar.symbol] = bar;
            } else if (!previous_bars_per_symbol.count(bar.symbol) &&
                       bar.timestamp < latest_bars_per_symbol[bar.symbol].timestamp) {
                // Track the second-most-recent bar as previous
                auto prev_it = previous_bars_per_symbol.find(bar.symbol);
                if (prev_it == previous_bars_per_symbol.end() ||
                    bar.timestamp > prev_it->second.timestamp) {
                    previous_bars_per_symbol[bar.symbol] = bar;
                }
            }
        }

        // Update execution manager with daily market data for each symbol
        int symbols_updated = 0;
        run_consumption.history_loop.reached = true;
        for (const auto& [symbol, latest_bar] : latest_bars_per_symbol) {
            double close = static_cast<double>(latest_bar.close);
            double volume = latest_bar.volume;
            double prev_close = close;  // Default to same if no previous

            auto prev_it = previous_bars_per_symbol.find(symbol);
            if (prev_it != previous_bars_per_symbol.end()) {
                prev_close = static_cast<double>(prev_it->second.close);
            }

            // Update the transaction cost manager with market data
            auto* history = run_consumption.append_symbol(run_consumption.history_updates, symbol);
            invoke_run_value(history ? &history->call : nullptr,
                [&](ExecutionMarketDataObservation* output) {
                    execution_manager->update_market_data(symbol, volume, close, output);
                });
            symbols_updated++;

            DEBUG("Updated market data for " + symbol + ": volume=" + std::to_string(volume) +
                  ", close=" + std::to_string(close) +
                  ", prev_close=" + std::to_string(prev_close));
        }
        run_consumption.history_loop.completed = true;

        INFO("Updated transaction cost manager with market data for " +
             std::to_string(symbols_updated) + " symbols");

        // ========================================
        // NORMAL TRADING DAY PROCESSING
        // Only run strategy calculations if NOT a non-trading day
        // ========================================
        if (!skip_strategy_processing) {
            run_consumption.preparation_stage.reached = true;
            // FIX: Seed strategy's positions_ from yesterday's DB snapshot BEFORE prewarm.
            // Each live invocation is a fresh process where positions_ defaults to zero.
            // The Carver position buffer reads positions_ as the comparison anchor; without
            // seeding it correctly the buffer can't absorb small day-to-day signal jitter
            // and triggers a phantom trade every day. Backtest doesn't need this because
            // its state is continuous in-memory across simulated days.
            {
                auto seed_previous_date = now - std::chrono::hours(24);
                std::string seed_strategy_name =
                    strategy_names.empty() ? std::string() : strategy_names[0];
                auto seed_result = db->load_positions_by_date(
                    combined_strategy_id, seed_strategy_name,
                    coordinator_config.portfolio_id, seed_previous_date, "trading.positions", QT_STREAM);
                if (seed_result.is_ok() && !seed_result.value().empty()) {
                    // Fix #1: seed strategy's positions_ for buffer correctness
                    auto seeded = tf_strategy->seed_positions(seed_result.value());
                    if (seeded.is_error()) {
                        WARN("Failed to seed strategy positions: " +
                             std::string(seeded.error()->what()));
                    }
                    // Fix #7: seed PortfolioManager's info.current_positions for optimizer
                    // baseline correctness. Without this, the optimizer's coordinate descent
                    // starts from current=0 every fresh process and converges to a
                    // zero-anchored compromise (e.g., MYM=2 instead of yesterday's actual=1),
                    // producing daily ±1 trades as market noise crosses rounding boundaries.
                    // PortfolioManager.strategies_ is keyed by the strategy's metadata.id
                    // (e.g. "TREND_FOLLOWING"), NOT the combined_strategy_id used for DB
                    // (e.g. "LIVE_TREND_FOLLOWING"). Use seed_strategy_name which matches.
                    int pm_seeded_count = 0;
                    for (const auto& [sym, pos] : seed_result.value()) {
                        auto pm_seed = portfolio->update_strategy_position(
                            seed_strategy_name, sym, pos);
                        if (pm_seed.is_error()) {
                            WARN("Failed to seed PortfolioManager position for " + sym +
                                 ": " + std::string(pm_seed.error()->what()));
                        } else {
                            pm_seeded_count++;
                        }
                    }
                    INFO("Seeded " + std::to_string(pm_seeded_count) +
                         " positions into PortfolioManager.current_positions for "
                         "optimizer-baseline correctness (strategy_name=" +
                         seed_strategy_name + ")");
                } else {
                    INFO("No yesterday positions to seed for strategy " +
                         seed_strategy_name + " (first run or no data)");
                }
            }

            // Pre-warm strategy state so portfolio can pull price history for optimization/risk
            INFO("Preprocessing data in strategy to populate price history...");
            if (run_consumption.collecting() &&
                run_consumption.admit_strategy(strategy_names[0])) {
                run_consumption.preparation.emplace();
                run_consumption.preparation->identity = strategy_names[0];
            }
            auto strat_prewarm = invoke_run_result(
                run_consumption.preparation ? &run_consumption.preparation->call : nullptr,
                [&](StrategyConsumptionTrace* output) {
                    return strategies[0]->on_data(all_bars, output);
                });
            run_consumption.observe_native_limits();
            if (strat_prewarm.is_error()) {
                std::cerr << "Failed to preprocess data in strategy: "
                          << strat_prewarm.error()->what() << std::endl;
                return 1;
            }
            run_consumption.preparation_stage.completed = true;

            // Process data through portfolio pipeline (optimization + risk), mirroring backtest
            INFO("Processing data through portfolio manager (optimization + risk)...");
            // Disable MarketDataBus to prevent duplicate processing during explicit data feed
            MarketDataBus::instance().set_publish_enabled(false);
            INFO("MarketDataBus publishing DISABLED before process_market_data");
            run_consumption.primary_stage.reached = true;
            if (run_consumption.collecting()) run_consumption.primary.emplace();
            auto port_process_result = invoke_run_result(
                run_consumption.primary ? &run_consumption.primary.value() : nullptr,
                [&](PortfolioConsumptionTrace* output) {
                    return portfolio->process_market_data(all_bars, false, std::nullopt, output);
                });
            INFO("MarketDataBus publishing RE-ENABLED after process_market_data");
            MarketDataBus::instance().set_publish_enabled(true);
            run_consumption.observe_native_limits();
            if (port_process_result.is_error()) {
                std::cerr << "Failed to process data in portfolio manager: "
                          << port_process_result.error()->what() << std::endl;
                return 1;
            }
            run_consumption.primary_stage.completed = true;
            INFO("Portfolio processing completed");

            // ========================================
            // PHASE 4: PER-STRATEGY SIGNALS STORAGE
            // Extract and store signals from each strategy after portfolio processing
            // ========================================
            INFO("PHASE 4: Storing per-strategy signals to database...");

            for (const auto& strategy : strategies) {
                const auto& metadata = strategy->get_metadata();
                std::string strategy_name = metadata.id;

                // Try to extract signals from either TrendFollowingStrategy or
                // TrendFollowingFastStrategy
                std::unordered_map<std::string, double> signals_map;
                bool signals_extracted = false;

                // Try TrendFollowingStrategy first
                auto tf_strategy_ptr = std::dynamic_pointer_cast<TrendFollowingStrategy>(strategy);
                if (tf_strategy_ptr) {
                    // Get all instrument data (contains signals for all symbols)
                    const auto& all_instrument_data = tf_strategy_ptr->get_all_instrument_data();

                    // Extract signals (current_forecast) from instrument data
                    for (const auto& [symbol, data] : all_instrument_data) {
                        // Use current_forecast as the signal value
                        signals_map[symbol] = data.current_forecast;
                    }
                    signals_extracted = true;
                } else {
                    // Try TrendFollowingFastStrategy
                    auto tf_fast_ptr =
                        std::dynamic_pointer_cast<TrendFollowingFastStrategy>(strategy);
                    if (tf_fast_ptr) {
                        // Get all instrument data from fast strategy
                        const auto& all_instrument_data = tf_fast_ptr->get_all_instrument_data();

                        // Extract signals (current_forecast) from instrument data
                        for (const auto& [symbol, data] : all_instrument_data) {
                            signals_map[symbol] = data.current_forecast;
                        }
                        signals_extracted = true;
                    }
                }

                if (signals_extracted) {
                    INFO("DEBUG PHASE 4: Strategy '" + strategy_name + "' has " +
                         std::to_string(signals_map.size()) + " signals");

                    if (!signals_map.empty()) {
                        auto save_result = db->store_signals(
                            signals_map,
                            combined_strategy_id,  // Combined strategy_id for tier 2
                            strategy_name,         // Individual strategy_name for tier 3
                            portfolio_id,          // Portfolio identifier
                            now, "trading.signals");

                        if (save_result.is_error()) {
                            ERROR("Failed to store signals for strategy " + strategy_name + ": " +
                                  std::string(save_result.error()->what()));
                        } else {
                            INFO("Successfully stored " + std::to_string(signals_map.size()) +
                                 " signals for strategy: " + strategy_name);
                        }
                    } else {
                        WARN("No signals to store for strategy: " + strategy_name);
                    }
                } else {
                    WARN("Strategy " + strategy_name +
                         " does not support signal extraction (not TrendFollowing or "
                         "TrendFollowingFast)");
                }
            }

            // Extract per-strategy positions map first; build the combined
            // map below by summing across strategies (Σ qᵢ). See
            // live_portfolio.cpp for rationale — get_portfolio_positions()
            // would double-apply allocation. No-op here since conservative
            // is single-strategy, but kept for code parity.
            INFO("Extracting per-strategy positions from PortfolioManager...");
            strategy_positions_map = portfolio->get_strategy_positions();
            INFO("Building combined portfolio positions by per-strategy sum...");
            positions.clear();
            for (const auto& [_, pos_map] : strategy_positions_map) {
                for (const auto& [symbol, pos] : pos_map) {
                    auto it = positions.find(symbol);
                    if (it == positions.end()) {
                        positions[symbol] = pos;
                    } else {
                        it->second.quantity += pos.quantity;
                    }
                }
            }
            INFO("DEBUG: Retrieved " + std::to_string(strategy_positions_map.size()) +
                 " strategies from PortfolioManager");
        }  // End of if (!skip_strategy_processing) - strategy processing block

        BookTailInputs book_tail_inputs {
            app_config,
            base_strategy_config.max_leverage,
            db,
            combined_strategy_id,
            portfolio_id,
            strategy_names,
            strategy_allocations,
            strategy_positions_map,
            positions,
            now,
            now_tm,
            day_of_week,
            use_override_date,
            send_email,
            skip_strategy_processing,
            initial_capital,
            start_date,
            end_date,
            trading_snapshot,
            symbols,
            all_bars,
            risk_config,
            portfolio_config,
            coordinator_config,
            data_loader,
            metrics_calculator,
            results_manager,
            price_manager,
            pnl_manager,
            execution_manager,
            margin_manager,
            csv_exporter,
            strategy_instances_map,
            run_consumption,
            evidence_token,
            QT_STREAM
        };
        LivePortfolioTailCallbacks book_tail_callbacks(book_tail_inputs, selection,
            base_strategy_config, portfolio_cfg, registry_ptr, tf_strategy, tf_strategy_typed,
            latest_bars);
        return run_book_tail(book_tail_inputs, book_tail_callbacks);

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
