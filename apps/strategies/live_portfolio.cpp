#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include "trade_ngin/core/config_loader.hpp"
#include "trade_ngin/core/email_sender.hpp"
#include "trade_ngin/core/holiday_checker.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/data/conversion_utils.hpp"
#include "trade_ngin/data/database_pooling.hpp"
#include "trade_ngin/data/market_data_bus.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/data/roll_series.hpp"
#include "trade_ngin/data/session_classifier.hpp"
#include "trade_ngin/instruments/futures.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/live/carried_day.hpp"
#include "trade_ngin/live/csv_exporter.hpp"
#include "trade_ngin/live/data_freshness.hpp"
#include "trade_ngin/live/execution_manager.hpp"
#include "trade_ngin/live/futures_cost_feed.hpp"
#include "trade_ngin/live/live_data_loader.hpp"
#include "trade_ngin/live/live_historical_metrics.hpp"
#include "trade_ngin/live/live_metrics_calculator.hpp"
#include "trade_ngin/live/live_pnl_manager.hpp"
#include "trade_ngin/live/live_price_manager.hpp"
#include "trade_ngin/live/live_estimator_history.hpp"
#include "trade_ngin/live/live_roll_legs.hpp"
#include "trade_ngin/live/live_sizing_read.hpp"
#include "trade_ngin/live/live_trading_coordinator.hpp"
#include "trade_ngin/live/book_exposure.hpp"
#include "trade_ngin/live/margin_manager.hpp"
#include "trade_ngin/live/risk_module_failure.hpp"
#include "trade_ngin/live/run_metadata_marks.hpp"
#include "trade_ngin/live/session_book_gate.hpp"
#include "trade_ngin/live/sleeve_seeding.hpp"
#include "trade_ngin/live/trading_days_anchor.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/portfolio/sizing_capital.hpp"
#include "trade_ngin/risk/risk_scale_report.hpp"
#include "trade_ngin/storage/live_results_manager.hpp"
#include "trade_ngin/transaction_cost/netting.hpp"
#include "trade_ngin/strategy/trend_following.hpp"

using namespace trade_ngin;

int main(int argc, char* argv[]) {
    try {
        // Parse command-line arguments for date override and email flag
        std::chrono::system_clock::time_point target_date;
        bool use_override_date = false;
        bool send_email = false;  // Default to false for historical runs

        // Parse command-line arguments
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];

            // Check for email flag
            if (arg == "--send-email") {
                send_email = true;
                continue;
            }

            // Try to parse as date.
            //
            // E2-F42 / M-08 IS STILL OPEN HERE, AND THIS std::mktime IS STILL WRONG.
            // It reads the operator's date as LOCAL midnight while every consumer of
            // target_date -- the position date, the T-1 lookup, the order-id stamp, the
            // trading-days target -- formats it back through gmtime, so on a host at a
            // positive UTC offset the whole run lands a day early. A New York host hides
            // it, which is why it has survived.
            //
            // The fix (parse_utc_date here, gmtime_r for now_tm below) was made in T-2,
            // MEASURED, and REVERTED, because it is not the class A change the ledger
            // expected. Moving `now` from 04:00/05:00Z to 00:00Z on this host also moves:
            //
            //   * the 730-day bar window. get_market_data asks `time BETWEEN start_ts AND
            //     end_ts` and futures bars are keyed at 00:00:00Z, so the start edge
            //     gained exactly one extra bar per symbol. Measured on the ten-day
            //     conservative chain from 2026-04-24: 225 of 6,480 stored signal_values
            //     moved, by up to 1.42 on a forecast that runs to about +/-20 -- not a
            //     rounding artefact. Quantities happened not to cross a rounding boundary
            //     on this window; on another window they would.
            //   * trading.equity_curve. Its ON CONFLICT key is (portfolio_id, strategy_id,
            //     timestamp, portfolio_type), so the Day T-1 rewrite no longer matched the
            //     existing row and INSERTED a second one: 2026-04-23 appeared twice, at
            //     05:00Z and 00:00Z, with the same equity. Every replay boundary would
            //     double a day.
            //   * positions.last_update and signals.timestamp (130 and 288 rows on that
            //     chain), and executions.execution_time, which embeds the instant
            //     (exec_id did too until it became EXEC_<symbol>_<YYYYMMDD>).
            //   * live_results.portfolio_var, max_correlation and risk_scale.
            //
            // So it belongs with the class C set, not with the guards: it needs a decision
            // on the bar-window boundary, a decision on the stored time-of-day contract
            // (E2-F22's lineage), and a plan for the equity_curve key, and its own A/B.
            // Reverted in this batch and reported; see the T-2 report, item A4.
            std::tm tm = {};
            std::istringstream ss(arg);
            ss >> std::get_time(&tm, "%Y-%m-%d");
            if (!ss.fail()) {
                target_date = std::chrono::system_clock::from_time_t(std::mktime(&tm));
                use_override_date = true;
                std::cout << "Running for historical date: " << arg << std::endl;
            } else if (arg != "--send-email") {
                std::cerr << "Invalid argument: " << arg << std::endl;
                std::cerr << "Usage: " << argv[0] << " [YYYY-MM-DD] [--send-email]" << std::endl;
                std::cerr << "Example: " << argv[0] << " 2025-01-01 --send-email" << std::endl;
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

        // ========================================
        // LOAD CONFIGURATION FROM MODULAR CONFIG FILES
        // ========================================
        INFO("Loading configuration from config/portfolios/base...");
        auto app_config_result = ConfigLoader::load("./config", "base");
        if (app_config_result.is_error()) {
            ERROR("Failed to load configuration: " +
                  std::string(app_config_result.error()->what()));
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

        // Load strategies from config (mirror bt_portfolio.cpp pattern)
        std::vector<std::string> strategy_names;
        std::unordered_map<std::string, double> strategy_allocations;
        std::unordered_map<std::string, nlohmann::json> strategy_configs;

        if (app_config.strategies_config.is_null() || !app_config.strategies_config.is_object()) {
            ERROR("No strategies section found in loaded configuration");
            return 1;
        }

        const auto& strategies_config = app_config.strategies_config;
        for (const auto& [strategy_id, strategy_def] : strategies_config.items()) {
            // Use enabled_live flag for live portfolio
            if (strategy_def.contains("enabled_live") && strategy_def["enabled_live"].get<bool>()) {
                double default_allocation = strategy_def.value("default_allocation", 0.5);
                strategy_allocations[strategy_id] = default_allocation;
                strategy_configs[strategy_id] = strategy_def;
                strategy_names.push_back(strategy_id);
                INFO("Loaded strategy: " + strategy_id +
                     " with allocation: " + std::to_string(default_allocation * 100.0) + "%");
            }
        }

        if (strategy_names.empty()) {
            ERROR("No enabled_live strategies found in loaded configuration");
            return 1;
        }

        // Normalize allocations to sum to 1.0. If the configured allocations
        // sum to less than 1.0 (e.g. 0.6 + 0.3, expecting 10% idle capital),
        // this loop silently rescales them to 1.0 — partial deployment is not
        // supported. Warn so operators can spot a config mistake.
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

        // Sort strategy names for deterministic combined ID (Tier 2)
        std::sort(strategy_names.begin(), strategy_names.end());

        // Generate combined strategy_id: LIVE_<sorted_names_joined_by_underscore>.
        // NEW-2: this line said "&" while the loop below has joined with "_" since
        // a88085ec, and "&" was never the separator in tree. The orphaned "&"-keyed
        // rows in the DB come from an out-of-tree binary; no runner can read them back
        // because the id is matched exactly.
        std::string combined_strategy_id = "LIVE_";
        for (size_t i = 0; i < strategy_names.size(); ++i) {
            if (i > 0)
                combined_strategy_id += "_";
            combined_strategy_id += strategy_names[i];
        }
        INFO("Combined strategy_id (Tier 2): " + combined_strategy_id);
        INFO("Total strategies enabled: " + std::to_string(strategy_names.size()));

        // Log normalized allocations
        for (const auto& [name, alloc] : strategy_allocations) {
            INFO("Strategy " + name + " normalized allocation: " + std::to_string(alloc * 100.0) +
                 "%");
        }

        // Get current date for daily processing (or use override date)
        auto now = use_override_date ? target_date : std::chrono::system_clock::now();
        auto now_time_t = std::chrono::system_clock::to_time_t(now);
        std::tm* now_tm = std::localtime(&now_time_t);

        // Set start date based on configured historical window
        auto start_date = now - std::chrono::hours(24 * app_config.live.historical_days);

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

        // E2-F8, futures side (with B-3 F-2's exit-0 half). A SKIPPED DAY MUST NOT
        // OPEN THE BOOK FROM FLAT.
        //
        // Every position this runner writes is sized as a DELTA from the previous
        // calendar day's book, which it reads with `load_positions_by_date(now - 24h)`
        // -- at the seed below, at the PnL lookup, at the T-1 finalization. When that
        // read comes back empty the runner does not distinguish "the strategy holds
        // nothing" from "nobody ran yesterday, so nothing was written". It seeds flat,
        // sizes the whole book as a fresh entry against contracts the broker still
        // holds, stores that, and exits 0. A monitor watching exit codes sees a clean
        // run while the book is being abandoned. That is the equity defect E2-F8, whose
        // guard has been at live_equity_mean_reversion.cpp:1125 since the equities
        // campaign, and B-3 F-2's complaint that the failure is not even distinguishable
        // at the exit-code level from a normal quiet day.
        //
        // The discriminator is NOT "are there positions" -- a flat book has none and is
        // a perfectly valid state. It is "did a run HAPPEN for that date". Every run
        // writes a live_results row whether or not it holds anything, so that row is the
        // evidence, and it is what this asks for.
        //
        // Deliberately NOT resolved by falling back to MAX(date): that would paper over
        // a broken invariant and could silently revive a stale book. Runs must be
        // sequential and complete -- inherent to the T-1 lag model, where day T's P&L is
        // finalized by day T+1's run -- so a hole means a run was missed, and the remedy
        // is to replay the missing dates in order, which works and needs no code. What
        // was missing was being TOLD.
        //
        // Placed here, before the run-metadata write and before any bar is loaded, so a
        // refused run writes nothing at all. On a consecutive chain the first query
        // returns a row and this block is silent.
        {
            const std::string prev_date_str = core::format_utc_date(now - std::chrono::hours(24));
            const std::string today_date_str = core::format_utc_date(now);

            auto first_cell = [](const Result<std::shared_ptr<arrow::Table>>& r) -> std::string {
                if (r.is_error() || !r.value() || r.value()->num_rows() == 0) return {};
                auto col = std::static_pointer_cast<arrow::StringArray>(
                    r.value()->column(0)->chunk(0));
                if (!col || col->length() == 0 || col->IsNull(0)) return {};
                return std::string(col->GetView(0));
            };

            auto prev_run = db->execute_query(
                "SELECT count(*)::text FROM trading.live_results "
                "WHERE strategy_id = '" + combined_strategy_id + "'"
                " AND portfolio_id = '" + portfolio_id + "'"
                " AND date = '" + prev_date_str + "'");

            long prev_run_rows = 0;
            const std::string prev_cell = first_cell(prev_run);
            if (!prev_cell.empty()) prev_run_rows = std::stol(prev_cell);

            if (prev_run.is_error()) {
                // Cannot establish the invariant either way. Say so rather than
                // treating an unanswered question as a clean answer.
                WARN("Could not check whether the previous day (" + prev_date_str +
                     ") ran: " + std::string(prev_run.error()->what()) +
                     ". Proceeding; verify the book by hand if this run writes "
                     "unexpected executions.");
            } else if (prev_run_rows == 0) {
                // No live_results row for T-1. Before refusing, ask whether the BOOK is
                // there anyway.
                //
                // The equity guard reaches its "did a run happen" question only from
                // inside `if (previous_positions.empty())`, so a day whose positions were
                // written but whose live_results write failed -- which the runner logs as
                // an ERROR and then exits 0 (live_portfolio.cpp, "Failed to save live
                // results") -- does not stop the next day there. This guard runs before
                // the book is loaded, so without this second question it would be
                // STRICTER than the guard it claims to port, and it would refuse a run
                // that has a complete book to seed from. Measured on the scratch copy of
                // production: 13 such dates exist, all on BASE_PORTFOLIO, 2 of them
                // immediately before a day that did run.
                //
                // Asking the database rather than loading the book keeps the guard where
                // it is, ahead of every write.
                auto prev_book = db->execute_query(
                    "SELECT count(*)::text FROM trading.positions "
                    "WHERE strategy_id = '" + combined_strategy_id + "'"
                    " AND portfolio_id = '" + portfolio_id + "'"
                    " AND date = '" + prev_date_str + "'");
                long prev_book_rows = 0;
                const std::string book_cell = first_cell(prev_book);
                if (!book_cell.empty()) prev_book_rows = std::stol(book_cell);

                if (prev_book_rows > 0) {
                    WARN("No live_results row for the previous day (" + prev_date_str +
                         "), but " + std::to_string(prev_book_rows) +
                         " position row(s) are stored for it, so the book is intact and this "
                         "run can seed from it. That day's results write did not complete; "
                         "its reported numbers are missing and should be back-filled.");
                    // fall through and run
                } else {
                    auto last_run = db->execute_query(
                        "SELECT COALESCE(MAX(date)::text, '') FROM trading.live_results "
                        "WHERE strategy_id = '" + combined_strategy_id + "'"
                        " AND portfolio_id = '" + portfolio_id + "'"
                        " AND date < '" + today_date_str + "'");
                    const std::string last_run_date = first_cell(last_run);

                    if (!last_run_date.empty()) {
                        ERROR("No run was recorded for the previous day (" + prev_date_str +
                              ") and no positions are stored for it either, but " +
                              combined_strategy_id + " / " + portfolio_id + " last ran on " +
                              last_run_date +
                              ". A run was missed. Replay every date from " + last_run_date +
                              " forward, in order, before running " + today_date_str +
                              " -- continuing would seed the book flat and size every "
                              "position as a fresh entry against contracts the broker still "
                              "holds. Refusing to run.");
                        return 1;
                    }
                    INFO("No prior run anywhere for " + combined_strategy_id + " / " +
                         portfolio_id + " -- genuine first run.");
                }
            }
        }

        // FUT-anchor-row / E2-F32, futures side. Is the annualization anchor consistent
        // with the book it annualizes?
        //
        // trading.get_trading_days(strategy, target, portfolio) takes its start date from
        // trading.strategy_trading_days_metadata.live_start_date and falls back to
        // MIN(date) over live_results only when NO row exists. A row LATER than the
        // book's own first day is therefore strictly worse than no row at all, and the
        // function cannot notice because it stops looking the moment it finds one: every
        // date before the anchor gets GREATEST(1, target - start + 1) = 1 trading day, so
        // total_annualized_return collapses onto total_cumulative_return, and just after
        // the anchor a small cumulative return annualizes into a number with no meaning.
        // On this book today, LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST / BASE_PORTFOLIO
        // is anchored at 2025-11-11 against live_results starting 2025-01-29.
        //
        // The equity runner has gone through assess_trading_days_anchor since E2-F32; the
        // helper was written to be shared and says so in its own header. This wires the
        // futures runners to it: the same comparison and the same WARN. The metadata row
        // stays authoritative; the count is never recomputed here.
        //
        // The metadata row itself is a one-row data correction on production and is NOT
        // made here.
        {
            auto anchor_cell = [](const Result<std::shared_ptr<arrow::Table>>& r) -> std::string {
                if (r.is_error() || !r.value() || r.value()->num_rows() == 0) return {};
                auto col = std::static_pointer_cast<arrow::StringArray>(
                    r.value()->column(0)->chunk(0));
                if (!col || col->length() == 0 || col->IsNull(0)) return {};
                return std::string(col->GetView(0));
            };

            auto anchor_q = db->execute_query(
                "SELECT COALESCE(MIN(live_start_date)::text, '') "
                "FROM trading.strategy_trading_days_metadata "
                "WHERE strategy_id = '" + combined_strategy_id + "'"
                " AND portfolio_id = '" + portfolio_id + "'");
            auto first_q = db->execute_query(
                "SELECT COALESCE(MIN(date)::text, '') FROM trading.live_results "
                "WHERE strategy_id = '" + combined_strategy_id + "'"
                " AND portfolio_id = '" + portfolio_id + "'");

            if (anchor_q.is_error() || first_q.is_error()) {
                WARN("Could not check the annualization anchor for " + portfolio_id +
                     "; total_annualized_return is reported as the DB function computes it.");
            } else {
                const auto anchor = assess_trading_days_anchor(anchor_cell(anchor_q),
                                                               anchor_cell(first_q));
                if (anchor.anchor_is_late) {
                    WARN("Annualization anchor is LATER than the book it annualizes: "
                         "strategy_trading_days_metadata.live_start_date = " +
                         anchor.metadata_anchor + " but live_results for " +
                         combined_strategy_id + " / " + portfolio_id + " start " +
                         anchor.earliest_result +
                         ". trading.get_trading_days would return 1 for every date before "
                         "the anchor and explode total_annualized_return just after it "
                         "(E2-F32). The anchor is NOT moved: the metadata row is authoritative and "
                         "the function's count stands. Fix the data: correct the row if it is "
                         "wrong, or remove the stray results if they are.");
                } else {
                    INFO("Annualization anchor " +
                         (anchor.effective_anchor.empty() ? std::string("(none yet)")
                                                          : anchor.effective_anchor) +
                         " is consistent with the book for " + combined_strategy_id + " / " +
                         portfolio_id);
                }
            }
        }

        double initial_capital = app_config.initial_capital;

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
        portfolio_config.max_strategy_allocation =
            app_config.strategy_defaults.max_strategy_allocation;
        portfolio_config.min_strategy_allocation =
            app_config.strategy_defaults.min_strategy_allocation;
        portfolio_config.use_optimization = app_config.use_optimization;
        portfolio_config.covariance_history_prices = app_config.covariance_history_prices;
        portfolio_config.covariance_stale_dates = app_config.covariance_stale_dates;
        portfolio_config.risk_modules = app_config.risk_schema.portfolio;
        portfolio_config.sleeve_risk_modules = app_config.risk_schema.sleeves;
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
        std::vector<std::shared_ptr<trade_ngin::StrategyInterface>> strategies;

        INFO("Creating " + std::to_string(strategy_names.size()) + " strategies from config");

        // Factory loop: create each strategy based on type
        for (const auto& strategy_name : strategy_names) {
            const auto& strategy_def = strategy_configs[strategy_name];
            std::string strategy_type = strategy_def.value("type", "TrendFollowingStrategy");
            double allocation = strategy_allocations[strategy_name];

            // Calculate capital allocation for this strategy
            trade_ngin::StrategyConfig strategy_config = base_strategy_config;
            strategy_config.capital_allocation = initial_capital * allocation;

            INFO("Creating strategy: " + strategy_name + " (type: " + strategy_type +
                 ", allocation: " + std::to_string(allocation * 100.0) + "%)");

            std::shared_ptr<trade_ngin::StrategyInterface> strategy;

            if (strategy_type == "TrendFollowingStrategy") {
                // Create TrendFollowingStrategy (normal speed)
                trade_ngin::TrendFollowingConfig trend_config;
                if (strategy_def.contains("config")) {
                    const auto& cfg = strategy_def["config"];
                    trend_config.weight = cfg.value("weight", 0.03);
                    trend_config.risk_target = cfg.value("risk_target", 0.2);
                    trend_config.idm = cfg.value("idm", 2.5);
                    trend_config.max_symbol_concentration =
                        cfg.value("max_symbol_concentration", 0.15);
                    trend_config.use_position_buffering = cfg.value("use_position_buffering", true);
                    trend_config.carver_buffer_floor = cfg.value(
                        "carver_buffer_floor", app_config.strategy_defaults.carver_buffer_floor);
                    trend_config.carver_buffer_position_factor =
                        cfg.value("carver_buffer_position_factor",
                                  app_config.strategy_defaults.carver_buffer_position_factor);
                    if (cfg.contains("ema_windows")) {
                        trend_config.ema_windows.clear();
                        for (const auto& window : cfg["ema_windows"]) {
                            trend_config.ema_windows.push_back(
                                {window[0].get<int>(), window[1].get<int>()});
                        }
                    }
                    trend_config.vol_lookback_short = cfg.value("vol_lookback_short", 32);
                    trend_config.vol_lookback_long = cfg.value("vol_lookback_long", 252);
                }
                // Set default FDM if not loaded
                if (trend_config.fdm.empty()) {
                    trend_config.fdm = app_config.strategy_defaults.fdm;
                }

                // The equity slow rule acts on the book's first sleeve only (LOOP_SPEC section 2.5,
                // D40); a first sleeve that does not carry the rule's pairs is refused when built.
                if (strategy_name == strategy_names.front()) {
                    trend_config.equity_slow_symbols = app_config.equity_slow_rule.symbols;
                    trend_config.equity_slow_pairs = app_config.equity_slow_rule.pairs;
                    // The first sleeve's own series and its risk target feed the risk overlay
                    // (LOOP_SPEC section 4: the three risk limits are ratios to this tau).
                    portfolio_config.overlay_sleeve = strategy_name;
                    portfolio_config.overlay_tau = trend_config.risk_target;
                }
                strategy = std::make_shared<trade_ngin::TrendFollowingStrategy>(
                    strategy_name, strategy_config, trend_config, db, registry_ptr);

            } else if (strategy_type == "TrendFollowingFastStrategy") {
                // The FAST sleeve: TrendFollowingStrategy on the fast configuration
                trade_ngin::TrendFollowingConfig trend_config =
                    trade_ngin::fast_trend_following_config();
                if (strategy_def.contains("config")) {
                    const auto& cfg = strategy_def["config"];
                    trend_config.weight = cfg.value("weight", 0.03);
                    trend_config.risk_target = cfg.value("risk_target", 0.25);
                    trend_config.idm = cfg.value("idm", 2.5);
                    trend_config.max_symbol_concentration =
                        cfg.value("max_symbol_concentration", 0.15);
                    trend_config.use_position_buffering =
                        cfg.value("use_position_buffering", false);
                    trend_config.carver_buffer_floor = cfg.value(
                        "carver_buffer_floor", app_config.strategy_defaults.carver_buffer_floor);
                    trend_config.carver_buffer_position_factor =
                        cfg.value("carver_buffer_position_factor",
                                  app_config.strategy_defaults.carver_buffer_position_factor);
                    if (cfg.contains("ema_windows")) {
                        trend_config.ema_windows.clear();
                        for (const auto& window : cfg["ema_windows"]) {
                            trend_config.ema_windows.push_back(
                                {window[0].get<int>(), window[1].get<int>()});
                        }
                    }
                    trend_config.vol_lookback_short = cfg.value("vol_lookback_short", 16);
                    trend_config.vol_lookback_long = cfg.value("vol_lookback_long", 252);
                }
                if (trend_config.fdm.empty()) {
                    trend_config.fdm = app_config.strategy_defaults.fdm;
                }

                // The equity slow rule acts on the book's first sleeve only (LOOP_SPEC section 2.5,
                // D40); a first sleeve that does not carry the rule's pairs is refused when built.
                if (strategy_name == strategy_names.front()) {
                    trend_config.equity_slow_symbols = app_config.equity_slow_rule.symbols;
                    trend_config.equity_slow_pairs = app_config.equity_slow_rule.pairs;
                    // The first sleeve's own series and its risk target feed the risk overlay
                    // (LOOP_SPEC section 4: the three risk limits are ratios to this tau).
                    portfolio_config.overlay_sleeve = strategy_name;
                    portfolio_config.overlay_tau = trend_config.risk_target;
                }
                strategy = std::make_shared<trade_ngin::TrendFollowingStrategy>(
                    strategy_name, strategy_config, trend_config, db, registry_ptr);

            } else {
                ERROR("Unknown strategy type: " + strategy_type +
                      " for strategy: " + strategy_name);
                return 1;
            }

            // Initialize strategy
            auto init_result = strategy->initialize();
            if (init_result.is_error()) {
                ERROR("Failed to initialize strategy " + strategy_name + ": " +
                      init_result.error()->what());
                return 1;
            }
            INFO("Strategy " + strategy_name + " initialization successful");

            // Start strategy
            auto start_result = strategy->start();
            if (start_result.is_error()) {
                ERROR("Failed to start strategy " + strategy_name + ": " +
                      start_result.error()->what());
                return 1;
            }
            INFO("Strategy " + strategy_name + " started successfully");

            strategies.push_back(strategy);
        }

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

            auto add_result =
                portfolio->add_strategy(strategy, allocation, portfolio_config.use_optimization);

            if (add_result.is_error()) {
                ERROR("Failed to add strategy " + strat_name +
                      " to portfolio: " + add_result.error()->what());
                return 1;
            }
            INFO("Strategy " + strat_name + " added to portfolio successfully");
        }

        INFO("All " + std::to_string(strategies.size()) + " strategies added to portfolio");

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

        // Load market data for daily processing
        INFO("Loading market data for daily processing...");
        // Disable MarketDataBus auto-publish to prevent duplicate processing
        MarketDataBus::instance().set_publish_enabled(false);
        INFO("MarketDataBus publishing DISABLED before get_market_data");
        auto market_data_result =
            db->get_market_data(symbols, start_date, end_date, trade_ngin::AssetClass::FUTURES,
                                trade_ngin::DataFrequency::DAILY, "ohlcv");
        INFO("MarketDataBus publishing RE-ENABLED after get_market_data");
        MarketDataBus::instance().set_publish_enabled(true);

        if (market_data_result.is_error()) {
            ERROR("Failed to load market data: " + std::string(market_data_result.error()->what()));
            return 1;
        }

        // Convert Arrow table to Bars using the same conversion as backtest
        auto conversion_result =
            trade_ngin::DataConversionUtils::arrow_table_to_bars(market_data_result.value());
        if (conversion_result.is_error()) {
            ERROR("Failed to convert market data to bars: " +
                  std::string(conversion_result.error()->what()));
            return 1;
        }

        auto all_bars = conversion_result.value();
        INFO("Loaded " + std::to_string(all_bars.size()) + " total bars");

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

        // DB-FUT-freshness: the equity runner's data-freshness guard (review T2.9,
        // corrected by BA-12), ported verbatim in policy to the futures runners.
        //
        // Nothing here ever asked how CURRENT the bars are. The check above only asks
        // whether ANY bar came back, so a feed that stopped three weeks ago passes it:
        // the 730-day window still returns tens of thousands of rows, every T-1 price
        // is a three-week-old close, and the book is re-sized against it and stored
        // without a word. `DB_AND_DATA_AUDIT_2026-08-27` R5.1/R5.2 decision 5.
        //
        // Measured from the STALEST symbol, not the freshest (BA-12): one symbol
        // printing today would otherwise report the whole feed current however far
        // behind the other thirty-five are, and a guard that cannot fail is not a
        // guard. The universe handed to `assess_feed_freshness` is the one this run
        // ASKED for, so a symbol that returned no rows at all is seen as absent
        // rather than being invisible to a map built from what came back.
        //
        // Thresholds and severity are the equity runner's, unchanged: WARN in
        // historical-replay mode, refuse in true-live mode, tolerance from
        // `live.data_staleness_tolerance_days`.
        {
            std::unordered_map<std::string, std::string> last_bar_date;
            for (const auto& bar : all_bars) {
                const std::string d = core::format_utc_date(bar.timestamp);
                auto it = last_bar_date.find(bar.symbol);
                if (it == last_bar_date.end() || d > it->second) last_bar_date[bar.symbol] = d;
            }

            const int tolerance_days = app_config.live.data_staleness_tolerance_days;
            const std::string as_of_ymd = core::format_utc_date(end_date);
            const auto freshness = assess_feed_freshness(last_bar_date, as_of_ymd, symbols);

            if (freshness.absent > 0) {
                // Absence is not "a few days behind" -- there is no date to measure. It
                // is reported on its own terms and treated as stale regardless of the
                // tolerance.
                const std::string msg =
                    "Futures feed is missing " + std::to_string(freshness.absent) + " of " +
                    std::to_string(freshness.symbols) +
                    " requested symbol(s) entirely, first: " + freshness.absent_symbol +
                    " (no bar of any date as of " + as_of_ymd + ").";
                if (use_override_date) {
                    WARN(msg + " Proceeding in historical-replay mode.");
                } else {
                    ERROR(msg + " Refusing to run live with an incomplete universe. "
                                "Refresh the OHLCV feed or remove the symbol from config.");
                    return 1;
                }
            }

            if (!freshness.any_data) {
                const std::string msg =
                    "Futures data freshness cannot be established: none of the " +
                    std::to_string(freshness.symbols) +
                    " loaded symbols carries a usable bar date as of " + as_of_ymd + ".";
                if (use_override_date) {
                    WARN(msg + " Proceeding in historical-replay mode.");
                } else {
                    ERROR(msg + " Refusing to run live without a feed. Refresh the OHLCV "
                                "feed.");
                    return 1;
                }
            } else if (freshness.days_behind > tolerance_days) {
                const std::string msg =
                    "Futures data is stale: the stalest of " +
                    std::to_string(freshness.symbols) + " symbols (" +
                    freshness.stalest_symbol + ") last printed " + freshness.stalest_date +
                    ", " + std::to_string(freshness.days_behind) +
                    " calendar days before " + as_of_ymd + " (tolerance " +
                    std::to_string(tolerance_days) + " days).";
                if (use_override_date) {
                    WARN(msg + " Proceeding in historical-replay mode.");
                } else {
                    ERROR(msg + " Refusing to run live on stale data. Refresh the OHLCV "
                                "feed or raise live.data_staleness_tolerance_days.");
                    return 1;
                }
            } else {
                INFO("Futures feed freshness: stalest of " +
                     std::to_string(freshness.symbols) + " symbols (" +
                     freshness.stalest_symbol + ") at " + freshness.stalest_date + ", " +
                     std::to_string(freshness.days_behind) + " days behind " + as_of_ymd +
                     " (tolerance " + std::to_string(tolerance_days) + ").");
            }
        }

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
        // Phase 6 §6a: path resolved via HolidayChecker::resolve_holidays_path.
        HolidayChecker holiday_checker(HolidayChecker::resolve_holidays_path());
        // BA-1: fail closed on a calendar that did not load fully. is_holiday
        // cannot distinguish "the market was open" from "the calendar is
        // missing", so a partial load silently turns a closure into a trading
        // day and the non-trading-day branch below never fires.
        if (!holiday_checker.loaded()) {
            std::cerr << "FATAL: market holiday calendar failed to load from "
                      << HolidayChecker::resolve_holidays_path()
                      << " - refusing to run. Every date would report as a trading day."
                      << std::endl;
            return 1;
        }
        auto yesterday_for_check = now - std::chrono::hours(24);
        auto yesterday_time_t_check = std::chrono::system_clock::to_time_t(yesterday_for_check);
        std::tm yesterday_tm_check = *std::gmtime(&yesterday_time_t_check);
        std::ostringstream yesterday_oss_check;
        yesterday_oss_check << std::put_time(&yesterday_tm_check, "%Y-%m-%d");
        std::string yesterday_date_str_check = yesterday_oss_check.str();

        // FUT-covers_date (C-5 B4). `loaded()` above says the FILE parsed. It says
        // nothing about whether the calendar reaches the date being asked about, and
        // outside the covered years `is_holiday` returns false because the answer is
        // UNKNOWN, not because the market was open. A run past the end of the calendar
        // would therefore treat every closure as a trading day, take the normal-day
        // branch on a holiday, and write executions for a day the exchange was shut --
        // with no error and no log line, because nothing here ever asked.
        //
        // The equity runner has failed closed on this since BA-1
        // (live_equity_mean_reversion.cpp:332). Same check here, on the two dates this
        // runner actually asks the calendar about: the run date, which fixes the data
        // window, and the previous calendar day, which is the argument `is_holiday`
        // is given directly below. Both, because coverage is per YEAR and a run on
        // 1 January asks about 31 December of the year before.
        {
            char cov_buf[11];
            std::tm cov_tm{};
            auto cov_t = std::chrono::system_clock::to_time_t(now);
            gmtime_r(&cov_t, &cov_tm);
            std::strftime(cov_buf, sizeof(cov_buf), "%Y-%m-%d", &cov_tm);
            for (const std::string& cov_date : {std::string(cov_buf), yesterday_date_str_check}) {
                if (!holiday_checker.covers_date(cov_date)) {
                    ERROR("Holiday calendar does not cover " + cov_date + " (loaded: " +
                          holiday_checker.coverage_description() + ", " +
                          std::to_string(holiday_checker.coverage_years()) +
                          " year(s)). Trading-day arithmetic would treat market closures "
                          "as open days. Extend the calendar via "
                          "scripts/generate_market_holidays.py before running this date.");
                    return 1;
                }
            }
        }
        bool is_yesterday_holiday = holiday_checker.is_holiday(yesterday_date_str_check);

        // ========================================
        // T-1 SESSION CLASSIFIER (T-7a C4; HD 2026-09-17 and 2026-09-19)
        // Every symbol's T-1 is SESSION, JUNK, NO_BAR(closure) or NO_BAR(feed hole), judged
        // against its own bars in this window (include/trade_ngin/data/session_classifier.hpp).
        // T-1 is the date the price manager's T-1 test uses, so a symbol has a T-1 price exactly
        // when its verdict is SESSION or JUNK. The day-classification abort ("DATA ISSUE
        // DETECTED", return 1) is deleted: when no symbol printed, the whole book is carried
        // below with the reason logged (a feed hole is an ERROR there, not an abort), so the
        // dead-Sunday Mondays 2026-05-18 and 05-25 no longer end the chain.
        // ========================================
        SessionClassifier session_classifier;
        session_classifier.add_bars(all_bars);
        // LOOP_SPEC v6.2 section 2.1 (N-2): a bar's K-01 verdict reads the classifier's trailing
        // history, the same in every engine. The classifier alone also reads the bars of a fixed
        // prefix before the window (live/live_roll_legs.hpp, kK01ClassifierHistoryDays), with their
        // ids, so a print near the window's start is judged as the backtest judges it and a bar
        // withheld on its own run is never fed later. The strategies' window, the price manager and
        // k01_consumed_bars below stay on the window.
        const Timestamp k01_history_start = k01_classifier_history_start(start_date);
        // The same load reaches back to the estimators' history start: the trend sleeves' window
        // is W consumed bars, more than the bar window holds. The bars before the window are
        // judged by their own classifier and the consumed ones seed the sleeves' history below
        // (live/live_estimator_history.hpp); nothing else reads them.
        std::vector<Bar> estimator_history_bars;
        {
            MarketDataBus::instance().set_publish_enabled(false);
            auto history_result = db->get_market_data(
                symbols, estimator_history_start(start_date), start_date,
                trade_ngin::AssetClass::FUTURES, trade_ngin::DataFrequency::DAILY, "ohlcv");
            MarketDataBus::instance().set_publish_enabled(true);
            if (history_result.is_error()) {
                ERROR("T1_CLASSIFIER history: failed to load the bars before the window: " +
                      std::string(history_result.error()->what()) +
                      ". Refusing to run: the window's first bars cannot be judged.");
                return 1;
            }
            auto history_bars =
                trade_ngin::DataConversionUtils::arrow_table_to_bars(history_result.value());
            if (history_bars.is_error()) {
                ERROR("T1_CLASSIFIER history: failed to convert the bars before the window: " +
                      std::string(history_bars.error()->what()) +
                      ". Refusing to run: the window's first bars cannot be judged.");
                return 1;
            }
            session_classifier.add_bars(k01_classifier_history(history_bars.value(), start_date));
            estimator_history_bars = estimator_history_consumed(
                history_bars.value(), start_date,
                db->get_futures_instrument_ids(symbols, estimator_history_start(start_date),
                                               start_date));
        }
        // T-7b-2 C10a (HD 2026-09-24 ruling 16): the instrument-id continuity limb reads each kept
        // bar's vendor id over the window the bars were loaded for (the backtest reads the same
        // query). T-1's verdict reads no later bar: an id change on T-1 is held today and T's bar
        // confirms it a roll or a one-day flip on the next run.
        {
            const auto id_feed = feed_instrument_ids(
                session_classifier,
                db->get_futures_instrument_ids(symbols, k01_history_start, end_date));
            if (id_feed.fed) {
                INFO(id_feed.line);
            } else {
                WARN(id_feed.line);
            }
        }
        const T1Classification t1_classification =
            classify_t1(session_classifier, symbols,
                        SessionClassifier::day_of(now - std::chrono::hours(24)),
                        holiday_lookup(holiday_checker));
        log_t1_classification(t1_classification,
                              app_config.live.data_staleness_tolerance_days);

        // FEED-HOLE REFUSAL (HD 2026-09-19; T-4c F4, T-5 F7): a HELD symbol with no bar for
        // longer than live.data_staleness_tolerance_days refuses the run when the run date is
        // the host's date (true live). A replay of a past date WARNs and runs. It sits above the
        // live_run_metadata upsert like the other refusals, so a refused run leaves no row.
        // T-7b-1 C7b R3: any no-bar verdict counts, a closure too (a feed dead for more than
        // eight weeks is reclassified as a closure and used to stop refusing).
        {
            const int hole_tolerance_days = app_config.live.data_staleness_tolerance_days;
            std::vector<HeldFeedHole> held_holes;
            if (max_no_bar_age_days(t1_classification) > hole_tolerance_days) {
                auto held_book_result = db->load_positions_by_date(
                    combined_strategy_id, "", coordinator_config.portfolio_id,
                    now - std::chrono::hours(24), "trading.positions");
                if (held_book_result.is_ok()) {
                    held_holes = held_feed_holes_past_tolerance(
                        t1_classification, held_book_result.value(), hole_tolerance_days);
                } else {
                    WARN("FEED_HOLE_REFUSAL could not read the held book to test the feed "
                         "holes: " +
                         std::string(held_book_result.error()->what()));
                }
            }
            if (!held_holes.empty()) {
                const bool true_live =
                    run_date_is_host_date(now, std::chrono::system_clock::now());
                for (const auto& hole : held_holes) {
                    const std::string msg =
                        "FEED_HOLE_REFUSAL held symbol " + hole.symbol + " (quantity " +
                        std::to_string(hole.held_quantity) + ") has had no bar for " +
                        std::to_string(hole.age_days) + " day(s) (last bar " +
                        hole.last_bar_date + "), past live.data_staleness_tolerance_days=" +
                        std::to_string(hole_tolerance_days);
                    if (true_live) {
                        ERROR(msg + ". Refusing to run live: refresh the OHLCV feed, then run "
                                    "this date.");
                    } else {
                        WARN(msg + ". Replay of a past date: not refused.");
                    }
                }
                if (true_live) {
                    return 1;
                }
            }
        }

        // ========================================
        // THE STRATEGY FEED (T-7a C4; built here by T-7b-1 C7b R10; LOOP_SPEC v6.2 section 2.1, K-01)
        // Every bar of the window except the WITHHELD ones: a JUNK bar or a thin first print, on any
        // date of the window, is never consumed and never fed (K-01 retires the earlier rule, which
        // withheld only the T-1 bar and fed it as T-2 on the next run). An unconfirmed instrument-id
        // change the classifier holds is consumed: it is the change bar, held under D37. The
        // strategies and the PortfolioManager are fed it below, and so are both cost managers
        // (T-7a_CODE_REVIEW R10: a junk print must not enter the volume and volatility the cost model
        // reads; K2 reads this feed) and the snapshot risk reader. A non-SESSION symbol is held on
        // every book today, so it has no fill to cost.
        // ========================================
        std::vector<SymbolDayVerdict> k01_withheld;
        const std::vector<Bar> k01_feed =
            k01_consumed_bars(session_classifier, all_bars, &k01_withheld);
        const std::vector<Bar>& strategy_feed_bars = k01_feed;
        std::vector<std::string> withheld_junk_bars;  // the symbols whose T-1 bar is withheld
        for (const auto& v : k01_withheld) {
            if (v.date == t1_classification.t1_date) withheld_junk_bars.push_back(v.symbol);
        }
        std::sort(withheld_junk_bars.begin(), withheld_junk_bars.end());
        // LOOP_SPEC v6.1 sections 2.1, 2.2 (D37): each symbol's roll status on the window's CONSUMED
        // bars (k01_feed: a withheld bar never walks it), evaluated bar by bar, so a run that consumes
        // several bars of a symbol (a catch-up after a missed run) reads the status of its LAST one.
        const auto roll_status = roll_series::roll_status_of(strategy_feed_bars);
        std::unordered_map<std::string, std::string> last_consumed_date;
        for (const auto& bar : strategy_feed_bars) {
            const std::string d = core::format_utc_date(bar.timestamp);
            auto& last = last_consumed_date[bar.symbol];
            if (d > last) last = d;
        }
        // LOOP_SPEC v6.2 section 6.5 (L-09, D3, B8): the rolls this run legs, by STATE: every roll a
        // consumed bar confirmed since the contract recorded on the symbol's stored T-1 positions
        // row (live/live_roll_legs.hpp), never the rolls of a calendar span, so a late (back-filled)
        // confirming bar is legged on the run that first consumes it. A re-run reads the contract its
        // own stored legs rolled out of: the first run re-wrote the T-1 row. Each roll is booked once;
        // a re-run of today re-books today's (the sweep below clears them).
        LiveRollState roll_state;
        {
            std::unordered_map<std::string, std::string> recorded_contract;
            for (const auto& sleeve : strategy_names) {
                auto stored_book = db->load_positions_by_date(
                    combined_strategy_id, sleeve, coordinator_config.portfolio_id,
                    now - std::chrono::hours(24), "trading.positions");
                if (stored_book.is_error()) {
                    ERROR("ROLL_LEG STOP: sleeve " + sleeve + "'s stored Day T-1 book is unreadable (" +
                          std::string(stored_book.error()->what()) +
                          "); refusing to run: the contract it holds, and so the rolls to book, "
                          "cannot be told");
                    std::cerr << "ROLL_LEG STOP: sleeve " << sleeve
                              << "'s stored Day T-1 book is unreadable" << std::endl;
                    return 1;
                }
                for (const auto& [symbol, position] : stored_book.value()) {
                    if (position.quantity.as_double() == 0.0) continue;
                    auto& contract = recorded_contract[symbol];
                    if (contract.empty()) contract = position.instrument_id;
                }
            }
            auto stored_legs = db->get_stored_roll_contracts(combined_strategy_id, portfolio_id, now,
                                                             "trading.executions");
            if (stored_legs.is_error()) {
                ERROR("ROLL_LEG STOP: today's stored ROLL legs are unreadable (" +
                      std::string(stored_legs.error()->what()) +
                      "); refusing to run: the rolls to book cannot be told");
                return 1;
            }
            for (const auto& [symbol, contract] : stored_legs.value()) {
                const auto held = recorded_contract.find(symbol);
                if (held != recorded_contract.end() && !contract.empty()) held->second = contract;
            }
            roll_state = live_rolls_by_state(strategy_feed_bars, recorded_contract,
                                             t1_classification.t1_date);
            if (!roll_state.unplaced.empty()) {
                std::string unplaced;
                for (const auto& symbol : roll_state.unplaced) {
                    unplaced += (unplaced.empty() ? "" : ", ") + symbol + " (" +
                                recorded_contract[symbol] + ")";
                }
                ERROR("ROLL_LEG STOP: the contract recorded on the stored T-1 row is at no consumed "
                      "bar of the window for " + unplaced +
                      "; refusing to run: the rolls to book cannot be told. The instrument_id on "
                      "that symbol's stored T-1 trading.positions row does not appear on the "
                      "consumed bars. Remedy: restore the symbol's bars or their instrument ids "
                      "(futures_data.ohlcv_1d, futures_data.ohlcv_1d_raw), or set that row's "
                      "instrument_id to NULL, after which the run legs only a roll its T-1 bar "
                      "confirms. Every run refuses until then.");
                return 1;
            }
            // T-ROLLX-FIX commit 6 (section 6.5, finding 4): a roll legged LATE books the moves of
            // every consumed bar after the stored state, LESS what earlier runs already booked of
            // them. That is read from the stored rows, never guessed from the bars: a run that
            // consumed a bar booked its move on the row dated that bar, a run that saw a hole
            // booked 0 (live/live_roll_legs.hpp, late_booked_points).
            if (!roll_state.late.empty()) {
                std::vector<std::string> late_symbols;
                std::string booked_after;
                for (const auto& [symbol, late] : roll_state.late) {
                    late_symbols.push_back(symbol);
                    if (booked_after.empty() || late.booked_after < booked_after) {
                        booked_after = late.booked_after;
                    }
                }
                std::sort(late_symbols.begin(), late_symbols.end());
                auto stored_rows = db->get_stored_realised_rows(
                    combined_strategy_id, portfolio_id, booked_after, t1_classification.t1_date,
                    "trading.positions");
                if (stored_rows.is_error()) {
                    ERROR("ROLL_LEG STOP: the stored rows after " + booked_after +
                          " are unreadable (" + std::string(stored_rows.error()->what()) +
                          "); refusing to run: what earlier runs booked of the late roll's moves "
                          "cannot be told");
                    std::cerr << "ROLL_LEG STOP: the stored rows of a late roll are unreadable"
                              << std::endl;
                    return 1;
                }
                for (const auto& symbol : late_symbols) {
                    auto& late = roll_state.late[symbol];
                    const LateBooked booked = late_booked_points(
                        symbol, late.booked_after, t1_classification.t1_date, stored_rows.value(),
                        pnl_manager->get_point_value(symbol));
                    if (!booked.undecided.empty()) {
                        ERROR("ROLL_LEG STOP " + symbol + ": the stored rows cannot tell what "
                              "earlier runs booked of its late roll's moves: " + booked.undecided +
                              "; refusing to run. Remedy: correct daily_realized_pnl on the "
                              "symbol's trading.positions rows dated after " + late.booked_after +
                              " and before " + t1_classification.t1_date +
                              " so that every sleeve's row of a date books the same move per "
                              "contract (0 where no run consumed a bar), then run this date "
                              "again. Every run refuses until then.");
                        std::cerr << "ROLL_LEG STOP " << symbol
                                  << ": the stored rows cannot decide a late roll's settlement"
                                  << std::endl;
                        return 1;
                    }
                    std::string booked_dates;
                    for (const auto& d : booked.dates) booked_dates += (booked_dates.empty() ? "" : ",") + d;
                    INFO("ROLL_LEG late " + symbol + ": the consumed bars after " + late.booked_after +
                         " moved " + std::to_string(late.settle_to - late.settle_from) +
                         " points; the stored rows already book " + std::to_string(booked.points) +
                         " (" + (booked_dates.empty() ? std::string("none") : booked_dates) +
                         "); the Day T-1 row books " +
                         std::to_string(late.settle_to - late.settle_from - booked.points));
                    late.settle_from += booked.points;
                }
            }
        }
        const std::vector<ConfirmedRoll>& confirmed_rolls = roll_state.rolls;
        // T-ROLLX-FIX (LOOP_SPEC v6.2 sections 2.1, 6.6): the T-1 settlement on the CONSUMED bars. A
        // symbol whose T-1 bar is a change bar (a roll's switch day or either bar of a flip) or a
        // WITHHELD bar (K-01) books no move for T-1; every other symbol books T-1 against the close of
        // its previous CONSUMED bar (a withheld bar between them is skipped), which is the T-2 close
        // whenever no bar was withheld; a symbol whose roll is legged late books its unbooked bars.
        // Built here, above the sizing read (D-B): the book is sized on this settlement, the one
        // PHASE 5 finalises Day T-1 with, so the sizing equity is the equity the stored rows show.
        const ConsumedT1Settlement t1_settlement = consumed_t1_settlement(
            strategy_feed_bars, t1_classification.t1_date, roll_status, withheld_junk_bars,
            price_manager->get_all_two_days_ago_prices(),
            price_manager->get_all_previous_day_prices(), roll_state.late);

        // ========================================
        // SIZING CAPITAL (T-7b-2 9c; HD 2026-09-25, compounding)
        // The book is sized on the account's equity at the close of T-1, the bar the strategies
        // size from, the quantity the futures backtest sizes on (its equity curve's last row).
        // Day T-1 is finalised only in STEP 4, after the rebalance, so it is rebuilt here from
        // STEP 4's own parts (portfolio/sizing_capital.hpp): the stored value of the row before
        // Day T-1, Day T-1's settlement move and Day T-1's stored costs. Day T-1's own stored value
        // is never read (a replayed date's is already finalised), and STEP 5's reading is logged
        // beside it. Every sizing input follows it: each sleeve's capital (x its allocation), the
        // optimizer's weights, the gate's leverage and the snapshot reporter. It sits above the
        // live_run_metadata upsert like the other refusals, so a run that cannot set it leaves no
        // row.
        // ========================================
        LiveSizingEquity sizing_equity;
        // LOOP_SPEC section 3.1 (D19): the capital the book is sized on is the half compounding of
        // the book's settled daily P&L (live/live_sizing_read.hpp), recomputed on every run from
        // the stored rows before Day T-1 and Day T-1's rebuilt net; `sizing_equity` above stays
        // the account's value rebuilt at the close of T-1, for the log and the check.
        LiveSizingRead sizing_capital_read;
        // T-7b-3 R-3 (HD 2026-09-27 ruling 5; live/live_sizing_read.hpp): each read's own "nothing
        // stored" answer sizes as before (no Day T-1 row, no row before it, a sleeve with no stored
        // book). A sleeve book that fails to load refuses the run here (exit 1, no row: nothing to
        // hold). With every book loaded, a failed Day T-1 row or previous-row read is a SIZING
        // HOLD: no figure is set, every strategy is held at its seeded T-1 book with no rebalance
        // and no order, today's live_run_metadata row is marked, the email is flagged and the run
        // exits kRiskModuleFailureExitCode, as a RISK_MODULE_FAILURE day does.
        std::optional<nlohmann::json> sizing_hold;
        {
            auto sizing_read = read_live_sizing_equity(
                *data_loader, *db, combined_strategy_id, coordinator_config.portfolio_id,
                strategy_names, now, initial_capital, t1_settlement.t1_close_prices,
                t1_settlement.t2_close_prices,
                [&](const std::string& symbol) { return pnl_manager->get_point_value(symbol); },
                t1_settlement.zero_pnl_symbols, [&] {
                    // The dates the run loaded a bar on, and the finalize's own two tests on the
                    // price manager's raw maps (PHASE 5's "No T-1 close prices available", STEP
                    // 4's first clause).
                    LiveSizingCalendar calendar;
                    for (const auto& bar : all_bars) {
                        calendar.bar_dates.insert(
                            SessionClassifier::ymd(SessionClassifier::day_of(bar.timestamp)));
                    }
                    if (!calendar.bar_dates.empty()) {
                        calendar.first_bar_date = *calendar.bar_dates.begin();
                    }
                    calendar.no_t1_closes = price_manager->get_all_previous_day_prices().empty();
                    calendar.no_t2_closes = price_manager->get_all_two_days_ago_prices().empty();
                    return calendar;
                }());
            if (sizing_read.outcome == LiveSizingOutcome::kRefuseRun) {
                ERROR("SIZING_CAPITAL refused: " + sizing_read.failure +
                      ". Refusing to run: the book cannot be sized and there is no book to hold.");
                std::cerr << "SIZING_CAPITAL refused: " << sizing_read.failure << std::endl;
                return 1;
            } else if (sizing_read.outcome == LiveSizingOutcome::kHoldBook) {
                sizing_hold = sizing_hold_refusal(sizing_read.failure);
                ERROR("SIZING_HOLD " + sizing_read.failure +
                      ": the book is not sized; every strategy is held at its seeded T-1 book and "
                      "no orders are sent; the day is stored as a REFUSE day and the run exits " +
                      std::to_string(kRiskModuleFailureExitCode));
            } else {
                sizing_equity = sizing_read.equity;
                sizing_capital_read = sizing_read;
                INFO(sizing_capital_log_line(core::format_utc_date(now), sizing_read));
                if (sizing_read.t1_unsettled) {
                    WARN(sizing_capital_unsettled_log_line(core::format_utc_date(now), sizing_read));
                }
                auto sized = portfolio->set_sizing_capital(sizing_read.capital.capital);
                if (sized.is_error()) {
                    ERROR("SIZING_CAPITAL refused: " + std::string(sized.error()->what()) +
                          ". Refusing to run: the book cannot be sized on the account's equity.");
                    std::cerr << "SIZING_CAPITAL refused: " << sized.error()->what() << std::endl;
                    return 1;
                }
            }
        }

        // ========================================
        // STORE LIVE RUN METADATA
        // Save run metadata (allocations, configs) for this trading day. Written only
        // now, after the run-gap (A3), feed-freshness (A2), calendar-coverage (A1) and
        // held-feed-hole (T-7a C4) guards have all passed: a refused run must leave no
        // row. (The day-classification refusal, S-1, is gone: a day with no T-1 price is
        // carried, never refused.)
        // ========================================
        INFO("Storing live run metadata for this trading day...");
        // Kept at this scope: a portfolio risk REFUSE found by process_market_data writes
        // this row a second time, from the same values, with the refusal marked.
        nlohmann::json portfolio_config_json;
        portfolio_config_json["total_capital"] = static_cast<double>(portfolio_config.total_capital);
        portfolio_config_json["use_optimization"] = portfolio_config.use_optimization;

        // Convert strategy_allocations to JSON
        nlohmann::json strategy_alloc_json(strategy_allocations);
        // T-7b-3 R-3: a sizing hold's row is written marked (risk_refusal, scope "sizing").
        if (sizing_hold) {
            portfolio_config_json = mark_risk_refusal(portfolio_config_json, *sizing_hold,
                                                      portfolio->risk_decisions_json());
        }
        {
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
                if (sizing_hold) {
                    INFO("Marked today's live_run_metadata row with the sizing hold");
                }
            }
        }

        // Flag to track if we should skip strategy processing
        bool skip_strategy_processing = false;
        // LOOP_SPEC v6.2 section 6.1 (D37, D-A): the symbols whose last consumed bar is pending,
        // held on EVERY rebalance by the book gate below, never only through a cut.
        std::unordered_set<std::string> change_bar_holds;

        // Data structures for non-trading day case
        std::unordered_map<std::string, std::unordered_map<std::string, Position>>
            strategy_positions_map;
        std::unordered_map<std::string, Position> positions;

        // The book-level rule (HD 2026-09-17): carry the whole book when NO symbol printed on T-1,
        // i.e. the T-1 price map is empty. It is a carry on every such day, never an abort; the
        // classifier names the reason (a closure is INFO, a feed hole an ERROR).
        if (early_previous_day_close_prices.empty()) {
            // ========================================
            // No symbol has a T-1 price: reuse previous positions, skip strategy processing
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
            log_whole_book_carry(t1_classification);

            INFO("No new market data available - positions will remain unchanged");
            INFO("Loading previous trading day positions to carry forward...");

            // Calculate previous date for position loading
            auto previous_date_nontrade = now - std::chrono::hours(24);

            // Load previous positions for each strategy and use as current
            for (const auto& [strategy_name, allocation] : strategy_allocations) {
                auto prev_result = db->load_positions_by_date(
                    combined_strategy_id, strategy_name, coordinator_config.portfolio_id,
                    previous_date_nontrade, "trading.positions");

                if (prev_result.is_ok() && !prev_result.value().empty()) {
                    strategy_positions_map[strategy_name] = prev_result.value();
                    INFO("Loaded " + std::to_string(prev_result.value().size()) +
                         " positions for strategy: " + strategy_name);
                } else {
                    INFO("No previous positions found for strategy: " + strategy_name);
                    strategy_positions_map[strategy_name] = {};
                }
            }

            // T-7b-1 C7b S1-3 (T-7a_S1_S3_CODE_REVIEW): the combined map is the SUM over the
            // sleeves, as on a trading day; it used to keep whichever sleeve was loaded last.
            rebuild_combined_positions(positions, strategy_positions_map);

            INFO("Total positions carried forward: " + std::to_string(positions.size()));
            INFO("═══════════════════════════════════════════════════════════════");
            INFO("Skipping strategy calculations - proceeding to storage phase");
            INFO("═══════════════════════════════════════════════════════════════");

            skip_strategy_processing = true;
        }

        // LOOP_SPEC v6.2 section 6.5: the rolls this run legs (found by state above).
        for (const auto& r : confirmed_rolls) {
            INFO("ROLL_CONFIRMED " + r.symbol + " date=" + r.confirm_date + " " + r.outgoing_id +
                 "->" + r.incoming_id + " closing_px=" + std::to_string(r.closing_price) +
                 " opening_px=" + std::to_string(r.opening_price) +
                 " change_bars=" + std::to_string(r.change_bars) +
                 ": the next consumed bar kept the new id; the legs are booked on this run");
        }

        // ========================================
        // UPDATE TRANSACTION COST MANAGER WITH MARKET DATA
        // K2 (T-7b-1 C8a): the fill day's own volume for participation and the impact tier,
        // the walk of returns ending at T-1 for the volatility term (futures_cost_feed.hpp).
        // The execution manager's cost manager is fed here, and (T-7b-1 C8d, H-2) the
        // PortfolioManager's right after it, from the same feed.
        // ========================================
        INFO("Updating execution manager with market data for transaction cost tracking...");

        // Map of latest bars per symbol (T-1 data), from the strategy feed: a JUNK symbol's
        // latest bar here is its T-2 bar. On a carried day the feed is every bar, so the
        // carried positions file's last marks (read from this map) are unchanged.
        std::unordered_map<std::string, Bar> latest_bars_per_symbol;
        for (const auto& bar : strategy_feed_bars) {
            auto it = latest_bars_per_symbol.find(bar.symbol);
            if (it == latest_bars_per_symbol.end() || bar.timestamp > it->second.timestamp) {
                latest_bars_per_symbol[bar.symbol] = bar;
            }
        }

        {
            auto& cost_model = execution_manager->get_transaction_cost_manager();
            // C8c3 (HD 2026-09-25 rulings 25 and 28): the run date decides the weekend merge's
            // rule for a symbol whose T-1 bar is a weekend stub (futures_cost_feed.hpp).
            const auto cost_feed = feed_futures_cost_model(cost_model, strategy_feed_bars, now);
            for (const auto& fed : cost_feed.symbols) {
                INFO("COST_FEED " + fed.symbol + " own_day=" +
                     core::format_utc_date(fed.own_day_time) +
                     " own_day_volume=" + std::to_string(fed.own_day_volume) +
                     " bars=" + std::to_string(fed.bars) +
                     " returns=" + std::to_string(fed.returns) +
                     " vol_mult=" + std::to_string(cost_model.get_volatility_multiplier(fed.symbol)) +
                     // C8c3 (HD 2026-09-25 rulings 25 and 28): the weekend merge, when it applied
                     (fed.merged_weekend_bars > 0
                          ? " weekend_merged_volume=" + std::to_string(fed.merged_weekend_volume) +
                                " weekend_bars=" + std::to_string(fed.merged_weekend_bars) +
                                " previous_session_volume=" +
                                std::to_string(fed.previous_session_volume) +
                                " participation_volume=" + std::to_string(fed.participation_volume)
                          : std::string()));
            }
            std::string thin_list;
            for (const auto& s : cost_feed.thin) thin_list += (thin_list.empty() ? "" : ", ") + s;
            INFO("Updated transaction cost manager with market data for " +
                 std::to_string(cost_feed.symbols.size()) + " symbols (" +
                 std::to_string(cost_feed.returns_fed) + " log returns; fewer than 21 bars: " +
                 (thin_list.empty() ? std::string("none") : thin_list) + ")");
            if (!cost_feed.repeated_instants.empty()) {
                std::string repeated;
                for (const auto& s : cost_feed.repeated_instants) {
                    repeated += (repeated.empty() ? "" : ", ") + s;
                }
                WARN("COST_FEED two bars at one instant for " + repeated +
                     " (the loader keeps one bar per symbol-instant; fed as given)");
            }
        }

        // H-2 (T-7b-1 C8d): the PortfolioManager's own cost manager prices the optimizer's cost
        // vector (calculate_trading_costs). Live never fed it, so every entry was priced off its
        // fallbacks (ADV 100,000, vol_mult 1.0). It is fed the SAME K2 feed as the execution
        // manager's, here, before process_market_data runs the optimizer below.
        {
            auto& optimizer_cost_model = portfolio->get_transaction_cost_manager();
            const auto optimizer_feed =
                feed_futures_cost_model(optimizer_cost_model, strategy_feed_bars, now);
            INFO("COST_FEED_OPTIMIZER fed the PortfolioManager's cost model (the optimizer's cost "
                 "vector) for " + std::to_string(optimizer_feed.symbols.size()) + " symbols (" +
                 std::to_string(optimizer_feed.returns_fed) + " log returns)");
        }

        // Set when a portfolio risk module could not answer and the PortfolioManager held the
        // book (T-7a C5, HD 2026-09-21 option b): the day is stored as a REFUSE day, the email
        // is flagged and main() exits kRiskModuleFailureExitCode instead of 0.
        std::optional<nlohmann::json> risk_module_failure;

        // ========================================
        // NORMAL TRADING DAY PROCESSING
        // Only run strategy calculations if NOT a non-trading day
        // ========================================
        if (!skip_strategy_processing) {
            // FIX: Seed every sleeve's positions_ and PortfolioManager slot from yesterday's DB
            // snapshot BEFORE the day's rebalance. Each live invocation is a fresh process where
            // positions_ and current_positions default to zero. The Carver position buffer reads
            // positions_ as the comparison anchor, and the optimizer reads current_positions as
            // its baseline; without seeding both correctly the book trades a phantom difference
            // every day. Backtest doesn't need this because its state is continuous in-memory
            // across simulated days.
            // T-7a C3: EVERY sleeve is seeded from its OWN stored rows (the runner used to seed
            // strategy_names[0] only, so a second sleeve was anchored at zero or at whatever the
            // PM held). The mechanism (seed_positions + update_strategy_position per row) is the
            // held-book seed of eae0d5a7, unchanged; see include/trade_ngin/live/sleeve_seeding.hpp.
            // NOTE (ledger BASE-opposite-sleeve-anchor): two sleeves holding OPPOSITE signs on one
            // symbol are each seeded with their own signed row, so the optimizer's anchor is the
            // net while each sleeve's execution diff is gross. The netted design (gross per sleeve
            // for attribution, the account and the risk gate on the net, the cross logged at zero
            // cost; HD 2026-09-19) lands with netting in T-7b, not here.
            {
                auto seed_previous_date = now - std::chrono::hours(24);
                seed_every_sleeve(strategies, strategy_names, *portfolio,
                                  [&](const std::string& seed_strategy_name) {
                                      return db->load_positions_by_date(
                                          combined_strategy_id, seed_strategy_name,
                                          coordinator_config.portfolio_id, seed_previous_date,
                                          "trading.positions");
                                  });
            }

            // The sleeves' estimator history before the window, ahead of the window's own feed.
            {
                auto seeded = portfolio->seed_strategy_history(estimator_history_bars);
                if (seeded.is_error()) {
                    ERROR("The estimators' history before the window could not be seeded: " +
                          std::string(seeded.error()->what()) + ". Refusing to run.");
                    return 1;
                }
            }

            // Process data through portfolio pipeline (optimization + risk), mirroring backtest
            INFO("Processing data through portfolio manager (optimization + risk)...");
            // Disable MarketDataBus to prevent duplicate processing during explicit data feed
            MarketDataBus::instance().set_publish_enabled(false);
            INFO("MarketDataBus publishing DISABLED before process_market_data");
            // JUNK (T-7a C4): a withheld T-1 bar (K-01) is kept out of the strategy and the
            // portfolio stage, so its signal is not updated today, and it is never fed on a later
            // run either. The price manager already has every bar: the mark uses it.
            // The feed (strategy_feed_bars) is built above the cost feed (T-7b-1 C7b R10).
            if (!withheld_junk_bars.empty()) {
                std::string withheld_list;
                for (const auto& s : withheld_junk_bars) {
                    withheld_list += (withheld_list.empty() ? "" : ", ") + s;
                }
                INFO("BOOK_GATE withheld the JUNK T-1 bar of " +
                     std::to_string(withheld_junk_bars.size()) + " symbol(s) from the strategy "
                     "and portfolio feed (signal not updated today): " + withheld_list);
            }
            // T-7b-3 D-1b (HD 2026-09-27): a symbol whose T-1 verdict is not SESSION is held at
            // its stored T-1 quantity after the rebalance (hold_non_session_symbols, the same
            // key), so a lap the risk gate cuts fixes it at its held quantity and never cuts it.
            {
                std::unordered_set<std::string> book_gate_holds;
                change_bar_holds.clear();
                for (const auto& symbol : symbols) {
                    if (!t1_classification.is_session(symbol)) book_gate_holds.insert(symbol);
                    // D37 (sections 2.1, 2.2): a symbol whose last consumed bar is pending (a change
                    // bar, either bar of a flip, an id-less bar inside a pending roll; code review
                    // D2) is held at its stored T-1 quantity on every book until its next consumed
                    // bar, whatever that bar's date.
                    const auto rs = roll_status.find(symbol);
                    if (rs == roll_status.end() || !rs->second.holds()) continue;
                    book_gate_holds.insert(symbol);
                    change_bar_holds.insert(symbol);
                    INFO("CHANGE_BAR_HOLD " + symbol + " date=" + core::format_utc_date(now) +
                         " kind=" +
                         (rs->second.flip ? "flip_revert"
                                          : rs->second.change ? "pending_change" : "idless_pending") +
                         " held_id=" + rs->second.held_id +
                         ": the last consumed bar is pending; held at the stored T-1 quantity, no "
                         "order today");
                    const auto lc = last_consumed_date.find(symbol);
                    if (rs->second.flip && lc != last_consumed_date.end() &&
                        lc->second == t1_classification.t1_date) {
                        INFO("FLIP_PAIR " + symbol + " date=" + t1_classification.t1_date +
                             " held_id=" + rs->second.held_id +
                             " bars=" + std::to_string(rs->second.bars_pending + 1) +
                             ": the id returned to the held contract; no legs, every bar of it held "
                             "and its returns excluded");
                    }
                }
                portfolio->set_book_gate_holds(std::move(book_gate_holds));
            }
            // T-7b-3 R-3: on a sizing hold the PortfolioManager is not run, so every strategy
            // keeps the seeded T-1 book above (no rebalance, no order, no signal stored today).
            auto port_process_result =
                sizing_hold ? Result<void>() : portfolio->process_market_data(strategy_feed_bars);
            INFO("MarketDataBus publishing RE-ENABLED after process_market_data");
            MarketDataBus::instance().set_publish_enabled(true);
            // T-RISK-ARCH Q2 (ruled yes): a portfolio-scope risk REFUSE is found here, after
            // today's live_run_metadata row was written. That row records a run that did
            // happen, so it is marked, not deleted: the same row is written again with the
            // refusal and the PM's decision record in its portfolio_config JSON. Placed before
            // the error check, so a refusal that fails the call (an unseeded scope) is marked.
            if (auto risk_refusal = portfolio_risk_refusal(portfolio->last_risk_decisions())) {
                WARN("Portfolio risk REFUSE by module " +
                     risk_refusal->value("module", std::string()) + ": " +
                     risk_refusal->value("reason", std::string()) +
                     "; marking today's live_run_metadata row");
                // T-7b-1 C7b R4: the marked JSON is kept, so a later mark on the same row (the
                // STRICT assertion's) is added to this one instead of replacing it.
                auto mark_result = db->store_live_run_metadata(
                    now, combined_strategy_id, portfolio_id, strategy_alloc_json,
                    portfolio_config_json = mark_risk_refusal(portfolio_config_json, *risk_refusal,
                                                              portfolio->risk_decisions_json()),
                    strategy_configs);
                if (mark_result.is_error()) {
                    ERROR("Failed to mark today's live_run_metadata row with the risk refusal: " +
                          std::string(mark_result.error()->what()));
                } else {
                    INFO("Marked today's live_run_metadata row with the risk refusal");
                }
            }
            // T-7a C5 (HD 2026-09-21, option b): a portfolio risk module that could not evaluate
            // the book refused the scope, so the PM holds every strategy at its seeded T-1 book
            // and no order follows. The run goes on to store the day as it stores any REFUSE day
            // (the row above is marked) and exits non-zero at the end, with the email flagged.
            risk_module_failure = portfolio_risk_module_failure(portfolio->last_risk_decisions());
            if (risk_module_failure) {
                ERROR("RISK_MODULE_FAILURE portfolio risk module " +
                      risk_module_failure->value("module", std::string()) +
                      " could not evaluate the book: " +
                      risk_module_failure->value("error", std::string()) +
                      "; the book is held at the seeded T-1 positions and no orders are sent; "
                      "the day is stored as a REFUSE day and the run exits " +
                      std::to_string(kRiskModuleFailureExitCode));
            }
            // T-7b-2 C10b (HD 2026-09-24 ruling 18): a SLEEVE-scope risk module that could not
            // answer refused its sleeve, as the portfolio rule does for the book: the PM held that
            // sleeve at its seeded T-1 book (no order for it) and the other sleeves traded. The day
            // is flagged as a portfolio failure is (the same exit code and email flag, which the
            // cron wrapper and the operator already read) and, unless a portfolio refusal marked it
            // above, today's live_run_metadata row carries the sleeve's risk_refusal mark.
            if (!risk_module_failure) {
                risk_module_failure =
                    sleeve_risk_module_failure(portfolio->last_risk_decisions());
                if (risk_module_failure) {
                    ERROR("RISK_MODULE_FAILURE sleeve risk module " +
                          risk_module_failure->value("module", std::string()) +
                          " could not evaluate sleeve " +
                          risk_module_failure->value("scope_id", std::string()) + ": " +
                          risk_module_failure->value("error", std::string()) +
                          "; that sleeve is held at its seeded T-1 book and sends no orders, the "
                          "other sleeves trade; the run exits " +
                          std::to_string(kRiskModuleFailureExitCode));
                    if (!portfolio_risk_refusal(portfolio->last_risk_decisions())) {
                        auto sleeve_mark = db->store_live_run_metadata(
                            now, combined_strategy_id, portfolio_id, strategy_alloc_json,
                            portfolio_config_json =
                                mark_risk_refusal(portfolio_config_json, *risk_module_failure,
                                                  portfolio->risk_decisions_json()),
                            strategy_configs);
                        if (sleeve_mark.is_error()) {
                            ERROR("Failed to mark today's live_run_metadata row with the sleeve "
                                  "risk refusal: " +
                                  std::string(sleeve_mark.error()->what()));
                        } else {
                            INFO("Marked today's live_run_metadata row with the sleeve risk "
                                 "refusal");
                        }
                    }
                }
            }
            // T-7b-3 ruling 7 (HD 2026-09-27): the risk gate's cut, delivered once with the
            // BOOK_GATE holds fixed, left the book above the gate's level because the held
            // contracts alone keep it there. The day is stored as it is and today's
            // live_run_metadata row carries the over_limit_by_hold mark; the run goes on.
            if (const auto hold_limit = portfolio->last_over_limit_by_hold();
                hold_limit.over_limit_by_hold) {
                auto hold_mark = db->store_live_run_metadata(
                    now, combined_strategy_id, portfolio_id, strategy_alloc_json,
                    portfolio_config_json = mark_over_limit_by_hold(
                        portfolio_config_json, hold_limit.symbols, hold_limit.target,
                        hold_limit.cut_book, hold_limit.lap),
                    strategy_configs);
                if (hold_mark.is_error()) {
                    ERROR("Failed to mark today's live_run_metadata row over the limit by hold: " +
                          std::string(hold_mark.error()->what()));
                } else {
                    INFO("Marked today's live_run_metadata row over the limit by hold");
                }
            }
            if (port_process_result.is_error()) {
                std::cerr << "Failed to process data in portfolio manager: "
                          << port_process_result.error()->what() << std::endl;
                return 1;
            }
            INFO("Portfolio processing completed");

            // ========================================
            // PHASE 4: PER-STRATEGY SIGNALS STORAGE
            // Extract and store signals from each strategy after portfolio processing
            // ========================================
            INFO("PHASE 4: Storing per-strategy signals to database...");

            for (const auto& strategy : strategies) {
                const auto& metadata = strategy->get_metadata();
                std::string strategy_name = metadata.id;

                // Extract signals from a TrendFollowingStrategy sleeve (TREND or FAST)
                std::unordered_map<std::string, double> signals_map;
                bool signals_extracted = false;

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

            // Extract per-strategy positions map first; we build the combined
            // map below by summing across strategies (Σ qᵢ). Using
            // get_portfolio_positions() here would double-apply allocation
            // and feed fractional quantities to the daily report, email
            // tables, and portfolio composition chart.
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
        // T-7b-3 R-3: a sizing hold is flagged and exits as a held book does (the PortfolioManager
        // did not run, so no risk module failure competes with it).
        if (sizing_hold) {
            risk_module_failure = sizing_hold;
        }

        // Load previous day positions for PnL calculation
        INFO("Loading previous day positions for PnL calculation...");
        auto previous_date = now - std::chrono::hours(24);
        auto previous_positions_result =
            db->load_positions_by_date(combined_strategy_id, "", coordinator_config.portfolio_id,
                                       previous_date, "trading.positions");
        std::unordered_map<std::string, Position> previous_positions;

        if (previous_positions_result.is_ok()) {
            previous_positions = previous_positions_result.value();
            INFO("Loaded " + std::to_string(previous_positions.size()) + " previous day positions");
        } else {
            INFO("No previous day positions found (first run or no data): " +
                 std::string(previous_positions_result.error()->what()));
        }

        INFO("DEBUG: Previous date used for lookup: " +
             std::to_string(std::chrono::system_clock::to_time_t(previous_date)));
        INFO("DEBUG: Current date: " + std::to_string(std::chrono::system_clock::to_time_t(now)));
        INFO("DEBUG: Previous positions loaded: " + std::to_string(previous_positions.size()));
        for (const auto& [symbol, pos] : previous_positions) {
            INFO("DEBUG: Previous position - " + symbol + ": " +
                 std::to_string(pos.quantity.as_double()));
        }

        // Get market prices from PriceManager - already extracted from bars
        INFO("Getting market prices for PnL lag model from PriceManager...");

        // PriceManager has already extracted T-1 and T-2 prices from bars
        // Make copies since we need to use [] operator in many places
        std::unordered_map<std::string, double> previous_day_close_prices =
            price_manager->get_all_previous_day_prices();
        std::unordered_map<std::string, double> two_days_ago_close_prices =
            price_manager->get_all_two_days_ago_prices();
        // F-2 (LOOP_SPEC v6.2 sections 2.1, 6.1): a WITHHELD T-1 print is not a usable close. The
        // Day T row of such a symbol is valued at its last consumed close, never at the print.
        std::unordered_map<std::string, double> day_t_mark_prices = previous_day_close_prices;
        for (const auto& symbol : withheld_junk_bars) {
            const auto last_consumed = latest_bars_per_symbol.find(symbol);
            if (last_consumed != latest_bars_per_symbol.end()) {
                day_t_mark_prices[symbol] = static_cast<double>(last_consumed->second.close);
            } else {
                day_t_mark_prices.erase(symbol);
            }
        }

        INFO("Retrieved prices from PriceManager: " +
             std::to_string(previous_day_close_prices.size()) + " Day T-1, " +
             std::to_string(two_days_ago_close_prices.size()) + " Day T-2");

        // ========================================
        // PREVIOUS DAY PER-STRATEGY BOOKS (Option A)
        // Loaded once, here, and used by both the book gate below and the execution step
        // (T-4c F8: the gate holds against the exact map the executions are diffed against).
        // ========================================
        INFO("DEBUG PHASE 4: Loading previous day positions per-strategy...");
        std::unordered_map<std::string, std::unordered_map<std::string, Position>>
            previous_strategy_positions;

        for (const auto& [strategy_name, _] : strategy_positions_map) {
            // Load previous positions filtering by BOTH combined_strategy_id AND individual
            // strategy_name to ensure we only get positions from this specific run
            auto prev_result =
                db->load_positions_by_date(combined_strategy_id,  // Combined strategy_id
                                           strategy_name,         // Individual strategy_name
                                           coordinator_config.portfolio_id,  // Portfolio ID
                                           previous_date, "trading.positions");

            if (prev_result.is_ok()) {
                previous_strategy_positions[strategy_name] = prev_result.value();
                INFO("DEBUG PHASE 4: Loaded " + std::to_string(prev_result.value().size()) +
                     " previous positions for strategy: " + strategy_name);

                // Log individual previous positions for debugging
                for (const auto& [symbol, pos] : prev_result.value()) {
                    DEBUG("DEBUG PHASE 4: Previous " + strategy_name + " - " + symbol +
                          " qty=" + std::to_string(pos.quantity.as_double()));
                }
            } else {
                INFO("No previous positions found for strategy: " + strategy_name +
                     " (first run or no data): " + std::string(prev_result.error()->what()));
                previous_strategy_positions[strategy_name] = {};
            }
        }

        // ========================================
        // BOOK GATE (T-7a C4; HD 2026-09-17, STAGE3_PLAN §27; T-4c J1 as re-keyed)
        // A symbol whose T-1 verdict is not SESSION is held at its stored T-1 quantity on EVERY
        // per-strategy book: no signal-driven change, no order, marked at its last mark (NO_BAR)
        // or at its T-1 bar (JUNK). The key is the verdict, never membership of the price map:
        // a JUNK symbol has a T-1 price and would otherwise be traded at the junk print. A
        // symbol held yesterday and absent from today's target is re-inserted, so the close-out
        // loop cannot flatten it at a stale mark. Every other symbol trades normally. This
        // subsumes the Monday agricultural block, which is retired: it held only a grain or
        // livestock root with a stored row on a Monday, missed a position opened from flat
        // (ZC/ZM 2026-03-02) and every weekday grain hole.
        // ========================================
        std::vector<BookHold> book_holds;
        if (!skip_strategy_processing) {
            book_holds = hold_non_session_symbols(strategy_positions_map,
                                                  previous_strategy_positions, t1_classification,
                                                  now, change_bar_holds);
            log_book_holds(book_holds);
            if (!book_holds.empty()) {
                rebuild_combined_positions(positions, strategy_positions_map);
            }
        }

        // Verify we have prices for all required symbols
        std::set<std::string> all_symbols;
        for (const auto& [symbol, position] : positions) {
            if (position.quantity.as_double() != 0.0) {
                all_symbols.insert(symbol);
            }
        }
        for (const auto& [symbol, position] : previous_positions) {
            all_symbols.insert(symbol);
        }

        for (const auto& symbol : all_symbols) {
            if (previous_day_close_prices.find(symbol) == previous_day_close_prices.end()) {
                // A closure is expected; a feed hole was logged as an ERROR by the classifier.
                const SymbolDayVerdict* t1_verdict = t1_classification.find(symbol);
                if (t1_verdict && t1_verdict->verdict == SessionVerdict::NO_BAR_CLOSURE) {
                    INFO("Expected: Missing T-1 price for " + symbol + " (NO_BAR(closure): " +
                         t1_verdict->reason + ")");
                } else {
                    WARN("Missing T-1 price for symbol: " + symbol);
                }
            }
            if (two_days_ago_close_prices.find(symbol) == two_days_ago_close_prices.end() &&
                previous_positions.find(symbol) != previous_positions.end()) {
                WARN("Missing T-2 price for symbol: " + symbol + " (needed for PnL finalization)");
            }
        }

        // ========================================
        // PHASE 5: PER-STRATEGY DAY T-1 FINALIZATION
        // Finalize previous day positions FOR EACH STRATEGY
        // ========================================
        INFO("PHASE 5: Finalizing Day T-1 PnL per-strategy using PnLManager...");

        // Check if we have T-1 price data for finalization
        if (previous_day_close_prices.empty() && !previous_positions.empty()) {
            WARN(
                "No T-1 close prices available (likely weekend/holiday) - all positions will have "
                "0 PnL");
            INFO("This is expected behavior when Day T-1 (" +
                 std::to_string(std::chrono::system_clock::to_time_t(previous_date)) +
                 ") was a non-trading day");
        }

        INFO("PnLManager initialized with InstrumentRegistry access");

        double aggregate_yesterday_total_pnl = 0.0;

        // The T-1 settlement on the CONSUMED bars, built above the sizing read (D-B).
        const auto& t1_zero_pnl_symbols = t1_settlement.zero_pnl_symbols;
        const auto& t2_consumed_close_prices = t1_settlement.t2_close_prices;

        // T-ROLLX-FIX (sections 6.5, 6.6): a T-1 whose only held moves were settled at 0 (change or
        // withheld bars) has a zero aggregate but WAS finalized; the live_results update below must
        // not skip it. F-1: the test is on the HELD symbols, not on the universe.
        bool t1_zero_pnl_held = false;
        // T-ROLLX-FIX commit 5 (section 6.5): the finalized Day T-1 rows carry the contract held
        // AFTER the T-1 bar, and the stored row's contract is the state the roll legs are booked
        // from. They are computed here and written only after the day's executions are stored
        // (below), so a run stopped anywhere leaves either the T-1 rows as they were (the replay
        // legs the same rolls and books the same late moves) or the stored legs a re-run reads.
        std::vector<std::pair<std::string, std::vector<Position>>> t1_position_rewrites;
        if (!two_days_ago_close_prices.empty() && pnl_manager) {
            INFO("Finalizing Day T-1 positions per-strategy...");

            // Finalize for each strategy separately
            for (const auto& [strategy_name, current_positions_map] : strategy_positions_map) {
                // Load previous day positions for THIS strategy
                // Filter by BOTH combined_strategy_id AND individual strategy_name
                // to ensure we only get positions from this specific run
                auto prev_strategy_positions_result =
                    db->load_positions_by_date(combined_strategy_id,  // Combined strategy_id
                                               strategy_name,         // Individual strategy_name
                                               coordinator_config.portfolio_id,  // Portfolio ID
                                               previous_date, "trading.positions");

                if (prev_strategy_positions_result.is_error()) {
                    INFO("No previous positions found for strategy " + strategy_name +
                         " (first run or no data): " +
                         std::string(prev_strategy_positions_result.error()->what()));
                    continue;  // Skip this strategy
                }

                auto prev_strategy_positions_map = prev_strategy_positions_result.value();

                if (prev_strategy_positions_map.empty()) {
                    INFO("No previous positions to finalize for strategy: " + strategy_name);
                    continue;
                }

                INFO("DEBUG PHASE 5: Strategy '" + strategy_name + "' has " +
                     std::to_string(prev_strategy_positions_map.size()) +
                     " previous day positions to finalize");

                // Convert map to vector for PnLManager
                std::vector<Position> prev_positions_vec;
                prev_positions_vec.reserve(prev_strategy_positions_map.size());
                for (const auto& [symbol, pos] : prev_strategy_positions_map) {
                    prev_positions_vec.push_back(pos);
                }

                // Get this strategy's allocation for capital calculation
                double strategy_allocation = 1.0;  // Default to full allocation
                if (strategy_allocations.find(strategy_name) != strategy_allocations.end()) {
                    strategy_allocation = strategy_allocations[strategy_name];
                }
                double strategy_capital = initial_capital * strategy_allocation;

                // Use PnLManager to finalize previous day for this strategy
                auto finalization_result =
                    pnl_manager->finalize_previous_day(prev_positions_vec,
                                                       t1_settlement.t1_close_prices,  // T-1 prices
                                                       t2_consumed_close_prices,  // T-2 prices
                                                       strategy_capital,
                                                       0.0,  // Commissions (will be handled later)
                                                       LivePnLManager::UnrealizedPolicy::SETTLED,
                                                       t1_zero_pnl_symbols);

                if (finalization_result.is_ok()) {
                    auto& result = finalization_result.value();
                    double strategy_yesterday_pnl = result.finalized_daily_pnl;
                    aggregate_yesterday_total_pnl += strategy_yesterday_pnl;

                    INFO("DEBUG PHASE 5: Strategy '" + strategy_name +
                         "' finalized Day T-1 PnL: $" + std::to_string(strategy_yesterday_pnl));

                    // Log individual position PnLs for this strategy
                    for (const auto& [symbol, pnl] : result.position_realized_pnl) {
                        DEBUG("PHASE 5: " + strategy_name + " - Position " + symbol +
                              " finalized PnL: $" + std::to_string(pnl));
                    }

                    // T-ROLLX-FIX (LOOP_SPEC v6.1 section 7, code review D4): the T-1 row carries
                    // the contract held AFTER the T-1 bar, as the backtest's row of that date
                    // does: on the confirming bar's row, the incoming contract.
                    std::vector<Position> finalized_positions = result.finalized_positions;
                    for (auto& finalized_pos : finalized_positions) {
                        const auto rs = roll_status.find(finalized_pos.symbol);
                        if (rs != roll_status.end()) finalized_pos.instrument_id = rs->second.held_id;
                        if (t1_zero_pnl_symbols.count(finalized_pos.symbol) &&
                            finalized_pos.quantity.as_double() != 0.0) {
                            t1_zero_pnl_held = true;
                        }
                    }

                    // The Day T-1 rows of this strategy, written after the executions (commit 5).
                    if (!finalized_positions.empty()) {
                        t1_position_rewrites.emplace_back(strategy_name,
                                                          std::move(finalized_positions));
                    }
                } else {
                    ERROR("PnLManager failed to finalize Day T-1 for strategy " + strategy_name +
                          ": " + std::string(finalization_result.error()->what()));
                }
            }

            INFO("PHASE 5: Total finalized Day T-1 PnL across all strategies: $" +
                 std::to_string(aggregate_yesterday_total_pnl));
        } else {
            INFO("Skipping Day T-1 finalization (no two_days_ago prices or no PnLManager)");
        }

        // ========================================
        // STEP 2: CREATE TODAY'S (Day T) POSITIONS WITH ZERO PnL
        // ========================================
        INFO("STEP 2: Creating Day T positions with zero PnL (placeholders)...");

        double total_daily_transaction_costs = 0.0;  // Will be calculated from executions
        double total_daily_roll_costs = 0.0;  // migration 017: the ROLL subset of the above

        // Update all current positions to have:
        // - average_price = Day T-1 close (execution price)
        // - market_price = Day T-1 close (last known price)
        // - realized_pnl = 0 (placeholder, will be finalized tomorrow)
        // - unrealized_pnl = 0 (always 0 for futures)

        for (auto& [symbol, current_position] : positions) {
            // Get Day T-1 close price for this symbol
            double yesterday_close = current_position.average_price.as_double();  // Default
            if (day_t_mark_prices.find(symbol) != day_t_mark_prices.end()) {
                yesterday_close = day_t_mark_prices[symbol];
            }

            // Set position fields for Day T
            current_position.average_price = Decimal(yesterday_close);  // Entry at Day T-1 close
            current_position.realized_pnl =
                Decimal(0.0);  // PLACEHOLDER - will be finalized tomorrow
            current_position.unrealized_pnl = Decimal(0.0);  // Always 0 for futures
            current_position.last_update = now;              // Today's timestamp

            INFO("Day T position for " + symbol +
                 ": qty=" + std::to_string(current_position.quantity.as_double()) +
                 " entry_price=" + std::to_string(yesterday_close) +
                 " realized_pnl=0 (placeholder)");
        }

        // ========================================
        // PHASE 4: PER-STRATEGY EXECUTIONS GENERATION
        // Generate executions for each strategy based on their position changes
        // Load previous positions per-strategy (Option A)
        // ========================================
        INFO("PHASE 4: Generating per-strategy executions...");

        // The previous day per-strategy books were loaded above, before the book gate.

        // Generate executions for each strategy
        std::unordered_map<std::string, std::vector<ExecutionReport>> all_strategy_executions;
        int total_executions = 0;
        // total_daily_transaction_costs already declared earlier at line 881

        // PricingPolicy::STRICT (T-7a C4): a fill is priced from a real T-1 close or not at
        // all. With the book gate above every changed symbol has one, so nothing is unpriced
        // by construction; a symbol that still is gets its stored row back (the rollback of
        // LiveDailyCycle::execute_day_t), is logged as a tripwire, and the assertion after the
        // loop fails the run if any book change is left without a price.
        std::vector<std::string> strict_rolled_back;
        for (auto& [strategy_name, current_positions_map] : strategy_positions_map) {
            auto prev_positions_map = previous_strategy_positions[strategy_name];

            INFO("DEBUG PHASE 4: Generating executions for strategy '" + strategy_name +
                 "' (current=" + std::to_string(current_positions_map.size()) +
                 ", previous=" + std::to_string(prev_positions_map.size()) + ")");

            auto exec_result =
                execute_strategy_day_strict(*execution_manager, current_positions_map,
                                            prev_positions_map, previous_day_close_prices, now);

            if (exec_result.is_ok()) {
                for (const auto& s : exec_result.value().rolled_back) {
                    ERROR("STRICT_TRIPWIRE " + s + " (" + strategy_name +
                          ") had a book change and no T-1 price: rolled back to its stored "
                          "row, not traded. The book gate should have held it.");
                    strict_rolled_back.push_back(s);
                }
                std::vector<ExecutionReport> strategy_executions = exec_result.value().executions;

                // LOOP_SPEC v6.1 section 6.5: the two ROLL legs of every roll confirmed in this
                // run's span, per symbol this sleeve holds (the stored T-1 book: no fill happened
                // between the confirming bar and this run), ahead of the day's fills (closing leg,
                // opening leg, then STRATEGY), priced by the execution manager's cost model, STRICT
                // (a leg without a usable close refuses the run), ids EXEC_<symbol>_<confirming
                // bar YYYYMMDD>_RC / _RO (D3); realised 0; outside netting; the cost in both totals.
                {
                    std::vector<ExecutionReport> roll_legs;
                    const std::string run_date = trade_ngin::core::format_utc_date(now);
                    // The cost model's inputs on each ROLL_LEG line (ADV, volatility multiplier), so
                    // a reader can recompute the leg's implicit cost.
                    auto& roll_cost_model = execution_manager->get_transaction_cost_manager();
                    auto model_input = [](double x) {
                        std::ostringstream o;
                        o << std::setprecision(12) << x;
                        return o.str();
                    };
                    for (const auto& r : confirmed_rolls) {
                        const auto held = prev_positions_map.find(r.symbol);
                        if (held == prev_positions_map.end()) continue;
                        const double q = held->second.quantity.as_double();
                        if (std::abs(q) < 1e-9) continue;
                        const std::string tag = r.symbol + "_" + compact_date(r.confirm_date);
                        std::vector<ExecutionReport> legs;
                        try {
                            legs = roll_series::make_roll_legs(
                                r.symbol, q, r.closing_price, r.opening_price, r.outgoing_id,
                                r.incoming_id, now, "EXEC_" + tag + "_RC", "ROLL_" + tag + "_RC",
                                "EXEC_" + tag + "_RO", "ROLL_" + tag + "_RO",
                                [&](const std::string& s, double signed_q, double px) {
                                    const auto c = roll_cost_model.calculate_costs(s, signed_q, px);
                                    return roll_series::RollLegCost{c.commissions_fees,
                                                                    c.implicit_price_impact,
                                                                    c.slippage_market_impact,
                                                                    c.total_transaction_costs};
                                });
                        } catch (const std::exception& e) {
                            ERROR(std::string("ROLL_LEG STOP ") + r.symbol + " (" + strategy_name +
                                  "): " + e.what() + ". Refusing to run: a leg without a usable close");
                            std::cerr << "ROLL_LEG STOP " << r.symbol << ": " << e.what() << std::endl;
                            return 1;
                        }
                        for (const auto& leg : legs) {
                            const bool closing = leg.exec_id == "EXEC_" + tag + "_RC";
                            INFO("ROLL_LEG " + strategy_name + " " + r.symbol + " " +
                                 (closing ? "RC" : "RO") + " " +
                                 (leg.side == Side::BUY ? "BUY" : "SELL") + " qty=" +
                                 std::to_string(leg.filled_quantity.as_double()) + " px=" +
                                 std::to_string(leg.fill_price.as_double()) + " instrument=" +
                                 leg.instrument_id + " cost=" +
                                 std::to_string(leg.total_transaction_costs.as_double()) + " adv=" +
                                 model_input(roll_cost_model.get_adv(r.symbol)) + " vol_mult=" +
                                 model_input(roll_cost_model.get_volatility_multiplier(r.symbol)) +
                                 " date=" + run_date + " confirmed=" + r.confirm_date +
                                 " (an upper bound: two outright legs; realised 0; outside netting)");
                            total_daily_roll_costs += leg.total_transaction_costs.as_double();
                        }
                        roll_legs.insert(roll_legs.end(), legs.begin(), legs.end());
                    }
                    if (!roll_legs.empty()) {
                        strategy_executions.insert(strategy_executions.begin(), roll_legs.begin(),
                                                   roll_legs.end());
                    }
                }

                INFO("DEBUG PHASE 4: Strategy '" + strategy_name + "' generated " +
                     std::to_string(strategy_executions.size()) + " executions");

                // Log each execution for debugging
                for (const auto& exec : strategy_executions) {
                    INFO("DEBUG PHASE 4: " + strategy_name + " execution - " + exec.symbol + " " +
                         (exec.side == Side::BUY ? "BUY" : "SELL") + " " +
                         std::to_string(exec.filled_quantity.as_double()) + " @ " +
                         std::to_string(exec.fill_price) + " commission=$" +
                         std::to_string(exec.total_transaction_costs.as_double()));

                    total_daily_transaction_costs += exec.total_transaction_costs.as_double();
                }

                all_strategy_executions[strategy_name] = strategy_executions;
                total_executions += strategy_executions.size();
            } else {
                ERROR("Failed to generate executions for strategy " + strategy_name + ": " +
                      std::string(exec_result.error()->what()));
                // F-3 (section 6.5): this sleeve books no leg today, and the stored state moves on,
                // so a roll owed to it would never be legged. A STOP, never a loss.
                for (const auto& r : confirmed_rolls) {
                    const auto held = prev_positions_map.find(r.symbol);
                    if (held == prev_positions_map.end() ||
                        std::abs(held->second.quantity.as_double()) < 1e-9) {
                        continue;
                    }
                    ERROR("ROLL_LEG STOP " + r.symbol + " (" + strategy_name +
                          "): the sleeve's executions could not be generated, so its roll confirmed " +
                          r.confirm_date + " would not be legged. Refusing to run");
                    std::cerr << "ROLL_LEG STOP " << r.symbol << ": no executions for "
                              << strategy_name << std::endl;
                    return 1;
                }
                all_strategy_executions[strategy_name] = {};
            }
        }

        if (!strict_rolled_back.empty()) {
            // The combined rows of the rolled-back symbols, restated as STEP 2 writes them.
            for (const auto& s : strict_rolled_back) {
                Position combined;
                bool any = false;
                for (const auto& [_, pos_map] : strategy_positions_map) {
                    auto it = pos_map.find(s);
                    if (it == pos_map.end()) continue;
                    if (!any) {
                        combined = it->second;
                        any = true;
                    } else {
                        combined.quantity += it->second.quantity;
                    }
                }
                if (!any) {
                    positions.erase(s);
                    continue;
                }
                auto px = day_t_mark_prices.find(s);
                if (px != day_t_mark_prices.end()) {
                    combined.average_price = Decimal(px->second);
                }
                combined.realized_pnl = Decimal(0.0);
                combined.unrealized_pnl = Decimal(0.0);
                combined.last_update = now;
                positions[s] = combined;
            }
        }
        {
            const auto unpriced_changes = unpriced_book_changes(
                strategy_positions_map, previous_strategy_positions, previous_day_close_prices);
            if (!unpriced_changes.empty()) {
                std::string list;
                for (const auto& s : unpriced_changes) list += (list.empty() ? "" : ", ") + s;
                ERROR("STRICT_ASSERTION failed: book change(s) with no T-1 price and no execution "
                      "remain after the rollback: " + list + ". Refusing to store this book.");
                // T-7b-1 C7b R4 (T-7a_CODE_REVIEW R4): today's live_run_metadata row was written
                // above, before the book existed. The run happened and stored nothing, so the row
                // is marked, as a portfolio risk REFUSE marks it (T-RISK-ARCH Q2), and the
                // watchdog (scripts/check_live_trading.py) reports the mark. The mark is merged
                // into the row's JSON as written so far, so a same-day risk_refusal mark stays.
                auto strict_mark_result = db->store_live_run_metadata(
                    now, combined_strategy_id, portfolio_id, strategy_alloc_json,
                    mark_strict_assertion(portfolio_config_json, unpriced_changes),
                    strategy_configs);
                if (strict_mark_result.is_error()) {
                    ERROR("Failed to mark today's live_run_metadata row with the STRICT "
                          "assertion: " + std::string(strict_mark_result.error()->what()));
                } else {
                    INFO("Marked today's live_run_metadata row with the STRICT assertion");
                }
                return 1;
            }
        }

        // K3, the netting adjustment (T-7b-2 8b; HD 2026-09-25 item 23: two credited fills).
        // Every sleeve row keeps its own cost; for a symbol two or more sleeves trade today the
        // account sends ONE order, the signed sum Q, so each row's netting_adjustment is its
        // pro-rata share of sum C(q_i) - C(Q), priced by the same cost manager and state the
        // fills used (C(0) = 0: no order). Written into the rows before they are stored; the
        // day's P&L cost above stays the sum of the rows' own costs (the book's P&L is gross).
        {
            std::vector<transaction_cost::SleeveExecution> sleeve_rows;
            for (auto& [netting_sleeve, netting_execs] : all_strategy_executions) {
                for (auto& e : netting_execs) {
                    // Section 6.5: ROLL legs never enter the netting (two legs at two prices would
                    // read as a mixed-price cross); their adjustment stays 0.
                    if (e.execution_type != ExecutionType::STRATEGY) continue;
                    sleeve_rows.push_back({netting_sleeve, &e});
                }
            }
            const auto netting = transaction_cost::apply_netting_adjustments(
                sleeve_rows, [&](const std::string& s, double q, double px) {
                    return execution_manager->get_transaction_cost_manager().calculate_costs(
                        s, q, px).total_transaction_costs;
                });
            for (const auto& line : netting.info_lines) INFO(line);
            for (const auto& line : netting.warn_lines) WARN(line);
        }

        INFO("PHASE 4: Total executions across all strategies: " +
             std::to_string(total_executions));
        INFO("PHASE 4: Total daily transaction costs: $" +
             std::to_string(total_daily_transaction_costs));

        // Section 6.5: the re-run sweep by type. Every sleeve's ROLL rows dated today are deleted
        // before this run's legs are stored, so a re-run that no longer rolls leaves none.
        // T-ROLLX-FIX commit 5: a sleeve that stores ROLL legs on this run is swept inside the
        // transaction that stores them (below), so its legs are never deleted and not re-stored.
        auto roll_legs_of = [&](const std::string& sleeve) {
            const auto it = all_strategy_executions.find(sleeve);
            if (it == all_strategy_executions.end()) return std::ptrdiff_t{0};
            return std::count_if(it->second.begin(), it->second.end(), [](const ExecutionReport& e) {
                return e.execution_type == ExecutionType::ROLL;
            });
        };
        // T-ROLLX-FIX commits 5 and 6 (section 6.5): the sleeves with ROLL legs are stored FIRST, in
        // name order, ALL in ONE transaction (per sleeve the ROLL sweep, the stale-order-id delete,
        // the insert), and a failure is a STOP before any execution, position or result of ANY
        // sleeve is written: the legs' costs are in today's totals and the Day T-1 rows below would
        // move the state the legs are owed from.
        {
            std::vector<std::pair<std::string, std::vector<ExecutionReport>>> leg_sleeves;
            for (const auto& [strategy_name, executions] : all_strategy_executions) {
                if (roll_legs_of(strategy_name) > 0) leg_sleeves.emplace_back(strategy_name, executions);
            }
            std::sort(leg_sleeves.begin(), leg_sleeves.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
            if (!leg_sleeves.empty()) {
                std::ptrdiff_t roll_legs_owed = 0;
                for (const auto& [strategy_name, executions] : leg_sleeves) {
                    const auto roll_legs = roll_legs_of(strategy_name);
                    roll_legs_owed += roll_legs;
                    INFO("ROLL_LEG store " + strategy_name + ": " + std::to_string(executions.size()) +
                         " executions (" + std::to_string(roll_legs) +
                         " ROLL legs) replace the sleeve's ROLL rows of the day and its rows of the "
                         "same order ids in one transaction, before the Day T-1 rows are re-written");
                }
                auto replaced = db->replace_roll_day_executions(
                    leg_sleeves, combined_strategy_id, portfolio_id, now, "trading.executions");
                if (replaced.is_error()) {
                    ERROR("ROLL_LEG STOP: the day's executions could not be stored (" +
                          std::string(replaced.error()->what()) + ") and " +
                          std::to_string(roll_legs_owed) +
                          " ROLL legs are owed. Refusing to run: no execution, no position and no "
                          "result is written; the stored state is the state before this run, so "
                          "running this date again legs the same rolls");
                    std::cerr << "ROLL_LEG STOP: the day's executions could not be stored"
                              << std::endl;
                    return 1;
                }
                for (const auto& [strategy_name, executions] : leg_sleeves) {
                    INFO("Successfully stored " + std::to_string(executions.size()) +
                         " executions for strategy: " + strategy_name);
                }
            }
        }

        for (const auto& strategy_name_rl : strategy_names) {
            if (roll_legs_of(strategy_name_rl) > 0) continue;
            auto sweep = db->delete_roll_executions(now, strategy_name_rl, portfolio_id,
                                                    "trading.executions");
            if (sweep.is_error()) {
                ERROR("ROLL_LEG STOP: today's ROLL executions of " + strategy_name_rl +
                      " could not be swept (" + std::string(sweep.error()->what()) +
                      "); refusing to run: a re-run would store them twice");
                return 1;
            }
        }

        // Store executions for each strategy without ROLL legs
        for (const auto& [strategy_name, executions] : all_strategy_executions) {
            if (roll_legs_of(strategy_name) > 0) continue;  // stored above
            if (!executions.empty()) {
                // Before inserting, delete any stale executions for today with the same order_ids
                try {
                    // Build unique order_id list
                    std::set<std::string> unique_order_ids;
                    for (const auto& exec : executions) {
                        unique_order_ids.insert(exec.order_id);
                    }

                    if (!unique_order_ids.empty()) {
                        // Convert set to vector for the delete method
                        std::vector<std::string> order_ids_vector(unique_order_ids.begin(),
                                                                  unique_order_ids.end());

                        INFO("Deleting stale executions for strategy " + strategy_name + " with " +
                             std::to_string(order_ids_vector.size()) + " order_ids");

                        // Use the delete_stale_executions method with strategy name
                        auto del_res = db->delete_stale_executions(
                            order_ids_vector, now, strategy_name, portfolio_id,
                            "trading.executions");
                        if (del_res.is_error()) {
                            WARN("Failed to delete stale executions for strategy " + strategy_name +
                                 ": " + std::string(del_res.error()->what()));
                        } else {
                            INFO("Stale executions (if any) deleted successfully for strategy: " +
                                 strategy_name);
                        }
                    }
                } catch (const std::exception& e) {
                    WARN("Exception while deleting stale executions for strategy " + strategy_name +
                         ": " + std::string(e.what()));
                }

                // Store executions with combined strategy_id and individual strategy_name
                auto save_result =
                    db->store_executions(executions,
                                         combined_strategy_id,  // Combined strategy_id for tier 2
                                         strategy_name,  // Individual strategy_name for tier 3
                                         portfolio_id,   // Portfolio identifier
                                         "trading.executions");

                if (save_result.is_error()) {
                    ERROR("Failed to store executions for strategy " + strategy_name + ": " +
                          std::string(save_result.error()->what()));
                } else {
                    INFO("Successfully stored " + std::to_string(executions.size()) +
                         " executions for strategy: " + strategy_name);
                }
            } else {
                INFO("No executions to store for strategy: " + strategy_name);
            }
        }

        // T-ROLLX-FIX commit 5: the Day T-1 rows PHASE 5 finalized, written now that the day's
        // executions (the ROLL legs first of all) are stored.
        for (const auto& [strategy_name, finalized_positions] : t1_position_rewrites) {
            auto update_result =
                db->store_positions(finalized_positions,
                                    combined_strategy_id,  // Combined strategy_id
                                    strategy_name,         // Individual strategy_name
                                    portfolio_id,          // Portfolio identifier
                                    "trading.positions");

            if (update_result.is_error()) {
                ERROR("Failed to update Day T-1 positions for strategy " + strategy_name + ": " +
                      std::string(update_result.error()->what()));
            } else {
                INFO("Successfully updated " + std::to_string(finalized_positions.size()) +
                     " Day T-1 positions with finalized PnL for strategy: " + strategy_name);
            }
        }

        std::cout << "\n======= Daily Position Report =======" << std::endl;
        std::cout << "Date: " << (now_tm->tm_year + 1900) << "-" << std::setfill('0')
                  << std::setw(2) << (now_tm->tm_mon + 1) << "-" << std::setfill('0')
                  << std::setw(2) << now_tm->tm_mday << std::endl;
        std::cout << "Total Positions: " << positions.size() << std::endl;
        std::cout << std::endl;

        // Add header for position table
        std::cout << std::setw(10) << "Symbol"
                  << " | " << std::setw(10) << "Quantity"
                  << " | " << std::setw(10) << "Mkt Price"
                  << " | " << std::setw(12) << "Notional"
                  << " | " << std::setw(10) << "Unreal PnL" << std::endl;
        std::cout << std::string(60, '-') << std::endl;

        // Use MarginManager for margin calculations
        INFO("Using MarginManager to calculate margin requirements...");

        auto margin_result = margin_manager->calculate_margin_requirements(
            positions, previous_day_close_prices, initial_capital);

        double gross_notional = 0.0;
        double net_notional = 0.0;
        double total_posted_margin = 0.0;  // Sum of per-contract initial margins times contracts
        double maintenance_requirement_today =
            0.0;  // Sum of per-contract maintenance margins times contracts
        int active_positions = 0;

        if (margin_result.is_ok()) {
            auto& metrics = margin_result.value();

            // Recompute notional AND margin from per-strategy positions.
            // The combined positions map fed to calculate_margin_requirements above is
            // get_portfolio_positions(), which scales each strategy's quantity by its
            // allocation (Σ qᵢ × allocᵢ). Strategies already size for their capital
            // slice, so that scaling is double-applied — the combined map under-states
            // both gross_notional and total_posted_margin by ~Σ allocᵢ². Iterating per
            // strategy and accumulating against |q| restores the additive invariant:
            // total_posted_margin = Σ_strategies Σ_symbols |q| × initial_margin.
            // T-7b-2 8b: the account's exposure from the sleeves' books (live/book_exposure.hpp).
            // A symbol the sleeves hold on the same side is summed per sleeve exactly as before;
            // a symbol they hold on OPPOSITE sides is taken once, on the net, because the account
            // holds the net and posts margin on it (HD 2026-09-19 / 2026-09-25 item 23).
            const BookExposure exposure = account_book_exposure(
                strategy_positions_map,
                [&](const std::string& symbol, const Position& pos) {
                    return previous_day_close_prices.count(symbol)
                               ? previous_day_close_prices.at(symbol)
                               : pos.average_price.as_double();
                },
                [&](const std::string& symbol, double qty, double price) {
                    return margin_manager->calculate_position_notional(symbol, qty, price);
                },
                [&](const std::string& symbol, double qty, double price) {
                    return margin_manager->calculate_position_margin(symbol, qty, price);
                });
            if (exposure.failed) {
                WARN("Failed per-strategy notional/margin for " + exposure.failed_symbol +
                     " in strategy " + exposure.failed_strategy +
                     ", falling back to MarginManager combined values");
                gross_notional = metrics.gross_notional;
                net_notional = metrics.net_notional;
                total_posted_margin = metrics.total_posted_margin;
                maintenance_requirement_today = metrics.maintenance_requirement;
            } else {
                gross_notional = exposure.gross_notional;
                net_notional = exposure.net_notional;
                total_posted_margin = exposure.posted_margin;
                maintenance_requirement_today = exposure.maintenance_margin;
                for (const auto& line : exposure.net_lines) INFO(line);
            }
            active_positions = exposure.active_positions;

            INFO("Per-strategy recompute: gross=$" + std::to_string(gross_notional) +
                 ", net=$" + std::to_string(net_notional) +
                 ", posted_margin=$" + std::to_string(total_posted_margin) +
                 " (combined was gross=$" + std::to_string(metrics.gross_notional) +
                 ", posted_margin=$" + std::to_string(metrics.total_posted_margin) + ")");
            INFO("MarginManager calculated: gross_notional=$" + std::to_string(gross_notional) +
                 ", posted_margin=$" + std::to_string(total_posted_margin) +
                 ", active_positions=" + std::to_string(active_positions));
        } else {
            ERROR("MarginManager failed: " + std::string(margin_result.error()->what()));
            // No fallback - component is required to work
            throw std::runtime_error("MarginManager failed");
        }

        std::cout << std::endl;
        std::cout << "Active Positions: " << active_positions << std::endl;
        std::cout << "Gross Notional: $" << std::fixed << std::setprecision(2) << gross_notional
                  << std::endl;
        std::cout << "Net Notional: $" << std::fixed << std::setprecision(2) << net_notional
                  << std::endl;
        std::cout << "Gross Leverage: " << std::fixed << std::setprecision(2)
                  << (gross_notional / initial_capital) << "x" << std::endl;
        // Posted margin should never be zero if there are active positions; enforce and warn
        if (active_positions > 0 && total_posted_margin <= 0.0) {
            ERROR(
                "Computed posted margin is non-positive while positions are active. Check "
                "instrument metadata.");
        }
        // Equity-to-Margin Ratio = portfolio_equity / total_posted_margin.
        // Higher = safer (more equity per dollar of margin posted).
        //
        // MAIN-post-#55, margin numerator. This was computed HERE, from
        // initial_capital, because current_portfolio_value is not known yet at this
        // point in the run -- and the comment said as much and called it a proxy. It is
        // not a proxy for a reported column: it is a constant. As the book gains or
        // loses money, the equity in "equity to margin" never moves, so the ratio that
        // reaches trading.live_results and the daily email answers a different question
        // from the one its name asks, and disagrees with the two other producers of the
        // same quantity -- live_metrics_calculator.cpp:307, which the main audit named
        // the reconciliation target, and MarginManager. L1-Q3 of MAIN_AUDIT_2026-08-27.
        //
        // The numerator is now current_portfolio_value, and the computation therefore
        // moves down to where that value exists, immediately below its own INFO block.
        // Only the declaration stays here, so the value is still in scope for the
        // storage and email sites further down that already read it.
        //
        // The <= 1.0 alarm moves with it: an alarm on a number that has not been
        // computed yet would fire on the constant, not on the account.
        double equity_to_margin_ratio = 0.0;

        // ========================================
        // PHASE 4: PER-STRATEGY POSITIONS STORAGE
        // Extract per-strategy positions from PortfolioManager
        // Each strategy's positions are stored separately with strategy_name tag
        // strategy_positions_map already extracted above for use by executions section
        // ========================================
        INFO("PHASE 4: Storing per-strategy positions to database...");

        // Store positions for each strategy with strategy_name tag
        int total_positions_saved = 0;
        for (const auto& [strategy_name, positions_map] : strategy_positions_map) {
            std::vector<Position> strategy_positions_vec;
            strategy_positions_vec.reserve(positions_map.size());

            INFO("DEBUG PHASE 4: Strategy '" + strategy_name + "' has " +
                 std::to_string(positions_map.size()) + " positions");

            for (const auto& [symbol, pos] : positions_map) {
                // Only save positions with non-zero quantity
                // Zero-quantity positions (closed positions) should NOT be stored
                bool has_quantity = std::abs(pos.quantity.as_double()) > 1e-10;

                if (!has_quantity) {
                    DEBUG("Skipping zero-quantity position: " + symbol);
                    continue;
                }

                // Create a new position with validated values
                Position validated_position;
                validated_position.symbol = pos.symbol;
                validated_position.quantity = pos.quantity;
                validated_position.last_update = now;  // Use current timestamp

                // CRITICAL: For PnL lag model, Day T positions must have ZERO PnL (placeholders)
                // The PnL will be finalized tomorrow when we run for Day T+1
                // Do NOT use pos.realized_pnl which contains calculated PnL from strategy
                // processing
                validated_position.realized_pnl =
                    Decimal(0.0);  // PLACEHOLDER - will be finalized tomorrow
                validated_position.unrealized_pnl = Decimal(0.0);  // Always 0 for futures
                // T-ROLLX-FIX (section 7, migration 016): the contract held after the symbol's last
                // consumed bar; tomorrow's T-1 finalize re-writes it after this day's bar.
                if (const auto rs = roll_status.find(symbol); rs != roll_status.end()) {
                    validated_position.instrument_id = rs->second.held_id;
                }

                // For Day T positions, average_price should be Day T-1 close (entry price)
                // This is the price at which positions were "executed" (opened at yesterday's
                // close)
                double avg_price_double =
                    previous_day_close_prices.find(symbol) != previous_day_close_prices.end()
                        ? previous_day_close_prices[symbol]
                        : static_cast<double>(pos.average_price);

                // Decimal limit is approximately 92,233,720,368,547.75807
                const double DECIMAL_MAX = 9.223372036854775807e13;  // INT64_MAX / SCALE
                if (avg_price_double > DECIMAL_MAX || avg_price_double < -DECIMAL_MAX) {
                    WARN("Position " + symbol + " has average_price " +
                         std::to_string(avg_price_double) +
                         " which exceeds Decimal limit, using Day T-1 close instead");
                    // Use Day T-1 close if available
                    if (previous_day_close_prices.find(symbol) != previous_day_close_prices.end()) {
                        validated_position.average_price =
                            Decimal(previous_day_close_prices[symbol]);
                    } else {
                        validated_position.average_price = Decimal(1.0);
                    }
                } else {
                    try {
                        validated_position.average_price = pos.average_price;
                    } catch (const std::exception& e) {
                        ERROR("Failed to validate average_price for " + symbol + ": " +
                              std::string(e.what()));
                        if (previous_day_close_prices.find(symbol) !=
                            previous_day_close_prices.end()) {
                            validated_position.average_price =
                                Decimal(previous_day_close_prices[symbol]);
                        } else {
                            validated_position.average_price = Decimal(1.0);
                        }
                    }
                }

                strategy_positions_vec.push_back(validated_position);

                INFO("DEBUG PHASE 4: " + strategy_name + " - " + symbol + " qty=" +
                     std::to_string(validated_position.quantity.as_double()) + " avg_price=" +
                     std::to_string(static_cast<double>(validated_position.average_price)) +
                     " realized_pnl=" +
                     std::to_string(static_cast<double>(validated_position.realized_pnl)));
            }

            if (!strategy_positions_vec.empty()) {
                INFO("Attempting to save " + std::to_string(strategy_positions_vec.size()) +
                     " positions for strategy: " + strategy_name);
                DEBUG("Database connection status: " +
                      std::string(db->is_connected() ? "connected" : "disconnected"));

                // Store with combined strategy_id and individual strategy_name
                auto save_result =
                    db->store_positions(strategy_positions_vec,
                                        combined_strategy_id,  // Combined strategy_id for tier 2
                                        strategy_name,  // Individual strategy_name for tier 3
                                        portfolio_id,   // Portfolio identifier
                                        "trading.positions");

                if (save_result.is_error()) {
                    ERROR("Failed to store positions for strategy " + strategy_name + ": " +
                          std::string(save_result.error()->what()));
                } else {
                    INFO("Successfully stored " + std::to_string(strategy_positions_vec.size()) +
                         " positions for strategy: " + strategy_name);
                    total_positions_saved += strategy_positions_vec.size();
                }
            } else {
                INFO("No non-zero positions to store for strategy: " + strategy_name);
            }
        }

        INFO("PHASE 4: Total positions saved across all strategies: " +
             std::to_string(total_positions_saved));

        // Compute portfolio-level snapshot metrics using RiskManager on today's state
        INFO("Retrieving strategy metrics...");
        // T-7b-2 9c: the reporter reads the book against the capital it was sized on, as the gate
        // does (its leverage term divides by it).
        RiskConfig snapshot_risk_config = risk_config;
        snapshot_risk_config.capital = Decimal(portfolio->sizing_capital());
        trade_ngin::RiskManager snapshot_rm(snapshot_risk_config);
        // K-01: the snapshot reads the consumed bars, as every return consumer does.
        auto market_data_snapshot = snapshot_rm.create_market_data(strategy_feed_bars);
        auto risk_eval = snapshot_rm.process_positions(positions, market_data_snapshot);

        std::cout << "\n======= Strategy Metrics =======" << std::endl;
        if (risk_eval.is_ok()) {
            const auto& r = risk_eval.value();
            // Use portfolio_var as annualized volatility proxy
            std::cout << "Volatility: " << std::fixed << std::setprecision(2)
                      << (r.portfolio_var * 100.0) << "%" << std::endl;
            std::cout << "Gross Leverage (Risk): " << std::fixed << std::setprecision(2)
                      << r.gross_leverage << std::endl;
            std::cout << "Net Leverage: " << std::fixed << std::setprecision(2) << r.net_leverage
                      << std::endl;
            std::cout << "Max Correlation: " << std::fixed << std::setprecision(2)
                      << r.correlation_risk << std::endl;
            std::cout << "Jump Risk (99th): " << std::fixed << std::setprecision(2) << r.jump_risk
                      << std::endl;
            std::cout << "Risk Scale: " << std::fixed << std::setprecision(2) << r.recommended_scale
                      << std::endl;
        } else {
            std::cout << "Volatility: N/A" << std::endl;
            std::cout << "Gross Leverage (Risk): N/A" << std::endl;
            std::cout << "Net Leverage: N/A" << std::endl;
            std::cout << "Max Correlation: N/A" << std::endl;
            std::cout << "Jump Risk (99th): N/A" << std::endl;
            std::cout << "Risk Scale: N/A" << std::endl;
        }
        // RA-01 (T-7b-1 C7): beside the stored value, the scale that actually moved the book.
        // reporter = the double stored as live_results.risk_scale below (the snapshot's
        // recommended_scale, 1.0 when its evaluation failed); the other four fields are the
        // PortfolioManager's own record of this run's rebalance, last_risk_decisions(), each
        // defined in risk_scale_report.hpp. Log only: no stored value reads it.
        INFO(trade_ngin::format_risk_scale_report(
            risk_eval.is_ok() ? risk_eval.value().recommended_scale : 1.0,
            trade_ngin::summarize_applied_risk(portfolio->last_risk_decisions())));
        // T-7b-2 C9a (T-VOL C4): the delivered cut beside the request: the stored book's gross
        // notional over the lap-1 optimizer book's, the PortfolioManager's measurement of the same
        // rebalance (risk_scale_report.hpp defines each field). Log only. C9a3: final_gross is the
        // book this runner stores, strategy_positions_map AFTER the BOOK_GATE hold (a held symbol
        // keeps its stored T-1 quantity), not the PortfolioManager's book before it.
        INFO(trade_ngin::format_risk_delivered(
            trade_ngin::summarize_applied_risk(portfolio->last_risk_decisions()),
            portfolio->delivered_cut_for_book(trade_ngin::account_book_of(strategy_positions_map))));
        // ========================================
        // STEP 3: CALCULATE TRANSACTION COSTS AND Day T PnL (ZERO)
        // ========================================
        INFO("STEP 3: Calculating transaction costs and Day T PnL...");

        // total_daily_transaction_costs already calculated in per-strategy executions loop above
        INFO("Total daily transaction costs (from per-strategy executions): $" +
             std::to_string(total_daily_transaction_costs));

        // Day T PnL is ZERO (placeholder) - positions were just opened at Day T-1 close
        // Update PnLManager with today's positions (all with 0 PnL as placeholders)
        for (const auto& [symbol, position] : positions) {
            pnl_manager->update_position_pnl(symbol, 0.0, 0.0);  // Zero PnL for Day T
        }

        double daily_realized_pnl = 0.0;
        double daily_unrealized_pnl = 0.0;
        double daily_pnl_for_today =
            -total_daily_transaction_costs;  // Only transaction costs on Day T

        INFO("Day T PnL (placeholder): $0.00");
        INFO("Day T transaction costs: $" + std::to_string(total_daily_transaction_costs));
        INFO("Day T total impact: $" + std::to_string(daily_pnl_for_today));

        // ========================================
        // STEP 4: UPDATE Day T-1 live_results AND equity_curve WITH FINALIZED PnL
        // ========================================
        // Skip if this is the first trading day (no previous positions to finalize)
        bool is_first_trading_day =
            previous_positions.empty() ||
            (previous_positions.size() > 0 &&
             std::all_of(previous_positions.begin(), previous_positions.end(),
                         [](const auto& p) { return p.second.quantity.as_double() == 0.0; }));

        // Declare yesterday's daily metrics outside the block so they're available for email
        double yesterday_daily_return_for_email = 0.0;
        double yesterday_daily_pnl_for_email = 0.0;
        double yesterday_realized_pnl_for_email = 0.0;
        double yesterday_unrealized_pnl_for_email = 0.0;

        if (!two_days_ago_close_prices.empty() &&
            (aggregate_yesterday_total_pnl != 0.0 || t1_zero_pnl_held) &&
            !is_first_trading_day) {
            INFO("STEP 4: Updating Day T-1 live_results with finalized PnL: $" +
                 std::to_string(aggregate_yesterday_total_pnl));

            // Get yesterday's transaction costs and other existing metrics from database
            double yesterday_transaction_costs = 0.0;
            double yesterday_gross_notional = 0.0;
            double yesterday_margin_posted = 0.0;

            std::stringstream yesterday_date_ss;
            auto yesterday_time_t = std::chrono::system_clock::to_time_t(previous_date);
            yesterday_date_ss << std::put_time(std::gmtime(&yesterday_time_t), "%Y-%m-%d");

            // Use LiveDataLoader to get yesterday's metrics
            try {
                INFO("Using LiveDataLoader to query yesterday's metrics for date: " +
                     yesterday_date_ss.str());
                auto live_results = data_loader->load_live_results(
                    combined_strategy_id, coordinator_config.portfolio_id, previous_date);

                if (live_results.is_ok()) {
                    auto& row = live_results.value();
                    yesterday_transaction_costs = row.daily_transaction_costs;
                    yesterday_gross_notional = row.gross_notional;
                    yesterday_margin_posted = row.margin_posted;

                    INFO("Successfully loaded yesterday's metrics via LiveDataLoader:");
                    INFO("  yesterday_transaction_costs: $" +
                         std::to_string(yesterday_transaction_costs));
                    INFO("  yesterday_gross_notional: $" +
                         std::to_string(yesterday_gross_notional));
                    INFO("  yesterday_margin_posted: $" + std::to_string(yesterday_margin_posted));
                } else {
                    WARN("LiveDataLoader failed to get yesterday's metrics: " +
                         std::string(live_results.error()->what()));
                    INFO("Using default values (0) for yesterday's metrics");
                }
            } catch (const std::exception& e) {
                WARN("Failed to get yesterday's metrics: " + std::string(e.what()));
            }

            // Use the commission value already loaded from LiveDataLoader
            double yesterday_transaction_costs_for_calc = yesterday_transaction_costs;
            INFO("Using yesterday_transaction_costs_for_calc from LiveDataLoader: $" +
                 std::to_string(yesterday_transaction_costs_for_calc));

            // Use the queried value from earlier (which may be 0 if query failed)
            double yesterday_daily_pnl_finalized =
                aggregate_yesterday_total_pnl - yesterday_transaction_costs;

            INFO("Day T-1 PnL breakdown:");
            INFO("  Position PnL (aggregate_yesterday_total_pnl): $" +
                 std::to_string(aggregate_yesterday_total_pnl));
            INFO("  Transaction costs (yesterday_transaction_costs): $" +
                 std::to_string(yesterday_transaction_costs));
            INFO("  Net PnL (yesterday_daily_pnl_finalized): $" +
                 std::to_string(yesterday_daily_pnl_finalized));

            // Get the day BEFORE yesterday's portfolio value, total_pnl, and
            // total_transaction_costs
            double day_before_yesterday_portfolio_value = initial_capital;
            double day_before_aggregate_yesterday_total_pnl = 0.0;
            double day_before_yesterday_total_transaction_costs = 0.0;
            try {
                auto db_ptr = std::dynamic_pointer_cast<PostgresDatabase>(db);
                if (db_ptr) {
                    auto prev_agg = db_ptr->get_previous_live_aggregates(
                        combined_strategy_id, coordinator_config.portfolio_id, previous_date,
                        "trading.live_results");
                    if (prev_agg.is_ok()) {
                        std::tie(day_before_yesterday_portfolio_value,
                                 day_before_aggregate_yesterday_total_pnl,
                                 day_before_yesterday_total_transaction_costs) = prev_agg.value();
                        INFO("Loaded day-before-yesterday aggregates: portfolio=$" +
                             std::to_string(day_before_yesterday_portfolio_value) +
                             ", total_pnl=$" +
                             std::to_string(day_before_aggregate_yesterday_total_pnl) +
                             ", total_transaction_costs=$" +
                             std::to_string(day_before_yesterday_total_transaction_costs));
                    }
                }
            } catch (const std::exception& e) {
                INFO("Could not load day-before-yesterday aggregates: " + std::string(e.what()));
            }

            // Calculate yesterday's cumulative values
            // NOTE: Since we may not have correct transaction costs, the cumulative values will be
            // recalculated by SQL using the daily_pnl formula (daily_realized_pnl -
            // daily_transaction_costs)
            double aggregate_yesterday_total_pnl_cumulative =
                day_before_aggregate_yesterday_total_pnl + yesterday_daily_pnl_finalized;
            double yesterday_total_transaction_costs_cumulative =
                day_before_yesterday_total_transaction_costs + yesterday_transaction_costs;
            double yesterday_total_realized_pnl_cumulative =
                aggregate_yesterday_total_pnl_cumulative +
                yesterday_total_transaction_costs_cumulative;
            double yesterday_portfolio_value_finalized =
                day_before_yesterday_portfolio_value + yesterday_daily_pnl_finalized;

            // Note: Yesterday's metrics for email will be loaded from database after update

            // Calculate yesterday's total cumulative return (non-annualized)
            double yesterday_total_cumulative_return = metrics_calculator->calculate_total_return(
                yesterday_portfolio_value_finalized, initial_capital);

            double yesterday_total_return_decimal = 0.0;
            if (initial_capital > 0.0) {
                yesterday_total_return_decimal =
                    (yesterday_portfolio_value_finalized - initial_capital) / initial_capital;
            }
            double yesterday_total_cumulative_return_pct =
                yesterday_total_cumulative_return;  // Already in %

            // Get trading days count for annualization using PostgreSQL function
            // This avoids issues with row multiplication/duplication in the database
            // Uses trading.strategy_trading_days_metadata table for live_start_date
            int trading_days_count = 1;
            try {
                // Call PostgreSQL function to calculate trading days
                // E2-F6: portfolio-scoped (3-arg) form. The 2-arg overload keys on
                // strategy_id alone with `ORDER BY live_start_date LIMIT 1` and NO portfolio
                // predicate, so it takes the earliest row across ALL portfolios.
                // LIVE_TREND_FOLLOWING has a metadata row under both BASE_PORTFOLIO and
                // CONSERVATIVE_PORTFOLIO; they agree only because both carry
                // live_start_date = 2025-10-05. Add or edit a BASE row with an earlier date
                // and the conservative book's annualization changes silently -- no error, no
                // log line. Definition is versioned in migrations/004.
                std::string trading_days_query = "SELECT trading.get_trading_days('" +
                                                 combined_strategy_id + "', DATE '" +
                                                 yesterday_date_ss.str() + "', '" + portfolio_id + "')";

                INFO("TRADING_DAYS_CALC [Day T-1]: Querying trading days...");
                INFO("TRADING_DAYS_CALC [Day T-1]: Query: " + trading_days_query);
                INFO("TRADING_DAYS_CALC [Day T-1]: Strategy ID: " + combined_strategy_id);
                INFO("TRADING_DAYS_CALC [Day T-1]: Target Date: " + yesterday_date_ss.str());

                auto trading_days_result = db->execute_query(trading_days_query);

                if (trading_days_result.is_ok()) {
                    auto table = trading_days_result.value();
                    if (table && table->num_rows() > 0 && table->num_columns() > 0) {
                        // execute_query returns StringArray for all columns
                        auto arr = std::static_pointer_cast<arrow::StringArray>(
                            table->column(0)->chunk(0));
                        if (arr && arr->length() > 0 && !arr->IsNull(0)) {
                            trading_days_count = std::max<int>(1, std::stoi(arr->GetString(0)));
                            INFO("TRADING_DAYS_CALC [Day T-1]: Result from DB: " +
                                 std::to_string(trading_days_count) + " trading days");
                            INFO(
                                "TRADING_DAYS_CALC [Day T-1]: This value comes from "
                                "strategy_trading_days_metadata.live_start_date");
                        }
                    }
                } else {
                    WARN("TRADING_DAYS_CALC [Day T-1]: Could not call get_trading_days function: " +
                         std::string(trading_days_result.error()->what()));
                }
            } catch (const std::exception& e) {
                WARN("TRADING_DAYS_CALC [Day T-1]: Failed to get trading days: " +
                     std::string(e.what()));
            }

            // Calculate yesterday's annualized return using LiveMetricsCalculator
            // Formula: annualized_return = ((1 + total_return)^(252/trading_days) - 1) * 100
            INFO("ANNUALIZED_RETURN_CALC [Day T-1]: Calculating annualized return...");
            INFO("ANNUALIZED_RETURN_CALC [Day T-1]: Input: total_return_decimal = " +
                 std::to_string(yesterday_total_return_decimal) + " (" +
                 std::to_string(yesterday_total_return_decimal * 100.0) + "%)");
            INFO("ANNUALIZED_RETURN_CALC [Day T-1]: Input: trading_days_count = " +
                 std::to_string(trading_days_count));
            INFO("ANNUALIZED_RETURN_CALC [Day T-1]: Formula: ((1 + " +
                 std::to_string(yesterday_total_return_decimal) + ")^(252/" +
                 std::to_string(trading_days_count) + ") - 1) * 100");

            double yesterday_total_return_annualized =
                metrics_calculator->calculate_annualized_return(yesterday_total_return_decimal,
                                                                trading_days_count);

            INFO("ANNUALIZED_RETURN_CALC [Day T-1]: Result: " +
                 std::to_string(yesterday_total_return_annualized) + "%");

            // Calculate yesterday's leverage and risk metrics
            // IMPORTANT: We MUST preserve existing values from the database
            // These were calculated correctly when Day T-1 was originally processed
            double yesterday_gross_leverage = 0.0;
            double yesterday_equity_to_margin_ratio = 0.0;

            // Load existing values from database using LiveDataLoader - DO NOT RECALCULATE
            try {
                auto margin_metrics = data_loader->load_margin_metrics(
                    combined_strategy_id, coordinator_config.portfolio_id, previous_date);
                if (margin_metrics.is_ok() && margin_metrics.value().valid) {
                    auto& metrics = margin_metrics.value();
                    yesterday_gross_leverage = metrics.gross_leverage;
                    yesterday_equity_to_margin_ratio = metrics.equity_to_margin_ratio;

                    // Also update the gross_notional and margin_posted if available
                    yesterday_gross_notional = metrics.gross_notional;
                    yesterday_margin_posted = metrics.margin_posted;

                    INFO("Preserved existing metrics from database via LiveDataLoader: leverage=" +
                         std::to_string(yesterday_gross_leverage) +
                         ", equity_to_margin=" + std::to_string(yesterday_equity_to_margin_ratio) +
                         ", gross_notional=" + std::to_string(yesterday_gross_notional) +
                         ", margin_posted=" + std::to_string(yesterday_margin_posted));
                } else {
                    INFO("No existing margin metrics found for yesterday via LiveDataLoader");
                }
            } catch (const std::exception& e) {
                WARN("Failed to load existing metrics: " + std::string(e.what()));
            }

            // UPDATE yesterday's live_results with ALL recalculated metrics
            // Note: We calculate daily_pnl, total_pnl, and current_portfolio_value in SQL
            // to properly incorporate the EXISTING daily_transaction_costs value
            // IMPORTANT: Only update portfolio_leverage and equity_to_margin_ratio if they are NULL
            // or 0
            std::string update_query =
                "WITH day_before AS ("
                "  SELECT COALESCE(current_portfolio_value, " +
                std::to_string(initial_capital) +
                ") as portfolio, "
                "         COALESCE(total_pnl, 0.0) as total_pnl, "
                "         COALESCE(total_realized_pnl, 0.0) as total_realized_pnl_prev "
                "  FROM trading.live_results "
                "  WHERE strategy_id = '" +
                combined_strategy_id + "' AND portfolio_id = '" + coordinator_config.portfolio_id +
                "' AND DATE(date) < '" + yesterday_date_ss.str() +
                "' "
                "  ORDER BY date DESC LIMIT 1"
                ") "
                "UPDATE trading.live_results SET "
                "daily_realized_pnl = " +
                std::to_string(aggregate_yesterday_total_pnl) +
                ", "
                "daily_pnl = " +
                std::to_string(aggregate_yesterday_total_pnl) +
                " - COALESCE(daily_transaction_costs, 0.0), "
                "total_pnl = COALESCE((SELECT total_pnl FROM day_before), 0.0) + (" +
                std::to_string(aggregate_yesterday_total_pnl) +
                " - COALESCE(daily_transaction_costs, 0.0)), "
                "total_realized_pnl = " +
                std::to_string(yesterday_total_realized_pnl_cumulative) +
                ", "
                "current_portfolio_value = COALESCE((SELECT portfolio FROM day_before), " +
                std::to_string(initial_capital) + ") + (" +
                std::to_string(aggregate_yesterday_total_pnl) +
                " - COALESCE(daily_transaction_costs, 0.0)), "
                "daily_return = CASE WHEN COALESCE((SELECT portfolio FROM day_before), " +
                std::to_string(initial_capital) +
                ") > 0 "
                "               THEN ((" +
                std::to_string(aggregate_yesterday_total_pnl) +
                " - COALESCE(daily_transaction_costs, 0.0)) / COALESCE((SELECT portfolio FROM "
                "day_before), " +
                std::to_string(initial_capital) +
                ")) * 100.0 "
                "               ELSE 0.0 END, "
                "total_cumulative_return = " +
                std::to_string(yesterday_total_cumulative_return_pct) +
                ", "
                "total_annualized_return = " +
                std::to_string(yesterday_total_return_annualized) +
                ", "
                "portfolio_leverage = CASE WHEN portfolio_leverage IS NULL OR portfolio_leverage = "
                "0 THEN " +
                std::to_string(yesterday_gross_leverage) +
                " ELSE portfolio_leverage END, "
                "equity_to_margin_ratio = CASE WHEN equity_to_margin_ratio IS NULL OR "
                "equity_to_margin_ratio = 0 THEN " +
                std::to_string(yesterday_equity_to_margin_ratio) +
                " ELSE equity_to_margin_ratio END, "
                "cash_available = COALESCE((SELECT portfolio FROM day_before), " +
                std::to_string(initial_capital) + ") + (" +
                std::to_string(aggregate_yesterday_total_pnl) +
                " - COALESCE(daily_transaction_costs, 0.0)) - COALESCE(margin_posted, 0.0) "
                "WHERE strategy_id = '" +
                combined_strategy_id + "' AND portfolio_id = '" + coordinator_config.portfolio_id +
                "' AND DATE(date) = '" + yesterday_date_ss.str() + "'";

            INFO("Executing UPDATE query for Day T-1 live_results...");
            INFO("UPDATE will set current_portfolio_value for date: " + yesterday_date_ss.str());

            auto update_result = db->execute_direct_query(update_query);
            if (update_result.is_error()) {
                ERROR("Failed to update Day T-1 live_results: " +
                      std::string(update_result.error()->what()));
            } else if (update_result.value() == 0) {
                // S-4: the statement succeeded and matched no row, so nothing was finalized.
                WARN("Day T-1 live_results UPDATE matched 0 rows for " + yesterday_date_ss.str() +
                     ": no live_results row exists for that date, so its finalized PnL and "
                     "metrics were NOT stored");
            } else {
                INFO(
                    "Successfully updated Day T-1 live_results with finalized PnL and all metrics");

                // Log the expected value
                INFO(
                    "Expected current_portfolio_value calculation: day_before_portfolio + "
                    "(yesterday_pnl - commissions)");
                INFO("  aggregate_yesterday_total_pnl: $" +
                     std::to_string(aggregate_yesterday_total_pnl));
                INFO("  yesterday_transaction_costs: $" +
                     std::to_string(yesterday_transaction_costs_for_calc));
            }

            // UPDATE yesterday's equity_curve using LiveResultsManager
            INFO("Updating Day T-1 equity_curve...");

            // Query the current portfolio value from updated live_results
            std::string get_equity_query =
                "SELECT current_portfolio_value FROM trading.live_results "
                "WHERE strategy_id = '" +
                combined_strategy_id + "' AND portfolio_id = '" + coordinator_config.portfolio_id +
                "' AND DATE(date) = '" + yesterday_date_ss.str() + "'";

            INFO("Querying for portfolio value with date: " + yesterday_date_ss.str());

            auto equity_result = db->execute_query(get_equity_query);
            if (equity_result.is_error()) {
                ERROR("Failed to get portfolio value for equity update: " +
                      std::string(equity_result.error()->what()));
            } else {
                auto table = equity_result.value();
                INFO("Query returned " + std::to_string(table->num_rows()) + " rows");

                if (table->num_rows() > 0) {
                    // NOTE: execute_query returns StringArray for all columns
                    auto array =
                        std::static_pointer_cast<arrow::StringArray>(table->column(0)->chunk(0));

                    // Check for NULL value before reading
                    if (array->IsNull(0)) {
                        ERROR(
                            "Cannot update Day T-1 equity_curve: current_portfolio_value is NULL "
                            "for date " +
                            yesterday_date_ss.str());
                    } else {
                        // Parse string to double
                        double portfolio_value = std::stod(array->GetString(0));
                        INFO("Raw value read from database: " + std::to_string(portfolio_value));

                        // Validate the value before using it
                        if (portfolio_value <= 0.0 || std::isnan(portfolio_value) ||
                            std::isinf(portfolio_value) || portfolio_value < 1000.0) {
                            ERROR("Invalid portfolio value for Day T-1 equity update: " +
                                  std::to_string(portfolio_value) + " (date: " +
                                  yesterday_date_ss.str() + "). Skipping equity_curve update.");
                            ERROR("  Validation failed: <= 0.0? " +
                                  std::string(portfolio_value <= 0.0 ? "YES" : "NO") + ", isnan? " +
                                  std::string(std::isnan(portfolio_value) ? "YES" : "NO") +
                                  ", isinf? " +
                                  std::string(std::isinf(portfolio_value) ? "YES" : "NO") +
                                  ", < 1000? " +
                                  std::string(portfolio_value < 1000.0 ? "YES" : "NO"));
                        } else {
                            INFO("✓ Valid portfolio value for Day T-1: $" +
                                 std::to_string(portfolio_value));

                            // DEBUG: Log the exact timestamp being used for the update
                            auto prev_time_t = std::chrono::system_clock::to_time_t(previous_date);
                            std::stringstream prev_ts_ss;
                            prev_ts_ss
                                << std::put_time(std::gmtime(&prev_time_t), "%Y-%m-%d %H:%M:%S");
                            INFO("DEBUG: previous_date timestamp for equity curve update: " +
                                 prev_ts_ss.str());

                            // DEBUG: Query existing equity_curve timestamp for this date
                            std::string debug_eq_query =
                                "SELECT timestamp, equity FROM trading.equity_curve "
                                "WHERE strategy_id = '" +
                                combined_strategy_id + "' AND portfolio_id = '" +
                                coordinator_config.portfolio_id +
                                "' "
                                "AND DATE(timestamp) = '" +
                                yesterday_date_ss.str() +
                                "' "
                                "ORDER BY timestamp";
                            auto debug_result = db->execute_query(debug_eq_query);
                            if (debug_result.is_ok() && debug_result.value()->num_rows() > 0) {
                                INFO("DEBUG: Existing equity_curve entries for " +
                                     yesterday_date_ss.str() + ":");
                                auto debug_table = debug_result.value();
                                for (int64_t i = 0; i < debug_table->num_rows(); ++i) {
                                    auto ts_arr = std::static_pointer_cast<arrow::StringArray>(
                                        debug_table->column(0)->chunk(0));
                                    // execute_query returns StringArray for all columns
                                    auto eq_arr = std::static_pointer_cast<arrow::StringArray>(
                                        debug_table->column(1)->chunk(0));
                                    if (!ts_arr->IsNull(i) && !eq_arr->IsNull(i)) {
                                        INFO("DEBUG:   Existing row: timestamp=" +
                                             ts_arr->GetString(i) +
                                             ", equity=" + eq_arr->GetString(i));
                                    }
                                }
                            } else {
                                INFO("DEBUG: No existing equity_curve entry found for " +
                                     yesterday_date_ss.str());
                            }

                            // Create a temporary LiveResultsManager for Day T-1 equity update
                            auto yesterday_manager = std::make_unique<LiveResultsManager>(
                                db, true, combined_strategy_id, coordinator_config.portfolio_id);
                            yesterday_manager->set_equity(portfolio_value);

                            auto update_equity_result =
                                yesterday_manager->save_equity_curve(previous_date);
                            if (update_equity_result.is_error()) {
                                ERROR("Failed to update Day T-1 equity_curve: " +
                                      std::string(update_equity_result.error()->what()));
                            } else {
                                INFO("Successfully updated Day T-1 equity_curve with value: " +
                                     std::to_string(portfolio_value));

                                // DEBUG: Verify what was actually saved
                                auto verify_result = db->execute_query(debug_eq_query);
                                if (verify_result.is_ok() &&
                                    verify_result.value()->num_rows() > 0) {
                                    INFO("DEBUG: After update, equity_curve entries for " +
                                         yesterday_date_ss.str() + ":");
                                    auto verify_table = verify_result.value();
                                    for (int64_t i = 0; i < verify_table->num_rows(); ++i) {
                                        auto ts_arr = std::static_pointer_cast<arrow::StringArray>(
                                            verify_table->column(0)->chunk(0));
                                        // execute_query returns StringArray for all columns
                                        auto eq_arr = std::static_pointer_cast<arrow::StringArray>(
                                            verify_table->column(1)->chunk(0));
                                        if (!ts_arr->IsNull(i) && !eq_arr->IsNull(i)) {
                                            INFO("DEBUG:   Row after update: timestamp=" +
                                                 ts_arr->GetString(i) +
                                                 ", equity=" + eq_arr->GetString(i));
                                        }
                                    }
                                }
                            }
                        }
                    }
                } else {
                    WARN("No live_results found for date " + yesterday_date_ss.str() +
                         ", skipping equity_curve update");
                }
            }

            // Recalculate historical performance metrics for Day T-1 and update live_results
            try {
                HistoricalMetrics yesterday_hist_metrics;

                if (data_loader && data_loader->is_connected()) {
                    auto returns_hist_res = data_loader->load_daily_returns_history(
                        combined_strategy_id, coordinator_config.portfolio_id, previous_date);
                    auto pnl_hist_res = data_loader->load_daily_pnl_history(
                        combined_strategy_id, coordinator_config.portfolio_id, previous_date);
                    auto equity_hist_res = data_loader->load_equity_curve_history(
                        combined_strategy_id, coordinator_config.portfolio_id, previous_date);
                    auto trades_hist_res = data_loader->load_total_trades_count(
                        combined_strategy_id, coordinator_config.portfolio_id, previous_date);

                    std::vector<double> returns_hist;
                    std::vector<double> pnl_hist;
                    std::vector<double> equity_hist;
                    int total_trades_hist = 0;

                    if (returns_hist_res.is_ok()) {
                        returns_hist = returns_hist_res.value();
                    }
                    if (pnl_hist_res.is_ok()) {
                        pnl_hist = pnl_hist_res.value();
                    }
                    if (equity_hist_res.is_ok()) {
                        equity_hist = equity_hist_res.value();
                    }
                    if (trades_hist_res.is_ok()) {
                        total_trades_hist = trades_hist_res.value();
                    }

                    LiveHistoricalMetricsCalculator hist_calc;
                    // NOTE: returns_hist already in PERCENT units (daily_return SQL has *100.0).
                    // Removed the *100 here — that was producing 100x volatility / 100x lower sharpe.
                    yesterday_hist_metrics =
                        hist_calc.calculate(returns_hist, pnl_hist, equity_hist,
                                            yesterday_total_return_annualized, total_trades_hist);

                    // Override total_days with authoritative trading days count
                    yesterday_hist_metrics.total_days = trading_days_count;
                    if (trading_days_count > 0) {
                        yesterday_hist_metrics.win_rate =
                            static_cast<double>(yesterday_hist_metrics.winning_days) /
                            static_cast<double>(trading_days_count) * 100.0;
                    }

                    INFO("HIST_METRICS [Day T-1]: volatility=" +
                         std::to_string(yesterday_hist_metrics.volatility) +
                         " sharpe=" + std::to_string(yesterday_hist_metrics.sharpe_ratio) +
                         " winning_days=" + std::to_string(yesterday_hist_metrics.winning_days) +
                         " losing_days=" + std::to_string(yesterday_hist_metrics.losing_days) +
                         " total_days=" + std::to_string(yesterday_hist_metrics.total_days) +
                         " best_day=" + std::to_string(yesterday_hist_metrics.best_day) +
                         " worst_day=" + std::to_string(yesterday_hist_metrics.worst_day));

                    std::unordered_map<std::string, double> metric_updates = {
                        {"sharpe_ratio", yesterday_hist_metrics.sharpe_ratio},
                        {"sortino_ratio", yesterday_hist_metrics.sortino_ratio},
                        {"max_drawdown", yesterday_hist_metrics.max_drawdown},
                        {"volatility", yesterday_hist_metrics.volatility},
                        {"downside_deviation", yesterday_hist_metrics.downside_deviation},
                        {"win_rate", yesterday_hist_metrics.win_rate},
                        {"avg_win", yesterday_hist_metrics.avg_win},
                        {"avg_loss", yesterday_hist_metrics.avg_loss},
                        {"profit_factor", yesterday_hist_metrics.profit_factor},
                        {"best_day", yesterday_hist_metrics.best_day},
                        {"worst_day", yesterday_hist_metrics.worst_day},
                        {"gross_profit", yesterday_hist_metrics.gross_profit},
                        {"gross_loss", yesterday_hist_metrics.gross_loss},
                        // Removed total_trades - column dropped from database
                        {"winning_days", static_cast<double>(yesterday_hist_metrics.winning_days)},
                        {"losing_days", static_cast<double>(yesterday_hist_metrics.losing_days)},
                        {"total_days", static_cast<double>(yesterday_hist_metrics.total_days)}};

                    auto yesterday_metrics_manager = std::make_unique<LiveResultsManager>(
                        db, true, combined_strategy_id, coordinator_config.portfolio_id);
                    auto update_metrics_result = yesterday_metrics_manager->update_live_results(
                        previous_date, metric_updates);
                    if (update_metrics_result.is_error()) {
                        WARN("Failed to update historical performance metrics for Day T-1: " +
                             std::string(update_metrics_result.error()->what()));
                    } else {
                        INFO(
                            "Successfully updated historical performance metrics for Day T-1 in "
                            "trading.live_results");
                    }
                } else {
                    WARN(
                        "LiveDataLoader not available or not connected; skipping Day T-1 "
                        "historical metrics update.");
                }
            } catch (const std::exception& e) {
                WARN("Exception while updating historical performance metrics for Day T-1: " +
                     std::string(e.what()));
            }

            // Load updated metrics from database for email - MUST do this AFTER the UPDATE
            try {
                std::string metrics_query =
                    "SELECT daily_return, daily_pnl, daily_realized_pnl, daily_unrealized_pnl, "
                    "portfolio_leverage, equity_to_margin_ratio "
                    "FROM trading.live_results "
                    "WHERE strategy_id = '" +
                    combined_strategy_id + "' AND portfolio_id = '" +
                    coordinator_config.portfolio_id + "' AND DATE(date) = '" +
                    yesterday_date_ss.str() + "'";

                INFO("Loading yesterday's metrics from database with query: " + metrics_query);
                auto metrics_result = db->execute_query(metrics_query);

                if (metrics_result.is_ok() && metrics_result.value()->num_rows() > 0) {
                    auto table = metrics_result.value();
                    if (table->num_columns() >= 4) {
                        // FUT-email-UB (C-5 B1). These four columns were read by
                        // static_pointer_cast<arrow::DoubleArray> on arrays that are NOT
                        // DoubleArrays. execute_query goes through convert_generic_to_arrow,
                        // which builds a StringBuilder for every column and stamps utf8 on
                        // the field (postgres_database.cpp, "Build a string array for all
                        // columns"), so every one of these is a StringArray. static_pointer_cast
                        // does not check; Value(0) then reads the string array's OFFSETS
                        // buffer as if it were a double. That is undefined behaviour, and on
                        // this host it printed 0.000000 for a row that held daily_return
                        // 1.2767 and daily_pnl 6347.25.
                        //
                        // Read by the array's actual type instead, the way chart_generator.cpp
                        // has always done it (:188-196). Dispatching rather than assuming utf8
                        // is deliberate: it stays correct if convert_generic_to_arrow is ever
                        // given real column types, which is the change condition 2 of
                        // STAGE3_PLAN section 20 pairs with this one.
                        auto numeric_cell = [](const std::shared_ptr<arrow::Table>& t, int col,
                                               double& out) -> bool {
                            if (!t || col >= t->num_columns()) return false;
                            auto column = t->column(col);
                            if (!column || column->num_chunks() == 0) return false;
                            auto chunk = column->chunk(0);
                            if (!chunk || chunk->length() == 0 || chunk->IsNull(0)) return false;
                            switch (chunk->type_id()) {
                                case arrow::Type::STRING: {
                                    auto a = std::static_pointer_cast<arrow::StringArray>(chunk);
                                    try {
                                        out = std::stod(a->GetString(0));
                                    } catch (const std::exception&) {
                                        return false;
                                    }
                                    return true;
                                }
                                case arrow::Type::LARGE_STRING: {
                                    auto a =
                                        std::static_pointer_cast<arrow::LargeStringArray>(chunk);
                                    try {
                                        out = std::stod(a->GetString(0));
                                    } catch (const std::exception&) {
                                        return false;
                                    }
                                    return true;
                                }
                                case arrow::Type::DOUBLE: {
                                    auto a = std::static_pointer_cast<arrow::DoubleArray>(chunk);
                                    out = a->Value(0);
                                    return true;
                                }
                                case arrow::Type::FLOAT: {
                                    auto a = std::static_pointer_cast<arrow::FloatArray>(chunk);
                                    out = static_cast<double>(a->Value(0));
                                    return true;
                                }
                                case arrow::Type::INT64: {
                                    auto a = std::static_pointer_cast<arrow::Int64Array>(chunk);
                                    out = static_cast<double>(a->Value(0));
                                    return true;
                                }
                                case arrow::Type::INT32: {
                                    auto a = std::static_pointer_cast<arrow::Int32Array>(chunk);
                                    out = static_cast<double>(a->Value(0));
                                    return true;
                                }
                                default:
                                    return false;
                            }
                        };

                        double cell = 0.0;
                        if (numeric_cell(table, 0, cell)) {
                            yesterday_daily_return_for_email = cell;
                            INFO("Loaded yesterday's daily_return: " +
                                 std::to_string(yesterday_daily_return_for_email));
                        }
                        if (numeric_cell(table, 1, cell)) {
                            yesterday_daily_pnl_for_email = cell;
                            INFO("Loaded yesterday's daily_pnl: " +
                                 std::to_string(yesterday_daily_pnl_for_email));
                        }
                        if (numeric_cell(table, 2, cell)) {
                            yesterday_realized_pnl_for_email = cell;
                            INFO("Loaded yesterday's daily_realized_pnl: " +
                                 std::to_string(yesterday_realized_pnl_for_email));
                        } else {
                            // If daily_realized_pnl is null or 0, use aggregate_yesterday_total_pnl
                            // as fallback
                            yesterday_realized_pnl_for_email = aggregate_yesterday_total_pnl;
                            INFO(
                                "Using calculated aggregate_yesterday_total_pnl as realized PnL: " +
                                std::to_string(yesterday_realized_pnl_for_email));
                        }
                        if (numeric_cell(table, 3, cell)) {
                            yesterday_unrealized_pnl_for_email = cell;
                            INFO("Loaded yesterday's daily_unrealized_pnl: " +
                                 std::to_string(yesterday_unrealized_pnl_for_email));
                        }

                        // For futures, unrealized PnL should always be 0, realized PnL is the total
                        // daily PnL
                        yesterday_unrealized_pnl_for_email = 0.0;  // Futures have no unrealized PnL

                        INFO("Successfully loaded yesterday's metrics from database for email");
                    }
                } else {
                    WARN("No metrics found in database for yesterday, using calculated values");
                    // Use the calculated values as fallback
                    yesterday_realized_pnl_for_email = aggregate_yesterday_total_pnl;
                    yesterday_daily_pnl_for_email =
                        aggregate_yesterday_total_pnl;  // For futures, daily PnL = realized PnL
                    yesterday_unrealized_pnl_for_email = 0.0;  // No unrealized for futures
                }
            } catch (const std::exception& e) {
                WARN("Failed to load updated yesterday's metrics: " + std::string(e.what()));
                // Use calculated values as fallback
                yesterday_realized_pnl_for_email = aggregate_yesterday_total_pnl;
                yesterday_daily_pnl_for_email = aggregate_yesterday_total_pnl;
                yesterday_unrealized_pnl_for_email = 0.0;
            }
        } else {
            if (is_first_trading_day) {
                INFO(
                    "Skipping Day T-1 update (first trading day - no previous positions to "
                    "finalize)");
            } else {
                INFO("Skipping Day T-1 live_results update (no two_days_ago prices or zero PnL)");
            }
        }

        // ========================================
        // STEP 5: LOAD UPDATED PREVIOUS DAY AGGREGATES AND CALCULATE Day T CUMULATIVE VALUES
        // ========================================
        INFO(
            "STEP 5: Loading updated previous day aggregates and calculating Day T cumulative "
            "values...");

        // Load previous day's aggregates (portfolio value, total pnl, total transaction costs)
        // This is done AFTER updating Day T-1 live_results to ensure we get the finalized values
        double previous_portfolio_value = initial_capital;  // Default to initial capital
        double previous_total_pnl = 0.0;
        double previous_total_transaction_costs = 0.0;

        try {
            auto db_ptr = std::dynamic_pointer_cast<PostgresDatabase>(db);
            if (db_ptr) {
                auto prev_agg = db_ptr->get_previous_live_aggregates(
                    combined_strategy_id, coordinator_config.portfolio_id, now,
                    "trading.live_results");
                if (prev_agg.is_ok()) {
                    std::tie(previous_portfolio_value, previous_total_pnl,
                             previous_total_transaction_costs) = prev_agg.value();
                    INFO("Loaded updated previous aggregates - portfolio_value: $" +
                         std::to_string(previous_portfolio_value) + ", total_pnl: $" +
                         std::to_string(previous_total_pnl) + ", total_transaction_costs: $" +
                         std::to_string(previous_total_transaction_costs));
                } else {
                    INFO("No previous aggregates found: " + std::string(prev_agg.error()->what()));
                }
            }
        } catch (const std::exception& e) {
            INFO("Could not load previous day aggregates: " + std::string(e.what()));
        }
        // LOOP_SPEC section 3.1: after STEP 4, the Day T-1 net the sizing read rebuilt beside the
        // one the finalize stored (Day T-1's stored value less the stored value of the row before
        // it). On an unsettled Day T-1 nothing was added and nothing is compared.
        {
            const double rebuilt_t1_net = sizing_equity.t1_settlement - sizing_equity.t1_costs;
            const double stored_t1_net = previous_portfolio_value - sizing_equity.day_before;
            const bool compared = sizing_equity.t1_row && !sizing_capital_read.t1_unsettled;
            INFO("SIZING_CAPITAL_CHECK date=" + core::format_utc_date(now) +
                 " t1_settled=" + (compared ? "1" : "0") +
                 " rebuilt_t1_net=" + std::to_string(compared ? rebuilt_t1_net : 0.0) +
                 " finalized_t1_daily_pnl=" + std::to_string(compared ? stored_t1_net : 0.0) +
                 " difference=" + std::to_string(compared ? rebuilt_t1_net - stored_t1_net : 0.0) +
                 " sized_on=" + std::to_string(sizing_capital_read.capital.capital));
        }

        // Calculate cumulative values for Day T
        double total_pnl = previous_total_pnl + daily_pnl_for_today;
        double current_portfolio_value = previous_portfolio_value + daily_pnl_for_today;
        double daily_pnl = daily_pnl_for_today;  // Only transaction costs on Day T
        double total_transaction_costs_cumulative =
            previous_total_transaction_costs + total_daily_transaction_costs;
        // Migration 017: the ROLL subset, cumulative (the previous row's total plus today's).
        auto previous_roll_costs = db->get_previous_total_roll_costs(
            combined_strategy_id, portfolio_id, now, "trading.live_results");
        if (previous_roll_costs.is_error()) {
            ERROR("ROLL_LEG STOP: the previous total_roll_costs is unreadable (" +
                  std::string(previous_roll_costs.error()->what()) + "); refusing to run");
            return 1;
        }
        const double total_roll_costs_cumulative =
            previous_roll_costs.value() + total_daily_roll_costs;

        // Since it's futures, all PnL is realized
        // total_realized_pnl = total_pnl + total_transaction_costs (GROSS)
        double total_realized_pnl = total_pnl + total_transaction_costs_cumulative;
        double total_unrealized_pnl = 0.0;

        // Calculate returns using LiveMetricsCalculator
        double daily_return =
            metrics_calculator->calculate_daily_return(daily_pnl, previous_portfolio_value);

        // Calculate total cumulative return (non-annualized)
        double total_cumulative_return =
            metrics_calculator->calculate_total_return(current_portfolio_value, initial_capital);

        double total_return_decimal = 0.0;
        if (initial_capital > 0.0) {
            total_return_decimal = (current_portfolio_value - initial_capital) / initial_capital;
        }
        double total_cumulative_return_pct = total_cumulative_return;  // Already in %

        // Get n = number of trading days using PostgreSQL function (robust against row duplication)
        // Uses trading.strategy_trading_days_metadata table for live_start_date
        int trading_days_count = 1;  // Default to 1 to avoid division by zero on first day
        try {
            // Format today's date for SQL query
            auto now_time_t_for_query = std::chrono::system_clock::to_time_t(now);
            std::stringstream now_date_ss;
            now_date_ss << std::put_time(std::gmtime(&now_time_t_for_query), "%Y-%m-%d");

            // Call PostgreSQL function to calculate trading days
            // E2-F6: portfolio-scoped (3-arg) form. The 2-arg overload keys on
            // strategy_id alone with `ORDER BY live_start_date LIMIT 1` and NO portfolio
            // predicate, so it takes the earliest row across ALL portfolios.
            // LIVE_TREND_FOLLOWING has a metadata row under both BASE_PORTFOLIO and
            // CONSERVATIVE_PORTFOLIO; they agree only because both carry
            // live_start_date = 2025-10-05. Add or edit a BASE row with an earlier date
            // and the conservative book's annualization changes silently -- no error, no
            // log line. Definition is versioned in migrations/004.
            std::string trading_days_query = "SELECT trading.get_trading_days('" +
                                             combined_strategy_id + "', DATE '" +
                                             now_date_ss.str() + "', '" + portfolio_id + "')";

            INFO("TRADING_DAYS_CALC [Day T]: Querying trading days...");
            INFO("TRADING_DAYS_CALC [Day T]: Query: " + trading_days_query);
            INFO("TRADING_DAYS_CALC [Day T]: Strategy ID: " + combined_strategy_id);
            INFO("TRADING_DAYS_CALC [Day T]: Target Date: " + now_date_ss.str());

            auto trading_days_result = db->execute_query(trading_days_query);

            if (trading_days_result.is_ok()) {
                auto table = trading_days_result.value();
                if (table && table->num_rows() > 0 && table->num_columns() > 0) {
                    // execute_query returns StringArray for all columns
                    auto arr =
                        std::static_pointer_cast<arrow::StringArray>(table->column(0)->chunk(0));
                    if (arr && arr->length() > 0 && !arr->IsNull(0)) {
                        trading_days_count = std::max<int>(1, std::stoi(arr->GetString(0)));
                        INFO("TRADING_DAYS_CALC [Day T]: Result from DB: " +
                             std::to_string(trading_days_count) + " trading days");
                        INFO(
                            "TRADING_DAYS_CALC [Day T]: This value comes from "
                            "strategy_trading_days_metadata.live_start_date");
                    }
                }
            } else {
                WARN("TRADING_DAYS_CALC [Day T]: Could not call get_trading_days function: " +
                     std::string(trading_days_result.error()->what()));
            }
        } catch (const std::exception& e) {
            WARN("TRADING_DAYS_CALC [Day T]: Failed to get trading days: " + std::string(e.what()));
        }

        // Calculate annualized return using LiveMetricsCalculator
        // Formula: annualized_return = ((1 + total_return)^(252/trading_days) - 1) * 100
        INFO("ANNUALIZED_RETURN_CALC [Day T]: Calculating annualized return...");
        INFO("ANNUALIZED_RETURN_CALC [Day T]: Input: total_return_decimal = " +
             std::to_string(total_return_decimal) + " (" +
             std::to_string(total_return_decimal * 100.0) + "%)");
        INFO("ANNUALIZED_RETURN_CALC [Day T]: Input: trading_days_count = " +
             std::to_string(trading_days_count));
        INFO("ANNUALIZED_RETURN_CALC [Day T]: Formula: ((1 + " +
             std::to_string(total_return_decimal) + ")^(252/" + std::to_string(trading_days_count) +
             ") - 1) * 100");

        double total_return_annualized = metrics_calculator->calculate_annualized_return(
            total_return_decimal, trading_days_count);

        INFO("ANNUALIZED_RETURN_CALC [Day T]: Result: " + std::to_string(total_return_annualized) +
             "%");

        INFO("Portfolio value calculation:");
        INFO("  Previous portfolio value: $" + std::to_string(previous_portfolio_value));
        INFO("  Daily PnL: $" + std::to_string(daily_pnl));
        INFO("  Current portfolio value: $" + std::to_string(current_portfolio_value));
        INFO("  Total PnL: $" + std::to_string(total_pnl));
        INFO("  Daily return: " + std::to_string(daily_return) + "%");
        INFO("  Annualized return: " + std::to_string(total_return_annualized) + "%");

        // MAIN-post-#55: the equity-to-margin numerator, computed where the equity is
        // known. Same formula and same guard as before, current_portfolio_value in place
        // of the initial_capital constant, matching LiveMetricsCalculator's
        // calculate_equity_to_margin_ratio(current_portfolio_value, margin_posted).
        equity_to_margin_ratio =
            (total_posted_margin > 0.0) ? (current_portfolio_value / total_posted_margin) : 0.0;
        if (equity_to_margin_ratio <= 1.0 && active_positions > 0) {
            WARN("Equity-to-Margin Ratio is <= 1.0 (account equity at or below "
                 "posted margin); verify margins and sizing.");
        }

        std::cout << "Total P&L: $" << std::fixed << std::setprecision(2) << total_pnl << std::endl;
        std::cout << "Realized P&L: $" << std::fixed << std::setprecision(2) << total_realized_pnl
                  << std::endl;
        std::cout << "Unrealized P&L: $" << std::fixed << std::setprecision(2)
                  << total_unrealized_pnl << std::endl;
        std::cout << "Current Portfolio Value: $" << std::fixed << std::setprecision(2)
                  << current_portfolio_value << std::endl;
        std::cout << "Total Return (Cumulative): " << std::fixed << std::setprecision(2)
                  << total_cumulative_return_pct << "%" << std::endl;
        std::cout << "Total Return (Annualized): " << std::fixed << std::setprecision(2)
                  << total_return_annualized << "%" << std::endl;
        std::cout << "Daily Return: " << std::fixed << std::setprecision(2) << daily_return << "%"
                  << std::endl;
        std::cout << "Gross Leverage: " << std::fixed << std::setprecision(2)
                  << (gross_notional / current_portfolio_value) << "x" << std::endl;
        std::cout << "Posted Margin (Initial×Contracts): $" << std::fixed << std::setprecision(2)
                  << total_posted_margin << std::endl;
        std::cout << "Equity-to-Margin Ratio: " << std::fixed << std::setprecision(2)
                  << equity_to_margin_ratio << "x" << std::endl;
        double margin_cushion = 0.0;
        if (maintenance_requirement_today > 0.0) {
            // Correct formula: margin_cushion = (equity - maintenance) / equity
            // This shows how much cushion we have above maintenance margin requirements
            margin_cushion =
                (current_portfolio_value - maintenance_requirement_today) / current_portfolio_value;
        } else {
            margin_cushion = -1.0;  // Invalid if no maintenance requirement
        }

        // Warnings per thresholds
        if (total_posted_margin > current_portfolio_value) {
            WARN("Posted margin exceeds current portfolio value; check sizing and risk limits.");
        }
        if (margin_cushion < 0.20) {
            WARN("Margin cushion below 20%.");
        }
        // Old WARN fired on `e2m_ratio > 4.0` — sensible only under the prior
        // (buggy) gross_notional/margin formula where high = more leverage.
        // After the formula fix, high e2m means more equity per dollar of
        // posted margin (safer), so a ceiling alarm is no longer meaningful.
        // Replaced with a low-floor alarm to catch true under-collateralization.
        if (equity_to_margin_ratio > 0.0 && equity_to_margin_ratio < 1.5) {
            WARN("Equity-to-Margin Ratio below 1.5x (low margin cushion).");
        }

        // Get forecasts for all symbols
        INFO("Retrieving current forecasts...");
        std::cout << "\n======= Current Forecasts =======" << std::endl;
        std::cout << std::setw(10) << "Symbol"
                  << " | " << std::setw(12) << "Forecast"
                  << " | " << std::setw(12) << "Position" << std::endl;
        std::cout << std::string(40, '-') << std::endl;

        // Collect signals for database storage
        std::unordered_map<std::string, double> signals_to_store;

        for (const auto& symbol : symbols) {
            double forecast = tf_strategy_typed ? tf_strategy_typed->get_forecast(symbol) : 0.0;
            double position = tf_strategy_typed ? tf_strategy_typed->get_position(symbol) : 0.0;

            signals_to_store[symbol] = forecast;

            std::cout << std::setw(10) << symbol << " | " << std::setw(12) << std::fixed
                      << std::setprecision(4) << forecast << " | " << std::setw(12) << std::fixed
                      << std::setprecision(2) << position << std::endl;
        }

        // NOTE: Signals are already stored per-strategy in PHASE 4 above.
        // Do NOT call results_manager->set_signals() here as it would cause duplicate
        // storage with the combined_strategy_id as both strategy_id AND strategy_name,
        // which creates incorrect duplicate entries in the signals table.
        // The signals_to_store map is only used for display purposes above.

        // Save trading results to results table
        INFO("Saving trading results to database...");
        try {
            // Calculate current date for results (use override date if specified)
            auto current_date = now;

            // Create configuration JSON
            nlohmann::json report_config_json;
            report_config_json["strategy_type"] = combined_strategy_id;  // From config (Phase 1)
            report_config_json["capital_allocation"] = initial_capital;
            report_config_json["max_leverage"] = base_strategy_config.max_leverage;
            report_config_json["weight"] = 0.03;      // Default weight
            report_config_json["risk_target"] = 0.2;  // Default risk target
            report_config_json["idm"] = 2.5;          // Default IDM
            report_config_json["active_positions"] = active_positions;
            report_config_json["gross_notional"] = gross_notional;
            report_config_json["net_notional"] = net_notional;
            report_config_json["gross_leverage"] = gross_notional / initial_capital;
            // T-7a C4: the day's T-1 classification (counts, feed holes, held symbols).
            report_config_json["t1_classification"] =
                t1_classification.to_json(held_symbols(book_holds));

            // Create SQL insert for live_results table with correct schema
            std::stringstream date_ss;
            auto time_t = std::chrono::system_clock::to_time_t(current_date);
            date_ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%d %H:%M:%S");

            // Use calculated metrics from position analysis
            double portfolio_var = 0.0;
            double net_leverage = 0.0;
            double max_correlation = 0.0;
            double jump_risk = 0.0;
            double risk_scale = 1.0;

            if (risk_eval.is_ok()) {
                const auto& r = risk_eval.value();
                portfolio_var = r.portfolio_var;
                max_correlation = r.correlation_risk;
                jump_risk = r.jump_risk;
                risk_scale = r.recommended_scale;
            }

            // Use LiveMetricsCalculator for portfolio metrics
            double gross_leverage = metrics_calculator->calculate_gross_leverage(
                gross_notional, current_portfolio_value);
            // Net leverage: net_notional / current_portfolio_value (standard definition)
            net_leverage =
                (current_portfolio_value > 0.0) ? (net_notional / current_portfolio_value) : 0.0;
            // equity_to_margin_ratio and margin_cushion already computed above

            // Calculate since-inception performance metrics (Sharpe, Sortino, MaxDD, etc.)
            HistoricalMetrics historical_metrics;
            try {
                if (data_loader && data_loader->is_connected()) {
                    // Use previous_date so history comes from all fully finalized days (<= Day T-1)
                    auto returns_hist_res = data_loader->load_daily_returns_history(
                        combined_strategy_id, coordinator_config.portfolio_id, previous_date);
                    auto pnl_hist_res = data_loader->load_daily_pnl_history(
                        combined_strategy_id, coordinator_config.portfolio_id, previous_date);
                    auto equity_hist_res = data_loader->load_equity_curve_history(
                        combined_strategy_id, coordinator_config.portfolio_id, previous_date);
                    auto trades_hist_res = data_loader->load_total_trades_count(
                        combined_strategy_id, coordinator_config.portfolio_id, current_date);

                    std::vector<double> returns_hist;
                    std::vector<double> pnl_hist;
                    std::vector<double> equity_hist;
                    int total_trades_hist = 0;

                    if (returns_hist_res.is_ok()) {
                        returns_hist = returns_hist_res.value();
                    }
                    if (pnl_hist_res.is_ok()) {
                        pnl_hist = pnl_hist_res.value();
                    }
                    if (equity_hist_res.is_ok()) {
                        equity_hist = equity_hist_res.value();
                    }
                    if (trades_hist_res.is_ok()) {
                        total_trades_hist = trades_hist_res.value();
                    }

                    // Append today's data so metrics are up-to-date as of Day T.
                    // NOTE: returns_hist already in PERCENT units. Removed *100 — was producing
                    // 100x volatility / 100x lower sharpe. daily_return is also already percent.
                    returns_hist.push_back(daily_return);
                    pnl_hist.push_back(daily_pnl);
                    equity_hist.push_back(current_portfolio_value);

                    LiveHistoricalMetricsCalculator hist_calc;
                    historical_metrics =
                        hist_calc.calculate(returns_hist, pnl_hist, equity_hist,
                                            total_return_annualized, total_trades_hist);

                    // Override total_days with authoritative trading days count from
                    // get_trading_days() DB function, which uses strategy_trading_days_metadata
                    historical_metrics.total_days = trading_days_count;
                    // Recalculate win_rate using actual trading days as denominator
                    if (trading_days_count > 0) {
                        historical_metrics.win_rate =
                            static_cast<double>(historical_metrics.winning_days) /
                            static_cast<double>(trading_days_count) * 100.0;
                    }
                } else {
                    WARN(
                        "LiveDataLoader not available or not connected; historical performance "
                        "metrics will remain at default values for today.");
                }
            } catch (const std::exception& e) {
                WARN("Exception while calculating historical performance metrics for today: " +
                     std::string(e.what()));
            }

            // Use the LiveResultsManager
            INFO("Setting metrics in LiveResultsManager...");

            // Prepare metrics maps
            std::unordered_map<std::string, double> double_metrics = {
                {"total_cumulative_return", total_cumulative_return_pct},
                {"total_annualized_return", total_return_annualized},
                {"volatility", historical_metrics.volatility},
                {"downside_deviation", historical_metrics.downside_deviation},
                {"sharpe_ratio", historical_metrics.sharpe_ratio},
                {"sortino_ratio", historical_metrics.sortino_ratio},
                {"max_drawdown", historical_metrics.max_drawdown},
                {"win_rate", historical_metrics.win_rate},
                {"avg_win", historical_metrics.avg_win},
                {"avg_loss", historical_metrics.avg_loss},
                {"profit_factor", historical_metrics.profit_factor},
                {"best_day", historical_metrics.best_day},
                {"worst_day", historical_metrics.worst_day},
                {"gross_profit", historical_metrics.gross_profit},
                {"gross_loss", historical_metrics.gross_loss},
                {"total_pnl", total_pnl},
                {"total_unrealized_pnl", total_unrealized_pnl},
                {"total_realized_pnl", total_realized_pnl},
                {"current_portfolio_value", current_portfolio_value},
                {"portfolio_var", portfolio_var},
                {"net_leverage", net_leverage},
                {"portfolio_leverage", gross_leverage},
                {"equity_to_margin_ratio", equity_to_margin_ratio},
                {"margin_cushion", margin_cushion},
                {"max_correlation", max_correlation},
                {"jump_risk", jump_risk},
                {"risk_scale", risk_scale},
                {"gross_notional", gross_notional},
                {"net_notional", net_notional},
                {"daily_return", daily_return},
                {"daily_pnl", daily_pnl},
                {"total_transaction_costs", total_transaction_costs_cumulative},
                {"daily_realized_pnl", daily_realized_pnl},
                {"daily_unrealized_pnl", daily_unrealized_pnl},
                {"daily_transaction_costs", total_daily_transaction_costs},
                {"daily_roll_costs", total_daily_roll_costs},        // migration 017
                {"total_roll_costs", total_roll_costs_cumulative},  // migration 017
                {"margin_posted", total_posted_margin},
                {"cash_available", current_portfolio_value - total_posted_margin}};

            std::unordered_map<std::string, int> int_metrics = {
                {"active_positions", active_positions},
                // Removed total_trades - will be implemented properly later with closing trades logic
                {"winning_days", historical_metrics.winning_days},
                {"losing_days", historical_metrics.losing_days},
                {"total_days", historical_metrics.total_days}};

            // Set all metrics at once
            results_manager->set_metrics(double_metrics, int_metrics);

            // Set config
            results_manager->set_config(report_config_json);

            // Set equity for equity curve tracking
            results_manager->set_equity(current_portfolio_value);
        } catch (const std::exception& e) {
            ERROR("Exception while saving trading results: " + std::string(e.what()));
        }

        // Phase 4: Use CSVExporter for position export
        INFO("Using CSVExporter to save positions to file...");

        // Note: previously called LiveDataLoader::load_commissions_by_symbol here, but it
        // was dead code (result map was unused; csv_exporter explicitly does
        // (void)symbol_commissions) and the SQL referenced a column that no longer exists.
        // All real transaction-cost data flows from cost_manager_->calculate_costs() at
        // execution time and lives on trading.executions / trading.live_results directly.

        // Export current positions with per-strategy breakdown
        std::string today_filename;
        // T-7a C3 (HD 2026-09-18, the Sunday positions CSV): on a day with no session the
        // strategies were fed nothing and no rebalance ran (the MarketDataBus is off at the
        // load), so their forecasts, volatilities and EMAs are empty. The file then carries the
        // real details instead of empty rows: the held book (strategy_positions_map, loaded by
        // the non-trading-day branch above), each symbol's last mark (its latest loaded close)
        // and each sleeve's last computed forecasts (its stored signals of the previous
        // session), with a note that no session occurred and the values are carried, not
        // computed.
        // T-7b-1 C7b R2 (T-7a_CODE_REVIEW R2): why the day had no session is the T-1
        // classification's answer, not the weekday's. A feed-hole carry (the dead Sundays) used
        // to write "no session on D ()" and send an email without any banner.
        const std::string carried_day_note = "no session on " + yesterday_date_str_check + " (" +
                                             carried_day_reason(t1_classification) + ")";
        auto export_positions_file = [&]() -> Result<std::string> {
            if (!skip_strategy_processing) {
                return csv_exporter->export_current_positions(
                    now, strategy_positions_map,
                    previous_day_close_prices,  // Market prices (Day T-1 close)
                    current_portfolio_value, gross_notional, net_notional,
                    strategy_instances_map);
            }
            std::unordered_map<std::string, CarriedMark> last_marks;
            for (const auto& [symbol, bar] : latest_bars_per_symbol) {
                last_marks[symbol] = CarriedMark{static_cast<double>(bar.close),
                                                 core::format_utc_date(bar.timestamp)};
            }
            std::unordered_map<std::string, CarriedForecasts> last_forecasts;
            for (const auto& strategy_name : strategy_names) {
                auto carried = data_loader->load_last_signals_before(
                    combined_strategy_id, strategy_name, portfolio_id, now);
                if (carried.is_error()) {
                    WARN("Carried forecasts for " + strategy_name + " could not be read: " +
                         std::string(carried.error()->what()));
                    continue;
                }
                INFO("Carried forecasts for " + strategy_name + ": " +
                     std::to_string(carried.value().forecasts.size()) + " from " +
                     (carried.value().session_date.empty() ? std::string("no stored session")
                                                           : carried.value().session_date));
                last_forecasts[strategy_name] = carried.value();
            }
            return csv_exporter->export_carried_positions(
                now, strategy_positions_map, last_marks, last_forecasts, current_portfolio_value,
                gross_notional, net_notional, carried_day_note);
        };
        auto current_export_result = export_positions_file();

        if (current_export_result.is_ok()) {
            today_filename = current_export_result.value();
            INFO("Today's positions saved to " + today_filename);
        } else {
            ERROR("Failed to export current positions: " +
                  std::string(current_export_result.error()->what()));
        }

        // Export yesterday's finalized positions with per-strategy breakdown (if not first trading
        // day)
        std::string yesterday_filename;
        if (!is_first_trading_day && !previous_strategy_positions.empty()) {
            INFO("Exporting yesterday's finalized positions with per-strategy breakdown...");

            auto yesterday_time = now - std::chrono::hours(24);

            auto finalized_export_result = csv_exporter->export_finalized_positions(
                now, yesterday_time, previous_strategy_positions,
                two_days_ago_close_prices,  // Entry prices (T-2)
                previous_day_close_prices   // Exit prices (T-1)
            );

            if (finalized_export_result.is_ok()) {
                yesterday_filename = finalized_export_result.value();
                INFO("Yesterday's finalized positions saved to " + yesterday_filename);
            } else {
                ERROR("Failed to export finalized positions: " +
                      std::string(finalized_export_result.error()->what()));
            }
        }
        // Store equity curve and save all results to database
        // Use the new LiveResultsManager - save all results at once
        INFO("Saving all live trading results using LiveResultsManager...");

        auto save_result = results_manager->save_all_results(combined_strategy_id, now);
        if (save_result.is_error()) {
            ERROR("Failed to save all live results: " + std::string(save_result.error()->what()));
        } else {
            INFO("Successfully saved all live trading results to database");
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
        std::cout << "Today's positions file: " << today_filename << std::endl;
        // Removed yesterday finalized positions file output per request
        // Only show processing time for real-time runs, not historical
        if (!use_override_date) {
            std::cout << "Total processing time: "
                      << std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now() - now)
                             .count()
                      << "ms" << std::endl;
        }

        INFO("Daily trend following position generation completed successfully");

        // Send email report with trading results (based on send_email flag)
        if (send_email) {
            INFO("Sending email report...");
            try {
                EmailSenderConfig email_config;
                email_config.smtp_host = app_config.email.smtp_host;
                email_config.smtp_port = app_config.email.smtp_port;
                email_config.username = app_config.email.username;
                email_config.password = app_config.email.password;
                email_config.from_email = app_config.email.from_email;
                email_config.use_tls = app_config.email.use_tls;
                email_config.to_emails = app_config.email.to_emails;

                auto email_sender = std::make_shared<EmailSender>(email_config);
                auto email_init_result = email_sender->initialize();
                if (email_init_result.is_error()) {
                    ERROR("Failed to initialize email sender: " +
                          std::string(email_init_result.error()->what()));
                } else {
                    // Prepare email data
                    std::string date_str =
                        std::to_string(now_tm->tm_year + 1900) + "-" +
                        std::string(2 - std::to_string(now_tm->tm_mon + 1).length(), '0') +
                        std::to_string(now_tm->tm_mon + 1) + "-" +
                        std::string(2 - std::to_string(now_tm->tm_mday).length(), '0') +
                        std::to_string(now_tm->tm_mday);

                    std::string subject = "Daily Trading Report - " + date_str;

                    // Load yesterday's finalized positions for email display
                    // Keyed by (sleeve, symbol) (T-7b-1 C7b S1-3): two sleeves holding one
                    // symbol are two rows; keyed by symbol, the last sleeve read won.
                    std::map<std::pair<std::string, std::string>, Position>
                        yesterday_positions_finalized;
                    std::map<std::string, double> yesterday_daily_metrics_final;
                    std::unordered_map<std::string, double>
                        yesterday_entry_prices;                                     // Day T-2 close
                    std::unordered_map<std::string, double> yesterday_exit_prices;  // Day T-1 close

                    // Calculate yesterday's date for email
                    auto yesterday_time_email = now - std::chrono::hours(24);
                    auto yesterday_time_t_email =
                        std::chrono::system_clock::to_time_t(yesterday_time_email);

                    // Load finalized positions from database for email
                    std::string yesterday_date_for_email;
                    std::ostringstream yss_email;
                    yss_email << std::put_time(std::gmtime(&yesterday_time_t_email), "%Y-%m-%d");
                    yesterday_date_for_email = yss_email.str();

                    INFO("Loading yesterday's finalized positions for email: " +
                         yesterday_date_for_email);

                    std::string positions_query_email =
                        "SELECT symbol, quantity, average_price, daily_realized_pnl, "
                        "daily_unrealized_pnl, last_update, strategy_name "
                        "FROM trading.positions "
                        "WHERE strategy_id = '" +
                        combined_strategy_id + "' AND portfolio_id = '" +
                        coordinator_config.portfolio_id +
                        "' AND DATE(last_update) = "
                        "'" +
                        yesterday_date_for_email + "'";

                    auto positions_result_email = db->execute_query(positions_query_email);

                    if (positions_result_email.is_ok() &&
                        positions_result_email.value()->num_rows() > 0) {
                        auto table_email = positions_result_email.value();
                        // All columns are StringArrays from generic converter
                        auto symbol_arr = std::static_pointer_cast<arrow::StringArray>(
                            table_email->column(0)->chunk(0));
                        auto quantity_arr = std::static_pointer_cast<arrow::StringArray>(
                            table_email->column(1)->chunk(0));
                        auto avg_price_arr = std::static_pointer_cast<arrow::StringArray>(
                            table_email->column(2)->chunk(0));
                        auto realized_pnl_arr = std::static_pointer_cast<arrow::StringArray>(
                            table_email->column(3)->chunk(0));
                        auto strategy_name_arr = std::static_pointer_cast<arrow::StringArray>(
                            table_email->column(6)->chunk(0));

                        for (int64_t i = 0; i < table_email->num_rows(); ++i) {
                            if (!symbol_arr->IsNull(i) && !quantity_arr->IsNull(i)) {
                                std::string symbol = symbol_arr->GetString(i);
                                double quantity = std::stod(quantity_arr->GetString(i));
                                double avg_price = std::stod(avg_price_arr->GetString(i));
                                double realized_pnl = std::stod(realized_pnl_arr->GetString(i));

                                // Skip positions with zero quantity
                                if (std::abs(quantity) < 0.0001)
                                    continue;

                                // Create Position object for yesterday's finalized position
                                Position pos;
                                pos.symbol = symbol;
                                pos.quantity = Decimal(quantity);
                                pos.average_price = Decimal(avg_price);
                                pos.realized_pnl = Decimal(realized_pnl);

                                const std::string strategy_name =
                                    strategy_name_arr->IsNull(i) ? std::string()
                                                                 : strategy_name_arr->GetString(i);
                                yesterday_positions_finalized[{strategy_name, symbol}] = pos;

                                // Populate entry and exit prices
                                if (two_days_ago_close_prices.find(symbol) !=
                                    two_days_ago_close_prices.end()) {
                                    yesterday_entry_prices[symbol] =
                                        two_days_ago_close_prices[symbol];
                                }
                                if (previous_day_close_prices.find(symbol) !=
                                    previous_day_close_prices.end()) {
                                    yesterday_exit_prices[symbol] =
                                        previous_day_close_prices[symbol];
                                }
                            }
                        }
                        INFO("Loaded " + std::to_string(yesterday_positions_finalized.size()) +
                             " finalized positions for email");

                        // Load yesterday's daily metrics from database for accurate display
                        std::string yesterday_metrics_query =
                            "SELECT daily_return, daily_unrealized_pnl, daily_realized_pnl, "
                            "daily_pnl, daily_transaction_costs, COALESCE(daily_roll_costs, 0) "
                            "FROM trading.live_results "
                            "WHERE strategy_id = '" +
                            combined_strategy_id + "' AND portfolio_id = '" +
                            coordinator_config.portfolio_id + "' AND date = '" +
                            yesterday_date_for_email +
                            "' "
                            "ORDER BY date DESC LIMIT 1";

                        INFO("Loading yesterday's daily metrics from live_results: " +
                             yesterday_metrics_query);
                        auto yesterday_metrics_result = db->execute_query(yesterday_metrics_query);

                        if (yesterday_metrics_result.is_ok() &&
                            yesterday_metrics_result.value()->num_rows() > 0) {
                            auto metrics_table = yesterday_metrics_result.value();
                            INFO("Retrieved " + std::to_string(metrics_table->num_rows()) +
                                 " rows from live_results");

                            auto daily_return_arr = std::static_pointer_cast<arrow::StringArray>(
                                metrics_table->column(0)->chunk(0));
                            auto daily_unrealized_arr =
                                std::static_pointer_cast<arrow::StringArray>(
                                    metrics_table->column(1)->chunk(0));
                            auto daily_realized_arr = std::static_pointer_cast<arrow::StringArray>(
                                metrics_table->column(2)->chunk(0));
                            auto daily_total_arr = std::static_pointer_cast<arrow::StringArray>(
                                metrics_table->column(3)->chunk(0));
                            auto daily_commissions_arr =
                                std::static_pointer_cast<arrow::StringArray>(
                                    metrics_table->column(4)->chunk(0));
                            auto daily_roll_costs_arr =  // migration 017, appended
                                std::static_pointer_cast<arrow::StringArray>(
                                    metrics_table->column(5)->chunk(0));

                            if (!daily_return_arr->IsNull(0)) {
                                yesterday_daily_metrics_final["Daily Return"] =
                                    std::stod(daily_return_arr->GetString(0));
                                INFO("Daily Return: " + daily_return_arr->GetString(0));
                            }
                            if (!daily_unrealized_arr->IsNull(0)) {
                                yesterday_daily_metrics_final["Daily Unrealized PnL"] =
                                    std::stod(daily_unrealized_arr->GetString(0));
                                INFO("Daily Unrealized PnL: " + daily_unrealized_arr->GetString(0));
                            }
                            if (!daily_realized_arr->IsNull(0)) {
                                yesterday_daily_metrics_final["Daily Realized PnL"] =
                                    std::stod(daily_realized_arr->GetString(0));
                                INFO("Daily Realized PnL: " + daily_realized_arr->GetString(0));
                            }
                            if (!daily_total_arr->IsNull(0)) {
                                yesterday_daily_metrics_final["Daily Total PnL"] =
                                    std::stod(daily_total_arr->GetString(0));
                                INFO("Daily Total PnL: " + daily_total_arr->GetString(0));
                            }

                            if (!daily_commissions_arr->IsNull(0)) {
                                yesterday_daily_metrics_final["Daily Transaction Costs"] =
                                    std::stod(daily_commissions_arr->GetString(0));
                                INFO("Daily Transaction Costs: " +
                                     daily_commissions_arr->GetString(0));
                            }
                            if (!daily_roll_costs_arr->IsNull(0)) {
                                yesterday_daily_metrics_final["Daily Roll Costs"] =
                                    std::stod(daily_roll_costs_arr->GetString(0));
                            }

                            INFO("Successfully loaded yesterday's daily metrics from live_results");
                        } else {
                            if (yesterday_metrics_result.is_error()) {
                                ERROR("Failed to query live_results: " +
                                      std::string(yesterday_metrics_result.error()->what()));
                            } else {
                                WARN("No rows found in live_results for date: " +
                                     yesterday_date_for_email);
                            }
                            // Fallback: calculate from positions if database query fails
                            double yesterday_daily_realized = 0.0;
                            for (const auto& [sleeve_symbol, pos] : yesterday_positions_finalized) {
                                yesterday_daily_realized += pos.realized_pnl.as_double();
                            }
                            yesterday_daily_metrics_final["Daily Realized PnL"] =
                                yesterday_daily_realized;
                            INFO(
                                "Calculated yesterday's metrics from positions (fallback) - Daily "
                                "Realized PnL: " +
                                std::to_string(yesterday_daily_realized));
                        }

                    } else {
                        INFO("No finalized positions found for yesterday's email table");
                    }

                    // Create strategy metrics map with all relevant metrics organized by category
                    std::map<std::string, double> strategy_metrics;

                    // Load today's stored metrics from live_results (including Sharpe, Sortino,
                    // etc.)
                    LiveResultsRow today_row;
                    bool has_today_row_metrics = false;
                    try {
                        auto today_row_result = data_loader->load_live_results(
                            combined_strategy_id, coordinator_config.portfolio_id, now);
                        if (today_row_result.is_ok()) {
                            today_row = today_row_result.value();
                            has_today_row_metrics = true;
                        } else {
                            WARN("Failed to load today's live_results row for email metrics: " +
                                 std::string(today_row_result.error()->what()));
                        }
                    } catch (const std::exception& e) {
                        WARN("Exception while loading today's live_results for email metrics: " +
                             std::string(e.what()));
                    }

                    // Performance Metrics
                    strategy_metrics["Daily Return"] = daily_return;
                    strategy_metrics["Daily Unrealized PnL"] = daily_unrealized_pnl;
                    strategy_metrics["Daily Realized PnL"] = daily_realized_pnl;
                    strategy_metrics["Daily Total PnL"] = daily_pnl;
                    strategy_metrics["Total Cumulative Return"] = total_cumulative_return_pct;
                    strategy_metrics["Total Annualized Return"] = total_return_annualized;
                    strategy_metrics["Total Unrealized PnL"] = total_unrealized_pnl;
                    strategy_metrics["Total Realized PnL"] = total_realized_pnl;
                    strategy_metrics["Total PnL"] = total_pnl;

                    if (has_today_row_metrics) {
                        // Risk-adjusted and distribution metrics from live_results
                        strategy_metrics["Sharpe Ratio"] = today_row.sharpe_ratio;
                        strategy_metrics["Sortino Ratio"] = today_row.sortino_ratio;
                        strategy_metrics["Max Drawdown"] = today_row.max_drawdown;
                        strategy_metrics["Volatility"] = today_row.volatility;
                        strategy_metrics["Win Rate"] = today_row.win_rate;
                        strategy_metrics["Average Win"] = today_row.avg_win;
                        strategy_metrics["Average Loss"] = today_row.avg_loss;
                        strategy_metrics["Profit Factor"] = today_row.profit_factor;
                        strategy_metrics["Best Day"] = today_row.best_day;
                        strategy_metrics["Worst Day"] = today_row.worst_day;
                        strategy_metrics["Downside Deviation"] = today_row.downside_deviation;
                        strategy_metrics["Gross Profit"] = today_row.gross_profit;
                        strategy_metrics["Gross Loss"] = today_row.gross_loss;
                        // Removed Total Trades - will be implemented properly later with closing trades logic
                        strategy_metrics["Winning Days"] =
                            static_cast<double>(today_row.winning_days);
                        strategy_metrics["Losing Days"] =
                            static_cast<double>(today_row.losing_days);
                        // Flat Days = total - winning - losing (Sat/Sun/holidays w/ zero PnL).
                        // Shown explicitly in email so the total math adds up cleanly.
                        strategy_metrics["Flat Days"] = static_cast<double>(
                            std::max(0, today_row.total_days - today_row.winning_days -
                                            today_row.losing_days));
                        strategy_metrics["Total Days"] = static_cast<double>(today_row.total_days);
                    }
                    // Portfolio VaR is always sourced from the live risk evaluation,
                    // separate from historical volatility stored in live_results
                    if (risk_eval.is_ok()) {
                        strategy_metrics["Portfolio VaR"] = risk_eval.value().portfolio_var * 100.0;
                    }
                    strategy_metrics["Total Transaction Costs"] =
                        total_transaction_costs_cumulative;
                    strategy_metrics["Total Roll Costs"] = total_roll_costs_cumulative;  // 017
                    strategy_metrics["Daily Roll Costs"] = total_daily_roll_costs;
                    strategy_metrics["Current Portfolio Value"] = current_portfolio_value;

                    // Leverage Metrics - Calculate values from position analysis
                    double gross_leverage_calc = (current_portfolio_value != 0.0)
                                                     ? (gross_notional / current_portfolio_value)
                                                     : 0.0;
                    double net_leverage_calc = (current_portfolio_value != 0.0)
                                                   ? (net_notional / current_portfolio_value)
                                                   : 0.0;

                    strategy_metrics["Gross Leverage"] = gross_leverage_calc;
                    strategy_metrics["Net Leverage"] = net_leverage_calc;
                    strategy_metrics["Equity-to-Margin Ratio"] = equity_to_margin_ratio;

                    // Risk & Liquidity Metrics
                    strategy_metrics["Margin Cushion"] =
                        margin_cushion * 100.0;  // Convert to percentage
                    strategy_metrics["Margin Posted"] = total_posted_margin;
                    strategy_metrics["Cash Available"] =
                        current_portfolio_value - total_posted_margin;

                    // Note: yesterday_daily_metrics_final is now loaded AFTER database updates
                    // above So we don't need to create it here anymore

                    // Generate email body with is_daily_strategy flag set to true and current
                    // prices. Pass strategy_positions_map and all_strategy_executions for
                    // per-strategy tables.
                    std::string email_body = email_sender->generate_trading_report_body(
                        strategy_positions_map,  // Per-strategy positions for grouped tables
                        positions,
                        risk_eval.is_ok() ? std::make_optional(risk_eval.value()) : std::nullopt,
                        strategy_metrics, all_strategy_executions, date_str,
                        portfolio_id,                 // Portfolio name for email header
                        true,                         // is_daily_strategy
                        previous_day_close_prices,    // Pass Day T-1 close prices for today's
                                                      // positions
                        db,                           // Pass database for symbols reference table
                        previous_strategy_positions,  // Per-strategy yesterday's positions for
                                                      // grouped tables
                        yesterday_exit_prices,   // Day T-1 close prices for yesterday's positions
                        yesterday_entry_prices,  // Day T-2 close prices for yesterday's positions
                        yesterday_daily_metrics_final  // Yesterday's metrics
                    );

                    // Send email with CSV attachments: today's positions and yesterday's finalized
                    // (if available)
                    std::vector<std::string> attachments = {today_filename};
                    if (!yesterday_filename.empty()) {
                        attachments.push_back(yesterday_filename);
                    }

                    // T-7a C5: a day the portfolio risk module could not answer is flagged in
                    // the subject and at the top of the body.
                    if (risk_module_failure) {
                        subject = risk_module_failure_email_subject(subject);
                        email_body = flag_email_body_for_risk_module_failure(
                            email_body, *risk_module_failure);
                    }
                    // T-7b-1 C7b R2: a carried day's email opens with the reason its positions
                    // file carries (a feed-hole carry read as a normal day with 0 executions).
                    if (skip_strategy_processing) {
                        email_body = flag_email_body_for_carried_day(email_body, carried_day_note);
                    }

                    auto send_result =
                        email_sender->send_email(subject, email_body, true, attachments);
                    if (send_result.is_error()) {
                        ERROR("Failed to send email: " + std::string(send_result.error()->what()));
                    } else {
                        std::string attachment_list = today_filename;
                        if (!yesterday_filename.empty()) {
                            attachment_list += ", " + yesterday_filename;
                        }
                        INFO("Email report sent successfully with CSV attachments: " +
                             attachment_list);
                    }
                }
            } catch (const std::exception& e) {
                ERROR("Exception during email sending: " + std::string(e.what()));
            }
        } else {
            INFO("Email reporting disabled");
        }

        std::cerr << "At end of main: initialized=" << Logger::instance().is_initialized()
                  << std::endl;

        if (risk_module_failure) {
            ERROR("RISK_MODULE_FAILURE the day is stored with the book held; exiting " +
                  std::to_string(kRiskModuleFailureExitCode));
        }
        return live_run_exit_code(risk_module_failure);

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
