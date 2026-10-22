#include "trade_ngin/backtest/backtest_coordinator.hpp"
#include "trade_ngin/live/live_estimator_history.hpp"
#include "trade_ngin/live/live_roll_legs.hpp"
#include "trade_ngin/strategy/trend_estimator.hpp"
#include "trade_ngin/backtest/equity_cost_warmup.hpp"
#include "trade_ngin/backtest/junk_signal_feed.hpp"
#include <unordered_set>
#include <algorithm>
#include <iomanip>
#include <cstdlib>
#include <set>
#include <sstream>
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/run_id_generator.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/data/conversion_utils.hpp"
#include "trade_ngin/data/listing_dates.hpp"
#include "trade_ngin/data/market_data_bus.hpp"
#include "trade_ngin/portfolio/sizing_capital.hpp"
#include "trade_ngin/risk/risk_scale_report.hpp"
#include "trade_ngin/storage/backtest_results_manager.hpp"
#include "trade_ngin/strategy/base_strategy.hpp"
#include "trade_ngin/strategy/trend_following.hpp"
#include "trade_ngin/strategy/types.hpp"
#include "trade_ngin/transaction_cost/netting.hpp"

namespace trade_ngin {
namespace backtest {

namespace {
// Listing dates and declared id relabels (the identity without portfolio.json's listing_dates and
// instrument_id_relabels): the id rows as data/listing_dates.hpp reads them.
Result<std::vector<market_data_utils::FuturesInstrumentId>> with_predecessor_ids(
    const std::vector<std::string>& symbols,
    Result<std::vector<market_data_utils::FuturesInstrumentId>> ids) {
    return ListingDates::instance().read_ids(symbols, std::move(ids));
}
}  // namespace

BacktestCoordinator::BacktestCoordinator(std::shared_ptr<PostgresDatabase> db,
                                         InstrumentRegistry* registry,
                                         const BacktestCoordinatorConfig& config)
    : config_(config),
      db_(std::move(db)),
      registry_(registry),
      current_portfolio_value_(config.initial_capital) {}

BacktestCoordinator::~BacktestCoordinator() = default;

Result<void> BacktestCoordinator::initialize() {
    if (is_initialized_) {
        return Result<void>();
    }

    // Validate database connection
    auto conn_result = validate_connection();
    if (conn_result.is_error()) {
        return conn_result;
    }

    // Create all components
    auto create_result = create_components();
    if (create_result.is_error()) {
        return create_result;
    }

    is_initialized_ = true;
    INFO("BacktestCoordinator initialized successfully");
    return Result<void>();
}

Result<void> BacktestCoordinator::create_components() {
    // Create data loader
    data_loader_ = std::make_unique<BacktestDataLoader>(db_);

    // Create metrics calculator (stateless)
    metrics_calculator_ = std::make_unique<BacktestMetricsCalculator>();

    // Create price manager
    price_manager_ = std::make_unique<BacktestPriceManager>();

    // Create PnL manager
    pnl_manager_ = std::make_unique<BacktestPnLManager>(config_.initial_capital, *registry_);

    // Create execution manager
    BacktestExecutionConfig exec_config;
    execution_manager_ = std::make_unique<BacktestExecutionManager>(exec_config);

    return Result<void>();
}

Result<void> BacktestCoordinator::validate_connection() const {
    if (!db_) {
        return make_error<void>(ErrorCode::CONNECTION_ERROR, "Database interface is null",
                                "BacktestCoordinator");
    }

    if (!db_->is_connected()) {
        auto connect_result = db_->connect();
        if (connect_result.is_error()) {
            return make_error<void>(
                connect_result.error()->code(),
                "Failed to connect to database: " + std::string(connect_result.error()->what()),
                "BacktestCoordinator");
        }
    }

    return Result<void>();
}

Result<BacktestResults> BacktestCoordinator::run_single_strategy(
    std::shared_ptr<StrategyInterface> strategy, const std::vector<std::string>& symbols,
    const Timestamp& start_date, const Timestamp& end_date, AssetClass asset_class,
    DataFrequency data_freq) {
    // Ensure initialized
    if (!is_initialized_) {
        auto init_result = initialize();
        if (init_result.is_error()) {
            return make_error<BacktestResults>(init_result.error()->code(),
                                               init_result.error()->what(), "BacktestCoordinator");
        }
    }

    // Reset state
    reset();

    // Load market data
    DataLoadConfig load_config;
    load_config.symbols = symbols;
    load_config.start_date = start_date;
    load_config.end_date = end_date;
    load_config.asset_class = asset_class;
    load_config.data_freq = data_freq;

    auto data_result = data_loader_->load_market_data(load_config);
    if (data_result.is_error()) {
        return make_error<BacktestResults>(data_result.error()->code(), data_result.error()->what(),
                                           "BacktestCoordinator");
    }

    auto& all_bars = data_result.value();
    auto grouped_bars = data_loader_->group_bars_by_timestamp(all_bars);

    // Initialize tracking
    std::vector<ExecutionReport> all_executions;
    std::vector<std::pair<Timestamp, double>> equity_curve;
    std::vector<RiskResult> risk_metrics;
    std::map<std::pair<Timestamp, std::string>, double> signals;

    // Process each day
    int day_count = 0;
    for (const auto& [timestamp, bars] : grouped_bars) {
        bool is_warmup = (day_count < config_.warmup_days);

        auto process_result = process_day(timestamp, bars, strategy, all_executions, equity_curve,
                                          risk_metrics, is_warmup);

        if (process_result.is_error()) {
            WARN("Error processing day: " + std::string(process_result.error()->what()));
        }

        day_count++;
    }

    // Calculate final metrics
    auto results = metrics_calculator_->calculate_all_metrics(equity_curve, all_executions,
                                                              config_.warmup_days);

    // Copy executions to results
    results.executions = all_executions;
    results.equity_curve = equity_curve;

    // Copy final positions
    for (const auto& [symbol, pos] : current_positions_) {
        results.positions.push_back(pos);
    }

    INFO("Backtest completed: " + std::to_string(day_count) + " days processed, " +
         std::to_string(all_executions.size()) + " executions");

    return results;
}

Result<BacktestResults> BacktestCoordinator::run_portfolio(
    std::shared_ptr<PortfolioManager> portfolio, const std::vector<std::string>& symbols,
    const Timestamp& start_date, const Timestamp& end_date, AssetClass asset_class,
    DataFrequency data_freq) {
    // Validate portfolio
    if (!portfolio) {
        return make_error<BacktestResults>(ErrorCode::INVALID_ARGUMENT,
                                           "Null portfolio manager provided for backtest",
                                           "BacktestCoordinator");
    }

    // Ensure initialized
    if (!is_initialized_) {
        auto init_result = initialize();
        if (init_result.is_error()) {
            return make_error<BacktestResults>(init_result.error()->code(),
                                               init_result.error()->what(), "BacktestCoordinator");
        }
    }

    // Reset all state
    reset();
    reset_portfolio_state();
    // The session hold (T-7a C4) is the futures book's; the equity backtest keeps its old path.
    session_hold_enabled_ = (asset_class == AssetClass::FUTURES);
    // T-ROLLX-FIX: the oracle acceptance's record of what this futures run consumes
    // (consumed_series_record.hpp), only when the environment names a directory; it writes no log
    // line and changes nothing the run computes or stores.
    consumed_record_ = ConsumedSeriesRecord();
    if (session_hold_enabled_) {
        if (const char* dir = std::getenv("TRADE_NGIN_SERIES_DUMP_DIR"); dir != nullptr && *dir) {
            consumed_record_.enable(dir);
        }
    }
    risk_scale_report_enabled_ = (asset_class == AssetClass::FUTURES);
    size_on_equity_enabled_ = (asset_class == AssetClass::FUTURES);
    // K1 (T-7b-2 8c): the per-bar re-tier is the equity book's; futures roots keep their static
    // per-root cost configs.
    equity_cost_retier_enabled_ = (asset_class == AssetClass::EQUITIES);
    // COST-H3 (T-7b-2 8c): the futures book's cost managers read live's own-day basis.
    own_day_cost_feed_enabled_ = (asset_class == AssetClass::FUTURES);

    // Store backtest dates for later use in save_portfolio_results_to_db
    backtest_start_date_ = start_date;
    backtest_end_date_ = end_date;

    // The portfolio's risk modules see RiskContext::is_backtest = true, and the PortfolioManager nets
    // each bar's sleeve reports (K3), which here are the fills the backtest stores (T-7b-2 C8b4)
    if (portfolio) {
        portfolio->set_backtest_mode(true);
    }

    // Disable MarketDataBus publishing during data loading
    INFO("Disabling MarketDataBus publishing during data loading");
    MarketDataBus::instance().set_publish_enabled(false);

    // Load market data
    DataLoadConfig load_config;
    load_config.symbols = symbols;
    load_config.start_date = start_date;
    load_config.end_date = end_date;
    load_config.asset_class = asset_class;
    load_config.data_freq = data_freq;

    auto data_result = data_loader_->load_market_data(load_config);

    // Re-enable publishing
    MarketDataBus::instance().set_publish_enabled(true);
    INFO("Re-enabled MarketDataBus publishing");

    if (data_result.is_error()) {
        return make_error<BacktestResults>(data_result.error()->code(), data_result.error()->what(),
                                           "BacktestCoordinator");
    }

    auto& all_bars = data_result.value();
    auto grouped_bars = data_loader_->group_bars_by_timestamp(all_bars);

    if (equity_cost_retier_enabled_) {
        load_equity_cost_retier(symbols, start_date, end_date);
    }

    // T-7b-2 C10a (HD 2026-09-24 ruling 16): the session classifier's instrument-id continuity
    // limb reads each kept bar's vendor id, over the window the bars were loaded for (the query
    // the live runners read). The verdict of a bar reads no later bar, although this classifier
    // holds the cycle's group before it classifies the signal group. The ids are read from the
    // start of the classifier's history prefix (seed_estimator_history feeds its bars), as the
    // live runners read them, so the id limb judges the window's first bars as live does.
    if (session_hold_enabled_) {
        auto pg = std::dynamic_pointer_cast<PostgresDatabase>(db_);
        const auto feed = feed_instrument_ids(
            session_classifier_,
            pg ? with_predecessor_ids(
                     symbols,
                     pg->get_futures_instrument_ids(symbols, k01_classifier_history_start(start_date),
                                                    end_date))
               : make_error<std::vector<market_data_utils::FuturesInstrumentId>>(
                     ErrorCode::NOT_INITIALIZED, "the backtest's database is not a PostgresDatabase",
                     "BacktestCoordinator"));
        if (feed.fed) {
            INFO(feed.line);
        } else {
            WARN(feed.line);
        }
    }

    // The estimators' window reaches back W consumed bars before the first sized day: the bars
    // before the backtest's window are judged by a classifier of their own, K-01 applied, and
    // handed to the sleeves as history. They are no cycle: nothing is marked, sized or stored on
    // them, and the window's own classifier and roll status start at the window as before.
    if (session_hold_enabled_) {
        auto seeded = seed_estimator_history(portfolio, symbols, start_date, asset_class, data_freq);
        if (seeded.is_error()) {
            return make_error<BacktestResults>(seeded.error()->code(), seeded.error()->what(),
                                               "BacktestCoordinator");
        }
    }

    // Get portfolio config
    const auto& portfolio_config = portfolio->get_config();
    double initial_capital = static_cast<double>(portfolio_config.total_capital);

    // Initialize tracking
    std::vector<ExecutionReport> all_executions;
    std::vector<std::pair<Timestamp, double>> equity_curve;
    std::vector<RiskResult> risk_metrics;

    // Initialize equity curve with starting point
    equity_curve.emplace_back(start_date, initial_capital);

    // Generate run_id for position storage
    std::vector<std::string> strategy_names_for_id;
    for (const auto& strategy : portfolio->get_strategies()) {
        try {
            const auto& metadata = strategy->get_metadata();
            if (!metadata.id.empty()) {
                strategy_names_for_id.push_back(metadata.id);
            } else {
                strategy_names_for_id.push_back("TREND_FOLLOWING");
            }
        } catch (...) {
            strategy_names_for_id.push_back("TREND_FOLLOWING");
        }
    }

    current_run_id_ = generate_portfolio_run_id(strategy_names_for_id, end_date);
    INFO("Generated portfolio backtest run_id: " + current_run_id_);

    // Enable backtest mode on all strategies
    for (auto& strategy : portfolio->get_strategies()) {
        strategy->set_backtest_mode(true);
    }
    INFO("Backtest mode enabled on " + std::to_string(portfolio->get_strategies().size()) +
         " strategies");

    // Calculate warmup days dynamically from strategy lookbacks
    int calculated_warmup_days = calculate_warmup_days(portfolio->get_strategies());
    INFO("Calculated warmup days from strategies: " + std::to_string(calculated_warmup_days) +
         ", total available days: " + std::to_string(grouped_bars.size()));

    // Initialize CSV exporter
    csv_exporter_ = std::make_unique<BacktestCSVExporter>(config_.csv_output_path);
    auto csv_init_result = csv_exporter_->initialize_files();
    if (csv_init_result.is_error()) {
        WARN("Failed to initialize CSV exporter: " + std::string(csv_init_result.error()->what()));
        csv_exporter_.reset();
    } else {
        INFO("CSV exporter initialized, output: " + config_.csv_output_path);
    }

    // Track last saved date
    std::string last_saved_date;

    // Process bars in chronological order
    int day_index = 0;
    for (const auto& [timestamp, bars] : grouped_bars) {
        bool is_warmup = (day_index < calculated_warmup_days);

        try {
            auto process_result =
                process_portfolio_day(timestamp, bars, portfolio, all_executions, equity_curve,
                                      risk_metrics, is_warmup, initial_capital);

            if (process_result.is_error() && netting_refused_stop_) {
                // A ROLL or BORROW row carrying a netting adjustment fails the run on the day the
                // row is met: never a warning, never the equity carried flat and the run going on.
                ERROR(std::string(process_result.error()->what()));
                return make_error<BacktestResults>(process_result.error()->code(),
                                                   process_result.error()->what(),
                                                   "BacktestCoordinator");
            }
            if (process_result.is_error() && roll_leg_stop_) {
                // LOOP_SPEC v6.1 section 6.5 (X-3): a leg without a usable close fails the run.
                ERROR(std::string(process_result.error()->what()));
                return make_error<BacktestResults>(process_result.error()->code(),
                                                   process_result.error()->what(),
                                                   "BacktestCoordinator");
            }
            if (process_result.is_error()) {
                WARN("Portfolio data processing failed: " +
                     std::string(process_result.error()->what()));
                // Use previous value for equity curve
                if (!equity_curve.empty()) {
                    equity_curve.emplace_back(timestamp, equity_curve.back().second);
                }
            }
        } catch (const std::exception& e) {
            // F-3 (section 6.5, commit 5): the exception path stops on an owed roll exactly as
            // the error return above does; it never warns and goes on with the roll un-legged.
            if (auto stop = roll_owed_stop(e.what()); stop.is_error() || roll_leg_stop_) {
                const std::string what =
                    stop.is_error() ? std::string(stop.error()->what()) : std::string(e.what());
                ERROR(what);
                return make_error<BacktestResults>(ErrorCode::INVALID_DATA, what,
                                                   "BacktestCoordinator");
            }
            WARN("Exception processing portfolio data: " + std::string(e.what()));
            if (!equity_curve.empty()) {
                equity_curve.emplace_back(timestamp, equity_curve.back().second);
            }
        }

        // Save positions daily if storage is enabled (skip during warmup)
        if (!is_warmup && config_.store_trade_details && db_ && !bars.empty()) {
            auto time_t = std::chrono::system_clock::to_time_t(timestamp);
            std::stringstream date_ss;
            std::tm time_info;
            core::safe_gmtime(&time_t, &time_info);
            date_ss << std::put_time(&time_info, "%Y-%m-%d");
            std::string current_date = date_ss.str();

            if (current_date != last_saved_date) {
                auto save_result = save_daily_positions(portfolio, current_run_id_, timestamp);
                if (!save_result.is_error()) {
                    last_saved_date = current_date;
                }
            }
        }

        day_index++;
    }
    if (!consumed_record_.write()) {
        record_file::report_failure(consumed_record_.dir(),
                                    "a file of the consumed series record could not be written");
    }

    if (equity_cost_retier_enabled_) {
        INFO("EQUITY_COST_RETIER_SUMMARY cycles=" + std::to_string(equity_cost_retier_cycles_) +
             " tier_changes=" + std::to_string(equity_cost_retier_changes_) +
             " splits=" + std::to_string(equity_cost_retier_.split_count()) +
             ": every cycle re-tiered both cost managers from the 20 bars ending at its signal "
             "bar, in window-end share units");
    }

    // Sort executions by timestamp: a stable sort that keeps the stored sub-order inside a bar,
    // ROLL legs (closing, then opening, as inserted) before the bar's STRATEGY fills and the
    // BORROW rows after them (LOOP_SPEC v6.1 section 6.5); the trade statistics are order-dependent.
    auto type_rank = [](const ExecutionReport& e) {
        return e.execution_type == ExecutionType::ROLL       ? 0
               : e.execution_type == ExecutionType::STRATEGY ? 1
                                                             : 2;
    };
    std::stable_sort(all_executions.begin(), all_executions.end(),
                     [&](const ExecutionReport& a, const ExecutionReport& b) {
                         if (a.fill_time != b.fill_time) return a.fill_time < b.fill_time;
                         return type_rank(a) < type_rank(b);
                     });

    // Migration 018: the run's cost totals from the stored rows themselves (STRATEGY + ROLL +
    // BORROW: the sum the equity curve charged, each fill at its cost after netting), the ROLL
    // subset (a ROLL leg is never netted: its own cost) and the count of ROLL rows. Taken BEFORE
    // the metrics: a ROLL or BORROW row carrying a netting adjustment (a BORROW row is appended
    // after its cycle's sum, so this is where one is first seen) fails the run here, by name, as
    // an error result, and nothing of the run is stored.
    transaction_cost::RunCostTotals cost_totals;
    try {
        cost_totals = transaction_cost::run_cost_totals(all_executions);
    } catch (const transaction_cost::NettingRefused& e) {
        ERROR(std::string("NETTING STOP at the end of the run: ") + e.what());
        return make_error<BacktestResults>(
            ErrorCode::INVALID_DATA,
            std::string("NETTING STOP: ") + e.what() + ". Failing the run",
            "BacktestCoordinator");
    }

    // Calculate final metrics
    INFO("Calculating portfolio backtest metrics");
    auto results = metrics_calculator_->calculate_all_metrics(equity_curve, all_executions,
                                                              calculated_warmup_days);
    results.warmup_days = calculated_warmup_days;
    results.transaction_costs = cost_totals.transaction_costs;
    results.roll_costs = cost_totals.roll_costs;
    results.total_roll_fills = cost_totals.roll_fills;

    // Add executions and equity curve to results
    results.executions = std::move(all_executions);
    results.equity_curve = std::move(equity_curve);
    if (!equity_risk_detail_.empty()) {
        results.equity_risk_detail.assign(results.equity_curve.size(), std::string());
        for (const auto& [index, detail] : equity_risk_detail_) {
            if (index < results.equity_risk_detail.size()) results.equity_risk_detail[index] = detail;
        }
    }

    // Get final portfolio positions: sum per-strategy quantities (Σ qᵢ).
    // get_portfolio_positions() applies allocation a second time, producing
    // fractional contracts for multi-strategy portfolios. Strategies already
    // size for their capital slice, so the broker holds the simple sum.
    try {
        auto strategy_positions = portfolio->get_strategy_positions();
        std::unordered_map<std::string, Position> portfolio_positions;
        for (const auto& [_, pos_map] : strategy_positions) {
            for (const auto& [symbol, pos] : pos_map) {
                auto it = portfolio_positions.find(symbol);
                if (it == portfolio_positions.end()) {
                    portfolio_positions[symbol] = pos;
                } else {
                    it->second.quantity += pos.quantity;
                }
            }
        }
        results.positions.reserve(portfolio_positions.size());
        for (const auto& [_, pos] : portfolio_positions) {
            results.positions.push_back(pos);
        }
    } catch (const std::exception& e) {
        WARN("Exception getting final portfolio positions: " + std::string(e.what()));
    }

    // Finalize CSV exporter
    if (csv_exporter_) {
        csv_exporter_->finalize();
        INFO("CSV export finalized to: " + config_.csv_output_path);
    }

    INFO("Portfolio backtest completed: " + std::to_string(day_index) + " days processed, " +
         std::to_string(results.executions.size()) + " executions");

    return results;
}

// DEAD-bt-single-strategy-path: RULING = LEAVE (stage 3, T-1, 2026-09-09).
//
// This function is reachable only from run_single_strategy(), which no app and
// no test calls; the portfolio path (run_portfolio_backtest) is what every
// backtest runner uses. So it is unreachable today, and the ledger offered
// "leave, or class A delete of the dead overload".
//
// Left, for two reasons rather than inertia:
//
//  * run_single_strategy() is a documented public entry point -- src/strategy/
//    README.md shows it as the way to backtest one strategy -- so deleting it
//    removes an advertised API in a batch whose whole claim is that it changes
//    nothing, and the deletion would have to reach into a doc as well, which
//    this batch's commits do not do.
//  * unlike the LivePriceManager stubs removed in the same batch, this path is
//    not misleading: it does what its name says, and calling it would work. A
//    stub that silently returns nothing is a trap; an unused but correct
//    function is only unused.
//
// `is_warmup` is genuinely ignored inside, which is the part worth knowing if
// this is ever revived: the caller passes it, the body does not consult it, so
// warm-up days would be treated exactly like live days. Fix that before using
// this for anything.
Result<void> BacktestCoordinator::process_day(
    const Timestamp& timestamp, const std::vector<Bar>& bars,
    std::shared_ptr<StrategyInterface> strategy, std::vector<ExecutionReport>& executions,
    std::vector<std::pair<Timestamp, double>>& equity_curve, std::vector<RiskResult>& risk_metrics,
    [[maybe_unused]] bool is_warmup) {
    try {
        // BEGINNING-OF-DAY MODEL:
        // 1. Use previous day's bars for signal generation
        // 2. Execute at previous day's close prices

        if (has_previous_bars_) {
            // Pass previous day's bars to strategy for signal generation
            auto data_result = strategy->on_data(previous_bars_);
            if (data_result.is_error()) {
                return data_result;
            }

            // Get new target positions from strategy (unordered_map)
            const auto& strategy_positions = strategy->get_positions();

            // Convert to std::map for execution manager
            std::map<std::string, Position> new_positions(strategy_positions.begin(),
                                                          strategy_positions.end());

            // Generate executions at previous day's close prices
            auto new_executions = execution_manager_->generate_executions(
                current_positions_, new_positions, price_manager_->get_all_previous_day_prices(),
                timestamp);

            // Generate executions first, then notify strategy of fills
            for (const auto& exec : new_executions) {
                strategy->on_execution(exec);
                executions.push_back(exec);
            }

            // Update current positions AFTER on_execution so average_price
            // and unrealized_pnl reflect fill-updated values (not stale on_data values)
            const auto& updated_positions = strategy->get_positions();
            for (const auto& [symbol, pos] : updated_positions) {
                current_positions_[symbol] = pos;
            }
        }

        // Update prices with today's bars
        price_manager_->update_from_bars(bars);

        // Update transaction cost manager with market data for ADV and volatility tracking
        for (const auto& bar : bars) {
            double close = static_cast<double>(bar.close);
            double volume = static_cast<double>(bar.volume);

            // Get previous close for log return calculation
            double prev_close = 0.0;
            bool found_prev = false;
            for (const auto& prev_bar : previous_bars_) {
                if (prev_bar.symbol == bar.symbol) {
                    prev_close = static_cast<double>(prev_bar.close);
                    found_prev = true;
                    break;
                }
            }
            if (!found_prev) {
                // E2-F3: record the VOLUME anyway; omit only the return.
                //
                // This used to `continue`, dropping update_volume along with the log return.
                // Today's volume is a valid, correct observation regardless of whether a
                // prior bar exists, and ADV is what sizes market impact. Dropping it left a
                // 20-observation ADV window that systematically excluded Mondays for the ten
                // agricultural/livestock symbols, which have no Sunday session while the rest
                // of the universe does -- 6.7% of all symbol-days, but ~21% of trading days
                // for those ten. KE.v.0 sits on the 20,000 ADV bucket boundary (20,092 with
                // Mondays, 19,948 without), so its impact coefficient flipped 60 -> 80 bps
                // purely as a function of which days got counted.
                //
                // Passing prev_close = 0.0 is deliberate and sufficient: update_market_data
                // records volume unconditionally and gates update_log_returns on
                // `prev_close_price > 0.0`, so the return is omitted rather than fabricated.
                //
                // Do NOT restore `prev_close = close` (what main does). That injects a
                // log(close/close) = 0 return -- a false observation that biases the
                // volatility estimate downward on 1 day in 5 for the affected symbols.
                WARN("No T-1 bar found for " + bar.symbol +
                     " -- recording volume, omitting the return");
            }

            execution_manager_->update_market_data(bar.symbol, volume, close, prev_close);
        }

        // Calculate portfolio value using today's close-to-previous-close PnL
        double portfolio_value = calculate_portfolio_value(current_positions_, bars);
        current_portfolio_value_ = portfolio_value;

        // Update equity curve
        equity_curve.emplace_back(timestamp, portfolio_value);

        // Store previous bars for next iteration
        previous_bars_ = bars;
        has_previous_bars_ = true;

        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::UNKNOWN_ERROR,
                                std::string("Error processing day: ") + e.what(),
                                "BacktestCoordinator");
    }
}

Result<void> BacktestCoordinator::process_portfolio_day(
    const Timestamp& timestamp, const std::vector<Bar>& bars,
    std::shared_ptr<PortfolioManager> portfolio, std::vector<ExecutionReport>& executions,
    std::vector<std::pair<Timestamp, double>>& equity_curve,
    std::vector<RiskResult>& /*risk_metrics*/, bool is_warmup, double initial_capital) {
    // risk_metrics is never written: its only writer was the coordinator's own risk gate,
    // which read a risk_manager_ nothing ever assigned, and has been deleted.
    cycle_rolls_owed_.clear();
    try {
        // BEGINNING-OF-DAY MODEL FOR PORTFOLIO BACKTEST:
        // - Use previous day's bars for signal generation via PortfolioManager
        // - Use today's bars for executions' slippage/valuation and equity curve

        // Check for empty data
        if (bars.empty()) {
            return make_error<void>(ErrorCode::MARKET_DATA_ERROR,
                                    "Empty market data provided for portfolio backtest",
                                    "BacktestCoordinator");
        }

        // T-7a C4: every group enters the session classifier as it arrives, so a group is
        // classified later (as the signal group) against strictly earlier bars only.
        if (session_hold_enabled_) session_classifier_.add_bars(bars);

        // T-ROLLX-FIX (LOOP_SPEC v6.1 sections 2.1, 6.6): this cycle's bar group as the marks
        // consume it. A WITHHELD bar (K-01) books no P&L and does not move the symbol's previous
        // close, so its next consumed bar books against the last consumed close. Its verdict reads
        // no later bar, so it is the verdict the next cycle's signal feed acts on.
        mark_withheld_.clear();
        mark_change_.clear();
        if (session_hold_enabled_) {
            for (const auto& v : classify_bar_group(session_classifier_, bars)) {
                if (v.k01_withheld()) mark_withheld_.insert(v.symbol);
            }
        }

        // K1 (T-7b-2 8c; T-4b BT-cost-tier-warmup): before this group reaches the cost models or
        // the PortfolioManager, both cost managers are re-tiered from the 20 bars ending at the
        // PREVIOUS group, the signal bar whose close prices this cycle's fills (live re-tiers on
        // every run from the 20 bars ending at T-1), in the window-end share unit
        // (equity_cost_retier.hpp). Then this group joins the trailing windows.
        if (equity_cost_retier_enabled_) {
            const auto pass =
                equity_cost_retier_.retier(execution_manager_->get_transaction_cost_manager(),
                                           portfolio->get_transaction_cost_manager());
            ++equity_cost_retier_cycles_;
            for (const auto& c : pass.changes) {
                ++equity_cost_retier_changes_;
                INFO("EQUITY_COST_RETIER date=" + core::format_utc_date(timestamp) +
                     " symbol=" + c.symbol + " tier " + c.from + "->" + c.to +
                     " adv=" + std::to_string(c.adv) + " bars=" + std::to_string(c.bars) +
                     " window=" + c.first_bar + ".." + c.last_bar +
                     " (split-consistent shares; both cost managers)");
            }
            equity_cost_retier_.append(bars);
        }

        // If this is the first bar set, initialize previous_bars and return early
        // to avoid processing day 1 twice (directly + as "previous bars" on day 2)
        bool had_previous_bars = portfolio_has_previous_bars_;
        if (!portfolio_has_previous_bars_) {
            portfolio_previous_bars_ = bars;
            portfolio_has_previous_bars_ = true;
            price_manager_->update_from_bars(bars);
            equity_curve.emplace_back(timestamp, initial_capital);
            return Result<void>();  // Early return, don't process day 1
        }

        // Update transaction cost manager with market data for ADV and volatility tracking.
        // COST-H3 (T-7b-2 8c): the futures book (own_day_cost_feed_enabled_) is fed below from the
        // signal feed, live's basis. C8c4 (HD 2026-09-25 ruling 26): the equity book is fed its
        // SIGNAL group (T-1, the bar whose close prices this cycle's fills), with each bar's return
        // against the group fed on the previous cycle, so its 20-bar ADV and its volatility window
        // end at T-1 as the live equity runner's do (LiveDailyCycle::feed_cost_model); it used to be
        // fed this cycle's own group, day T, before its fills were priced (a one-bar look-ahead).
        static const std::vector<Bar> kNotFedHere;
        const std::vector<Bar>& signal_group_cost_feed =
            own_day_cost_feed_enabled_ ? kNotFedHere : portfolio_previous_bars_;
        for (const auto& bar : signal_group_cost_feed) {
            double close = static_cast<double>(bar.close);
            // K1 (T-7b-2 8c; T-4b ADVERSARIAL A-3): an equity's volume in the window-end share
            // unit, so the ADV that scales participation is in the unit of the traded quantity and
            // of the tier's ADV (live's two ADVs are the same twenty observations).
            double volume = equity_cost_retier_enabled_
                                ? equity_cost_retier_.split_consistent_volume(bar)
                                : static_cast<double>(bar.volume);

            // Get previous close for log return calculation
            double prev_close = 0.0;
            bool found_prev = false;
            for (const auto& prev_bar : cost_feed_previous_group_) {
                if (prev_bar.symbol == bar.symbol) {
                    prev_close = static_cast<double>(prev_bar.close);
                    found_prev = true;
                    break;
                }
            }
            if (!found_prev) {
                // E2-F3: record the VOLUME anyway; omit only the return.
                //
                // This used to `continue`, dropping update_volume along with the log return.
                // Today's volume is a valid, correct observation regardless of whether a
                // prior bar exists, and ADV is what sizes market impact. Dropping it left a
                // 20-observation ADV window that systematically excluded Mondays for the ten
                // agricultural/livestock symbols, which have no Sunday session while the rest
                // of the universe does -- 6.7% of all symbol-days, but ~21% of trading days
                // for those ten. KE.v.0 sits on the 20,000 ADV bucket boundary (20,092 with
                // Mondays, 19,948 without), so its impact coefficient flipped 60 -> 80 bps
                // purely as a function of which days got counted.
                //
                // Passing prev_close = 0.0 is deliberate and sufficient: update_market_data
                // records volume unconditionally and gates update_log_returns on
                // `prev_close_price > 0.0`, so the return is omitted rather than fabricated.
                //
                // Do NOT restore `prev_close = close` (what main does). That injects a
                // log(close/close) = 0 return -- a false observation that biases the
                // volatility estimate downward on 1 day in 5 for the affected symbols.
                WARN("No T-1 bar found for " + bar.symbol +
                     " -- recording volume, omitting the return");
            }

            execution_manager_->update_market_data(bar.symbol, volume, close, prev_close);

            // Also update portfolio's cost manager for execution cost calculation
            portfolio->update_cost_manager_market_data(bar.symbol, volume, close, prev_close);
        }
        if (!own_day_cost_feed_enabled_) cost_feed_previous_group_ = portfolio_previous_bars_;

        // Track strategy execution counts BEFORE processing (for commission calculation)
        std::unordered_map<std::string, size_t> strategy_exec_counts_before;
        if (had_previous_bars && !is_warmup) {
            auto strategy_execs_before = portfolio->get_strategy_executions();
            for (const auto& [strategy_id, execs] : strategy_execs_before) {
                strategy_exec_counts_before[strategy_id] = execs.size();
            }
        }

        // Process market data through portfolio manager
        // Use previous day's bars for signal generation
        const auto& bars_for_signals = had_previous_bars ? portfolio_previous_bars_ : bars;

        // The backtest predicate (T-7a C4; T-4c J1 on the classifier): a symbol trades on this
        // cycle only when its bar in the signal group is a SESSION. A symbol with no bar there,
        // or a JUNK bar, gets no fill and no book change (the PM holds it at its filled ledger).
        std::unordered_set<std::string> signal_group_sessions;
        const std::unordered_set<std::string>* session_symbols = nullptr;
        // LOOP_SPEC v6.1 section 2.1 (K-01, LOCKED; it supersedes T-7b-1 7a's one-cycle delayed
        // feed): a JUNK bar and a thin first print of the signal group are WITHHELD. They leave this
        // cycle's feed and are never fed later, so no consumer (the strategies, the
        // PortfolioManager's history, the cost models fed below, the roll status) ever consumes
        // them, and the next consumed bar's return is taken against the last consumed close. An
        // unconfirmed instrument-id change the classifier holds is NOT withheld: it is the change bar
        // (section 2.2), consumed and held. Every non-SESSION symbol is held on this cycle (no fill,
        // no book change). Warm-up included, as the hold is.
        const std::vector<Bar>* signal_feed = &bars_for_signals;
        std::vector<Bar> k01_feed;
        // The symbols whose bar fed on this cycle CONFIRMED a roll, with that bar's date: their legs
        // are booked on this cycle, once (section 6.5).
        std::map<std::string, std::string> confirmed_now;
        if (session_hold_enabled_ && had_previous_bars) {
            std::set<std::string> withheld_symbols;
            const std::vector<SymbolDayVerdict> verdicts =
                classify_bar_group(session_classifier_, bars_for_signals);
            for (const auto& v : verdicts) {
                if (!is_warmup && !v.id_note.empty()) {
                    INFO("BT_SESSION_CLASSIFIER INSTRUMENT_ID " + v.symbol + " " + v.date + ": " +
                         v.id_note);
                }
                if (v.is_session()) {
                    signal_group_sessions.insert(v.symbol);
                } else {
                    if (v.k01_withheld()) withheld_symbols.insert(v.symbol);
                    if (!is_warmup) {
                        INFO("BT_SESSION_CLASSIFIER JUNK " + v.symbol + " " + v.date + ": " +
                             v.reason + " -- no fill and no book change on this cycle");
                    }
                }
            }
            session_symbols = &signal_group_sessions;
            if (!withheld_symbols.empty()) {
                const std::string cycle = "(signal group of " +
                                          SessionClassifier::ymd(SessionClassifier::day_of(timestamp)) +
                                          ", warmup=" + (is_warmup ? "1" : "0") + "): ";
                auto k01 = k01_signal_feed(bars_for_signals, withheld_symbols);
                for (const auto& b : k01.withheld) {
                    INFO("BT_JUNK_FEED withheld " + b.symbol + " " +
                         SessionClassifier::ymd(SessionClassifier::day_of(b.timestamp)) + " " + cycle +
                         "kept out of the strategies, the PortfolioManager's history and the cost "
                         "models; never fed later (LOOP_SPEC v6.1 section 2.1, K-01)");
                }
                k01_feed = std::move(k01.feed);
                signal_feed = &k01_feed;
                consumed_record_.add_cycle(
                    SessionClassifier::ymd(SessionClassifier::day_of(bars_for_signals.front().timestamp)),
                    verdicts, k01.withheld, *signal_feed);
            } else {
                consumed_record_.add_cycle(
                    SessionClassifier::ymd(SessionClassifier::day_of(bars_for_signals.front().timestamp)),
                    verdicts, {}, *signal_feed);
            }
            // LOOP_SPEC v6.1 sections 2.1, 2.2 (D37): each symbol's roll status on its CONSUMED
            // sequence (the bars fed this cycle; a withheld bar never walks it), one bar at a time. A
            // symbol whose LAST consumed bar is pending (a change bar, either bar of a flip, an id-less
            // bar inside a pending roll) is HELD at this rebalance: out of the session set, so the
            // PortfolioManager fixes it at its filled quantity, counts it at that quantity and books no
            // fill in it. The status persists across cycles that consume no bar of the symbol (the hold
            // covers them); the confirming bar ends it. Warm-up included, as the session hold is.
            std::set<std::string> fed_now;
            for (const auto& b : *signal_feed) {
                signal_roll_status_[b.symbol] =
                    roll_trackers_[b.symbol].add(b.instrument_id, static_cast<double>(b.close));
                fed_now.insert(b.symbol);
                if (signal_roll_status_[b.symbol].confirm) {
                    confirmed_now[b.symbol] =
                        SessionClassifier::ymd(SessionClassifier::day_of(b.timestamp));
                }
            }
            for (const auto& [symbol, st] : signal_roll_status_) {
                if (!st.holds()) continue;
                signal_group_sessions.erase(symbol);
                if (is_warmup) continue;
                INFO("CHANGE_BAR_HOLD " + symbol + " date=" + core::format_utc_date(timestamp) +
                     " kind=" + (st.flip ? "flip_revert" : st.change ? "pending_change" : "idless_pending") +
                     " held_id=" + st.held_id +
                     ": the last consumed bar is pending; held at the filled quantity, no fill");
                if (st.flip && fed_now.count(symbol)) {
                    INFO("FLIP_PAIR " + symbol + " date=" +
                         core::format_utc_date(bars_for_signals.front().timestamp) +
                         " held_id=" + st.held_id + " bars=" + std::to_string(st.bars_pending + 1) +
                         ": the id returned to the held contract; no legs, every bar of it held and "
                         "its returns excluded");
                }
            }
        }

        // COST-H3 (T-7b-2 8c; T-VOL §4 proved the parent's order: day T's bar reached both cost
        // managers before the book was sized on the T-1 group and filled at the T-1 close). The
        // futures book's two managers are fed the cycle's SIGNAL feed, the bars the strategies and
        // the PortfolioManager are fed below (a JUNK bar withheld on its cycle, as live's strategy
        // feed withholds it), on live's basis (futures_cost_feed.hpp): each symbol's own-day volume
        // (its signal bar's) is the impact model's only observation, and its returns walk ends at
        // that bar. So the optimizer's cost vector and every fill's cost read what a live run with
        // this T-1 reads, and nothing of day T.
        if (own_day_cost_feed_enabled_) {
            // C8c3 (HD 2026-09-25 rulings 25 and 28): the participation volume is the weekend
            // merge's on this cycle's fill day (futures_cost_feed.hpp, rules 1-3): a weekday
            // signal bar takes the weekend stub(s) right before it; a weekend signal bar on a
            // weekday cycle is priced on the last session before the stub(s) plus the stub(s).
            const auto fed = feed_futures_cost_model_step(
                execution_manager_->get_transaction_cost_manager(), *signal_feed, timestamp,
                execution_cost_carry_);
            feed_futures_cost_model_step(portfolio->get_transaction_cost_manager(), *signal_feed,
                                         timestamp, portfolio_cost_carry_);
            size_t merged_symbols = 0;
            size_t stub_signal_symbols = 0;
            double merged_volume = 0.0;
            double previous_session_volume = 0.0;
            auto count_merge = [&](const FuturesCostFeedSymbol& s) {
                if (s.merged_weekend_bars == 0) return;
                ++merged_symbols;
                merged_volume += s.merged_weekend_volume;
                if (futures_cost_feed_detail::is_weekend_day(s.own_day_time)) {
                    ++stub_signal_symbols;
                    previous_session_volume += s.previous_session_volume;
                }
            };
            for (const auto& s : fed.symbols) count_merge(s);
            for (const auto& s : fed.reevaluated) count_merge(s);
            if (!is_warmup && (merged_symbols > 0 || !fed.reevaluated.empty())) {
                INFO("BT_COST_FEED_WEEKEND_MERGE date=" + core::format_utc_date(timestamp) +
                     " symbols=" + std::to_string(merged_symbols) +
                     " weekend_volume=" + std::to_string(merged_volume) +
                     " stub_signal_symbols=" + std::to_string(stub_signal_symbols) +
                     " previous_session_volume=" + std::to_string(previous_session_volume) +
                     " reevaluated=" + std::to_string(fed.reevaluated.size()) +
                     ": a weekday signal bar's participation volume includes its symbol's "
                     "weekend stub; a stub signal bar on a weekday cycle is priced on the last "
                     "session plus the stub");
            }
            if (!is_warmup) {
                INFO("BT_COST_FEED date=" + core::format_utc_date(timestamp) + " signal_group=" +
                     (bars_for_signals.empty()
                          ? std::string("none")
                          : core::format_utc_date(bars_for_signals.front().timestamp)) +
                     " symbols=" + std::to_string(fed.symbols.size()) +
                     " returns=" + std::to_string(fed.returns_fed) +
                     ": both cost managers read each symbol's own-day volume and the returns "
                     "walk ending at its signal bar (live's basis)");
            }
        }

        // LOOP_SPEC section 3.1 (D19, half compounding): the book is sized on the starting capital
        // less the drawdown of the cumulative net P&L from its running peak, never above the
        // starting capital. The settled history is the equity curve's own rows up to its LAST row,
        // the previous cycle's (this cycle's row is appended below, after its fills and its marks);
        // it is recomputed from the curve on every cycle and nothing persists it. Every sizing
        // input follows it (PortfolioManager::set_sizing_capital). Warm-up rows are flat at the
        // initial capital, so warm-up sizes on it and is not logged.
        if (size_on_equity_enabled_) {
            const HalfCompounding sizing = backtest_half_compounding(equity_curve, initial_capital);
            cycle_account_value_ = sizing.account;
            auto sized = portfolio->set_sizing_capital(sizing.capital);
            if (sized.is_error()) {
                return sized;
            }
            if (!is_warmup) {
                INFO("SIZING_CAPITAL date=" + core::format_utc_date(timestamp) +
                     " capital=" + std::to_string(sizing.capital) +
                     " account=" + std::to_string(sizing.account) +
                     " peak=" + std::to_string(sizing.peak) + " settled_through=" +
                     (equity_curve.empty() ? std::string("none")
                                           : core::format_utc_date(equity_curve.back().first)) +
                     " source=equity_curve");
            }
        }

        // Section 6.5: the held book at the START of the bar, per sleeve, the quantity the roll legs
        // are booked at. The book is the FILLED one: a sleeve's target that warm-up set and no fill
        // stands behind is not held, and a roll confirmed on the first traded cycle has no leg.
        const auto start_of_bar_book = confirmed_now.empty()
                                           ? std::unordered_map<std::string, std::unordered_map<std::string, Position>>{}
                                           : portfolio->get_filled_strategy_positions();
        // F-3 (commit 5): the rolls owed from here until the legs are booked below (the tracker has
        // consumed their confirming bars); the day's catch and the run loop's read it.
        if (!is_warmup) {
            for (const auto& [symbol, confirm_date] : confirmed_now) {
                for (const auto& [strategy_id, book] : start_of_bar_book) {
                    const auto held = book.find(symbol);
                    if (held == book.end() ||
                        std::abs(static_cast<double>(held->second.quantity)) < 1e-9) {
                        continue;
                    }
                    cycle_rolls_owed_.push_back(symbol + " (" + strategy_id + ") confirmed " +
                                                confirm_date);
                }
            }
        }

        if (signal_feed->empty()) {
            // Every bar of the signal group is JUNK and nothing is carried: live's feed would hold
            // no new bar either, so the strategies' signals and the book stay where they are.
            INFO("BT_JUNK_FEED the signal group of " +
                 SessionClassifier::ymd(SessionClassifier::day_of(timestamp)) +
                 " holds only JUNK bars: nothing is fed to the strategies or the PortfolioManager "
                 "on this cycle");
            if (consumed_record_.enabled() && had_previous_bars) {
                portfolio->record_no_pass(
                    SessionClassifier::ymd(SessionClassifier::day_of(bars_for_signals.front().timestamp)),
                    timestamp, is_warmup);
            }
        } else {
            auto data_result = portfolio->process_market_data(*signal_feed, is_warmup, timestamp,
                                                              session_symbols);
            if (data_result.is_error()) {
                // F-3 (section 6.5): the tracker has consumed this cycle's confirming bars, so a
                // cycle that returns here would leave a held symbol's roll un-legged for good. A
                // STOP, never a loss.
                if (!is_warmup) {
                    for (const auto& [symbol, confirm_date] : confirmed_now) {
                        for (const auto& [strategy_id, book] : start_of_bar_book) {
                            const auto held = book.find(symbol);
                            if (held == book.end() ||
                                std::abs(static_cast<double>(held->second.quantity)) < 1e-9) {
                                continue;
                            }
                            roll_leg_stop_ = true;
                            return make_error<void>(
                                ErrorCode::INVALID_DATA,
                                "ROLL_LEG STOP " + symbol + " (" + strategy_id +
                                    "): the cycle failed (" + data_result.error()->what() +
                                    ") and its roll confirmed " + confirm_date +
                                    " would not be legged. Failing the run",
                                "BacktestCoordinator");
                        }
                    }
                }
                return data_result;
            }
        }

        // LOOP_SPEC v6.1 section 6.5: the two ROLL legs of every roll the signal feed CONFIRMED on
        // this cycle, per sleeve holding the symbol at the start of the bar, inserted ahead of this
        // bar's STRATEGY fills (stored order: closing leg, opening leg, then the fills), priced by the
        // PortfolioManager's cost model (the model the stored fills use), STRICT: a leg without a
        // usable close fails the run (X-3). Post-warm-up only (warm-up clears every execution).
        if (!is_warmup && had_previous_bars) {
            // The cost model's inputs on each ROLL_LEG line (ADV, volatility multiplier), so a reader can
            // recompute the leg's implicit cost.
            auto& cost_model = portfolio->get_transaction_cost_manager();
            auto model_input = [](double x) {
                std::ostringstream o;
                o << std::setprecision(12) << x;
                return o.str();
            };
            for (const auto& [symbol, confirm_date] : confirmed_now) {
                const auto& st = signal_roll_status_.at(symbol);
                INFO("ROLL_CONFIRMED " + symbol + " date=" + confirm_date + " " + st.previous_held_id +
                     "->" + st.held_id + " closing_px=" + std::to_string(st.last_close_before_change) +
                     " opening_px=" + std::to_string(st.change_bar_close) +
                     " change_bars=" + std::to_string(st.bars_pending) +
                     ": the next consumed bar kept the new id; the legs are booked on this cycle");
                for (const auto& [strategy_id, book] : start_of_bar_book) {
                    const auto held = book.find(symbol);
                    if (held == book.end()) continue;
                    const double q = static_cast<double>(held->second.quantity);
                    if (std::abs(q) < 1e-9) continue;
                    size_t& seq = roll_leg_seq_[strategy_id];
                    const std::string id_close = "RL-" + strategy_id + "-" + std::to_string(seq);
                    const std::string id_open = "RL-" + strategy_id + "-" + std::to_string(seq + 1);
                    std::vector<ExecutionReport> legs;
                    try {
                        legs = roll_series::make_roll_legs(
                            symbol, q, st.last_close_before_change, st.change_bar_close,
                            st.previous_held_id, st.held_id, timestamp, id_close, id_close, id_open,
                            id_open, [&](const std::string& s, double signed_q, double px) {
                                const auto c = cost_model.calculate_costs(s, signed_q, px);
                                return roll_series::RollLegCost{c.commissions_fees,
                                                                c.implicit_price_impact,
                                                                c.slippage_market_impact,
                                                                c.total_transaction_costs};
                            });
                    } catch (const std::exception& e) {
                        roll_leg_stop_ = true;
                        return make_error<void>(ErrorCode::INVALID_DATA,
                                                "ROLL_LEG STOP " + symbol + " (" + strategy_id +
                                                    "): " + e.what() +
                                                    ". Failing the run: a leg without a usable close",
                                                "BacktestCoordinator");
                    }
                    seq += 2;
                    const size_t at = strategy_exec_counts_before.count(strategy_id)
                                          ? strategy_exec_counts_before.at(strategy_id)
                                          : 0;
                    portfolio->insert_executions_at(strategy_id, at, legs);
                    for (const auto& leg : legs) {
                        INFO("ROLL_LEG " + strategy_id + " " + symbol + " " +
                             (leg.exec_id == id_close ? "RC" : "RO") + " " +
                             (leg.side == Side::BUY ? "BUY" : "SELL") + " qty=" +
                             std::to_string(static_cast<double>(leg.filled_quantity)) + " px=" +
                             std::to_string(static_cast<double>(leg.fill_price)) + " instrument=" +
                             leg.instrument_id + " cost=" +
                             std::to_string(static_cast<double>(leg.total_transaction_costs)) +
                             " adv=" + model_input(cost_model.get_adv(symbol)) + " vol_mult=" +
                             model_input(cost_model.get_volatility_multiplier(symbol)) +
                             " date=" + core::format_utc_date(timestamp) + " confirmed=" +
                             confirm_date +
                             " (an upper bound: two outright legs; realised 0; outside netting)");
                    }
                }
            }
        }
        cycle_rolls_owed_.clear();  // booked (or none owed): a later failure of the cycle is not F-3's

        // WARMUP HANDLING: keep equity flat, no executions
        if (is_warmup) {
            // Clear any executions that might have been generated
            portfolio->clear_all_executions();

            // Update previous close prices for first post-warmup day (a withheld bar is never
            // a previous close: K-01)
            std::unordered_map<std::string, double> warmup_closes;
            for (const auto& bar : bars) {
                if (mark_withheld_.count(bar.symbol)) continue;
                warmup_closes[bar.symbol] = static_cast<double>(bar.close);
            }
            pnl_manager_->update_previous_closes(warmup_closes);

            // Keep equity flat during warmup
            equity_curve.emplace_back(timestamp, initial_capital);

            // Update previous bars for next iteration
            portfolio_previous_bars_ = bars;
            price_manager_->update_from_bars(bars);

            return Result<void>();
        }

        // POST-WARMUP: Normal trading logic

        // RA-01 (T-7b-1 C7): the rebalance's applied risk scale, once per post-warmup cycle, from
        // the PortfolioManager's own record of the call above (risk_scale_report.hpp defines each
        // field). reporter=na: the backtest has no snapshot RiskManager and stores no risk figure.
        // date = this cycle's timestamp (UTC), the date its fills and equity row carry. Log only.
        // A cycle whose signal group held only JUNK bars never called process_market_data, so the
        // PM's record is still the previous cycle's: this cycle ran no risk lap and reports an
        // empty record (laps=0) rather than repeating yesterday's.
        if (risk_scale_report_enabled_) {
            const std::vector<RiskDecisionRecord> this_cycle =
                signal_feed->empty() ? std::vector<RiskDecisionRecord>{}
                                     : portfolio->last_risk_decisions();
            INFO(format_risk_scale_report(std::string("na"), summarize_applied_risk(this_cycle),
                                          core::format_utc_date(timestamp)));
            // T-7b-2 C9a (T-VOL C4): the delivered cut beside the request, from the same call's
            // measurement (risk_scale_report.hpp defines each field); an all-JUNK cycle ran no
            // rebalance and reports the empty measurement (every figure na). Log only.
            INFO(format_risk_delivered(
                summarize_applied_risk(this_cycle),
                signal_feed->empty() ? DeliveredCut{} : portfolio->last_delivered_cut(),
                core::format_utc_date(timestamp), "capped_target_gross"));
        }

        std::vector<ExecutionReport> period_executions;

        if (had_previous_bars) {
            try {
                // Use per-strategy executions (the same accurate data saved to DB)
                // instead of portfolio-level phantom executions
                auto all_strategy_execs = portfolio->get_strategy_executions();
                for (const auto& [strategy_id, strat_execs] : all_strategy_execs) {
                    // Only take new executions since last check
                    size_t prev_count = strategy_exec_counts_before.count(strategy_id) > 0
                                            ? strategy_exec_counts_before.at(strategy_id)
                                            : 0;
                    for (size_t i = prev_count; i < strat_execs.size(); ++i) {
                        period_executions.push_back(strat_execs[i]);
                    }
                }
            } catch (const std::exception& e) {
                WARN("Exception getting strategy executions: " + std::string(e.what()));
                period_executions.clear();
            }
        }

        // The period's fills are reported as the PortfolioManager stored them (T-7b-3 R-2): these
        // reports are the rows save_portfolio_results_to_db writes to backtest.executions and the
        // costs the equity curve charges (calculate_period_transaction_costs), priced by the
        // PortfolioManager's cost model and netted per symbol-day with that same model (K3), and
        // stamped with this cycle's timestamp (process_market_data's current_timestamp). They are
        // not re-priced here: a second model's costs beside the first model's netting_adjustment
        // would break "the net costs of a symbol-day sum to C(Q)" on the rows the trade statistics
        // and the strategies' on_execution read, and would part them from the stored rows.
        executions.insert(executions.end(), period_executions.begin(), period_executions.end());

        // Feed executions back to strategies
        for (const auto& exec : period_executions) {
            try {
                for (auto strategy_ptr : portfolio->get_strategies()) {
                    auto execution_result = strategy_ptr->on_execution(exec);
                    if (execution_result.is_error()) {
                        WARN("Failed to process execution for strategy: " +
                             execution_result.error()->to_string());
                    }
                }
            } catch (const std::exception& e) {
                WARN("Exception feeding execution to strategies: " + std::string(e.what()));
            }
        }

        // PNL CALCULATION (SINGLE SOURCE OF TRUTH via pnl_manager_)
        double total_portfolio_pnl = 0.0;

        // Build current close prices map from bars
        std::unordered_map<std::string, double> current_close_prices;
        for (const auto& bar : bars) {
            current_close_prices[bar.symbol] = static_cast<double>(bar.close);
        }

        // T-ROLLX-FIX (LOOP_SPEC v6.1 sections 2.2, 6.6, 7): each consumed bar of this group read
        // against its symbol's roll status (the trackers hold every consumed bar before it: the
        // signal feed walked them above), without moving the tracker (the next cycle's signal
        // feed walks this bar). A change bar (a roll's switch day or either bar of a flip) books
        // no P&L: the move onto it is the splice's price gap, not the held contract's move. Every
        // row stored for this cycle carries the contract held after this bar (no-bar and withheld
        // rows: after the symbol's last consumed bar).
        if (session_hold_enabled_) {
            row_held_id_.clear();
            for (const auto& [symbol, tracker] : roll_trackers_) row_held_id_[symbol] = tracker.held_id();
            for (const auto& bar : bars) {
                if (mark_withheld_.count(bar.symbol)) continue;
                roll_series::RollTracker probe = roll_trackers_[bar.symbol];
                const auto st = probe.add(bar.instrument_id, static_cast<double>(bar.close));
                if (st.change) mark_change_.insert(bar.symbol);
                row_held_id_[bar.symbol] = st.held_id;
            }
            if (consumed_record_.enabled()) {
                std::vector<ConsumedSeriesRecord::FinalMark> marks;
                for (const auto& bar : bars) {
                    ConsumedSeriesRecord::FinalMark m;
                    m.symbol = bar.symbol;
                    m.date = SessionClassifier::ymd(SessionClassifier::day_of(bar.timestamp));
                    m.close = static_cast<double>(bar.close);
                    m.instrument_id = bar.instrument_id;
                    m.withheld = mark_withheld_.count(bar.symbol) > 0;
                    m.change = mark_change_.count(bar.symbol) > 0;
                    m.held_id = row_held_id_[bar.symbol];
                    marks.push_back(std::move(m));
                }
                consumed_record_.set_final_marks(std::move(marks));
            }
        }

        // Calculate transaction costs from per-strategy executions
        double total_transaction_costs =
            calculate_period_transaction_costs(portfolio, strategy_exec_counts_before);

        // Calculate PnL for each strategy using its individual quantities
        auto strategy_positions = portfolio->get_strategy_positions();

        // Phase 4 §1.14: build per-strategy PnL accounting method lookup so
        // we can branch on REALIZED_ONLY (futures: daily MTM IS realized) vs
        // MIXED/UNREALIZED_ONLY (equities: realized_pnl only on actual close,
        // written by on_execution -- coordinator must NOT stamp realized_pnl
        // here for equities).
        std::unordered_map<std::string, PnLAccountingMethod> pnl_method_by_strategy;
        // E2-F8: and a lookup of each strategy's OWN fill-maintained holdings, which is
        // where a real cost basis lives.
        //
        // The positions this loop iterates come from get_strategy_positions() ==
        // info.current_positions == the TARGET positions the strategy produced, and
        // MeanReversionStrategy::get_target_positions() sets
        // `pos.average_price = inst_data.current_price` (mean_reversion.cpp:214) -- a MARK,
        // not a basis. Measured: the mark implied by day D's stored unrealized equals day
        // D+1's stored average_price on ~87% of consecutive rows, i.e. the column was a
        // one-day mark change rather than a position-lifetime unrealized.
        //
        // BaseStrategy::on_execution() is the sole legitimate writer of a volume-weighted
        // basis (base_strategy.cpp:216-252) and keeps it in positions_, which
        // get_target_positions() deliberately does NOT read. on_execution has already run
        // for this bar by the time we get here (executions are applied ~40 lines above), so
        // positions_ carries the post-fill basis.
        //
        // This is the backtest half of the fix documented in docs/AVERAGE_PRICE_LIFECYCLE.md
        // -- the live path closes the same gap in LiveDailyCycle::resolve_and_apply_basis
        // (its "step 8"), and the backtest never had an equivalent.
        //
        // Futures are unaffected: TrendFollowingStrategy::on_execution only bumps a counter
        // and never populates positions_, so the lookup misses and the basis is untouched --
        // and under REALIZED_ONLY the basis is not consulted at all.
        std::unordered_map<std::string, const std::unordered_map<std::string, Position>*>
            fill_positions_by_strategy;
        for (const auto& s : portfolio->get_strategies()) {
            if (auto bs = std::dynamic_pointer_cast<BaseStrategy>(s)) {
                pnl_method_by_strategy[bs->get_metadata().id] = bs->get_pnl_accounting().method;
                fill_positions_by_strategy[bs->get_metadata().id] = &bs->get_positions();
            }
        }

        for (const auto& [strategy_id, positions_map] : strategy_positions) {
            // E2-F54: the accounting method decides whether this strategy's rows carry a
            // realized FLOW at all. Everything added below is gated on it, so futures
            // (REALIZED_ONLY) take exactly the path they took before.
            auto strategy_method_it = pnl_method_by_strategy.find(strategy_id);
            const PnLAccountingMethod strategy_method =
                (strategy_method_it != pnl_method_by_strategy.end())
                    ? strategy_method_it->second
                    : PnLAccountingMethod::REALIZED_ONLY;

            for (const auto& [symbol, pos] : positions_map) {
                double qty = static_cast<double>(pos.quantity);

                // E2-F54 (a): the cumulative comes from the strategy's FILL-maintained
                // record, not the target snapshot this loop iterates. `pos` was copied
                // before on_execution ran (~40 lines above), so `pos.realized_pnl` is
                // realized through YESTERDAY's fills -- reading it here puts a sale on the
                // next bar.
                //
                // E2-F59: the ledger is advanced ONLY where the row is actually written.
                // Three paths below return without writing (no close for this symbol, no
                // previous close, invalid P&L); advancing before them does not defer the
                // realized, it loses it. Hence peek here, commit at each write site.
                const std::string flow_key = strategy_id + "|" + symbol;
                auto fill_cumulative = [&]() {
                    double cumulative = static_cast<double>(pos.realized_pnl);
                    auto flow_it = fill_positions_by_strategy.find(strategy_id);
                    if (flow_it != fill_positions_by_strategy.end() && flow_it->second) {
                        auto held = flow_it->second->find(symbol);
                        if (held != flow_it->second->end()) {
                            cumulative = static_cast<double>(held->second.realized_pnl);
                        }
                    }
                    return cumulative;
                };

                // Skip zero quantity positions -- unless this is the close-day row and it
                // realized something (E2-F54 (c): the live is_dead_row rule). Futures are
                // unaffected: under REALIZED_ONLY this is the original unconditional skip.
                if (std::abs(qty) < 1e-8) {
                    if (strategy_method == PnLAccountingMethod::REALIZED_ONLY) continue;
                    // The exit's P&L needs somewhere to live. AVERAGE_PRICE_LIFECYCLE
                    // rule 5: a closed row carries no basis and no mark.
                    //
                    // The flow is written back on EVERY flat bar, not only the closing one,
                    // and it is 0 on the bars after the close. That is not redundant:
                    // update_strategy_position writes into current_positions, and
                    // save_daily_positions persists that whole map once per bar. Skipping
                    // the write on a later flat bar leaves the CLOSING bar's realized
                    // sitting in the map, and it is then re-persisted every day for the
                    // rest of the backtest -- measured on DD, whose 76.145406 exit repeated
                    // on all 22 rows from 2026-08-03 to 2026-09-02 and made the column sum
                    // to 137x the position's actual realized. Writing the zero lets the
                    // persist layer's dead-row filter drop the row instead.
                    const double closed_cumulative = fill_cumulative();
                    const auto closed_flow = BacktestPnLManager::realized_row_peek(
                        qty, closed_cumulative, last_cumulative_realized_[flow_key]);
                    Position closed_row = pos;
                    closed_row.quantity = Decimal(0.0);
                    closed_row.average_price = Decimal(0.0);
                    closed_row.unrealized_pnl = Decimal(0.0);
                    closed_row.realized_pnl =
                        Decimal(closed_flow.keep ? closed_flow.flow : 0.0);
                    auto closed_update =
                        portfolio->update_strategy_position(strategy_id, symbol, closed_row);
                    if (closed_update.is_error()) {
                        WARN("Failed to write close-day row for " + symbol + ": " +
                             std::string(closed_update.error()->what()));
                    }
                    // The row was written (or deliberately zeroed): the ledger may advance.
                    BacktestPnLManager::commit_realized_row(closed_cumulative,
                                                            last_cumulative_realized_[flow_key]);
                    continue;
                }

                // A futures row (REALIZED_ONLY) the day has no P&L for books nothing that day: its
                // realized P&L is 0, quantity and average price kept. Without this write the row
                // the PortfolioManager rebuilt this cycle keeps the sleeve's own figure (its
                // unrounded position times its last bar's move), on a day the symbol printed no
                // bar. The other accounting methods defer their realized flow to the next written
                // row (E2-F59) and are not written here.
                auto write_unbooked_row = [&]() {
                    if (strategy_method != PnLAccountingMethod::REALIZED_ONLY) return;
                    Position unbooked = pos;
                    unbooked.realized_pnl = Decimal(0.0);
                    auto unbooked_update =
                        portfolio->update_strategy_position(strategy_id, symbol, unbooked);
                    if (unbooked_update.is_error()) {
                        WARN("Failed to write the no-bar row for " + symbol + ": " +
                             std::string(unbooked_update.error()->what()));
                    }
                };

                // Get current close price
                auto curr_it = current_close_prices.find(symbol);
                if (curr_it == current_close_prices.end()) {
                    write_unbooked_row();
                    continue;
                }
                double current_close = curr_it->second;

                // Check if we have previous close (a withheld bar never becomes one: K-01)
                if (!pnl_manager_->has_previous_close(symbol)) {
                    if (!mark_withheld_.count(symbol)) pnl_manager_->set_previous_close(symbol, current_close);
                    write_unbooked_row();
                    continue;
                }

                double prev_close = pnl_manager_->get_previous_close(symbol);

                // Calculate PnL using BacktestPnLManager
                auto pnl_result =
                    pnl_manager_->calculate_position_pnl(symbol, qty, prev_close, current_close);
                // T-ROLLX-FIX (sections 2.1, 6.6): no P&L on a withheld bar (never consumed) or on a
                // change bar (the splice's gap).
                if (pnl_result.valid &&
                    (mark_withheld_.count(symbol) || mark_change_.count(symbol))) {
                    pnl_result.daily_pnl = 0.0;
                }

                if (pnl_result.valid) {
                    // Update this strategy's position with calculated PnL
                    Position updated_pos = pos;
                    // Phase 4 §1.14: only stamp realized_pnl with daily MTM
                    // when the strategy uses REALIZED_ONLY accounting
                    // (futures). For equities (MIXED / UNREALIZED_ONLY),
                    // leave realized_pnl untouched -- on_execution writes
                    // realized when positions actually close. total_portfolio_pnl
                    // below still aggregates daily_pnl independently so equity
                    // curve totals are unchanged.
                    auto pnl_method_it = pnl_method_by_strategy.find(strategy_id);
                    PnLAccountingMethod method = (pnl_method_it != pnl_method_by_strategy.end())
                                                     ? pnl_method_it->second
                                                     : PnLAccountingMethod::REALIZED_ONLY;
                    // The §1.14 branch itself lives in BacktestPnLManager::realized_for_row
                    // so it can be asserted; the pin 7e3d07c2 claimed for it tested only the
                    // accessor and passed with this file reverted (C-5 §9-A2).
                    if (method == PnLAccountingMethod::REALIZED_ONLY) {
                        updated_pos.realized_pnl = Decimal(BacktestPnLManager::realized_for_row(
                            method, pnl_result.daily_pnl, 0.0));
                    } else {
                        // E2-F19 / E2-F54 / E2-F59: a per-bar FLOW, taken from the
                        // fill-maintained record and computed HERE -- on the path that
                        // actually writes the row -- so a bar the symbol had no close for
                        // defers its realized to the next written row instead of losing it.
                        const double held_cumulative = fill_cumulative();
                        const auto held_flow = BacktestPnLManager::realized_row_peek(
                            qty, held_cumulative, last_cumulative_realized_[flow_key]);
                        updated_pos.realized_pnl = Decimal(BacktestPnLManager::realized_for_row(
                            method, pnl_result.daily_pnl, held_flow.flow));
                        BacktestPnLManager::commit_realized_row(
                            held_cumulative, last_cumulative_realized_[flow_key]);
                    }
                    // E2-F8: prefer the strategy's fill-maintained basis over the target
                    // position's average_price, which for mean reversion is the day's close
                    // (a mark) rather than what the position cost. A basis of 0 means "no
                    // basis known" and is left as such -- it must never be replaced by a
                    // mark, which is the substitution this fix exists to remove.
                    double cost_basis = static_cast<double>(pos.average_price);
                    if (method != PnLAccountingMethod::REALIZED_ONLY) {
                        auto fp_it = fill_positions_by_strategy.find(strategy_id);
                        if (fp_it != fill_positions_by_strategy.end() && fp_it->second) {
                            auto held = fp_it->second->find(symbol);
                            if (held != fp_it->second->end()) {
                                cost_basis = static_cast<double>(held->second.average_price);
                            }
                        }
                        // DELIBERATELY NOT WRITTEN BACK: `updated_pos.average_price` keeps
                        // whatever the target position carried. The basis is used ONLY to
                        // measure unrealized, just above.
                        //
                        // Writing the basis into average_price looks right -- it would make
                        // the stored row self-consistent, so a reader recomputing
                        // qty * (close - average_price) would reproduce unrealized_pnl. It is
                        // wrong, and measurably so. This position goes to
                        // PortfolioManager::update_strategy_position() -> current_positions,
                        // and RiskManager reads average_price off those as a MARK to size
                        // notional and leverage (risk_manager.cpp:64, :236, :243). On a book
                        // holding gains the basis sits below the mark, so leverage is
                        // understated, less risk scaling is applied, and positions come out
                        // bigger.
                        //
                        // NOTE ON EVIDENCE: an equity-backtest sizing shift of 1.29x-1.75x was
                        // observed in the same run this was written in, and initially blamed on
                        // this line. Reverting it did NOT restore the baseline, so this is NOT
                        // the cause of that shift -- the reasoning below stands on the code
                        // path alone, not on a measurement.
                        //
                        // This is exactly the ambiguity docs/AVERAGE_PRICE_LIFECYCLE.md maps:
                        // the field carries THREE meanings and risk wants the mark while P&L
                        // wants the basis. Consuming the basis locally is safe; storing it is
                        // not. If the stored row must ever be made self-consistent, the basis
                        // needs its OWN column -- do not reuse this one.
                    }

                    // E2-F2: unrealized is gated on the SAME accounting method as
                    // realized, and dollarised with point_value. Both were missing here:
                    // futures rows carried the settled move a second time, divided by the
                    // contract multiplier. The rule and the full rationale live in
                    // BacktestPnLManager::unrealized_for_accounting -- read it before
                    // changing this line.
                    updated_pos.unrealized_pnl =
                        Decimal(BacktestPnLManager::unrealized_for_accounting(
                            method, qty, cost_basis, current_close, pnl_result.point_value));

                    auto update_result =
                        portfolio->update_strategy_position(strategy_id, symbol, updated_pos);

                    if (!update_result.is_error()) {
                        total_portfolio_pnl += pnl_result.daily_pnl;
                    }
                }
            }
        }

        // Update previous closes for next iteration (not from a withheld bar: K-01)
        {
            std::unordered_map<std::string, double> consumed_closes = current_close_prices;
            for (const auto& symbol : mark_withheld_) consumed_closes.erase(symbol);
            pnl_manager_->update_previous_closes(consumed_closes);
        }

        // Phase 2 §3.2: accrue overnight borrow fees on open short equity
        // positions. Per-strategy attribution: iterate strategy_positions,
        // compute each strategy's borrow fee on its own shorts, both add to
        // today's transaction costs (equity curve) AND emit a synthetic
        // zero-quantity ExecutionReport so the DB / CSV / metrics audit
        // trail stays in sync with the equity-curve drop.
        // No-op when no shorts are open (the common case for long-only).
        if (execution_manager_ && registry_) {
            auto& tcm = execution_manager_->get_transaction_cost_manager();
            auto strategy_positions_for_borrow = portfolio->get_strategy_positions();
            for (const auto& [strategy_id, strat_positions] : strategy_positions_for_borrow) {
                auto strat_borrow_fees = tcm.calculate_overnight_borrow_fees(
                    strat_positions, current_close_prices, *registry_);
                for (const auto& [sym, fee] : strat_borrow_fees) {
                    if (fee <= 0.0) continue;
                    total_transaction_costs += fee;

                    // Synthesize a zero-quantity exec so per-strategy and
                    // per-symbol metrics (profit_factor, win_rate, symbol
                    // PnL) include the borrow drag instead of silently
                    // excluding it.
                    ExecutionReport borrow_exec;
                    borrow_exec.execution_type = ExecutionType::BORROW;  // migration 015
                    borrow_exec.exec_id = "BORROW_" + strategy_id + "_" + sym;
                    borrow_exec.order_id = borrow_exec.exec_id;
                    borrow_exec.symbol = sym;
                    borrow_exec.side = Side::SELL;
                    borrow_exec.filled_quantity = Quantity(0.0);
                    auto price_it = current_close_prices.find(sym);
                    borrow_exec.fill_price = Price(
                        price_it != current_close_prices.end() ? price_it->second : 0.0);
                    borrow_exec.fill_time = timestamp;
                    borrow_exec.commissions_fees = Decimal(fee);
                    borrow_exec.implicit_price_impact = Decimal(0.0);
                    borrow_exec.slippage_market_impact = Decimal(0.0);
                    borrow_exec.total_transaction_costs = Decimal(fee);
                    borrow_exec.is_partial = false;
                    portfolio->append_synthetic_execution(strategy_id, borrow_exec);
                    // X-4: the row reaches the run's executions too (the period's fills were
                    // collected before this block), so 018's transaction_costs and the metrics
                    // carry the borrow cost the equity curve charged.
                    executions.push_back(borrow_exec);
                }
            }
        }

        // Calculate portfolio value: previous value + daily PnL - transaction costs
        double portfolio_value =
            equity_curve.empty() ? initial_capital : equity_curve.back().second;
        portfolio_value += (total_portfolio_pnl - total_transaction_costs);

        // Add to equity curve
        equity_curve.emplace_back(timestamp, portfolio_value);
        // Section 7.3: the row of a sized rebalance carries the loop's record of it. An all-JUNK
        // cycle ran no rebalance and a refused one stores none: both rows stay NULL.
        if (!signal_feed->empty()) {
            const OnePassDay one_pass = portfolio->last_one_pass();
            if (one_pass.stores_detail()) {
                equity_risk_detail_[equity_curve.size() - 1] =
                    risk_detail_json(one_pass, cycle_account_value_).dump();
            }
        }

        // Build the portfolio-level positions map by simple per-strategy sum
        // (Σ qᵢ). get_portfolio_positions() applies allocation a second time,
        // producing fractional contracts and under-stated risk/notional.
        // Strategies size for their capital slice already; the broker holds
        // the simple sum.
        auto build_portfolio_sum = [&]() {
            std::unordered_map<std::string, Position> result;
            auto strategy_positions = portfolio->get_strategy_positions();
            for (const auto& [_, pos_map] : strategy_positions) {
                for (const auto& [symbol, pos] : pos_map) {
                    auto it = result.find(symbol);
                    if (it == result.end()) {
                        result[symbol] = pos;
                    } else {
                        it->second.quantity += pos.quantity;
                    }
                }
            }
            return result;
        };

        // CSV export: append daily and finalized positions
        if (csv_exporter_) {
            // Build market_prices map
            std::unordered_map<std::string, double> market_prices;
            for (const auto& bar : bars) {
                market_prices[bar.symbol] = static_cast<double>(bar.close);
            }

            auto portfolio_positions = build_portfolio_sum();

            // Calculate gross and net notional
            double gross_notional = 0.0;
            double net_notional = 0.0;
            auto& csv_registry = InstrumentRegistry::instance();
            for (const auto& [symbol, pos] : portfolio_positions) {
                double qty = static_cast<double>(pos.quantity);
                if (std::abs(qty) < 1e-10) continue;
                auto price_it = market_prices.find(symbol);
                double price = (price_it != market_prices.end()) ? price_it->second : 0.0;
                auto instrument = csv_registry.get_instrument(symbol);
                // get_notional_value is a magnitude (futures.cpp): carry the position's sign so
                // a short adds to the net notional with its sign, not as a long.
                double notional = instrument
                    ? std::copysign(instrument->get_notional_value(qty, price), qty)
                    : qty * price;
                gross_notional += std::abs(notional);
                net_notional += notional;
            }

            csv_exporter_->append_equity_curve(timestamp, portfolio_value);

            csv_exporter_->append_daily_positions(
                timestamp, portfolio_positions, market_prices,
                portfolio_value, gross_notional, net_notional,
                portfolio->get_strategies());

            csv_exporter_->append_finalized_positions(
                timestamp, portfolio_positions, portfolio_previous_positions_,
                market_prices);

            // Update previous positions for next day's finalized tracking
            portfolio_previous_positions_ = portfolio_positions;
        }

        // Update previous_bars for next day
        portfolio_previous_bars_ = bars;
        price_manager_->update_from_bars(bars);

        return Result<void>();

    } catch (const transaction_cost::NettingRefused& e) {
        // A ROLL or BORROW row carrying a netting adjustment reached the day's sum: a HARD STOP on
        // this day, as a ROLL_LEG STOP is. The run loop fails the run on netting_refused_stop_.
        netting_refused_stop_ = true;
        return make_error<void>(ErrorCode::INVALID_DATA,
                                std::string("NETTING STOP on ") + core::format_utc_date(timestamp) +
                                    ": " + e.what() + ". Failing the run",
                                "BacktestCoordinator");
    } catch (const std::exception& e) {
        // F-3 (section 6.5, commit 5): an exception while a roll is owed is a STOP, as the
        // PortfolioManager's error return is; the run loop fails the run on roll_leg_stop_.
        if (auto stop = roll_owed_stop(e.what()); stop.is_error()) return stop;
        return make_error<void>(ErrorCode::UNKNOWN_ERROR,
                                std::string("Error processing portfolio data: ") + e.what(),
                                "BacktestCoordinator");
    }
}

Result<void> BacktestCoordinator::seed_estimator_history(
    std::shared_ptr<PortfolioManager> portfolio, const std::vector<std::string>& symbols,
    const Timestamp& start_date, AssetClass asset_class, DataFrequency data_freq) {
    // Every bar dated before the window's first instant, back to the history start.
    DataLoadConfig load_config;
    load_config.symbols = symbols;
    load_config.start_date = estimator_history_start(start_date);
    load_config.end_date = start_date - std::chrono::seconds(1);
    load_config.asset_class = asset_class;
    load_config.data_freq = data_freq;
    MarketDataBus::instance().set_publish_enabled(false);
    auto loaded = data_loader_->load_market_data(load_config);
    MarketDataBus::instance().set_publish_enabled(true);
    if (loaded.is_error()) {
        // No bar before the window is not an error: the symbols start at their first bar. (The
        // loader reports an empty load as an error; any other failure refuses the run.)
        const std::string what = loaded.error()->what();
        if (what.find("No market data loaded") != std::string::npos ||
            what.find("returned an empty table") != std::string::npos) {
            INFO("ESTIMATOR_HISTORY empty: no bar before the window, every symbol's estimators "
                 "start at its first bar in the window");
            return Result<void>();
        }
        return make_error<void>(loaded.error()->code(),
                                "the bars before the backtest's window could not be loaded: " +
                                    std::string(loaded.error()->what()),
                                "BacktestCoordinator");
    }
    // LOOP_SPEC v6.2 section 2.1 (N-2): a bar's K-01 verdict reads the classifier's trailing
    // history, the same in every engine. The window's classifier is given the prefix the live
    // runners give theirs (live/live_roll_legs.hpp: the bars of kK01ClassifierHistoryDays
    // calendar days before the window), from the history loaded here, so the window's first bars
    // are judged on a warm norm: a thin first print of the window is withheld by a 2-year run as
    // it is by a longer run and by live. The prefix's bars are not cycles and are consumed by
    // nothing else; the window's roll status still starts at the window.
    session_classifier_.add_bars(k01_classifier_history(loaded.value(), start_date));
    // The same rule as the live runners' (live/live_estimator_history.hpp): the history's bars are
    // judged in date order by their own classifier, with their vendor ids, each against the bars
    // before it; the withheld ones (K-01) are never consumed.
    auto pg = std::dynamic_pointer_cast<PostgresDatabase>(db_);
    std::vector<SymbolDayVerdict> withheld;
    const std::vector<Bar> history = estimator_history_consumed(
        loaded.value(), start_date,
        pg ? with_predecessor_ids(symbols,
                                  pg->get_futures_instrument_ids(
                                      symbols, estimator_history_start(start_date), start_date))
           : make_error<std::vector<market_data_utils::FuturesInstrumentId>>(
                 ErrorCode::NOT_INITIALIZED, "the backtest's database is not a PostgresDatabase",
                 "BacktestCoordinator"),
        &withheld);
    consumed_record_.add_history(withheld, history);
    return portfolio->seed_strategy_history(history);
}

Result<void> BacktestCoordinator::roll_owed_stop(const std::string& what) {
    if (cycle_rolls_owed_.empty()) return Result<void>();
    std::string owed;
    for (const auto& roll : cycle_rolls_owed_) owed += (owed.empty() ? "" : "; ") + roll;
    cycle_rolls_owed_.clear();
    roll_leg_stop_ = true;
    return make_error<void>(ErrorCode::INVALID_DATA,
                            "ROLL_LEG STOP " + owed + ": the cycle failed (" + what +
                                ") and the roll would not be legged. Failing the run",
                            "BacktestCoordinator");
}

void BacktestCoordinator::reset() {
    has_previous_bars_ = false;
    previous_bars_.clear();
    current_positions_.clear();
    current_portfolio_value_ = config_.initial_capital;

    if (price_manager_) {
        price_manager_->reset();
    }
    if (pnl_manager_) {
        pnl_manager_->reset();
    }
    if (execution_manager_) {
        execution_manager_->reset();
    }
}

double BacktestCoordinator::calculate_portfolio_value(
    const std::map<std::string, Position>& positions, const std::vector<Bar>& bars) {
    // Start with last known portfolio value
    double portfolio_value = current_portfolio_value_;

    // Build price map from bars
    std::unordered_map<std::string, double> current_prices;
    for (const auto& bar : bars) {
        current_prices[bar.symbol] = static_cast<double>(bar.close);
    }

    // Calculate daily PnL for each position
    for (const auto& [symbol, pos] : positions) {
        double quantity = static_cast<double>(pos.quantity);
        if (std::abs(quantity) < 1e-6)
            continue;

        auto curr_it = current_prices.find(symbol);
        auto prev_result = price_manager_->get_previous_day_price(symbol);

        if (curr_it != current_prices.end() && !prev_result.is_error()) {
            double current_price = curr_it->second;
            double previous_price = prev_result.value();

            // Get point value from PnL manager
            double point_value = pnl_manager_ ? pnl_manager_->get_point_value(symbol) : 1.0;

            // Daily PnL = quantity * (current - previous) * point_value
            double daily_pnl = quantity * (current_price - previous_price) * point_value;
            portfolio_value += daily_pnl;
        }
    }

    return portfolio_value;
}

// ========== Portfolio Backtest Helpers ==========

int BacktestCoordinator::calculate_warmup_days(
    const std::vector<std::shared_ptr<StrategyInterface>>& strategies) {
    int max_lookback = 0;

    for (const auto& strat : strategies) {
        if (!strat) {
            continue;
        }

        // A trend sleeve (TREND or FAST: one class, two configurations)
        auto trend_following = std::dynamic_pointer_cast<TrendFollowingStrategy>(strat);
        if (trend_following) {
            int strat_lookback = trend_following->get_max_required_lookback();
            max_lookback = std::max(max_lookback, strat_lookback);
            continue;
        }

        // For other strategy types, assume 0 (no warmup required)
    }

    return max_lookback;
}

void BacktestCoordinator::reset_portfolio_state() {
    portfolio_has_previous_bars_ = false;
    portfolio_previous_bars_.clear();
    session_classifier_ = SessionClassifier();
    session_hold_enabled_ = false;
    roll_trackers_.clear();
    signal_roll_status_.clear();
    roll_leg_seq_.clear();
    roll_leg_stop_ = false;
    netting_refused_stop_ = false;
    cycle_rolls_owed_.clear();
    mark_withheld_.clear();
    mark_change_.clear();
    row_held_id_.clear();
    risk_scale_report_enabled_ = false;
    equity_risk_detail_.clear();
    cycle_account_value_ = 0.0;
    size_on_equity_enabled_ = false;
    equity_cost_retier_enabled_ = false;
    equity_cost_retier_.reset();
    equity_cost_retier_cycles_ = 0;
    equity_cost_retier_changes_ = 0;
    own_day_cost_feed_enabled_ = false;
    execution_cost_carry_ = FuturesCostFeedCarry{};
    portfolio_cost_carry_ = FuturesCostFeedCarry{};
    cost_feed_previous_group_.clear();
    current_run_id_.clear();
    portfolio_previous_positions_.clear();
    // E2-F54 (c): without this, a second portfolio backtest in the same process opens with
    // the previous book's cumulative realized and reports its first bar as a difference.
    last_cumulative_realized_.clear();
    csv_exporter_.reset();
}

void BacktestCoordinator::load_equity_cost_retier(const std::vector<std::string>& symbols,
                                                  const Timestamp& start_date,
                                                  const Timestamp& end_date) {
    equity_cost_retier_.reset();
    equity_cost_retier_cycles_ = 0;
    equity_cost_retier_changes_ = 0;
    // The first window is the cost warm-up's (equity_cost_warmup.hpp, H-13): the 30 calendar
    // days before start_date, ending strictly before it, so the first cycle's tier is set from
    // bars that existed when the backtest starts.
    const auto window = equity_cost_warmup_window(start_date);
    const std::string from = core::format_utc_date(window.start);
    const std::string to = core::format_utc_date(end_date);

    // The split events that put a bar's volume in the window-end share unit: every ex-date from
    // the first window's first day to end_date (the adjusted prices' frame).
    auto actions = db_->get_per_bar_corporate_actions(symbols, from, to);
    if (actions.is_error()) {
        WARN("EQUITY_COST_RETIER the split events " + from + ".." + to +
             " could not be read (" + std::string(actions.error()->what()) +
             "): every volume stays in raw shares, so a bar before a split is tiered in its own "
             "share unit");
    } else {
        for (const auto& row : actions.value()) {
            if (row.action != "split") continue;
            equity_cost_retier_.add_split(row.ticker, row.date_str, row.value);
            INFO("EQUITY_COST_RETIER split symbol=" + row.ticker + " ex=" + row.date_str +
                 " factor=" + std::to_string(row.value) +
                 ": the volume of every earlier bar is multiplied by it");
        }
    }

    // The bars before start_date, read with MarketDataBus publishing off (as run_portfolio reads
    // its own window), so they reach the trailing windows and nothing else.
    const bool publishing = MarketDataBus::instance().is_publish_enabled();
    MarketDataBus::instance().set_publish_enabled(false);
    Result<std::shared_ptr<arrow::Table>> seed = [&]() {
        try {
            return db_->get_market_data(symbols, window.start, window.end, AssetClass::EQUITIES,
                                        DataFrequency::DAILY, "ohlcv");
        } catch (...) {
            MarketDataBus::instance().set_publish_enabled(publishing);
            throw;
        }
    }();
    MarketDataBus::instance().set_publish_enabled(publishing);

    size_t seed_bars = 0;
    std::set<std::string> seeded;
    if (seed.is_error()) {
        WARN("EQUITY_COST_RETIER the bars before start_date could not be read (" +
             std::string(seed.error()->what()) +
             "): the first cycles re-tier from the in-window bars only");
    } else {
        auto converted = DataConversionUtils::arrow_table_to_bars(seed.value());
        if (converted.is_error()) {
            WARN("EQUITY_COST_RETIER the bars before start_date could not be converted (" +
                 std::string(converted.error()->what()) +
                 "): the first cycles re-tier from the in-window bars only");
        } else {
            auto bars = converted.value();
            std::stable_sort(bars.begin(), bars.end(), [](const Bar& a, const Bar& b) {
                return a.timestamp < b.timestamp;
            });
            equity_cost_retier_.append(bars);
            seed_bars = bars.size();
            for (const auto& b : bars) seeded.insert(b.symbol);
        }
    }
    INFO("EQUITY_COST_RETIER window=[" + core::format_utc_datetime(window.start) + "Z, " +
         core::format_utc_datetime(window.end) + "Z] bars=" + std::to_string(seed_bars) +
         " symbols=" + std::to_string(seeded.size()) + "/" + std::to_string(symbols.size()) +
         " splits=" + std::to_string(equity_cost_retier_.split_count()) +
         ": the first tier window; every cycle re-tiers both cost managers from the 20 bars ending "
         "at its signal bar, in window-end share units, and feeds the impact ADV in that unit");
}

std::string BacktestCoordinator::generate_portfolio_run_id(
    const std::vector<std::string>& strategy_names, const Timestamp& end_date) {
    return RunIdGenerator::generate_portfolio_run_id(strategy_names, end_date);
}

Result<void> BacktestCoordinator::save_daily_positions(std::shared_ptr<PortfolioManager> portfolio,
                                                       const std::string& run_id,
                                                       const Timestamp& timestamp) {
    if (!db_)
        return Result<void>();

    auto strategy_positions = portfolio->get_strategy_positions();
    int total_positions_saved = 0;
    int strategies_with_positions = 0;

    for (const auto& [strategy_id, positions_map] : strategy_positions) {
        if (positions_map.empty()) {
            continue;
        }

        std::vector<Position> positions_vec;
        positions_vec.reserve(positions_map.size());

        for (const auto& [symbol, pos] : positions_map) {
            Position pos_with_date = pos;
            pos_with_date.last_update = timestamp;
            // T-ROLLX-FIX (section 7, migration 016): the contract held after this cycle's bar.
            if (const auto held = row_held_id_.find(symbol); held != row_held_id_.end()) {
                pos_with_date.instrument_id = held->second;
            }
            positions_vec.push_back(pos_with_date);
        }

        // E2-F54: a cash book keeps the close-day row so the exit's realized flow has a
        // date to live on. Futures (REALIZED_ONLY) keep the historical filter, which drops
        // every zero-quantity row, so their stored rows are unchanged.
        bool keep_closed_rows = false;
        for (const auto& s : portfolio->get_strategies()) {
            if (auto bs = std::dynamic_pointer_cast<BaseStrategy>(s)) {
                if (bs->get_metadata().id == strategy_id) {
                    keep_closed_rows =
                        bs->get_pnl_accounting().method != PnLAccountingMethod::REALIZED_ONLY;
                    break;
                }
            }
        }

        if (!positions_vec.empty()) {
            std::string composite_run_id = run_id + "|" + strategy_id;
            auto save_result = db_->store_backtest_positions(
                positions_vec, composite_run_id, config_.portfolio_id, "backtest.final_positions",
                keep_closed_rows);

            if (save_result.is_error()) {
                WARN("Failed to save daily positions for strategy " + strategy_id +
                     ", error: " + std::string(save_result.error()->what()));
            } else {
                total_positions_saved += static_cast<int>(positions_vec.size());
                strategies_with_positions++;
            }
        }
    }

    if (strategies_with_positions > 0) {
        DEBUG("Saved " + std::to_string(total_positions_saved) + " positions across " +
              std::to_string(strategies_with_positions) + " strategies");
    }

    return Result<void>();
}

double BacktestCoordinator::calculate_period_transaction_costs(
    std::shared_ptr<PortfolioManager> portfolio,
    const std::unordered_map<std::string, size_t>& exec_counts_before) {
    double total_transaction_costs = 0.0;

    auto all_strategy_executions = portfolio->get_strategy_executions();

    for (const auto& [strategy_id, execs] : all_strategy_executions) {
        size_t count_before =
            exec_counts_before.count(strategy_id) > 0 ? exec_counts_before.at(strategy_id) : 0;

        // Only the new executions (those added after count_before), each at its cost after
        // netting: the PortfolioManager set every fill's netting_adjustment inside the
        // process_market_data call of this cycle, before these rows were read.
        total_transaction_costs =
            transaction_cost::add_net_costs(total_transaction_costs, execs, count_before);
    }

    return total_transaction_costs;
}

std::optional<Bar> BacktestCoordinator::find_bar_for_symbol(const std::vector<Bar>& bars,
                                                            const std::string& symbol) {
    for (const auto& bar : bars) {
        if (bar.symbol == symbol) {
            return bar;
        }
    }
    return std::nullopt;
}

Result<void> BacktestCoordinator::save_portfolio_results_to_db(
    const BacktestResults& results, const std::vector<std::string>& strategy_names,
    const std::unordered_map<std::string, double>& strategy_allocations,
    std::shared_ptr<PortfolioManager> portfolio, const nlohmann::json& portfolio_config) const {
    if (!config_.store_trade_details) {
        return Result<void>();
    }

    INFO("Using BacktestResultsManager for portfolio-level storage");

    auto db_ptr = std::dynamic_pointer_cast<PostgresDatabase>(db_);
    if (!db_ptr) {
        ERROR("Database is not a PostgresDatabase instance");
        return make_error<void>(ErrorCode::DATABASE_ERROR, "Invalid database type",
                                "BacktestCoordinator");
    }

    // Use the run_id from daily position storage if available, otherwise generate a new one
    std::string portfolio_run_id;
    if (!current_run_id_.empty()) {
        portfolio_run_id = current_run_id_;
        INFO("Using run_id from daily position storage: " + portfolio_run_id);
    } else {
        // Fallback: Generate portfolio run_id (combined strategy names)
        portfolio_run_id = RunIdGenerator::generate_portfolio_run_id(
            strategy_names, std::chrono::system_clock::now());
        INFO("Generated new portfolio run_id: " + portfolio_run_id);
    }

    // Create results manager for portfolio-level storage
    auto results_manager = std::make_unique<BacktestResultsManager>(
        db_ptr, config_.store_trade_details, portfolio_run_id, config_.portfolio_id);

    // Set metadata with portfolio configuration
    nlohmann::json hyperparameters;
    hyperparameters["initial_capital"] = config_.initial_capital;
    hyperparameters["use_optimization"] = config_.use_optimization;
    hyperparameters["portfolio_config"] = portfolio_config;

    // Use the actual backtest start/end dates that were stored in run_portfolio()
    results_manager->set_metadata(backtest_start_date_, backtest_end_date_, hyperparameters,
                                  "Portfolio Backtest Run: " + portfolio_run_id,
                                  "Multi-strategy portfolio backtest");

    // Set performance metrics (portfolio-level)
    std::unordered_map<std::string, double> metrics = {
        {"total_return", results.total_return},
        {"sharpe_ratio", results.sharpe_ratio},
        {"sortino_ratio", results.sortino_ratio},
        {"max_drawdown", results.max_drawdown},
        {"calmar_ratio", results.calmar_ratio},
        {"volatility", results.volatility},
        {"total_trades", static_cast<double>(results.total_trades)},
        {"win_rate", results.win_rate},
        {"profit_factor", results.profit_factor},
        {"avg_win", results.avg_win},
        {"avg_loss", results.avg_loss},
        {"max_win", results.max_win},
        {"max_loss", results.max_loss},
        {"avg_holding_period", results.avg_holding_period},
        {"var_95", results.var_95},
        {"cvar_95", results.cvar_95},
        {"beta", results.beta},
        {"correlation", results.correlation},
        {"downside_volatility", results.downside_volatility},
        {"transaction_costs", results.transaction_costs},  // migration 018
        {"roll_costs", results.roll_costs},
        {"total_roll_fills", static_cast<double>(results.total_roll_fills)}};
    results_manager->set_performance_metrics(metrics);

    // Set portfolio-level equity curve
    std::vector<std::pair<Timestamp, double>> equity_points;
    for (const auto& [timestamp, equity] : results.equity_curve) {
        equity_points.push_back({timestamp, equity});
    }
    results_manager->set_equity_curve(equity_points);
    results_manager->set_equity_risk_detail(results.equity_risk_detail);

    // Collect per-strategy executions from PortfolioManager
    if (portfolio) {
        auto strategy_executions_map = portfolio->get_strategy_executions();

        // Process each strategy - only save executions, not positions (positions saved daily)
        for (const auto& [strategy_id, executions] : strategy_executions_map) {
            results_manager->set_strategy_executions(strategy_id, executions);
        }

        INFO("Skipping final positions save - positions already saved daily during backtest");
    }

    // Save portfolio-level results (summary, equity curve)
    auto save_result = results_manager->save_all_results(portfolio_run_id, backtest_end_date_);

    if (save_result.is_error()) {
        ERROR("Failed to save portfolio results: " + std::string(save_result.error()->what()));
        return save_result;
    }

    // Save per-strategy executions
    auto executions_result = results_manager->save_strategy_executions(portfolio_run_id);
    if (executions_result.is_error()) {
        WARN("Failed to save strategy executions: " +
             std::string(executions_result.error()->what()));
        // Non-fatal, continue
    }

    // Save per-strategy metadata
    auto metadata_result = results_manager->save_strategy_metadata(
        portfolio_run_id, strategy_allocations, portfolio_config);
    if (metadata_result.is_error()) {
        WARN("Failed to save strategy metadata: " + std::string(metadata_result.error()->what()));
        // Non-fatal, continue
    }

    INFO("Successfully saved portfolio backtest results");
    return Result<void>();
}

}  // namespace backtest
}  // namespace trade_ngin
