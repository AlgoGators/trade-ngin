// benchmark_replay -- ADR-005 §5.3: the deferred benchmark computation.
//
// Replays the untouched-counterfactual ("benchmark") portfolio day-by-day
// from recorded trading.run_inputs rows, instead of the live path computing
// it every day (PR #55's benchmark.mode = "live" second strategy pass).
// See adr/ADR-005-dual-portfolio-attribution.md §5, §6, §7.
//
// Usage:
//   benchmark_replay --portfolio <config_name> [--through YYYY-MM-DD]
//                     [--engine frozen|current]
//
// --engine frozen (default, ADR-005 §6.3): replay each day with the exact
//   build that produced it (run_inputs.trade_ngin_sha) -- "what would the
//   algorithm as it then existed have done without QT?" This is the mode
//   the §7 parity gate requires. Resolving a historical SHA to a real
//   checked-out/built binary needs ADR-005 DEC-5.1 (an image-digest
//   retention policy), which is not yet decided. Rather than silently
//   substitute today's binary -- which would let this mode's output claim
//   a determinism it does not have -- frozen mode refuses immediately with
//   a clear error. Implement DEC-5.1 first, then this refusal becomes the
//   one line that changes.
// --engine current (ADR-005 §6.3, opt-in): replay every day with today's
//   binary -- "what would today's algorithm have done over history?", a
//   research re-benchmark, not attribution. Writes to the distinct
//   'benchmark_rebench' stream (migration 006) so it is never mixed with
//   the 'benchmark' stream frozen/live modes produce.
//
// Construction of the strategy set itself (build_strategy_instances,
// select_enabled_live_strategies) is shared with live_portfolio_runner.cpp
// via trade_ngin/apps/live_portfolio_helpers.hpp specifically so the §7
// parity gate compares positions produced by identical code on both sides.

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "trade_ngin/apps/live_portfolio_helpers.hpp"
#include "trade_ngin/core/config_loader.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/data/conversion_utils.hpp"
#include "trade_ngin/data/database_pooling.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/git_version.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"

using namespace trade_ngin;

namespace {

// Current-mode ("re-benchmark") counterfactual stream (new, migration 006) --
// kept distinct so a research re-run is never mixed into the attribution
// stream (ADR-005 §6.3). Frozen mode would write to "benchmark" (the
// existing live-mode stream) once DEC-5.1 unblocks it -- not used today
// since frozen mode refuses before reaching any stream-writing code.
constexpr const char* kRebenchStream = "benchmark_rebench";

struct CliArgs {
    std::string portfolio;               // config_name, e.g. "base" -- matches
                                          // ConfigLoader::load's portfolio_name.
    std::string through_date;            // YYYY-MM-DD, inclusive; default: today (UTC).
    std::string engine_mode = "frozen";  // "frozen" | "current" (ADR-005 §6.3).
};

std::string format_date(const std::tm& tm_val) {
    std::ostringstream oss;
    oss << std::put_time(&tm_val, "%Y-%m-%d");
    return oss.str();
}

std::string today_utc_date_string() {
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm_utc{};
    core::safe_gmtime(&tt, &tm_utc);
    return format_date(tm_utc);
}

// Parses "YYYY-MM-DD" into a Timestamp at 00:00:00 UTC. Returns false on a
// malformed string.
bool parse_date(const std::string& s, Timestamp* out) {
    std::tm tm_val{};
    std::istringstream iss(s);
    iss >> std::get_time(&tm_val, "%Y-%m-%d");
    if (iss.fail()) {
        return false;
    }
    tm_val.tm_hour = 0;
    tm_val.tm_min = 0;
    tm_val.tm_sec = 0;
#if defined(_WIN32)
    std::time_t tt = _mkgmtime(&tm_val);
#else
    std::time_t tt = timegm(&tm_val);
#endif
    if (tt == static_cast<std::time_t>(-1)) {
        return false;
    }
    *out = std::chrono::system_clock::from_time_t(tt);
    return true;
}

bool parse_args(int argc, char* argv[], CliArgs* args, std::string* error) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--portfolio" && i + 1 < argc) {
            args->portfolio = argv[++i];
        } else if (arg == "--through" && i + 1 < argc) {
            args->through_date = argv[++i];
        } else if (arg == "--engine" && i + 1 < argc) {
            args->engine_mode = argv[++i];
        } else {
            *error = "Unrecognized argument: " + arg;
            return false;
        }
    }
    if (args->portfolio.empty()) {
        *error = "--portfolio is required";
        return false;
    }
    if (args->engine_mode != "frozen" && args->engine_mode != "current") {
        *error = "--engine must be 'frozen' or 'current', got: " + args->engine_mode;
        return false;
    }
    if (args->through_date.empty()) {
        args->through_date = today_utc_date_string();
    }
    return true;
}

// Reads one text/JSONB column from an execute_query() result as a string.
// execute_query returns StringArray for every column regardless of the
// underlying SQL type (confirmed against existing call sites in
// live_portfolio_runner.cpp), so this is the one place that needs the cast.
std::optional<std::string> column_as_string(const std::shared_ptr<arrow::Table>& table,
                                            int64_t row, int column) {
    auto arr = std::static_pointer_cast<arrow::StringArray>(table->column(column)->chunk(0));
    if (!arr || row >= arr->length() || arr->IsNull(row)) {
        return std::nullopt;
    }
    return arr->GetString(row);
}

// One trading.run_inputs row, parsed.
struct RunInputsRow {
    std::string date;             // YYYY-MM-DD
    std::string trade_ngin_sha;
    nlohmann::json config_snapshot;
    std::vector<std::string> universe;
    nlohmann::json data_window;
};

// Reconstructs the [start, end) market-data window a live run would have
// used for `date`, from config_snapshot.live.historical_days -- mirrors
// live_portfolio_runner.cpp's own start_date/end_date formula for a
// historical (non-live) run. Used as a fallback when data_window.start/end
// are the pre-fix placeholder zeros an older run_inputs row may still carry
// (see build_run_inputs_row's start_date/end_date parameters).
void data_window_from_historical_days(const Timestamp& date, const nlohmann::json& config_snapshot,
                                      Timestamp* start_out, Timestamp* end_out) {
    int historical_days = 300;
    if (config_snapshot.contains("live") && config_snapshot["live"].contains("historical_days")) {
        historical_days = config_snapshot["live"]["historical_days"].get<int>();
    }
    *start_out = date - std::chrono::hours(24 * historical_days);
    *end_out = date - std::chrono::hours(24);
}

}  // namespace

int main(int argc, char* argv[]) {
    CliArgs args;
    std::string parse_error;
    if (!parse_args(argc, argv, &args, &parse_error)) {
        std::cerr << "Error: " << parse_error << std::endl;
        std::cerr << "Usage: " << argv[0]
                  << " --portfolio <config_name> [--through YYYY-MM-DD] "
                     "[--engine frozen|current]"
                  << std::endl;
        return 2;
    }

    // ADR-005 §6.3 / DEC-5.1: frozen mode needs to resolve a historical
    // trade_ngin_sha to a real checked-out/built binary. The image-digest
    // retention policy that would make that possible has not been decided.
    // Refusing here (rather than silently running today's binary against a
    // day it did not produce) keeps every future 'benchmark' stream row
    // honest about which build actually computed it.
    if (args.engine_mode == "frozen") {
        std::cerr
            << "benchmark_replay: --engine frozen is not yet implemented.\n"
            << "ADR-005 §6.3 requires resolving each day's recorded trade_ngin_sha to a\n"
            << "real checked-out/built binary; the image-digest retention policy that would\n"
            << "make that possible (ADR-005 DEC-5.1) has not been decided yet.\n"
            << "Refusing rather than silently replaying with today's binary, which would\n"
            << "make the result claim a determinism it does not have.\n"
            << "Use --engine current for a research re-benchmark with today's build\n"
            << "(writes to the 'benchmark_rebench' stream, never mixed with 'benchmark').\n";
        return 3;
    }

    try {
        auto& logger = Logger::instance();
        LoggerConfig logger_config;
        logger_config.min_level = LogLevel::INFO;
        logger_config.destination = LogDestination::BOTH;
        logger_config.log_directory = "logs";
        logger_config.filename_prefix = "benchmark_replay";
        logger.initialize(logger_config);

        INFO("benchmark_replay starting: portfolio=" + args.portfolio +
             " through=" + args.through_date + " engine=" + args.engine_mode);

        Timestamp through_date;
        if (!parse_date(args.through_date, &through_date)) {
            std::cerr << "Error: invalid --through date: " << args.through_date << std::endl;
            return 2;
        }

        // Config is used only for DB connection info, the portfolio_id this
        // portfolio maps to, and the registry -- NOT for strategy behavior.
        // Each replayed day reconstructs its own strategy selection and
        // config from that day's own run_inputs.config_snapshot, never from
        // today's config files (that is the entire point of recording it).
        auto app_config_result = ConfigLoader::load("./config", args.portfolio);
        if (app_config_result.is_error()) {
            ERROR("Failed to load configuration: " +
                  std::string(app_config_result.error()->what()));
            return 1;
        }
        auto app_config = app_config_result.value();
        std::string portfolio_id = app_config.portfolio_id;
        INFO("Resolved portfolio_id: " + portfolio_id);

        std::string conn_string = app_config.database.get_connection_string();
        auto pool_result = DatabasePool::instance().initialize(
            conn_string, app_config.database.num_connections);
        if (pool_result.is_error()) {
            std::cerr << "Failed to initialize connection pool: " << pool_result.error()->what()
                      << std::endl;
            return 1;
        }
        auto db_guard = DatabasePool::instance().acquire_connection();
        auto db = db_guard.get();
        if (!db || !db->is_connected()) {
            std::cerr << "Failed to acquire database connection from pool" << std::endl;
            return 1;
        }

        auto& registry = InstrumentRegistry::instance();
        auto registry_init_result = registry.initialize(db);
        if (registry_init_result.is_error()) {
            std::cerr << "Failed to initialize instrument registry: "
                      << registry_init_result.error()->what() << std::endl;
            return 1;
        }
        auto load_result = registry.load_instruments();
        if (load_result.is_error() || registry.get_all_instruments().empty()) {
            std::cerr << "Failed to load futures instruments: "
                      << (load_result.is_error() ? load_result.error()->what()
                                                  : "registry is empty")
                      << std::endl;
            return 1;
        }
        auto registry_ptr = std::shared_ptr<InstrumentRegistry>(&registry, [](InstrumentRegistry*) {});

        const std::string stream = kRebenchStream;

        // ADR-005 §5.3 step 1: find the last checkpointed date for this
        // portfolio+stream, or the earliest recorded run_inputs date if this
        // is the first replay (there is no fixed "migration 002 boundary"
        // constant -- migration 002 backfilled whatever history existed at
        // the time it ran, not a specific date -- so "earliest run_inputs
        // row" is the correct, self-describing start: that is the earliest
        // day this replay CAN reconstruct).
        std::optional<std::string> checkpoint_date;
        {
            std::ostringstream q;
            q << "SELECT MAX(date) FROM trading.positions WHERE portfolio_id = '" << portfolio_id
              << "' AND portfolio_type = '" << stream << "'";
            auto result = db->execute_query(q.str());
            if (result.is_error()) {
                ERROR("Checkpoint query failed: " + std::string(result.error()->what()));
                return 1;
            }
            auto table = result.value();
            if (table && table->num_rows() > 0) {
                checkpoint_date = column_as_string(table, 0, 0);
            }
        }

        std::string range_start_clause;
        if (checkpoint_date.has_value()) {
            INFO("Resuming from checkpoint: " + *checkpoint_date + " (exclusive)");
            range_start_clause = "date > '" + *checkpoint_date + "'";
        } else {
            std::ostringstream q;
            q << "SELECT MIN(date) FROM trading.run_inputs WHERE portfolio_id = '" << portfolio_id
              << "'";
            auto result = db->execute_query(q.str());
            if (result.is_error()) {
                ERROR("Backfill-start query failed: " + std::string(result.error()->what()));
                return 1;
            }
            auto table = result.value();
            std::optional<std::string> earliest;
            if (table && table->num_rows() > 0) {
                earliest = column_as_string(table, 0, 0);
            }
            if (!earliest.has_value()) {
                INFO("No trading.run_inputs rows found for portfolio_id=" + portfolio_id +
                     " -- nothing to replay.");
                return 0;
            }
            INFO("No checkpoint found; starting from earliest run_inputs date: " + *earliest);
            range_start_clause = "date >= '" + *earliest + "'";
        }

        std::vector<RunInputsRow> rows;
        {
            std::ostringstream q;
            q << "SELECT date, trade_ngin_sha, config_snapshot, universe, data_window "
                 "FROM trading.run_inputs WHERE portfolio_id = '"
              << portfolio_id << "' AND " << range_start_clause << " AND date <= '"
              << args.through_date << "' ORDER BY date ASC";
            auto result = db->execute_query(q.str());
            if (result.is_error()) {
                ERROR("run_inputs query failed: " + std::string(result.error()->what()));
                return 1;
            }
            auto table = result.value();
            int64_t n = table ? table->num_rows() : 0;
            for (int64_t i = 0; i < n; ++i) {
                RunInputsRow row;
                row.date = column_as_string(table, i, 0).value_or("");
                row.trade_ngin_sha = column_as_string(table, i, 1).value_or("");
                auto cfg_str = column_as_string(table, i, 2);
                auto universe_str = column_as_string(table, i, 3);
                auto window_str = column_as_string(table, i, 4);
                if (row.date.empty() || !cfg_str.has_value() || !universe_str.has_value() ||
                    !window_str.has_value()) {
                    WARN("Skipping malformed run_inputs row at index " + std::to_string(i));
                    continue;
                }
                row.config_snapshot = nlohmann::json::parse(*cfg_str);
                row.universe = nlohmann::json::parse(*universe_str)
                                   .get<std::vector<std::string>>();
                row.data_window = nlohmann::json::parse(*window_str);
                rows.push_back(std::move(row));
            }
        }

        if (rows.empty()) {
            INFO("No run_inputs rows to replay in the requested range.");
            return 0;
        }
        INFO("Replaying " + std::to_string(rows.size()) + " day(s), " + rows.front().date +
             " through " + rows.back().date);

        std::string from_date_for_record = rows.front().date;

        int days_computed = 0;
        bool data_restated = false;
        std::string data_restated_date;

        for (size_t idx = 0; idx < rows.size(); ++idx) {
            const auto& row = rows[idx];
            Timestamp row_date;
            if (!parse_date(row.date, &row_date)) {
                WARN("Skipping run_inputs row with unparseable date: " + row.date);
                continue;
            }

            // ADR-005 §5.3 step 2a: --engine current always uses today's build.
            const std::string engine_sha_used = TRADE_NGIN_GIT_SHA;

            // Step 2d inputs: reconstruct the exact selection and config the
            // live run made that day, from its recorded config_snapshot --
            // never from today's config files.
            nlohmann::json strategies_cfg = row.config_snapshot.value(
                "strategies", nlohmann::json::object());
            auto selection_result = select_enabled_live_strategies(strategies_cfg);
            if (selection_result.is_error()) {
                ERROR("Day " + row.date + ": failed to select strategies: " +
                      std::string(selection_result.error()->what()) + " -- aborting replay.");
                break;
            }
            auto selection = selection_result.value();
            std::string combined_strategy_id = build_combined_strategy_id(selection.names);

            // Step 2b: recompute content_hash over freshly-loaded bars for
            // the SAME universe and window the live run recorded, and refuse
            // to proceed on a mismatch (a futures back-adjustment restated
            // history since this day was recorded).
            Timestamp window_start, window_end;
            long long recorded_start = row.data_window.value("start", 0LL);
            long long recorded_end = row.data_window.value("end", 0LL);
            if (recorded_start > 0 && recorded_end > 0) {
                window_start = std::chrono::system_clock::from_time_t(recorded_start);
                window_end = std::chrono::system_clock::from_time_t(recorded_end);
            } else {
                // Placeholder-zero row predating the start/end fix -- fall back to
                // reconstructing the same window live_portfolio_runner.cpp would
                // have used that day.
                data_window_from_historical_days(row_date, row.config_snapshot, &window_start,
                                                 &window_end);
            }

            auto market_data_result = db->get_market_data(row.universe, window_start, window_end,
                                                           AssetClass::FUTURES,
                                                           DataFrequency::DAILY, "ohlcv");
            if (market_data_result.is_error()) {
                ERROR("Day " + row.date + ": failed to load market data: " +
                      std::string(market_data_result.error()->what()) + " -- aborting replay.");
                break;
            }
            auto bars_result = DataConversionUtils::arrow_table_to_bars(market_data_result.value());
            if (bars_result.is_error()) {
                ERROR("Day " + row.date + ": failed to convert market data to bars: " +
                      std::string(bars_result.error()->what()) + " -- aborting replay.");
                break;
            }
            auto all_bars = bars_result.value();

            std::string recomputed_hash = hash_bars(all_bars);
            std::string recorded_hash = row.data_window.value("content_hash", std::string());
            if (!recorded_hash.empty() && recomputed_hash != recorded_hash) {
                ERROR("DATA_RESTATED on " + row.date +
                      ": recomputed content_hash does not match the hash recorded in "
                      "run_inputs (recorded=" +
                      recorded_hash + " recomputed=" + recomputed_hash +
                      "). Historical prices changed since this day was recorded (e.g. a "
                      "futures back-adjustment). Stopping replay -- not proceeding on data "
                      "that no longer matches what the live run saw.");
                data_restated = true;
                data_restated_date = row.date;
                break;
            }

            auto latest_bars = latest_bar_by_symbol(all_bars);

            // Step 2d: rebuild the strategy set from the recorded config,
            // via the same construction code the live path uses.
            StrategyConfig base_strategy_config;
            base_strategy_config.asset_classes = {AssetClass::FUTURES};
            base_strategy_config.frequencies = {DataFrequency::DAILY};
            base_strategy_config.max_drawdown = row.config_snapshot.value("max_drawdown", 0.4);
            base_strategy_config.max_leverage = row.config_snapshot.value("max_leverage", 4.0);
            double position_limit_live = 500.0;
            if (row.config_snapshot.contains("execution")) {
                position_limit_live =
                    row.config_snapshot["execution"].value("position_limit_live", 500.0);
            }
            for (const auto& symbol : row.universe) {
                base_strategy_config.position_limits[symbol] = position_limit_live;
            }

            double initial_capital = row.config_snapshot.value("initial_capital", 500000.0);

            StrategyDefaultsConfig strategy_defaults;
            if (row.config_snapshot.contains("strategy_defaults")) {
                strategy_defaults.from_json(row.config_snapshot["strategy_defaults"]);
            }

            std::vector<std::shared_ptr<StrategyInterface>> strategies;
            try {
                // slow_max_symbol_concentration_override: the per-binary override
                // (base=0.15, conservative=default) is a LivePortfolioConfig value,
                // never part of AppConfig/config_snapshot, so it cannot be recovered
                // here. Only affects TrendFollowingSlowStrategy's hardcoded-defaults
                // fallback branch (no explicit "config" object) -- not exercised by
                // either tracked portfolio today (both use TrendFollowingStrategy /
                // *Fast with explicit config). Documented gap, not a silent guess.
                strategies = build_strategy_instances(selection, base_strategy_config,
                                                      initial_capital, strategy_defaults,
                                                      std::nullopt, db, registry_ptr);
            } catch (const std::exception& e) {
                ERROR("Day " + row.date + ": failed to build strategies: " + std::string(e.what()) +
                      " -- aborting replay.");
                break;
            }

            PortfolioConfig portfolio_config;
            portfolio_config.total_capital = Decimal(initial_capital);
            double reserve_capital_pct = row.config_snapshot.value("reserve_capital_pct", 0.10);
            portfolio_config.reserve_capital = Decimal(initial_capital * reserve_capital_pct);
            portfolio_config.max_strategy_allocation = strategy_defaults.max_strategy_allocation;
            portfolio_config.min_strategy_allocation = strategy_defaults.min_strategy_allocation;
            portfolio_config.use_optimization = strategy_defaults.use_optimization;
            portfolio_config.use_risk_management = strategy_defaults.use_risk_management;
            portfolio_config.benchmark_mode = "deferred";
            if (row.config_snapshot.contains("optimization")) {
                portfolio_config.opt_config.from_json(row.config_snapshot["optimization"]);
            }
            portfolio_config.opt_config.capital = initial_capital;  // mirrors the live path's override
            if (row.config_snapshot.contains("risk")) {
                portfolio_config.risk_config.from_json(row.config_snapshot["risk"]);
            }
            portfolio_config.risk_config.capital = Decimal(initial_capital);  // ditto

            auto replay_portfolio = std::make_shared<PortfolioManager>(
                portfolio_config, "PORTFOLIO_MANAGER_BENCHMARK_REPLAY", registry_ptr);

            for (size_t i = 0; i < strategies.size(); ++i) {
                const std::string& strat_name = selection.names[i];
                auto add_result = replay_portfolio->add_strategy(
                    strategies[i], selection.allocations.at(strat_name),
                    portfolio_config.use_optimization, portfolio_config.use_risk_management);
                if (add_result.is_error()) {
                    WARN("Day " + row.date + ": failed to add strategy " + strat_name + ": " +
                         std::string(add_result.error()->what()));
                }
            }

            // Step 2c: seed from THIS stream's own prior positions (never
            // qt/system) -- the counterfactual compounds only on itself.
            Timestamp last_seed_date;
            if (idx == 0 && checkpoint_date.has_value()) {
                parse_date(*checkpoint_date, &last_seed_date);
            } else if (idx > 0) {
                parse_date(rows[idx - 1].date, &last_seed_date);
            } else {
                // First row, no checkpoint: nothing to seed from -- starts flat,
                // same as the live path's first run after migration 003.
                last_seed_date = row_date - std::chrono::hours(24);
            }
            {
                std::string seed_name = selection.names.empty() ? std::string() : selection.names[0];
                auto seed_result = db->load_positions_by_date(combined_strategy_id, seed_name,
                                                               portfolio_id, last_seed_date,
                                                               "trading.positions", stream);
                if (seed_result.is_ok() && !seed_result.value().empty() && !strategies.empty()) {
                    auto seeded = strategies[0]->seed_positions(seed_result.value());
                    if (seeded.is_error()) {
                        WARN("Day " + row.date + ": failed to seed strategy positions: " +
                             std::string(seeded.error()->what()));
                    }
                    for (const auto& [sym, pos] : seed_result.value()) {
                        replay_portfolio->update_strategy_position(seed_name, sym, pos);
                    }
                }
            }

            auto process_result = replay_portfolio->process_market_data(all_bars);
            if (process_result.is_error()) {
                ERROR("Day " + row.date + ": process_market_data failed: " +
                      std::string(process_result.error()->what()) + " -- aborting replay.");
                break;
            }

            auto positions_map = replay_portfolio->get_strategy_positions();
            int stored = 0;
            for (const auto& [strat_name, pos_map] : positions_map) {
                std::vector<Position> non_zero;
                non_zero.reserve(pos_map.size());
                for (const auto& [sym, pos] : pos_map) {
                    if (pos.quantity.as_double() != 0.0) {
                        non_zero.push_back(pos);
                    }
                }
                if (non_zero.empty()) {
                    continue;
                }
                auto save_result = db->store_positions(non_zero, combined_strategy_id, strat_name,
                                                        portfolio_id, "trading.positions", stream);
                if (save_result.is_error()) {
                    WARN("Day " + row.date + ": failed to store positions for " + strat_name +
                         ": " + std::string(save_result.error()->what()));
                } else {
                    stored += static_cast<int>(non_zero.size());
                }
            }

            double equity = 0.0;
            for (const auto& [strat_name, pos_map] : positions_map) {
                equity += compute_mark_to_market_equity(pos_map, latest_bars);
            }
            auto equity_result = db->store_trading_equity_curve(
                combined_strategy_id, row_date, equity, portfolio_id, "trading.equity_curve",
                stream);
            if (equity_result.is_error()) {
                WARN("Day " + row.date + ": failed to store equity curve: " +
                     std::string(equity_result.error()->what()));
            }

            INFO("Day " + row.date + ": replayed OK -- " + std::to_string(stored) +
                 " position(s), equity=$" + std::to_string(equity));
            days_computed++;
        }

        std::string through_date_for_record =
            days_computed > 0 ? rows[days_computed - 1].date : from_date_for_record;
        std::string status = data_restated ? "failed" : "completed";

        std::ostringstream insert_sql;
        insert_sql << "INSERT INTO trading.benchmark_replays "
                      "(portfolio_id, from_date, through_date, engine_sha_used, engine_mode, "
                      "days_computed, status, finished_at) VALUES ('"
                   << portfolio_id << "', '" << from_date_for_record << "', '"
                   << through_date_for_record << "', '" << TRADE_NGIN_GIT_SHA << "', 'current', "
                   << days_computed << ", '" << status << "', now())";
        auto record_result = db->execute_query(insert_sql.str());
        if (record_result.is_error()) {
            WARN("Failed to record benchmark_replays row: " +
                 std::string(record_result.error()->what()));
        }

        if (data_restated) {
            std::cerr << "DATA_RESTATED on " << data_restated_date
                      << " -- replay stopped. " << days_computed
                      << " day(s) computed and stored before the mismatch." << std::endl;
            return 4;
        }

        INFO("benchmark_replay complete: " + std::to_string(days_computed) + " day(s) computed.");
        std::cout << "Replayed " << days_computed << " day(s) for portfolio " << portfolio_id
                  << " (" << from_date_for_record << " through " << through_date_for_record << ")"
                  << std::endl;
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "benchmark_replay failed: " << e.what() << std::endl;
        return 1;
    }
}
