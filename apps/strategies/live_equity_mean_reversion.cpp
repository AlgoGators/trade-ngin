#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <chrono>
#include <ctime>
#include <set>
#include <algorithm>
#include <nlohmann/json.hpp>
#include "trade_ngin/core/config_loader.hpp"
#include "trade_ngin/core/holiday_checker.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/data/database_pooling.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/data/conversion_utils.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/instruments/equity.hpp"
#include "trade_ngin/live/corporate_actions_applier.hpp"
#include "trade_ngin/live/corporate_actions_classification.hpp"
#include "trade_ngin/live/corporate_actions_lifecycle.hpp"
#include "trade_ngin/live/corp_action_window.hpp"
#include "trade_ngin/live/data_freshness.hpp"
#include "trade_ngin/live/corp_action_feed_status.hpp"
#include "trade_ngin/live/trading_days_anchor.hpp"
#include "trade_ngin/live/broker_frame.hpp"
#include "trade_ngin/live/corporate_actions_audit_log.hpp"
#include "trade_ngin/live/live_daily_cycle.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/strategy/mean_reversion.hpp"
#include "trade_ngin/strategy/equity_strategy_builder.hpp"
#include "trade_ngin/core/email_sender.hpp"
#include "trade_ngin/storage/live_results_manager.hpp"
#include "trade_ngin/live/live_data_loader.hpp"
#include "trade_ngin/live/live_metrics_calculator.hpp"
#include "trade_ngin/live/live_historical_metrics.hpp"
#include "trade_ngin/live/live_trading_coordinator.hpp"
#include "trade_ngin/live/live_price_manager.hpp"
#include "trade_ngin/live/execution_price_resolver.hpp"
#include "trade_ngin/live/live_pnl_manager.hpp"
#include "trade_ngin/live/execution_manager.hpp"
#include "trade_ngin/live/margin_manager.hpp"
#include "trade_ngin/live/csv_exporter.hpp"

using namespace trade_ngin;

// Storage identity for this runner. The read path
// (load_positions_by_date / corp-action queries) and the write path
// (LiveResultsManager -> ResultsManagerBase) must agree on all three key columns:
// (strategy_id, strategy_name, portfolio_id). They did not before FIX-0 -- the
// coordinator defaulted portfolio_id to the futures book's BASE_PORTFOLIO and
// ResultsManagerBase substituted strategy_id for strategy_name -- so writes landed
// under a key no read would ever match. Named here so the two paths cannot drift again.
static constexpr const char* kEquityStrategyId = "LIVE_EQUITY_MEAN_REVERSION";
static constexpr const char* kEquityStrategyName = "EQUITY_MEAN_REVERSION";

// Resolve the on-disk dedup state directory for the live equity app.
// Honors TRADE_NGIN_STATE_DIR (treated as the parent directory) when set;
// otherwise roots an absolute path at the current working directory. Always
// returns an absolute path so cron-triggered runs from a different CWD cannot
// silently relocate state and reset dedup (ultrareview bug_013). The directory
// itself is created on first save() by CorporateActionsAuditLog.
static std::string resolve_corp_actions_state_dir(const std::string& strategy_id) {
    namespace fs = std::filesystem;
    if (const char* env = std::getenv("TRADE_NGIN_STATE_DIR")) {
        return (fs::path(env) / strategy_id).string();
    }
    return (fs::current_path() / "state" / strategy_id).string();
}

// Phase 6 §6c: removed unused format_sql_date helper (Phase 5 introduced
// trade_ngin::core::format_utc_date as the only approved primitive for
// UTC date-string keys).

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

            // Try to parse as date. UTC midnight, not local midnight: every
            // consumer of target_date formats it back through gmtime, so a
            // std::mktime parse moves the entire run to the previous day on any
            // host at a positive UTC offset (E3 mktime sites).
            std::chrono::system_clock::time_point parsed_date;
            if (core::parse_utc_date(arg, parsed_date)) {
                target_date = parsed_date;
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
        logger_config.filename_prefix = "live_equity_mr";
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
        INFO("Loading configuration from config/portfolios/equity_mr...");
        auto app_config_result = ConfigLoader::load("./config", "equity_mr");
        if (app_config_result.is_error()) {
            ERROR("Failed to load equity_mr configuration: " +
                  std::string(app_config_result.error()->what()));
            return 1;
        }
        auto app_config = app_config_result.value();
        INFO("Configuration loaded successfully for portfolio: " + app_config.portfolio_id);

        // Storage identity comes from config, never a literal: this runner writes to
        // its own portfolio namespace (EQUITY_MR_PORTFOLIO), not the futures book's.
        const std::string portfolio_id = app_config.portfolio_id;

        // Wire the shared HolidayChecker into EquityInstrument's static slot so
        // that EquityInstrument::is_market_open() actually consults the calendar.
        // Same checker is reused below for the previous-trading-day lookup.
        // Phase 6 §6a: path resolved via HolidayChecker::resolve_holidays_path
        // (honors TRADE_NGIN_HOLIDAYS_JSON env var; falls back through the
        // dev/deploy/system-wide chain).
        auto holiday_checker_ptr = std::make_shared<HolidayChecker>(
            HolidayChecker::resolve_holidays_path());
        // BA-1: a calendar that did not load fully is fatal, not an ERROR log.
        // Every downstream use -- the non-trading-day skip, the previous-trading-
        // day walk, EquityInstrument::is_market_open -- reads "not a holiday"
        // and "never loaded" as the same value, so proceeding books a day that
        // did not exist against a T-1 book that was never there.
        if (!holiday_checker_ptr->loaded()) {
            std::cerr << "FATAL: market holiday calendar failed to load from "
                      << HolidayChecker::resolve_holidays_path()
                      << " - refusing to run. Every date would report as a trading day."
                      << std::endl;
            return 1;
        }
        EquityInstrument::set_holiday_checker(holiday_checker_ptr);

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

        // Load futures instruments (if any exist in DB metadata)
        auto load_result = registry.load_instruments();
        if (load_result.is_error()) {
            WARN("Could not load instruments from DB (may not have futures metadata): " +
                 std::string(load_result.error()->what()));
        }

        // Resolve the corp-actions state directory once (absolute path) so all
        // three call sites (daily corp-action apply, metrics build, email body)
        // see the same on-disk store regardless of CWD.
        const std::string ca_state_dir =
            resolve_corp_actions_state_dir("LIVE_EQUITY_MEAN_REVERSION");
        INFO("Corp-actions state directory: " + ca_state_dir);

        // Get current date for daily processing (or use override date)
        auto now = use_override_date ? target_date : std::chrono::system_clock::now();
        auto now_time_t = std::chrono::system_clock::to_time_t(now);
        // UTC, and a local struct rather than localtime's shared static buffer.
        // A run-date override parses to UTC midnight, so rendering it through
        // localtime on the deployed image (TZ=America/New_York) names the
        // PREVIOUS day -- shifting the trading-date key this whole run writes
        // under, and the day-of-week it branches on.
        std::tm now_tm_storage{};
        gmtime_r(&now_time_t, &now_tm_storage);
        std::tm* now_tm = &now_tm_storage;

        // Set start date based on config historical_days
        int historical_days = app_config.live.historical_days;
        auto start_date = now - std::chrono::hours(24 * historical_days);

        // Set end date based on run type to avoid lookahead bias
        auto end_date = use_override_date ? (now - std::chrono::hours(24)) : now;

        DEBUG("Run type: " + std::string(use_override_date ? "HISTORICAL" : "LIVE"));
        DEBUG("Start date: " + std::to_string(std::chrono::system_clock::to_time_t(start_date)));
        DEBUG("End date: " + std::to_string(std::chrono::system_clock::to_time_t(end_date)));
        DEBUG("Target date (now): " + std::to_string(std::chrono::system_clock::to_time_t(now)));

        double initial_capital = app_config.initial_capital;
        double commission_rate = app_config.execution.commission_rate;
        double slippage_model = app_config.execution.slippage_bps;

        // Collect enabled strategies (validates types, ERRORs on an unknown type
        // instead of silently skipping it -- review §F10).
        auto strat_entries_result = trade_ngin::apps::collect_enabled_equity_strategies(
            app_config.strategies_config, "enabled_live");
        if (strat_entries_result.is_error()) {
            ERROR(std::string(strat_entries_result.error()->what()));
            return 1;
        }
        const auto& strat_entries = strat_entries_result.value();
        // The live runner's storage layer is keyed to a single strategy id today
        // (LIVE_EQUITY_MEAN_REVERSION, used at ~20 storage/query sites), so require
        // exactly one enabled strategy rather than silently running only the first.
        if (strat_entries.size() != 1) {
            ERROR("Live equity runner supports exactly one enabled strategy today; found " +
                  std::to_string(strat_entries.size()));
            return 1;
        }
        const auto& strat_entry = strat_entries.front();

        // Load symbols config-first (from the strategy def); fall back to a full DB
        // scan only if none are configured. Previously this unconditionally loaded
        // the entire equity universe and tripped the 1000-symbol cap (review §L1/T2.1).
        std::vector<std::string> symbols;
        if (strat_entry.def.contains("symbols")) {
            for (const auto& sym : strat_entry.def["symbols"]) {
                symbols.push_back(sym.get<std::string>());
            }
            INFO("Loaded " + std::to_string(symbols.size()) + " symbols from config");
        }
        if (symbols.empty()) {
            WARN("No symbols in strategy config, falling back to database scan (slow)");
            auto symbols_result = db->get_symbols(trade_ngin::AssetClass::EQUITIES);
            if (symbols_result.is_error()) {
                ERROR("Failed to get symbols: " + std::string(symbols_result.error()->what()));
                return 1;
            }
            symbols = symbols_result.value();
        }
        if (symbols.empty()) {
            ERROR("No equity symbols found");
            return 1;
        }

        // Moved above instrument registration for E2-F34: the effective universe below
        // needs the previous trading day so it can read the book before the universe is
        // fixed. Nothing between here and the price-manager update consumes
        // `previous_date`, so the move is order-preserving for every other consumer.
        // ========================================
        // NON-TRADING DAY DETECTION
        // Find the actual previous trading day (skip weekends and holidays)
        // Mirrors the logic in live_portfolio.cpp for futures
        // ========================================
        const HolidayChecker& holiday_checker = *holiday_checker_ptr;

        // The calendar covers a finite range of years. Outside it every date
        // reports as a non-holiday because the answer is unknown, which would
        // silently make find_previous_trading_day land on a closed day and
        // return an empty previous-day book -- skipping corp actions and
        // mis-stating PnL with no visible symptom. Fail closed instead.
        {
            char cov_buf[11];
            std::tm cov_tm{};
            auto cov_t = std::chrono::system_clock::to_time_t(now);
            gmtime_r(&cov_t, &cov_tm);
            std::strftime(cov_buf, sizeof(cov_buf), "%Y-%m-%d", &cov_tm);
            if (!holiday_checker.covers_date(cov_buf)) {
                ERROR("Holiday calendar does not cover " + std::string(cov_buf) +
                      " (loaded: " + holiday_checker.coverage_description() +
                      "). Trading-day arithmetic would treat market closures as "
                      "open days. Extend the calendar via "
                      "scripts/generate_market_holidays.py before running this date.");
                return 1;
            }
        }

        // Find the most recent trading day strictly before `now`. Walks back
        // up to 14 days to cover worst-case US closure stacks (Christmas-week
        // holidays + weekends, or 9/11-style multi-day exchange closures).
        // Fails closed if exhausted rather than silently using a stale date.
        auto prev_day_opt = holiday_checker.find_previous_trading_day(now);
        if (!prev_day_opt.has_value()) {
            ERROR("Failed to find a previous trading day within lookback bound. "
                  "Holiday calendar may be misconfigured or stale. Aborting.");
            return 1;
        }
        auto previous_date = *prev_day_opt;

        // ========================================
        // EFFECTIVE UNIVERSE (E2-F34 / F-4) -- finalized AFTER the book is known
        // ========================================
        // The universe used to be config and nothing else, fixed here, while
        // apply_renames ran ~1,500 lines below. A held position whose successor is not
        // in config therefore got no instrument, no bars, no cost config and no target:
        // the day-T pass logged "Missing T-1 price for symbol with a non-zero position",
        // execute_day_t rule 3 rolled its target back to the carried quantity, and the
        // same thing happened again the next session -- an unpriceable zombie carried
        // forever under a key nothing can price. So the book has to be read first.
        //
        // This read is deliberately separate from (and earlier than) the authoritative
        // load below: it exists only to answer "what else must this run be able to
        // price". Everything it produces -- the alias table and the current-holding
        // start dates -- is reused by the class-2 block later in the run, so the
        // universe and the re-keying cannot disagree about which renames apply.
        std::vector<TickerAlias> ticker_aliases;
        bool ticker_aliases_ok = false;
        std::unordered_map<std::string, std::string> holding_start_dates;
        bool holding_start_read_ok = false;
        {
            const std::string as_of_ymd_universe =
                format_ymd_utc(std::chrono::system_clock::to_time_t(now));

            std::unordered_map<std::string, Position> seed_held;
            auto seed_book = db->load_positions_by_date(kEquityStrategyId, kEquityStrategyName,
                                                       portfolio_id, previous_date,
                                                       "trading.positions");
            if (seed_book.is_ok()) {
                std::unordered_map<std::string, Position> seed_closed;
                LiveDailyCycle::split_open_and_closed(seed_book.value(), seed_held, seed_closed);
            } else {
                INFO("No previous-day book for the universe check (first run or no data): " +
                     std::string(seed_book.error()->what()));
            }

            auto alias_result = db->get_ticker_aliases();
            if (alias_result.is_error()) {
                WARN("Failed to fetch ticker aliases: " +
                     std::string(alias_result.error()->what()) +
                     " -- the universe stays as configured and SERIES_CONTINUITY handling "
                     "is skipped this run");
            } else {
                ticker_aliases.reserve(alias_result.value().size());
                for (const auto& row : alias_result.value()) {
                    ticker_aliases.push_back(TickerAlias{row.historical_ticker,
                                                         row.current_symbol,
                                                         row.effective_until, row.note});
                }
                ticker_aliases_ok = true;
            }

            if (!seed_held.empty()) {
                std::vector<std::string> held_syms;
                held_syms.reserve(seed_held.size());
                for (const auto& [sym, pos] : seed_held) held_syms.push_back(sym);

                // BA-2 / C-3 D1: the class-2 era test needs the start of the CURRENT
                // holding, not the lifetime min(date) class 1 uses. A ticker closed years
                // ago and re-bought last month otherwise satisfies the era test for an
                // alias from the previous issuer's lifetime and gets re-keyed onto a
                // symbol with no bars. Measured on this book on 2026-09-03: META's
                // lifetime inception is 2026-06-11, its current holding began 2026-07-31.
                auto holding_start = db->get_current_holding_start_dates(
                    kEquityStrategyId, kEquityStrategyName, portfolio_id, held_syms);
                if (holding_start.is_error()) {
                    WARN("Could not read current-holding start dates (" +
                         std::string(holding_start.error()->what()) +
                         ") -- SERIES_CONTINUITY handling is skipped this run "
                         "(fail-narrow) and the universe stays as configured");
                } else {
                    holding_start_dates = holding_start.value();
                    holding_start_read_ok = true;
                }
            }

            if (ticker_aliases_ok && holding_start_read_ok && !seed_held.empty()) {
                const auto extended = LiveDailyCycle::effective_universe(
                    symbols, seed_held, ticker_aliases, as_of_ymd_universe,
                    holding_start_dates);
                if (extended.size() != symbols.size()) {
                    std::string added;
                    for (size_t i = symbols.size(); i < extended.size(); ++i) {
                        added += (added.empty() ? "" : ", ") + extended[i];
                    }
                    WARN("Effective universe extended beyond config for held positions "
                         "whose ticker has been renamed: " + added +
                         ". Without this the renamed holding would have no bars, no "
                         "instrument and no target, and would be carried unpriceable "
                         "forever (E2-F34).");
                    symbols = extended;
                }
            }
            INFO("Effective universe: " + std::to_string(symbols.size()) +
                 " symbol(s) (config plus successors of held positions)");
        }

        // Register equity instruments. Pass the exchange JSON path so per-symbol
        // exchanges populate correctly instead of defaulting to NYSE. Audit §1.2.
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

        std::cout << "Symbols: ";
        for (const auto& symbol : symbols) {
            std::cout << symbol << " ";
        }
        std::cout << std::endl;

        std::cout << "Retrieved " << symbols.size() << " symbols" << std::endl;
        std::cout << "Initial capital: $" << initial_capital << std::endl;
        // E2-C6: describe the model that actually charges, not two inert config values.
        //
        // These printed `execution.commission_rate` and `execution.slippage_bps` from
        // config/defaults.json as though they were the live cost model. Neither is used:
        // commission_rate is written into StrategyConfig::costs, which has NO reader anywhere
        // in the tree, and slippage_bps is read into a local and never consulted. The banner
        // told an operator the book paid "0.05 bps" while TransactionCostManager was charging
        // a $1.00-per-order floor -- on the observed window that floor was 96% of all
        // commission paid.
        //
        // Print the real schedule instead. If the config values are ever wired up, print them
        // here again -- but they must drive the model first.
        std::cout << "Commission model: IBKR Pro Fixed -- $0.005/share, $1.00 min/order, "
                     "1% of trade value max, fees included" << std::endl;
        std::cout << "Implicit costs: spread + market impact, charged via "
                     "total_transaction_costs" << std::endl;
        std::cout << "  (config execution.commission_rate=" << commission_rate
                  << ", execution.slippage_bps=" << slippage_model
                  << " are NOT used by the cost model)" << std::endl;

        INFO("Configuration loaded successfully. Processing " +
             std::to_string(symbols.size()) + " symbols from " +
             std::to_string(std::chrono::system_clock::to_time_t(start_date)) +
             " to " +
             std::to_string(std::chrono::system_clock::to_time_t(end_date)));

        // Pre-run margin metadata validation for equity instruments only
        // Skip this validation for equities (they don't use futures-style margin requirements)
        // Equities use a different model: position value = shares * price, no contract multipliers
        INFO("Asset class: EQUITIES - skipping futures margin validation");
        INFO("Equity instruments use price-per-share model without futures-style margin requirements");

        // Configure portfolio from loaded config
        RiskConfig risk_config = app_config.risk_config;
        risk_config.capital = Decimal(initial_capital);

        DynamicOptConfig opt_config = app_config.opt_config;
        opt_config.capital = initial_capital;

        trade_ngin::PortfolioConfig portfolio_config;
        portfolio_config.total_capital = initial_capital;
        portfolio_config.reserve_capital = initial_capital * app_config.reserve_capital_pct;
        portfolio_config.max_strategy_allocation = app_config.strategy_defaults.max_strategy_allocation;
        portfolio_config.min_strategy_allocation = app_config.strategy_defaults.min_strategy_allocation;
        // Mean reversion does NOT use dynamic optimization (HD, 2026-09-01). The optimizer
        // is a trend-following construct -- position-buffered contract optimization over a
        // futures universe -- and it cannot even see this strategy: PortfolioManager
        // populates its symbol data only for TrendFollowingStrategy
        // (portfolio_manager.cpp:993), so every equity symbol fell through to a flat 0.01
        // weight (the true weight is price/capital, wrong by 1.4x-7.7x) and zero cost
        // against a cost penalty of 50 -- 30,692 "not found in trading data" warnings in a
        // single run. bt_equity_mean_reversion.cpp already disables it; this makes live
        // agree rather than optimising on values it made up.
        portfolio_config.use_optimization = false;
        portfolio_config.use_risk_management = app_config.strategy_defaults.use_risk_management;
        portfolio_config.opt_config = opt_config;
        portfolio_config.risk_config = risk_config;

        // Strategy config was resolved above by collect_enabled_equity_strategies
        // (guaranteed to contain a "config" block). The strategy is still
        // constructed with the id "LIVE_EQUITY_MEAN_REVERSION" below to match the
        // live storage keys -- not strat_entry.id.
        const auto& mr_cfg = strat_entry.def["config"];

        // Create mean reversion strategy configuration
        trade_ngin::StrategyConfig mr_config;
        mr_config.capital_allocation = initial_capital;
        mr_config.asset_classes = {trade_ngin::AssetClass::EQUITIES};
        mr_config.frequencies = {trade_ngin::DataFrequency::DAILY};
        mr_config.max_drawdown = app_config.max_drawdown;
        mr_config.max_leverage = app_config.max_leverage;
        for (const auto& symbol : symbols) {
            mr_config.position_limits[symbol] = app_config.execution.position_limit_live;
            mr_config.trading_params[symbol] = 1.0;
            mr_config.costs[symbol] = commission_rate;
        }

        // Configure mean reversion parameters from config via the shared builder so
        // live and backtest construct the strategy identically (review T-OR.5).
        trade_ngin::MeanReversionConfig mean_rev_config =
            trade_ngin::apps::build_mean_reversion_config(mr_cfg);

        // Create and initialize the strategies
        // Before MeanReversionStrategy
        std::cerr << "Before MeanReversionStrategy: initialized="
                  << Logger::instance().is_initialized() << std::endl;
        INFO("Initializing MeanReversionStrategy for equities...");
        std::cout << "Strategy capital allocation: $" << mr_config.capital_allocation << std::endl;
        std::cout << "Max leverage: " << mr_config.max_leverage << "x" << std::endl;
        std::cout << "Lookback period: " << mean_rev_config.lookback_period << " days" << std::endl;
        std::cout << "Entry threshold: " << mean_rev_config.entry_threshold << " std devs" << std::endl;

        // Create a shared_ptr that doesn't own the singleton registry
        auto registry_ptr =
            std::shared_ptr<InstrumentRegistry>(&registry, [](InstrumentRegistry*) {});

        auto mr_strategy = std::make_shared<trade_ngin::MeanReversionStrategy>(
            "LIVE_EQUITY_MEAN_REVERSION", mr_config, mean_rev_config, db, registry_ptr);

        auto init_result = mr_strategy->initialize();
        if (init_result.is_error()) {
            std::cerr << "Failed to initialize strategy: " << init_result.error()->what()
                      << std::endl;
            return 1;
        }
        INFO("Strategy initialization successful");

        // Start the strategy
        INFO("Starting strategy...");
        auto start_result = mr_strategy->start();
        if (start_result.is_error()) {
            std::cerr << "Failed to start strategy: " << start_result.error()->what() << std::endl;
            return 1;
        }
        INFO("Strategy started successfully");

        // Create portfolio manager and add strategy
        INFO("Creating portfolio manager...");
        // A fractional target is a legitimate end state when the strategy allows
        // fractional shares, so the optimizer/risk loop must not keep iterating
        // toward whole units -- each extra lap re-applies the risk scale to an
        // already-scaled book (E2-F1). Set from the strategy's own config so live
        // and backtest agree. Futures leave this false and are unaffected.
        portfolio_config.allow_fractional_positions = mean_rev_config.allow_fractional_shares;
        INFO(std::string("Fractional positions permitted: ") +
             (portfolio_config.allow_fractional_positions
                  ? "yes (from strategy allow_fractional_shares)"
                  : "no"));
        auto portfolio = std::make_shared<trade_ngin::PortfolioManager>(portfolio_config);
        auto add_result =
            portfolio->add_strategy(mr_strategy, 1.0, portfolio_config.use_optimization,
                                    portfolio_config.use_risk_management);
        if (add_result.is_error()) {
            std::cerr << "Failed to add strategy to portfolio: " << add_result.error()->what()
                      << std::endl;
            return 1;
        }
        INFO("Strategy added to portfolio successfully");

        // Per-run provenance: what this run was configured with. The futures runners have
        // always written it (live_portfolio_conservative.cpp:620); the equity runner never
        // did, so trading.live_run_metadata held 245 futures rows and nothing for
        // equities. Nothing reads this table -- it exists so a past run's allocations and
        // config can be reconstructed after the fact, which is exactly what you want when
        // a number from months ago has to be explained.
        {
            nlohmann::json portfolio_config_json;
            portfolio_config_json["total_capital"] =
                static_cast<double>(portfolio_config.total_capital);
            portfolio_config_json["reserve_capital"] =
                static_cast<double>(portfolio_config.reserve_capital);
            portfolio_config_json["use_optimization"] = portfolio_config.use_optimization;
            portfolio_config_json["use_risk_management"] = portfolio_config.use_risk_management;
            portfolio_config_json["allow_fractional_positions"] =
                portfolio_config.allow_fractional_positions;

            // Single-strategy runner: the live equity path rejects more than one strategy.
            nlohmann::json strategy_alloc_json;
            strategy_alloc_json[kEquityStrategyName] = 1.0;

            nlohmann::json strategy_configs_json;
            strategy_configs_json[kEquityStrategyName] = {
                {"lookback_period", mean_rev_config.lookback_period},
                {"entry_threshold", mean_rev_config.entry_threshold},
                {"exit_threshold", mean_rev_config.exit_threshold},
                {"risk_target", mean_rev_config.risk_target},
                {"position_size", mean_rev_config.position_size},
                {"vol_lookback", mean_rev_config.vol_lookback},
                {"use_stop_loss", mean_rev_config.use_stop_loss},
                {"stop_loss_pct", mean_rev_config.stop_loss_pct},
                {"allow_fractional_shares", mean_rev_config.allow_fractional_shares}};

            auto metadata_result = db->store_live_run_metadata(
                now, kEquityStrategyId, portfolio_id, strategy_alloc_json,
                portfolio_config_json, strategy_configs_json);

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
        coordinator_config.strategy_id = kEquityStrategyId;
        // Without these two the write key is (LIVE_EQUITY_MEAN_REVERSION,
        // LIVE_EQUITY_MEAN_REVERSION, BASE_PORTFOLIO) while every read uses
        // (LIVE_EQUITY_MEAN_REVERSION, EQUITY_MEAN_REVERSION, <configured portfolio>):
        // two of three columns differ, so run 2 loads an empty book. The futures runner
        // has always set portfolio_id here; equity omitted it.
        coordinator_config.strategy_name = kEquityStrategyName;
        coordinator_config.portfolio_id = portfolio_id;
        coordinator_config.schema = "trading";
        coordinator_config.initial_capital = mr_config.capital_allocation;
        coordinator_config.store_results = true;
        coordinator_config.calculate_risk_metrics = true;
        auto coordinator = std::make_unique<LiveTradingCoordinator>(db, registry, coordinator_config);

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
        // E2-F38 / BA-7: this is an equity book. Tell the PnL manager so, because a
        // symbol that misses the InstrumentRegistry otherwise falls through to a
        // SUBSTRING match over futures contract codes -- AAPL matches "PL" (platinum,
        // 50), LEN matches "LE" (live cattle, 40000). The futures runners pass nothing
        // and keep the fallback table.
        if (pnl_manager) {
            pnl_manager->set_asset_type(AssetType::EQUITY);
        }

        // Create Phase 3 managers
        INFO("Creating ExecutionManager and MarginManager for Phase 3");
        auto execution_manager = std::make_unique<ExecutionManager>();
        auto margin_manager = std::make_unique<MarginManager>(registry);

        // Create Phase 4 CSV exporter
        INFO("Creating CSVExporter for Phase 4");
        auto csv_exporter = std::make_unique<CSVExporter>(".");  // Current directory for output

        // Load market data for daily processing
        INFO("Loading market data for daily processing...");
        auto market_data_result = db->get_market_data(
            symbols, start_date, end_date,
            trade_ngin::AssetClass::EQUITIES,
            trade_ngin::DataFrequency::DAILY, "ohlcv");
        
        if (market_data_result.is_error()) {
            ERROR("Failed to load market data: " + std::string(market_data_result.error()->what()));
            return 1;
        }
        
        // Convert Arrow table to Bars using the same conversion as backtest
        auto conversion_result = trade_ngin::DataConversionUtils::arrow_table_to_bars(market_data_result.value());
        if (conversion_result.is_error()) {
            ERROR("Failed to convert market data to bars: " + std::string(conversion_result.error()->what()));
            return 1;
        }
        
        auto all_bars = conversion_result.value();

        // E2-F15: the newest loaded bar PER SYMBOL. Hoisted here (it used to be built
        // inside the delisting-staleness block) because the corporate-action code needs it
        // too, and both must agree on one definition of "how far does this symbol's price
        // series actually reach".
        //
        // This is the horizon that decides whether a corporate action may be applied. See
        // the gate below and docs/E2_FINDING_15_SPLIT_PRICE_UNIT_DESYNC.md.
        std::unordered_map<std::string, std::string> last_bar_date;
        for (const auto& bar : all_bars) {
            const std::string d =
                format_ymd_utc(std::chrono::system_clock::to_time_t(bar.timestamp));
            auto it = last_bar_date.find(bar.symbol);
            if (it == last_bar_date.end() || d > it->second) last_bar_date[bar.symbol] = d;
        }
        INFO("Loaded " + std::to_string(all_bars.size()) + " total bars");

        // Data-freshness guard (review T2.9): the newest bar actually loaded for our
        // symbols tells us how current the feed is. If it lags end_date by more than
        // the configured tolerance, the run is on stale data -- WARN in historical-
        // replay mode, ERROR (refuse) in true-live mode. (Computed from the loaded
        // bars rather than a separate query: symbol-scoped and no extra round-trip.)
        //
        // BA-12: measured from the STALEST symbol, not the freshest. This used to take
        // the global max bar timestamp, so one symbol printing today reported the whole
        // feed current however far behind the other 851 were -- a guard that cannot
        // fail is not a guard. It also skipped entirely on an empty load and measured
        // in instants rather than calendar days.
        // Annualization anchor (E2-F32). trading.get_trading_days() takes its start
        // date from strategy_trading_days_metadata and only falls back to MIN(date)
        // over live_results when NO row exists -- so a metadata row dated after the
        // book's own first day is worse than no row at all, and the function cannot
        // notice because it stops looking the moment it finds one. Measured on this
        // book: anchor 2026-07-24 against a book starting 2026-04-01 gave 1 trading
        // day for 115 of 126 rows and annualized -3.5 % into -1674 % on 07-27. The
        // run exited 0 and stored it.
        //
        // The check is one comparison the function cannot make. On a mismatch the
        // correct anchor is the book's own first day, and the count below is computed
        // from it rather than taken from the function.
        std::string trading_days_anchor_override;
        {
            auto anchor_q = db->execute_query(
                "SELECT COALESCE(MIN(live_start_date)::text, '') "
                "FROM trading.strategy_trading_days_metadata "
                "WHERE strategy_id = '" + std::string(kEquityStrategyId) + "'"
                " AND portfolio_id = '" + portfolio_id + "'");
            auto first_q = db->execute_query(
                "SELECT COALESCE(MIN(date)::text, '') FROM trading.live_results "
                "WHERE strategy_id = '" + std::string(kEquityStrategyId) + "'"
                " AND portfolio_id = '" + portfolio_id + "'");

            auto first_cell = [](const Result<std::shared_ptr<arrow::Table>>& r) -> std::string {
                if (r.is_error() || !r.value() || r.value()->num_rows() == 0) return {};
                auto col = std::static_pointer_cast<arrow::StringArray>(
                    r.value()->column(0)->chunk(0));
                if (!col || col->length() == 0 || col->IsNull(0)) return {};
                return std::string(col->GetView(0));
            };

            if (anchor_q.is_error() || first_q.is_error()) {
                WARN("Could not check the annualization anchor for " + portfolio_id +
                     "; total_annualized_return is reported as the DB function computes it.");
            } else {
                const auto anchor = assess_trading_days_anchor(first_cell(anchor_q),
                                                               first_cell(first_q));
                if (anchor.anchor_is_late) {
                    trading_days_anchor_override = anchor.effective_anchor;
                    WARN("Annualization anchor is LATER than the book it annualizes: "
                         "strategy_trading_days_metadata.live_start_date = " +
                         anchor.metadata_anchor + " but live_results for " + portfolio_id +
                         " start " + anchor.earliest_result +
                         ". trading.get_trading_days would return 1 for every date before "
                         "the anchor and explode total_annualized_return just after it "
                         "(E2-F32). Using " + anchor.effective_anchor +
                         " for this run; seed the metadata row to that date, or delete it "
                         "so the MIN(date) fallback applies.");
                } else {
                    INFO("Annualization anchor " +
                         (anchor.effective_anchor.empty() ? std::string("(none yet)")
                                                          : anchor.effective_anchor) +
                         " is consistent with the book for " + portfolio_id);
                }
            }
        }

        // Deal-terms feed freshness -- the OTHER feed, and a different question.
        // equities_data.corporate_action supplies class-3 deal terms and class-2
        // renames, and it stopped receiving events in 2025. That fact used to be a
        // constant compiled into the binary and quoted in every termination WARN, so
        // a restarted subscription would have gone unannounced and the WARNs would
        // have kept naming a date that was no longer true (E4 item 3). Measure it,
        // say it out loud once per run, and hand it to the handler that quotes it.
        std::string corp_action_feed_last_date;
        {
            const std::string as_of_ymd =
                format_ymd_utc(std::chrono::system_clock::to_time_t(end_date));
            auto feed_max = db->get_corp_action_feed_last_date(as_of_ymd);
            if (feed_max.is_error()) {
                WARN("Could not measure the corporate-action deal-terms feed: " +
                     std::string(feed_max.error()->what()) +
                     " -- termination WARNs will quote the compiled-in date instead.");
            } else {
                corp_action_feed_last_date = feed_max.value();
            }
            const auto feed = assess_corp_action_feed(corp_action_feed_last_date, as_of_ymd);
            INFO(describe_corp_action_feed(feed));
            if (feed.revived_since_build) {
                WARN("The corporate-action deal-terms feed has rows after " +
                     std::string(kCorpActionTableFrozenAfter) +
                     ", the date this binary was built against. Deal-terms rollover and "
                     "survivor-keyed TERMINATION rows are now reachable; confirm E4 "
                     "NEW-5(A) has landed before running this book at universe scale.");
            }
        }

        {
            const int tolerance_days = app_config.live.data_staleness_tolerance_days;
            const std::string as_of_ymd =
                format_ymd_utc(std::chrono::system_clock::to_time_t(end_date));
            const auto freshness = assess_feed_freshness(last_bar_date, as_of_ymd);

            if (!freshness.any_data) {
                // No symbol has a usable bar date. Maximally stale, never neutral.
                const std::string msg =
                    "Equity data freshness cannot be established: none of the " +
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
                    "Equity data is stale: the stalest of " +
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
                INFO("Equity feed freshness: stalest of " +
                     std::to_string(freshness.symbols) + " symbols (" +
                     freshness.stalest_symbol + ") at " + freshness.stalest_date + ", " +
                     std::to_string(freshness.days_behind) + " days behind " + as_of_ymd +
                     " (tolerance " + std::to_string(tolerance_days) + ").");
            }
        }

        // NOTE: the price manager is deliberately NOT updated here. It is updated below,
        // immediately after `previous_date` is resolved, so the T-1 price lookup and the
        // T-1 position book are keyed on the SAME trading day. See E2-F14 there.
        if (!price_manager) {
            ERROR("Price manager not initialized");
            return 1;
        }

        if (all_bars.empty()) {
            ERROR("No historical data loaded. Cannot calculate positions.");
            ERROR("This may be due to missing market data for the requested date.");
            ERROR("Please check if market data exists for " + std::to_string(std::chrono::system_clock::to_time_t(now)) +
                  " and the 300 days prior.");
            return 1;
        }

        // Register tier-appropriate equity cost configs using actual recent
        // ADV per symbol from the bars we just loaded. Closes audit §1.1:
        // before this, unconfigured equities fell through to the futures
        // default ($1.50/share commission, point_value=100).
        {
            std::unordered_map<std::string, std::vector<trade_ngin::Bar>> bars_by_symbol;
            for (const auto& bar : all_bars) {
                bars_by_symbol[bar.symbol].push_back(bar);
            }
            execution_manager->get_transaction_cost_manager()
                .register_equity_costs_from_bars(symbols, bars_by_symbol);
        }


        // E2-F14: extract T-1/T-2 prices keyed on the trading day we just resolved, NOT on
        // `now - 24h`.
        //
        // This call used to sit ~70 lines above, before `previous_date` existed, and passed
        // only `now`. LivePriceManager then derived T-1 as `now - 24h` with no fallback,
        // while the book being finalized came from find_previous_trading_day(). On a Sunday
        // or Monday run those disagree -- the book says FRIDAY, the price lookup asks for
        // Saturday/Sunday, and equities_data.ohlcv_1d has no weekend rows -- so an
        // already-finalized Friday was re-finalized against an empty price map and its
        // position rows overwritten with 0/0 by the store_positions call in STEP 1, which
        // runs outside the `yesterday_total_pnl != 0.0` gate that protects live_results.
        //
        // Measured on the 2026-07-24..08-04 weekend-inclusive replay: Sat 08-01 correctly
        // finalized Friday at $864.759444, then Sun 08-02 and Mon 08-03 each re-zeroed it.
        // L5 residual was -864.7594 on 07-31 and -14.6574 on 07-24, 0.0000 elsewhere.
        //
        // Passing the resolved date makes the two lookups agree by construction and makes
        // re-finalization idempotent. Futures is unaffected: both futures runners pass no
        // t1_date and their own book lookup is `now - 24h`, the same value this defaults to.
        // Do NOT move this call back above the resolution, and do NOT "simplify" it by
        // dropping the argument.
        {
            auto price_update_result = price_manager->update_from_bars(all_bars, now, previous_date);
            if (price_update_result.is_error()) {
                ERROR("Failed to update price manager with bar data: " +
                      std::string(price_update_result.error()->what()));
                return 1;
            }
            INFO("Price manager updated - T-1/T-2 prices keyed on resolved trading day");
        }

        // Check if today itself is a non-trading day
        std::ostringstream today_oss;
        today_oss << std::put_time(now_tm, "%Y-%m-%d");
        std::string today_date_str = today_oss.str();
        // T-OR.3: one predicate, in a testable place. This was five inline copies of the
        // weekend/holiday test and nothing covered any of them.
        const bool today_is_non_trading =
            LiveDailyCycle::is_non_trading_day(*now_tm, holiday_checker);

        // A closed market does NOT mean "do nothing". You still hold the book over a
        // weekend or holiday even though no new bar exists to signal from, so the day is
        // processed as a CARRY-FORWARD: positions, live_results and equity_curve are all
        // written with the previous day's book, and signal generation and execution are
        // skipped. Futures already behaves this way -- CONSERVATIVE_PORTFOLIO has rows on
        // all seven weekdays across positions (244 Sat / 244 Sun), live_results and
        // equity_curve alike.
        //
        // Previously this returned early, and only in real-time mode: an explicit replay
        // date bypassed the guard entirely and processed the weekend as a normal trading
        // day, which would book a fill dated Saturday at Friday's close. Equities cannot
        // trade Saturday, so that execution is factually false and would break broker
        // reconciliation, even though the net position lands the same. The T-1 lag already
        // handles the real case: Monday's run reaches back to Friday's close via the
        // widened lookup and trades there.
        if (today_is_non_trading) {
            INFO("Today (" + today_date_str +
                 ") is a non-trading day - carrying the book forward: no signals, no "
                 "executions, positions/live_results/equity_curve written from the "
                 "previous session.");
        }

        // Load previous day positions for PnL calculation
        INFO("Loading previous day positions for PnL calculation...");
        auto previous_positions_result = db->load_positions_by_date(kEquityStrategyId, kEquityStrategyName, portfolio_id, previous_date, "trading.positions");
        std::unordered_map<std::string, Position> previous_positions;
        
        if (previous_positions_result.is_ok()) {
            previous_positions = previous_positions_result.value();
            INFO("Loaded " + std::to_string(previous_positions.size()) + " previous day positions");
        } else {
            INFO("No previous day positions found (first run or no data): " + std::string(previous_positions_result.error()->what()));
        }

        // E2-F19: a position closed to zero keeps its row for the day it closed, so the
        // exit's realized P&L has somewhere to live (see LiveDailyCycle::is_dead_row).
        // Everything below this line -- corporate actions, the run-gap guard, seeding,
        // execution, basis resolution -- is written against a book of HELD positions,
        // so closed rows are split out here and reach exactly one place: the T-1 write
        // set, where they are re-appended verbatim so the DELETE-then-INSERT that
        // rewrites the T-1 date does not destroy them.
        std::unordered_map<std::string, Position> previous_closed_rows;
        {
            std::unordered_map<std::string, Position> open_rows;
            LiveDailyCycle::split_open_and_closed(previous_positions, open_rows,
                                                  previous_closed_rows);
            previous_positions = std::move(open_rows);
            if (!previous_closed_rows.empty()) {
                INFO("Loaded " + std::to_string(previous_closed_rows.size()) +
                     " closed row(s) for Day T-1 (realized carried on the close date); " +
                     std::to_string(previous_positions.size()) + " held position(s) remain");
            }
        }

        // An empty prior book is legitimate on a genuine first run, and catastrophic
        // otherwise. The two are indistinguishable from here -- both are an empty map --
        // and the consequences of guessing wrong are total: the strategy seeds flat, the
        // corporate-action block is skipped by its own !previous_positions.empty() guard,
        // every position silently disappears from trading.positions, and the next day's
        // executions are sized as deltas from zero against stock the broker still holds.
        //
        // Runs MUST be sequential and complete -- that is inherent to the T-1 lag model,
        // where day T's P&L is finalized by day T+1's run -- so a hole here means a run was
        // missed, not that the strategy is flat. The remedy is to replay the missing dates
        // in order, which works and needs no code. What was missing is being TOLD: this
        // previously exited 0 with no ERROR and no WARN, so a monitor watching exit codes
        // saw a clean run while the book was being abandoned (E2-F8).
        //
        // Deliberately NOT resolved by falling back to MAX(date): that would paper over a
        // broken invariant and could silently revive a stale book.
        if (previous_positions.empty()) {
            // An empty prior book has two very different causes and they must not be
            // conflated:
            //   (a) the strategy legitimately holds nothing -- it exited everything, or it
            //       is a genuine first run;
            //   (b) a run was MISSED, so the prior day was never written at all.
            //
            // (b) is silent and destructive: the strategy seeds flat, the corporate-action
            // block is skipped by its own !previous_positions.empty() guard, every position
            // vanishes from trading.positions, and the next day's executions are sized as
            // deltas from zero against stock the broker still holds (E2-F8).
            //
            // The discriminator is NOT "are there positions" -- a flat book has none and is
            // perfectly valid. It is "did a run HAPPEN for that date". Every run writes a
            // live_results row whether or not it holds anything, so that row is the evidence
            // a run occurred. Testing positions instead would refuse to start every time the
            // strategy went flat, which is a normal state.
            //
            // Deliberately NOT resolved by falling back to MAX(date): that would paper over
            // a broken invariant and could silently revive a stale book. Runs must be
            // sequential -- inherent to the T-1 lag model -- so the remedy is to replay the
            // missing dates in order, which works and needs no code change. What was missing
            // was being TOLD.
            const std::string prev_date_str = trade_ngin::core::format_utc_date(previous_date);
            auto prev_run = db->execute_query(
                "SELECT count(*)::text FROM trading.live_results "
                "WHERE strategy_id = '" + std::string(kEquityStrategyId) + "'"
                " AND portfolio_id = '" + portfolio_id + "'"
                " AND date = '" + prev_date_str + "'");

            long prev_run_rows = 0;
            if (prev_run.is_ok() && prev_run.value()->num_rows() > 0) {
                auto col = std::static_pointer_cast<arrow::StringArray>(
                    prev_run.value()->column(0)->chunk(0));
                if (!col->IsNull(0)) prev_run_rows = std::stol(std::string(col->GetView(0)));
            }

            if (prev_run_rows > 0) {
                INFO("Previous trading day (" + prev_date_str +
                     ") ran and held no positions -- the strategy is flat, which is a valid "
                     "state. Continuing.");
            } else {
                auto last_run = db->execute_query(
                    "SELECT MAX(date)::text FROM trading.live_results "
                    "WHERE strategy_id = '" + std::string(kEquityStrategyId) + "'"
                    " AND portfolio_id = '" + portfolio_id + "'"
                    " AND date < '" + today_date_str + "'");

                std::string last_run_date;
                if (last_run.is_ok() && last_run.value()->num_rows() > 0) {
                    auto col = std::static_pointer_cast<arrow::StringArray>(
                        last_run.value()->column(0)->chunk(0));
                    if (!col->IsNull(0)) last_run_date = std::string(col->GetView(0));
                }

                if (!last_run_date.empty()) {
                    ERROR("No run was recorded for the previous trading day (" + prev_date_str +
                          "), but this strategy last ran on " + last_run_date +
                          ". A run was missed. Replay every date from " + last_run_date +
                          " forward, in order, before running " + today_date_str +
                          " -- continuing would seed the strategy flat and silently abandon "
                          "the book. Refusing to run.");
                    return 1;
                }
                INFO("No prior run anywhere for this strategy -- genuine first run.");
            }
        }

        // The book AS IT STOOD on T-1, captured before any corporate action touches it.
        //
        // The corp-action blocks below mutate `previous_positions` IN PLACE (non-const ref
        // by design -- see AVERAGE_PRICE_LIFECYCLE.md step 2). The Day T-1 finalization then
        // builds its input from that same map and writes the result back at the T-1 DATE, so
        // the post-action quantities and basis landed on a row for a day the action had not
        // yet happened. Measured: a position seeded at 10 @ 4194.31 on 2026-04-02 came back
        // as 250 @ 167.7724 on 2026-04-02 after the 2026-04-06 split was applied -- history
        // restated four days before the ex-date.
        //
        // For a split that is "only" falsified quantity history, since notional is
        // preserved. For a DIVIDEND it is worse: the basis changes without notional being
        // preserved, so the T-1 row would carry a cost basis that was not true that day and
        // any P&L computed over it would be wrong.
        //
        // T-1 is finalized from this snapshot; the adjustment belongs to the ex-date forward.
        const std::unordered_map<std::string, Position> previous_positions_pre_action =
            previous_positions;

        // Executions synthesised from corporate actions (terminations), merged into the
        // day's executions after execute_day_t so a broker statement has a counterpart.
        //
        // E2-F12 (comment correction): this used to claim the executions are what carry the
        // realized P&L into live_results and the equity curve. That was FALSE.
        // live_results.daily_realized_pnl has never been derived from executions -- it comes
        // from the day-T aggregate and the T-1 finalization -- so appending an execution gave
        // the corp-action delta no route into any aggregate at all. The row said 122.00 and
        // live_results said 0.00.
        //
        // The execution row is still worth emitting: it is the broker-reconcilable
        // counterpart, and without it a liquidation appears on a statement with nothing in
        // trading.executions to match. But it is NOT the P&L path. That is
        // corp_action_realized_total below (E2-F7).
        std::vector<ExecutionReport> corp_action_executions;

        // E2-F7: the realized P&L a termination locks in, summed so it can be folded into
        // the day's aggregate.
        //
        // CorporateActionsLifecycle computes `realized_delta = (exit_price - avg_price) *
        // qty_before` and writes it onto the position and the adjustment record -- and until
        // now its ONLY other consumer in the entire tree was a log string. No aggregate read
        // it, so a terminated holding's gain or loss never reached daily_realized_pnl,
        // total_pnl, current_portfolio_value or the equity curve.
        //
        // It is booked on day T, the ex-date, NOT threaded through finalize_previous_day.
        // That function writes the T-1 row, so passing the delta to it would book a day-T
        // cash flow onto the previous day -- exactly the restatement 8a1a96ef removed. It
        // would also put the T-1 aggregate out of agreement with the T-1 executions and
        // position rows.
        //
        // Booking it here touches NO shared file, so the futures binaries are unchanged by
        // construction rather than by argument.
        double corp_action_realized_total = 0.0;
        // E2-F19 (R4): the same deltas, per symbol, so the terminated symbol's day-T
        // ROW carries what the aggregate carries. Filled under the identical gate as
        // corp_action_realized_total so the two can never disagree about which events
        // were recognised.
        std::unordered_map<std::string, double> corp_action_realized_by_symbol;

        // E2-F15: ex-date of any class-1 event APPLIED on this run, per symbol. Used to pick
        // the right T-1 snapshot for finalization -- see the selection below.
        std::unordered_map<std::string, std::string> applied_class1_ex_date;


        DEBUG("Previous date used for lookup: " + std::to_string(std::chrono::system_clock::to_time_t(previous_date)));
        DEBUG("Current date: " + std::to_string(std::chrono::system_clock::to_time_t(now)));
        DEBUG("Previous positions loaded: " + std::to_string(previous_positions.size()));
        for (const auto& [symbol, pos] : previous_positions) {
            DEBUG("Previous position - " + symbol + ": " + std::to_string(pos.quantity.as_double()));
        }
        
        // Get market prices from PriceManager - already extracted from bars
        INFO("Getting market prices for PnL lag model from PriceManager...");

        // PriceManager has already extracted T-1 and T-2 prices from bars
        // Make copies since we need to use [] operator in many places
        std::unordered_map<std::string, double> previous_day_close_prices = price_manager->get_all_previous_day_prices();
        std::unordered_map<std::string, double> two_days_ago_close_prices = price_manager->get_all_two_days_ago_prices();

        INFO("Retrieved prices from PriceManager: " +
             std::to_string(previous_day_close_prices.size()) + " Day T-1, " +
             std::to_string(two_days_ago_close_prices.size()) + " Day T-2");

        // ========================================
        // CORPORATE ACTIONS, BY MECHANICAL EFFECT CLASS
        //
        // Class 1 PRICE_RESTATING (splits, ADR ratios, spin-offs, dividends):
        //   sourced from the per-bar div_cash / split_factor columns on
        //   equities_data.ohlcv_1d -- current and never restated. (Until
        //   Phase 4.3 this read equities_data.corporate_action, whose feed
        //   stopped on 2025-08-29, so the applier silently saw zero events.)
        // Class 2 SERIES_CONTINUITY (ticker renames): equities_data.ticker_aliases.
        // Class 3 TERMINATION (mergers, acquisitions, delistings): exit timing
        //   from ohlcv_1d.delisting_date; deal terms still only from the frozen
        //   corporate_action table, so today the handler takes its documented
        //   final-close exit. A revived feed activates the terms path with no
        //   code change.
        // Class 4 INFORMATIONAL: no handler.
        //
        // Adjustments land BEFORE the strategy generates today's targets.
        // See docs/CORP_ACTIONS_DATA_BOUNDARY.md. Closes audit §1.12, §1.15.
        // ========================================
        // Position inception is read in the class-1 block below and is CLASS 1's alone:
        // it is min(date) over all history and fails wide, which is right for a price
        // window and wrong for an era test. Class 2 reads its own dates -- the start of
        // the CURRENT holding, up with the effective universe (BA-2).

        if (!previous_positions.empty()) {
            // Lookback is DERIVED FROM POSITION HISTORY, never a fixed
            // constant and never from last_update.
            //
            // A fixed 14-day window silently dropped every event older than it:
            // live last wrote 2026-05-03, and of the 9 dividends the configured
            // universe has seen since, 8 fall outside 14 days. A dropped
            // dividend leaves cost basis in the pre-dividend frame against a
            // post-dividend mark, and because dividend income is deliberately
            // informational-only, that value is simply lost from P&L --
            // permanently, since no later event repairs a basis.
            //
            // Widening to a bigger constant is not sufficient either: a book can
            // outlive any constant (the live futures book already spans 459
            // days). Neither is deriving from previous_positions.last_update --
            // load_positions_by_date() selects WHERE DATE(last_update) =
            // DATE($n), so every row it returns carries the requested date by
            // construction (the table has zero rows where last_update differs
            // from date). That derivation always collapsed to "yesterday",
            // leaving the effective window at the 14-day floor it was meant to
            // replace. The window is therefore derived from when positions were
            // actually ESTABLISHED, via position history.
            //
            // A holding older than the bulk price load does not truncate the
            // window: the closes those events need are topped up per symbol
            // below (~45 ms for one symbol over ten years, against the ~25 s
            // full-universe adjusted-series query). Only a symbol with no bars
            // at all is unrecoverable, and that errors loudly.
            //
            // Safe only because dedup is now durable in trading.corp_action_applied
            // (migration 002) rather than a JSON file under a container path with
            // no volume, where state loss was the default: an over-wide window
            // costs query time, never a double-applied event.
            auto today_t = std::chrono::system_clock::to_time_t(now);

            constexpr long kSecondsPerDay = 24 * 60 * 60;
            constexpr long kMinLookbackDays = 14;
            const long max_lookback_days = static_cast<long>(historical_days);

            std::vector<std::string> held_symbols;
            held_symbols.reserve(previous_positions.size());
            for (const auto& [sym, pos] : previous_positions) {
                if (pos.quantity.as_double() != 0.0) held_symbols.push_back(sym);
            }

            const auto bulk_start_t = today_t - max_lookback_days * kSecondsPerDay;

            // One read, TWO consumers with OPPOSITE failure semantics, so the raw
            // result is kept alongside the class-1 fallback map rather than being
            // overwritten by it:
            //   * class 1 (price rescale) fails WIDE -- under-applying a rescale is
            //     the permanent error, so an unreadable inception widens the window
            //     to the bulk edge.
            //   * class 2 (renames) fails NARROW -- it skips entirely. Feeding it
            //     class-1's bulk-start sentinel would be worse than useless: a
            //     sentinel dated two years back satisfies `inception <= effective_until`
            //     for any recent alias, which is exactly the era test's failure mode.
            std::unordered_map<std::string, std::string> inception_dates;
            auto inception_result = db->get_position_inception_dates(
                kEquityStrategyId, kEquityStrategyName,
                portfolio_id, held_symbols);
            //
            // BA-9: "the read failed" and "the read succeeded but cannot account for
            // this holding" are the SAME epistemic state for class 1, and both must
            // fail wide. Only is_error() used to widen, so a successful read that
            // returned no row for a held symbol contributed nothing to the derivation
            // and left the window at its 14-day floor -- the same silent narrowing
            // this derivation replaced, reached by a different route.
            const std::string bulk_start_ymd = format_ymd_utc(bulk_start_t);
            if (inception_result.is_error()) {
                WARN("Could not read position inception dates (" +
                     std::string(inception_result.error()->what()) +
                     ") -- falling back to the full " +
                     std::to_string(max_lookback_days) + "-day price window");
                inception_dates =
                    inception_with_unknowns_widened(held_symbols, {}, bulk_start_ymd);
            } else {
                inception_dates = inception_with_unknowns_widened(
                    held_symbols, inception_result.value(), bulk_start_ymd);

                // This map is class 1's only. Class 2 never sees it: a bulk-start
                // sentinel would satisfy `inception <= effective_until` for any recent
                // alias, and even the un-widened min(date) is a previous holding's date
                // for a reused ticker (BA-2).
                const auto unexplained =
                    held_symbols_without_inception(held_symbols, inception_result.value());
                for (const auto& sym : unexplained) {
                    WARN("No inception row for held symbol " + sym +
                         " -- widening its class-1 price window to the bulk edge " +
                         bulk_start_ymd +
                         " rather than letting it sit at the floor. Class-2 renames "
                         "still skip it, which is the correct direction for them.");
                }
            }

            const auto window = derive_corp_action_window(
                today_t, kMinLookbackDays, max_lookback_days, inception_dates);
            auto window_start_t = window.start;
            // window.deep_symbols / window.deep_start no longer drive a second
            // "top up the deep holdings" read: the single raw-close read below spans
            // [window.start, today], and window.start already equals window.deep_start
            // whenever there are deep symbols. They stay on the struct because the
            // derivation tests pin them and they name the reason the window is wide.
            if (!window.deep_symbols.empty()) {
                INFO(std::to_string(window.deep_symbols.size()) +
                     " holding(s) predate the bulk price load; corp-action window opens at " +
                     format_ymd_utc(window.deep_start));
            }

            // UTC, not localtime: bar timestamps are true UTC instants, and on
            // the deployed image (TZ=America/New_York) localtime pushes every
            // key a day early.
            const std::string today_buf = format_ymd_utc(today_t);
            const std::string start_buf = format_ymd_utc(window_start_t);

            auto ca_result = db->get_per_bar_corporate_actions(
                symbols, std::string(start_buf), std::string(today_buf));
            if (ca_result.is_error()) {
                WARN("Failed to fetch per-bar corporate actions: " +
                     std::string(ca_result.error()->what()) +
                     " -- continuing without corp-action adjustments");
            } else {
                const auto& rows = ca_result.value();
                // Name the rule, not just the date. A window at the floor while
                // positions are held is the signature of deriving from
                // last_update -- which always reports "yesterday" and silently
                // collapsed the window to 14 days -- and that is
                // indistinguishable from a legitimate floor if only the date is
                // logged. Corroborating query, no new schema needed:
                //   SELECT min(ex_date) FROM trading.corp_action_applied
                //    WHERE strategy_id = '...' AND strategy_name = '...';
                // an ex_date older than 14 days can only have been applied by a
                // window that genuinely reached back.
                std::string window_reason =
                    window.source == CorpActionWindowSource::Inception
                        ? "inception of " + window.source_symbol
                        : std::to_string(kMinLookbackDays) + "-day floor; no held "
                          "position reaches further back";
                INFO("Fetched " + std::to_string(rows.size()) +
                     " PRICE_RESTATING events from per-bar columns in window [" +
                     start_buf + ", " + today_buf + "], start derived from " +
                     window_reason);

                CorporateActionsAuditLog audit_log(ca_state_dir, db,
                                                  portfolio_id,
                                                  "LIVE_EQUITY_MEAN_REVERSION",
                                                  "EQUITY_MEAN_REVERSION");
                // A file-backed log cannot consult ticker_aliases, so it cannot
                // tell that an event applied under the old ticker is the one now
                // resurfacing under the new one -- it would be applied twice.
                // The runner is the point where that becomes position damage, so
                // refuse here rather than trusting construction to stay correct.
                if (!audit_log.bridges_renames()) {
                    ERROR("Corp-action dedup log is not database-backed, so it cannot "
                          "recognise events across a ticker rename and would re-apply "
                          "them. Refusing to adjust positions.");
                    return 1;
                }
                // E2-F23: the run's OWN as-of date, not the wall clock. Every dedup
                // row this pass writes is stamped with it, and load() refuses to run
                // behind a row a LATER pass wrote -- the case the ex-date detector
                // below cannot see, because that row's ex-date is in the past.
                audit_log.set_run_date(today_date_str);
                auto dedup_loaded = audit_log.load();
                if (dedup_loaded.is_error()) {
                    // Abort rather than adjust positions against an unknown
                    // dedup state. An empty applied-set makes every is_applied()
                    // false, so every event in the window is re-applied: splits
                    // re-multiply quantity, dividends re-rescale cost basis, and
                    // the result is persisted to trading.positions. Skipping
                    // today is recoverable -- the window reaches back to
                    // position inception, so the next successful run applies
                    // what was missed. Double-applying is not recoverable.
                    ERROR("Cannot read the corp-action dedup record: " +
                          std::string(dedup_loaded.error()->what()) +
                          ". Refusing to apply corporate actions against an "
                          "unknown applied-set; re-run once the database is "
                          "reachable.");
                    return 1;
                }
                if (!dedup_loaded.value()) {
                    INFO("Corp-action dedup record read successfully and is "
                         "empty -- genuine first run for this strategy");
                }
                // E2-F23: a dedup row dated on or after today was written by a LATER pass
                // -- an un-reset replay, or a re-run of a day that applied an event with
                // today's ex-date. Honouring it would skip the event and finalize T-1 in
                // the wrong frame (the E2-F16 phantom, measured at -4,824 on BKNG).
                // A re-run is only valid when the dedup rows and the book come from the
                // same pass: reset both together, then run.
                {
                    const std::string latest_ex = audit_log.latest_applied_ex_date();
                    if (!latest_ex.empty() && latest_ex >= today_date_str) {
                        ERROR("Corp-action dedup record holds an entry with ex_date " + latest_ex +
                              " >= today (" + today_date_str + "). It was written by a later "
                              "pass over this book. Refusing to run: reset trading.corp_action_applied "
                              "for this portfolio TOGETHER with the book (positions, live_results, "
                              "equity_curve, executions) from the replay start date, then replay in "
                              "order. Never re-run a day after a later day has run.");
                        return 1;
                    }
                }

                // Dividend-denominator closes, keyed (symbol, YYYY-MM-DD).
                //
                // RAW closes, from one range read, and nothing else. Two separate
                // defects made the old construction wrong, and this single source
                // removes both:
                //
                // 1. Frame. The map was built from `all_bars`, which are ADJUSTED
                //    closes. The applier's per-event basis rescale must equal
                //    compute_backward_adjustment_factors' per-event step, and that
                //    step works in raw closes. An adjusted close already carries
                //    every LATER event in the window, so the two agree only when no
                //    later event exists. Stacked div-then-split in one catch-up
                //    batch rescaled basis by 1 + split*d/c instead of 1 + d/c:
                //    10 sh @ $100 with a $1 dividend then a 2:1 split gave 49.01
                //    against the correct 49.505.
                // 2. Range. The old deep top-up passed [deep_start, window_start) --
                //    and window.start EQUALS window.deep_start whenever deep_symbols
                //    is non-empty, because the globally-oldest inception is always
                //    itself a deep symbol. The half-open range therefore collapsed
                //    to a single day, and a holding older than the bulk load got a
                //    denominator from the wrong end of its history with no error.
                //
                // window_start_t already reaches back to the oldest inception (that
                // is what made the collapse possible), so one read over
                // [window_start, today] covers the deep holdings too: no second
                // call, no deep/bulk seam, no two frames to reconcile. The read is
                // an indexed point lookup per (symbol, date) -- the denominator
                // needs a close AT each ex-date, not a contiguous series -- and is
                // scoped to symbols that actually have events in the window, so it
                // is far cheaper than the ~25 s full-universe adjusted-series query.
                std::vector<std::string> event_symbols;
                {
                    std::set<std::string> uniq;
                    for (const auto& row : rows) {
                        if (previous_positions.find(row.ticker) != previous_positions.end()) {
                            uniq.insert(row.ticker);
                        }
                    }
                    event_symbols.assign(uniq.begin(), uniq.end());
                }

                std::unordered_map<std::string, std::map<std::string, double>>
                    close_by_symbol_date;
                if (!event_symbols.empty()) {
                    const auto denom_range = denominator_fetch_range(window, today_t);
                    INFO("Loading raw closes for " + std::to_string(event_symbols.size()) +
                         " held symbol(s) with corp-action events, from " +
                         denom_range.start + " to " + denom_range.end);

                    auto closes_result = db->get_historical_closes(
                        event_symbols, denom_range.start, denom_range.end);
                    if (closes_result.is_error()) {
                        ERROR("Could not load raw closes for the dividend denominator: " +
                              std::string(closes_result.error()->what()) +
                              ". Every dividend in this window is therefore SKIPPED and left "
                              "unapplied; none is recorded in trading.corp_action_applied, so "
                              "all of them are retried on a later run. Cost bases are stale "
                              "until then, not wrong.");
                    } else {
                        size_t added = 0;
                        for (const auto& [sym, by_date] : closes_result.value()) {
                            for (const auto& [d, close] : by_date) {
                                close_by_symbol_date[sym][d] = close;
                                ++added;
                            }
                        }
                        INFO("Added " + std::to_string(added) +
                             " historical closes for corp-action denominators");

                        // The only genuinely unrecoverable case: no closes at all
                        // for a symbol we hold and that has an event.
                        std::vector<std::string> no_data;
                        for (const auto& sym : event_symbols) {
                            auto it = close_by_symbol_date.find(sym);
                            if (it == close_by_symbol_date.end() || it->second.empty()) {
                                no_data.push_back(sym);
                            }
                        }
                        if (!no_data.empty()) {
                            std::string sym_list;
                            for (size_t i = 0; i < no_data.size(); ++i) {
                                if (i) sym_list += ", ";
                                sym_list += no_data[i];
                            }
                            ERROR("No price history exists for held symbol(s): " +
                                  sym_list +
                                  ". Dividends on them are SKIPPED and left unapplied (no "
                                  "dedup record is written, so they are retried once price "
                                  "history covers the ex-date). Splits and renames, which "
                                  "need no close, still apply. Reconcile these positions.");
                        }
                    }
                }

                // Last close strictly BEFORE ex_date (walks back over
                // weekends/holidays via the map's ordering). 0.0 if no bar.
                // Close ON ex_date -- the denominator the price series uses.
                auto close_on = [&](const std::string& symbol,
                                    const std::string& ex_date) -> double {
                    auto sym_it = close_by_symbol_date.find(symbol);
                    if (sym_it == close_by_symbol_date.end()) return 0.0;
                    auto it = sym_it->second.find(ex_date);
                    return it == sym_it->second.end() ? 0.0 : it->second;
                };

                auto close_before = [&](const std::string& symbol,
                                        const std::string& ex_date) -> double {
                    auto sym_it = close_by_symbol_date.find(symbol);
                    if (sym_it == close_by_symbol_date.end()) return 0.0;
                    const auto& by_date = sym_it->second;
                    auto it = by_date.lower_bound(ex_date);
                    if (it == by_date.begin()) return 0.0;
                    --it;
                    return it->second;
                };

                // Per-unique-ex_date positions cache for qty_at_ex_date
                // lookup (ultrareview bug_021). Queries positions at
                // ex_date - 1 (eligibility cutoff for cash dividends).
                std::unordered_map<std::string, std::unordered_map<std::string, Position>>
                    positions_at_date_cache;
                auto qty_at_ex_date = [&](const std::string& symbol,
                                          const std::string& ex_date) -> double {
                    auto cached = positions_at_date_cache.find(ex_date);
                    if (cached == positions_at_date_cache.end()) {
                        // UTC midnight, then minus one day. Parsed with
                        // parse_utc_date, NOT std::mktime: mktime reads the
                        // ex-date as LOCAL midnight, so on a host at a positive
                        // UTC offset this asked for the book two days before the
                        // ex-date instead of one (E3 mktime sites).
                        std::chrono::system_clock::time_point ex_tp;
                        if (!core::parse_utc_date(ex_date, ex_tp)) return 0.0;
                        auto tt = std::chrono::system_clock::to_time_t(ex_tp) - 24 * 60 * 60;
                        auto tp = std::chrono::system_clock::from_time_t(tt);
                        auto r = db->load_positions_by_date(
                            kEquityStrategyId, kEquityStrategyName,
                            portfolio_id, tp, "trading.positions");
                        auto& slot = positions_at_date_cache[ex_date];
                        if (r.is_ok()) slot = r.value();
                        cached = positions_at_date_cache.find(ex_date);
                    }
                    auto p = cached->second.find(symbol);
                    if (p == cached->second.end()) return 0.0;
                    return p->second.quantity.as_double();
                };

                // E2-F17 signal S2. The SAME lookup keyed on the ex-date ITSELF, not ex_date-1.
                // The two dates answer different questions and must not be conflated:
                //   ex_date - 1  -> who was on the register for the dividend CASH (above)
                //   end of ex_date -> whether the basis being restated is pre-event
                // A fill on run E is priced at close(E-1), i.e. pre-event, so a position opened
                // ON the ex-date run has a clean basis and MUST still be restated. Measuring at
                // E-1 would call it "flat at the ex-date" and skip a real adjustment.
                //
                // Parsed as UTC midnight, NOT with `std::mktime`, which reads the tm as LOCAL
                // time and then formats UTC -- on a host at a positive UTC offset that lands
                // a day early. The ex_date-1 lambda above now goes through the same helper.
                auto parse_ex_date = [](const std::string& d)
                    -> std::optional<std::chrono::system_clock::time_point> {
                    std::chrono::system_clock::time_point tp;
                    if (!core::parse_utc_date(d, tp)) return std::nullopt;
                    return tp;
                };

                std::unordered_map<std::string, std::unordered_map<std::string, Position>>
                    positions_at_exdate_cache;
                auto qty_at_end_of_ex_date = [&](const std::string& symbol,
                                                 const std::string& ex_date) -> double {
                    auto cached = positions_at_exdate_cache.find(ex_date);
                    if (cached == positions_at_exdate_cache.end()) {
                        auto parsed = parse_ex_date(ex_date);
                        if (!parsed) return 0.0;
                        auto r = db->load_positions_by_date(
                            kEquityStrategyId, kEquityStrategyName,
                            portfolio_id, *parsed, "trading.positions");
                        auto& slot = positions_at_exdate_cache[ex_date];
                        if (r.is_ok()) slot = r.value();
                        cached = positions_at_exdate_cache.find(ex_date);
                    }
                    auto p = cached->second.find(symbol);
                    if (p == cached->second.end()) return 0.0;
                    return p->second.quantity.as_double();
                };

                // E2-F17: is the book's state at the END OF THE EX-DATE actually KNOWN? Every
                // run writes a live_results row whether or not it holds anything (the same
                // evidence E2-F8 uses to tell "flat" from "never ran"), so that row is what
                // turns an empty position lookup from "unknown" into "verifiably flat".
                // Without it, absent history would be read as flat and real adjustments would
                // be dropped.
                //
                // Keyed on the EX-DATE, not ex_date-1 -- see qty_at_end_of_ex_date above for
                // why the two dates are not interchangeable.
                std::unordered_map<std::string, bool> run_on_record_cache;
                auto run_on_record_at_ex_date = [&](const std::string& ex_date) -> bool {
                    auto cached = run_on_record_cache.find(ex_date);
                    if (cached != run_on_record_cache.end()) return cached->second;

                    bool known = false;
                    if (auto parsed = parse_ex_date(ex_date)) {
                        const std::string prev_str = trade_ngin::core::format_utc_date(*parsed);
                        auto r = db->execute_query(
                            "SELECT count(*)::text FROM trading.live_results "
                            "WHERE strategy_id = '" + std::string(kEquityStrategyId) + "'"
                            " AND portfolio_id = '" + portfolio_id + "'"
                            " AND date = '" + prev_str + "'");
                        if (r.is_ok() && r.value()->num_rows() > 0) {
                            auto col = std::static_pointer_cast<arrow::StringArray>(
                                r.value()->column(0)->chunk(0));
                            if (!col->IsNull(0)) {
                                known = std::stol(std::string(col->GetView(0))) > 0;
                            }
                        }
                    }
                    run_on_record_cache[ex_date] = known;
                    return known;
                };

                // E2-F17 signal S1 (primary): the last date this strategy BOUGHT each symbol.
                // A fill on run D is priced at close(D-1), so a BUY dated strictly AFTER the
                // ex-date was struck at a price the market had already adjusted -- positive
                // evidence that the basis now held is post-event and must NOT be restated.
                // A SELL cannot re-form a long book's basis, so BUY alone is sufficient.
                //
                // One batched query for every symbol with an event, floored at the earliest
                // candidate ex-date so the scan stays small.
                std::unordered_map<std::string, std::string> last_buy_by_symbol;
                bool exec_history_readable = false;
                if (!event_symbols.empty()) {
                    std::string min_ex_date;
                    for (const auto& row : rows) {
                        if (previous_positions.find(row.ticker) == previous_positions.end()) continue;
                        if (min_ex_date.empty() || row.date_str < min_ex_date) min_ex_date = row.date_str;
                    }
                    if (!min_ex_date.empty()) {
                        // Bounded ABOVE by T-1, the date of the book being restated. Two
                        // reasons, and the first is not optional:
                        //
                        // 1. NO LOOKAHEAD. On a replay, trading.executions also holds rows for
                        //    dates after the run being processed. Unbounded, this reported
                        //    "BUY 2026-07-30" as evidence against a 2026-06-08 ex-date while
                        //    replaying June, and skipped a dividend that had to apply.
                        //
                        // 2. IT IS THE RIGHT QUESTION. The applier restates `previous_positions`
                        //    -- the end-of-T-1 book -- BEFORE today's trading. Fills dated after
                        //    T-1 have not happened yet at apply time and cannot have touched the
                        //    basis being restated. Only a fill in (ex_date, T-1] can, which is
                        //    exactly a LATE apply: the event was deferred or the symbol was
                        //    unheld when it was first offered, and the book moved in between.
                        //
                        // Without the upper bound the rule degenerates for a strategy that
                        // re-sizes daily: nearly every held name has some later BUY, so nearly
                        // every real adjustment gets skipped.
                        const std::string t1_str = trade_ngin::core::format_utc_date(previous_date);
                        auto lb = db->get_last_buy_dates(kEquityStrategyId, kEquityStrategyName,
                                                         portfolio_id, event_symbols,
                                                         min_ex_date, t1_str);
                        if (lb.is_ok()) {
                            last_buy_by_symbol = lb.value();
                            exec_history_readable = true;
                        } else {
                            WARN("E2-F17: could not read execution history for basis provenance ("
                                 + std::string(lb.error()->what()) +
                                 "). Every event this run resolves to UNKNOWN and will be APPLIED "
                                 "with a warning rather than skipped.");
                        }
                    }
                }

                // ================= E2-F31: which of these rows are really SPINOFFS =========
                //
                // A spinoff arrives in the per-bar columns wearing somebody else's clothes.
                // Tiingo puts the parent's price step in `split_factor` (FTV 2025-06-30 =
                // 1.327), or in `div_cash` (MMM 2024-04-01 = 17.3875), or nowhere at all
                // (LEN 2025-02-07). Read as what it looks like, the first mints 32.7 phantom
                // FTV shares and the second books $1,738.75 of income that never arrived --
                // and in every case the CHILD, the entire point of the event, is never
                // created.
                //
                // The only trustworthy statement that a row IS a spinoff, and of how many
                // child shares it pays, is the `spinoff` row in
                // equities_data.corporate_action -- ticker = parent, contraticker = child,
                // value = child shares per parent share. Verified 5/5 against the real
                // distributions (GE/GEV 0.25, WDC/SNDK 0.33333, FTV/RAL 0.33333, LEN/MRP 0.5,
                // MMM/SOLV 0.25). NEVER inferred from the magnitude of the step: that is
                // exactly the provenance rule E2-F9 exists to enforce, and a 1.327 split
                // factor is perfectly plausible as a real split.
                //
                // This table is frozen after 2025-08-29, so a spinoff AFTER that date is not
                // detectable at all and still takes the mangling path. That is data-blocked,
                // not code-blocked, and the tripwire for it is E2-F41's class-1-row check.
                //
                // E2-F49: a VECTOR per key, not a scalar. Fifteen (parent, ex-date) pairs in
                // this table carry more than one `spinoff` row -- RTX 2020-04-03 delivers
                // OTIS 0.5 AND CARR 1.0, HLT 2017-01-04 delivers PK 0.6 AND HGV 0.33333 --
                // and a map to one (child, ratio) let the last row read overwrite every
                // earlier one, so one arbitrary child was delivered and the rest were never
                // created at all.
                std::map<std::pair<std::string, std::string>,
                         std::vector<std::pair<std::string, double>>>
                    spinoff_terms;
                {
                    auto spin = db->get_corporate_actions(symbols, std::string(start_buf),
                                                          std::string(today_buf), {"spinoff"});
                    if (spin.is_error()) {
                        // Fail LOUD and fail CLOSED downstream: without the terms every
                        // spinoff in the window is indistinguishable from a split, and the
                        // per-bar row would be applied as one.
                        ERROR("Could not read spinoff deal terms (" +
                              std::string(spin.error()->what()) +
                              ") -- any spinoff in this window will be MISREAD as a split or a "
                              "dividend by the class-1 applier (E2-F31). Refusing to apply "
                              "corporate actions this run; re-run once the database is "
                              "reachable.");
                        return 1;
                    }
                    for (const auto& row : spin.value()) {
                        if (row.contra_ticker.empty() || row.contra_ticker == "N/A") continue;
                        if (row.contra_ticker == row.ticker) continue;
                        if (!(row.value > 0.0) || !std::isfinite(row.value)) continue;
                        auto& terms = spinoff_terms[{row.ticker, row.date_str}];
                        // Guard the feed against a duplicate row for the same child; the
                        // ratio is the vendor's and the first one read stands.
                        bool already = false;
                        for (const auto& t : terms) {
                            if (t.first == row.contra_ticker) already = true;
                        }
                        if (!already) terms.emplace_back(row.contra_ticker, row.value);
                    }
                    if (!spinoff_terms.empty()) {
                        size_t child_rows = 0;
                        for (const auto& [key, terms] : spinoff_terms)
                            child_rows += terms.size();
                        INFO("Spinoff deal terms in window: " +
                             std::to_string(spinoff_terms.size()) +
                             " (parent, ex-date) pair(s) carrying " +
                             std::to_string(child_rows) + " child ratio(s)");
                        for (const auto& [key, terms] : spinoff_terms) {
                            if (terms.size() < 2) continue;
                            std::string joined;
                            for (const auto& t : terms) {
                                if (!joined.empty()) joined += ", ";
                                joined += t.first + " x" + std::to_string(t.second);
                            }
                            INFO("Spinoff deal terms | " + key.first + " on " + key.second +
                                 " delivers " + std::to_string(terms.size()) +
                                 " children: " + joined + " (E2-F49)");
                        }
                    }
                }

                // The child's first REAL close on or after the ex-date. It prices the cash in
                // lieu of the fractional share and, under liquidate_at_first_close, the whole
                // child position. One read for every child we might receive.
                //
                // Both real 2025 children in this database (RAL, MRP) have ZERO rows in
                // equities_data.ohlcv_1d, so this map is legitimately empty for them and the
                // handler refuses the event rather than guessing a price.
                std::unordered_map<std::string, std::pair<std::string, double>>
                    child_first_close;  // child -> (date, close)
                {
                    std::vector<std::string> children;
                    std::string earliest_ex;
                    for (const auto& row : rows) {
                        auto sp = spinoff_terms.find({row.ticker, row.date_str});
                        if (sp == spinoff_terms.end()) continue;
                        if (previous_positions.find(row.ticker) == previous_positions.end())
                            continue;
                        // E2-F49: EVERY child of the pair, not the one the map happened to
                        // keep. A close missing for any one of them refuses the whole event.
                        for (const auto& t : sp->second) children.push_back(t.first);
                        if (earliest_ex.empty() || row.date_str < earliest_ex)
                            earliest_ex = row.date_str;
                    }
                    if (!children.empty()) {
                        auto ch = db->get_historical_closes(children, earliest_ex,
                                                            std::string(today_buf));
                        if (ch.is_ok()) {
                            for (const auto& [sym, series] : ch.value()) {
                                for (const auto& [d, close] : series) {  // std::map: ascending
                                    if (close > 0.0 && std::isfinite(close)) {
                                        child_first_close[sym] = {d, close};
                                        break;
                                    }
                                }
                            }
                        } else {
                            WARN("Could not load spinoff child closes (" +
                                 std::string(ch.error()->what()) +
                                 ") -- affected spinoffs are refused, not guessed.");
                        }
                    }
                }

                // ---- E2-F47 (BA-20): one BAR, one routing -------------------------
                //
                // get_per_bar_corporate_actions emits a bar's split_factor and its div_cash
                // as two separate rows carrying the same (ticker, ex_date), and seven real
                // spinoff bars in this database carry both. Matching each row against the
                // terms key on its own routed the same distribution TWICE: two SpinoffEvents
                // for one bar, the child delivered twice and its realized booked twice.
                //
                // Collect the bar's columns first, so the routing below is per BAR rather
                // than per row, and so the parent's restatement factor is the product the
                // price series actually took across the whole bar instead of one column of
                // it. The comment that used to sit at the apply site -- "which no row in
                // this database does" -- was simply wrong; the seven are named in
                // SpinoffBarColumns.
                std::map<std::pair<std::string, std::string>, SpinoffBarColumns>
                    spinoff_bar_columns;
                for (const auto& row : rows) {
                    if (spinoff_terms.find({row.ticker, row.date_str}) == spinoff_terms.end())
                        continue;
                    auto& col = spinoff_bar_columns[{row.ticker, row.date_str}];
                    if (row.action == "split" || row.action == "adrratiosplit") {
                        col.has_split = true;
                        col.split_factor = row.value;
                    } else if (row.action == "dividend") {
                        col.has_dividend = true;
                        col.dividend_cash = row.value;
                    }
                    if (!(col.close_at_ex_date > 0.0)) {
                        const double c_on = close_on(row.ticker, row.date_str);
                        col.close_at_ex_date =
                            c_on > 0.0 ? c_on : close_before(row.ticker, row.date_str);
                    }
                }
                // E2-F48 (BA-21): announce the decomposition of every spinoff bar before
                // anything is routed, so the log says which rule was applied to which bar.
                //
                // HELD SYMBOLS ONLY. `close_by_symbol_date` is loaded for symbols that both
                // have an event in the window AND are in the book (`event_symbols` filters on
                // previous_positions), so for a configured-but-not-held name every close is
                // 0.0, `dividend_factor()` falls back to exactly 1, and a dividend-encoded
                // spinoff therefore looks like a bar with no distribution factor left. That
                // produced a "SPINOFF REFUSED ... nothing to allocate" WARN about a book
                // position that does not exist, with numbers derived from a close that was
                // never fetched -- a false alarm on the loudest line in the block, and one
                // that would train a reader to skim past the real ones. The row loop below
                // already skips a non-held ticker outright, so nothing here can ever be
                // routed for one; the honest thing is silence.
                for (const auto& [key, col] : spinoff_bar_columns) {
                    if (previous_positions.find(key.first) == previous_positions.end()) continue;
                    // E2-F51: a split_factor ABOVE 1 on a spinoff ex-date is folded into the
                    // distribution's own factor rather than applied as a share-count change,
                    // and until now that decision was taken in silence. It is the right
                    // decision -- on all five such bars in this database (ABT 2004-05-03, BX
                    // 2015-10-01, K 2023-10-02, MET 2017-08-07, RTX 2020-04-03) the vendor is
                    // encoding part of the distribution in that column and the holder's share
                    // count did not move, which is verified against the adjusted series in
                    // SpinoffBarColumns -- but it is a judgement the log has to state, because
                    // a genuine forward split coincident with a spinoff would fall outside it
                    // and the only way anyone would notice is a share count that did not grow.
                    if (col.folds_a_forward_split()) {
                        WARN("Spinoff bar carries a split_factor ABOVE 1: " + key.first +
                             " on " + key.second + " has split_factor " +
                             std::to_string(col.split_factor) +
                             ", which is read as PART OF THE DISTRIBUTION and folded into the "
                             "restatement factor (" + std::to_string(col.spinoff_factor()) +
                             "), NOT applied as a share-count change -- the parent keeps its "
                             "quantity. That is what the vendor means on every such bar in "
                             "this database, verified against adjusted_close on both sides of "
                             "the bar. If this ticker really did split forward on its spinoff "
                             "ex-date, the share count is now wrong and this line is the only "
                             "warning of it (E2-F51).");
                    }
                    if (col.has_reverse_split()) {
                        WARN("Spinoff bar carries a coincident REVERSE SPLIT: " + key.first +
                             " on " + key.second + " has split_factor " +
                             std::to_string(col.split_factor) +
                             " (< 1), which is a real share-count change and NOT the "
                             "distribution's factor. It is routed to the class-1 applier "
                             "exactly as it was before the spinoff path existed; the "
                             "distribution keeps the rest of the bar's step, " +
                             std::to_string(col.total_factor()) + " / " +
                             std::to_string(col.reverse_split_factor()) + " = " +
                             std::to_string(col.spinoff_factor()) + " (E2-F48).");
                    }
                    if (!col.routes_a_spinoff()) {
                        WARN("SPINOFF REFUSED for " + key.first + " on " + key.second +
                             ": once the coincident reverse split (" +
                             std::to_string(col.reverse_split_factor()) +
                             ") is taken out of the bar's step (" +
                             std::to_string(col.total_factor()) +
                             ") there is no distribution factor left (" +
                             std::to_string(col.spinoff_factor()) +
                             " <= 1), so the price series priced NO value out of the parent "
                             "and there is nothing to allocate to the child. Delivering it "
                             "anyway would give the child a zero or negative cost basis and "
                             "book its whole first close as realized gain. Every class-1 row "
                             "on this bar is applied as it was before the spinoff path "
                             "existed; the child is NOT delivered and this book holds none "
                             "of it. No dedup row is written -- it retries every run "
                             "(E2-F48).");
                    }
                }
                for (const auto& [key, col] : spinoff_bar_columns) {
                    if (previous_positions.find(key.first) == previous_positions.end()) continue;
                    if (!col.carries_both_columns()) continue;
                    WARN("Spinoff bar carries BOTH class-1 columns: " + key.first +
                         " on " + key.second + " has split_factor " +
                         std::to_string(col.split_factor) + " AND div_cash " +
                         std::to_string(col.dividend_cash) + " (close " +
                         std::to_string(col.close_at_ex_date) +
                         "). The per-bar feed emits those as two rows on the same "
                         "(ticker, ex_date); they are ONE event and are routed once. The "
                         "factor the parent's price series took across the bar is the "
                         "product " + std::to_string(col.split_step()) + " x " +
                         std::to_string(col.dividend_factor()) + " = " +
                         std::to_string(col.total_factor()) + " (E2-F47).");
                }

                // The (ticker, ex_date) keys already routed to the spinoff handler this
                // batch. A second per-bar row for a key that has been routed is DROPPED --
                // its contribution is already inside total_factor() above.
                std::set<std::pair<std::string, std::string>> spinoff_routed_in_batch;

                // E2-F48: the reverse-split factor the class-1 applier is still going to
                // apply to a spinoff parent AFTER the distribution has restated it. The
                // spinoff runs first (the children come off the PRE-split share count), so
                // between the two the parent's basis is in an intermediate frame and the
                // G1 basis-vs-mark bound below has to know that or it fires on a correct
                // restatement: HLT's basis lands at B/1.422 while its mark is already
                // post-1-for-3, a ratio of 0.33 against a bound of 0.2.
                std::unordered_map<std::string, double> pending_class1_split_factor;

                std::vector<SpinoffEvent> spinoff_events;
                std::vector<CorpActionEvent> events;
                events.reserve(rows.size());
                // Intra-batch dedup (ultrareview bug_037): if the same
                // (ticker, ex_date, action) tuple appears twice in this fetch
                // (data-quality dupe), the persisted audit-log check above
                // wouldn't see the duplicate within the same batch yet -- we
                // must guard locally so we don't double-apply within one run.
                std::set<std::tuple<std::string, std::string, CorpActionType>> seen_in_batch;
                for (const auto& row : rows) {
                    // E2-F31: a spinoff is routed AWAY from the applier and the class-1 event
                    // is SUPPRESSED. Letting it through is the defect itself, and the two must
                    // be one decision: applying both would restate the parent twice.
                    {
                        auto sp = spinoff_terms.find({row.ticker, row.date_str});
                        const auto bar_it =
                            spinoff_bar_columns.find({row.ticker, row.date_str});
                        const bool is_spinoff_bar = sp != spinoff_terms.end() &&
                                                    bar_it != spinoff_bar_columns.end();

                        // E2-F48: which rows of a spinoff bar still belong to class 1.
                        //
                        //  - a REVERSE split row always does: it is a share-count change the
                        //    distribution does not make, and it applied correctly before the
                        //    spinoff path existed;
                        //  - EVERY row does when the bar has no distribution factor left
                        //    once the reverse split is taken out (DD 2019-06-03 / CTVA),
                        //    which is the refusal announced above.
                        //
                        // Anything else is the distribution and is routed away below.
                        const bool row_stays_class1 =
                            is_spinoff_bar &&
                            (!bar_it->second.routes_a_spinoff() ||
                             ((row.action == "split" || row.action == "adrratiosplit") &&
                              bar_it->second.has_reverse_split()));

                        if (is_spinoff_bar && !row_stays_class1) {
                            // E2-F49: every child of this (parent, ex-date), never one of
                            // them. `children_label` is only for the log lines below.
                            const auto& child_terms = sp->second;
                            std::string children_label;
                            for (const auto& t : child_terms) {
                                if (!children_label.empty()) children_label += ", ";
                                children_label += t.first;
                            }

                            // E2-F47: the bar was routed by an earlier row of the same bar.
                            // Drop this one -- its column is already inside the factor that
                            // routing used -- and keep it suppressed from the class-1 path,
                            // which is what routing the bar means.
                            if (!spinoff_routed_in_batch.insert({row.ticker, row.date_str})
                                     .second) {
                                continue;
                            }

                            if (previous_positions.find(row.ticker) == previous_positions.end())
                                continue;
                            if (audit_log.is_applied(row.ticker, row.date_str,
                                                     CorpActionType::SPINOFF))
                                continue;

                            // Same horizon gate as class 1, for the same reason: the parent's
                            // basis is divided by F, and F is the step the PRICE SERIES takes
                            // on the ex-date bar. Restating before that bar is loaded leaves
                            // basis and marks in different frames (E2-F15).
                            auto lb = last_bar_date.find(row.ticker);
                            if (lb == last_bar_date.end() || row.date_str > lb->second) {
                                WARN("Deferring SPINOFF for " + row.ticker + " -> " + children_label +
                                     " (ex_date " + row.date_str +
                                     "): its loaded price series ends " +
                                     (lb == last_bar_date.end()
                                          ? std::string("nowhere")
                                          : lb->second) +
                                     ", so the parent bars are still in PRE-event units "
                                     "(E2-F15). No dedup row is written; it applies on the "
                                     "next run once the ex-date bar is in the window.");
                                continue;
                            }

                            // E2-F17, unchanged in force. Suppressing the class-1 event also
                            // removes it from the applier's provenance gate, so the SAME test
                            // has to run here: this path divides the parent's basis by F just
                            // as a split does, and restating a basis formed AFTER the ex-date
                            // double-adjusts it. Only POSITIVE evidence may skip; UNKNOWN
                            // applies, because dropping a real distribution is worse.
                            {
                                auto lbuy = last_buy_by_symbol.find(row.ticker);
                                const bool bought_after = lbuy != last_buy_by_symbol.end() &&
                                                          lbuy->second > row.date_str;
                                const bool verifiably_flat =
                                    run_on_record_at_ex_date(row.date_str) &&
                                    !(std::abs(qty_at_end_of_ex_date(row.ticker,
                                                                     row.date_str)) > 1e-9);
                                if (bought_after || verifiably_flat) {
                                    WARN("Skipping SPINOFF for " + row.ticker + " -> " + children_label +
                                         " (ex_date " + row.date_str +
                                         "): the shares held now were acquired AFTER the "
                                         "ex-date, at a price the market had already adjusted, "
                                         "so this book received no child shares and its basis "
                                         "must not be restated. Evidence: " +
                                         (bought_after ? "BUY " + lbuy->second
                                                       : "book verifiably flat at end of "
                                                         "ex-date " + row.date_str) +
                                         " (E2-F17).");
                                    continue;
                                }
                            }

                            // F -- the factor the price series restates the parent by.
                            //
                            // E2-F47: taken from the WHOLE BAR, not from the one row that
                            // happened to be reached first. It is the product of what the
                            // class-1 path would have applied to each of the bar's columns,
                            // which is exactly the step the vendor's adjusted series takes
                            // across the bar (measured on all eight real cases -- see
                            // SpinoffBarColumns). Same columns, same denominators, same
                            // frame; only the INTERPRETATION changes.
                            //
                            // E2-F48: with the coincident reverse split, if any, taken back
                            // out -- that part is a share-count change and goes to class 1.
                            // The two together reproduce total_factor() exactly.
                            const auto& bar_columns = bar_it->second;
                            const double F = bar_columns.spinoff_factor();

                            // E2-F50: has the coincident reverse split ALREADY been applied?
                            //
                            // A refused distribution writes no dedup row, so it retries every
                            // run -- but the reverse split it was decomposed from is NOT
                            // refused with it: that half goes to class 1, applies, and IS
                            // dedup'd. The moment the child price series appears, the retry
                            // therefore runs against a book that has already been split, and
                            // two things silently go wrong:
                            //
                            //  1. the vendor's ratio is child shares per PRE-split parent
                            //     share (HLT distributed 0.6 PK and 0.33333 HGV against the
                            //     330 M shares outstanding before its 1-for-3, not after), so
                            //     applying it to the post-split 33.33 shares delivers 20 PK
                            //     instead of 60 -- a third of the entitlement, silently;
                            //  2. `pending_class1_split_factor` would be set from the bar even
                            //     though nothing is pending any more, and the G1 basis-vs-mark
                            //     bound would divide an already-post-split basis by 0.3333 a
                            //     second time -- a 3x error in the guard that exists to catch
                            //     3x errors.
                            //
                            // Rescaling the ratios by 1/F_split fixes both and is exact, not
                            // approximate: q_post = q_pre * F_split and r_post = r / F_split,
                            // so q_post * r_post == q_pre * r -- the same children, the same
                            // counts. The basis follows too, because B_post = B_pre / F_split
                            // makes the pool and the FMV weights scale together and cancel, so
                            // the retry lands on exactly the book a same-run apply would have.
                            const auto frame = bar_columns.retry_frame(
                                audit_log.is_applied(row.ticker, row.date_str,
                                                     CorpActionType::SPLIT) ||
                                audit_log.is_applied(row.ticker, row.date_str,
                                                     CorpActionType::ADR_SPLIT));
                            const double ratio_scale = frame.child_ratio_scale;
                            if (frame.pending_split_factor != 1.0) {
                                // Still pending: class 1 applies it after this block, so the
                                // children come off the PRE-split count and the G1 bound has
                                // to know a division is still to come (E2-F48).
                                pending_class1_split_factor[row.ticker] =
                                    frame.pending_split_factor;
                            }
                            if (frame.split_already_applied) {
                                WARN("Spinoff retry on an ALREADY-SPLIT book: " + row.ticker +
                                     " (ex_date " + row.date_str +
                                     ") -- the coincident reverse split " +
                                     std::to_string(bar_columns.reverse_split_factor()) +
                                     " was applied and dedup'd by an earlier run (the "
                                     "distribution was refused then and writes no dedup row of "
                                     "its own, so it retries). The vendor's ratios are per "
                                     "PRE-split share, so they are scaled by " +
                                     std::to_string(ratio_scale) +
                                     " to the post-split share count, and no class-1 split is "
                                     "pending for the basis-vs-mark bound (E2-F50).");
                            }

                            SpinoffEvent sev;
                            sev.parent = row.ticker;
                            sev.ex_date = row.date_str;
                            sev.parent_restatement_factor = F;
                            for (const auto& t : child_terms) {
                                SpinoffChildTerms ct;
                                ct.symbol = t.first;
                                ct.ratio = t.second * ratio_scale;
                                auto cf = child_first_close.find(t.first);
                                if (cf != child_first_close.end()) {
                                    ct.first_close = cf->second.second;
                                    INFO("Spinoff child price | " + t.first +
                                         " first close after ex-date " + row.date_str +
                                         " is " + std::to_string(cf->second.second) +
                                         " from " + cf->second.first);
                                } else {
                                    WARN("Spinoff child price | " + t.first +
                                         " has NO close on or after ex-date " + row.date_str +
                                         ", so the whole " + row.ticker +
                                         " distribution cannot be allocated and is refused: "
                                         "an allocation computed over a subset of the "
                                         "children is not an allocation (E2-F49).");
                                }
                                sev.children.push_back(std::move(ct));
                            }
                            spinoff_events.push_back(std::move(sev));
                            // SUPPRESSED: no CorpActionEvent is built for this row.
                            continue;
                        }
                    }

                    CorpActionType type = CorporateActionsApplier::type_from_action_string(row.action);
                    if (type == CorpActionType::UNKNOWN) continue;
                    if (audit_log.is_applied(row.ticker, row.date_str, type)) continue;
                    if (previous_positions.find(row.ticker) == previous_positions.end()) continue;
                    if (!seen_in_batch.emplace(row.ticker, row.date_str, type).second) continue;

                    // E2-F15 HORIZON GATE: only apply an event the PRICE SERIES already knows
                    // about.
                    //
                    // The applier's real contract is "restate the basis into the frame the
                    // price series is already in". Two windows made that false: the event
                    // fetch reads split_factor / div_cash from the ex-date bar ROW and so
                    // spans day D, while the bar load ends at the last completed session on a
                    // replay (`end_date = now - 24h`, :226). build_equity_adjusted_query
                    // back-adjusts a bar by the steps of every LATER bar, so with the ex-date
                    // bar outside the window there is no step to apply and the closes stay
                    // raw -- correctly, given what it was given.
                    //
                    // The position then moved into post-event units while every price stayed
                    // pre-event. Reproduced live: BKNG 25:1 on 2026-04-06 restated qty 10 ->
                    // 250 and basis 4194.31 -> 167.7724, then sold 248.799399 shares at the
                    // PRE-split 4194.31 -- $1,001,800 of realized P&L on a $100,000 book. It
                    // also mis-sized the target (1.200601 pre-split shares read as post-split)
                    // and, on a REVERSE split, inverts the stop-loss into a spurious
                    // liquidation.
                    //
                    // Deferring costs one run and is self-healing by design:
                    // derive_corp_action_window reaches back to position inception and the
                    // dedup record is durable, precisely so a deferred event is picked up
                    // later. No PositionAdjustment is produced, so no dedup row is written and
                    // the event is reconsidered next run -- the same recovery shape the
                    // dividend-denominator skip above already relies on.
                    //
                    // In TRUE LIVE this gate is a no-op: `end_date = now`, so once the ex-date
                    // bar is published it is loaded and the horizon passes. The defect lives
                    // on the replay branch.
                    //
                    // DO NOT "fix" this by rescaling the price maps instead. previous_day_ and
                    // two_days_ago_close_prices are also the inputs to finalize_previous_day,
                    // which marks previous_positions_pre_action -- the deliberately
                    // un-restated T-1 snapshot from 8a1a96ef. Rescaling them restates Day T-1
                    // by the ratio and trades a $1M error for a $40k one.
                    {
                        auto lb = last_bar_date.find(row.ticker);
                        if (lb == last_bar_date.end() || row.date_str > lb->second) {
                            WARN("Deferring " +
                                 std::string(CorporateActionsApplier::type_to_string(type)) +
                                 " for " + row.ticker + " (ex_date " + row.date_str +
                                 "): its loaded price series ends " +
                                 (lb == last_bar_date.end() ? std::string("nowhere -- the symbol "
                                  "is held but has no bars in the load window")
                                                            : lb->second) +
                                 ", so the bars are still in PRE-event units and restating the "
                                 "basis now would leave price and position in different frames "
                                 "(E2-F15). No dedup row is written; it applies on the next run "
                                 "once the ex-date bar is in the window.");
                            continue;
                        }
                    }

                    CorpActionEvent ev;
                    ev.symbol = row.ticker;
                    ev.ex_date = row.date_str;
                    ev.type = type;
                    ev.value = row.value;
                    if (type == CorpActionType::DIVIDEND) {
                        // Denominator must be THIS event's EX-DATE close, because
                        // that is the close the price series itself divides by.
                        // build_equity_adjusted_query scales every pre-dividend
                        // bar by close_D / (close_D + div_D), so a basis rescaled
                        // by 1 + div/close_(D-1) would land in a slightly
                        // different frame than the marks it is compared against
                        // -- a small systematic drift on every dividend.
                        //
                        // Drift-D: this note used to end "the dividend amount is
                        // still a raw dollar figure and the close is still an
                        // ADJUSTED one", describing the 05-22 §B6 frame mix as
                        // preserved. It is not, and has not been since the
                        // denominator map was rebuilt: `close_by_symbol_date` is
                        // loaded from ONE raw range read (see the comment above
                        // its construction), precisely because an adjusted close
                        // already carries every LATER event in the window and a
                        // stacked div-then-split batch rescaled a basis by
                        // 1 + split*d/c instead of 1 + d/c. BOTH sides of the
                        // ratio are raw now: a raw dividend over a raw ex-date
                        // close. The frame mix is gone, not preserved.
                        //
                        // Note this was previously masked: with dates formatted
                        // via localtime on a TZ=America/New_York host, keys
                        // shifted a day and close_before() happened to return the
                        // ex-date close. Fixing the timezone alone would have
                        // moved the denominator to the wrong close, which is why
                        // the two land together.
                        double c = close_on(row.ticker, row.date_str);
                        if (c <= 0.0) {
                            // No bar on the ex-date itself (suspended, holiday
                            // mislabel): the last close before it is the closest
                            // available stand-in.
                            c = close_before(row.ticker, row.date_str);
                        }
                        if (c <= 0.0) {
                            // Deliberately NO fallback to today's T-1 close. The
                            // denominator has to be the close as at the ex-date: the
                            // ratio is 1 + dividend / close_at_ex_date, so a close from
                            // a different date yields a different ratio, and the result
                            // is written into average_price and then stamped in
                            // trading.corp_action_applied, which stops the event ever
                            // being reconsidered. DD's 2025-11-03 dividend of 47.50
                            // against its ex-date close of 34.69 is a ratio of 2.3693;
                            // against a later close of 136.98 it is 1.3468, and a true
                            // basis of 100.00 is persisted as 175.92 forever.
                            //
                            // Leaving c at 0.0 makes CorporateActionsApplier skip the
                            // event (it rejects a non-positive close), which produces no
                            // PositionAdjustment, so no dedup row is recorded and the
                            // dividend is reconsidered on a later run once the closes
                            // are available. An unapplied adjustment is recoverable; a
                            // wrongly applied one is not.
                            ERROR("No close at or before ex_date " + row.date_str +
                                  " for " + row.ticker +
                                  " -- the dividend denominator cannot be established, so "
                                  "this event is SKIPPED and left unapplied. It carries no "
                                  "dedup record and will be retried once price history "
                                  "covers the ex-date.");
                        }
                        ev.close_at_ex_date = c;
                        // bug_021: prefer qty at ex_date - 1; fall back to
                        // current qty if the historical positions row is
                        // missing (e.g. first-week catch-up runs).
                        ev.qty_at_ex_date = qty_at_ex_date(row.ticker, row.date_str);
                    }

                    // E2-F17: basis provenance, resolved for EVERY class-1 type.
                    //
                    // THE PLACEMENT IS THE FIX. This previously sat inside the DIVIDEND branch
                    // above, so splits and ADR splits always reached the applier with the
                    // default and the guard could never fire on them -- leaving the
                    // catastrophic case (a 25:1 split restating a post-ex-date basis by 2400%)
                    // completely unguarded, while the dividend case looked covered.
                    //
                    // Only POSITIVE evidence may skip; everything else applies.
                    {
                        auto lb = last_buy_by_symbol.find(row.ticker);
                        const bool bought_after =
                            lb != last_buy_by_symbol.end() && lb->second > row.date_str;

                        if (bought_after) {
                            // S1: an actual BUY priced after the event.
                            ev.basis_provenance =
                                CorpActionEvent::BasisProvenance::FORMED_AFTER_EX_DATE;
                            ev.basis_provenance_evidence = "BUY " + lb->second;
                        } else if (run_on_record_at_ex_date(row.date_str) &&
                                   !(std::abs(qty_at_end_of_ex_date(row.ticker, row.date_str)) >
                                     1e-9)) {
                            // S2: a run is on record for the ex-date and the book held nothing
                            // in this symbol at the end of it, so whatever is held now was
                            // acquired later. Covers a book with no execution history.
                            ev.basis_provenance =
                                CorpActionEvent::BasisProvenance::FORMED_AFTER_EX_DATE;
                            ev.basis_provenance_evidence =
                                "book verifiably flat at end of ex-date " + row.date_str;
                        } else if (exec_history_readable) {
                            ev.basis_provenance =
                                CorpActionEvent::BasisProvenance::FORMED_ON_OR_BEFORE_EX_DATE;
                        } else {
                            ev.basis_provenance = CorpActionEvent::BasisProvenance::UNKNOWN;
                        }
                    }

                    events.push_back(std::move(ev));
                }

                // ---- E2-F31: spinoffs, BEFORE the class-1 applier ----
                //
                // Ordering matters whenever a symbol carries a spinoff AND a class-1 event
                // on the same ex-date, and it is NOT true that "no row in this database
                // does" -- the comment that used to stand here. Eight bars do: HLT
                // 2017-01-04, K 2023-10-02, MET 2017-08-07, LDOS 2013-09-30, RTX
                // 2020-04-03, ABT 2004-05-03, BX 2015-10-01 all carry split_factor AND
                // div_cash on the spinoff ex-date, and DD 2019-06-03 carries a coincident
                // reverse split (E2-F47, E2-F48).
                //
                // Spinoffs go first because they are the event that decides WHAT is held:
                // the children are distributed on the share count held BEFORE any
                // coincident split (HLT distributed 0.6 PK and 0.33333 HGV per PRE-reverse-
                // split share, then did the 1-for-3), and an ordinary dividend applying
                // afterwards rescales the correct, already-restated parent basis rather
                // than a basis about to be split.
                std::vector<LifecycleAdjustment> spinoff_log;
                if (!spinoff_events.empty()) {
                    const auto policy = spinoff_child_policy_from_string(
                        app_config.live.spinoff_child_policy);
                    INFO("Applying " + std::to_string(spinoff_events.size()) +
                         " SPINOFF event(s); spinoff_child_policy=" +
                         std::string(spinoff_child_policy_to_string(policy)));
                    spinoff_log = CorporateActionsLifecycle::apply_spinoffs(
                        previous_positions, spinoff_events, policy);

                    for (const auto& adj : spinoff_log) {
                        if (adj.outcome == LifecycleOutcome::SKIPPED_NO_CHILD_PRICE) {
                            // The class-1 event for this row was ALREADY suppressed, which is
                            // the point: the parent keeps its real share count instead of
                            // gaining phantom ones. Say plainly what the book now holds, so
                            // this is never mistaken for a clean run.
                            WARN("SPINOFF NOT APPLIED and its class-1 distribution rows "
                                 "SUPPRESSED: " + adj.symbol + " (" + adj.event_date +
                                 "). The book therefore carries " + adj.symbol +
                                 " at a PRE-spinoff cost basis (" +
                                 std::to_string(adj.avg_price_before) +
                                 ") against a POST-spinoff price series, and holds none of "
                                 "the children. Unrealized P&L on this name is overstated as "
                                 "a loss by the distributed value until the child price "
                                 "series exist. No dedup row is written -- it retries every "
                                 "run. (A coincident reverse split, if the bar carried one, "
                                 "was NOT suppressed and applied through class 1.)");
                            continue;
                        }
                        if (adj.outcome != LifecycleOutcome::SPUN_OFF_CHILD_HELD &&
                            adj.outcome != LifecycleOutcome::SPUN_OFF_CHILD_SOLD) {
                            continue;
                        }

                        // The realized figure -- cash in lieu, plus the disposal under
                        // liquidate_at_first_close -- reaches live_results and the equity
                        // curve by the same route a termination's does (E2-F7/E2-F11). It is
                        // booked against the PARENT's row: the child has no position row of
                        // its own under the default policy, and attributing it there instead
                        // would be a row for a holding the book never carried.
                        if (adj.realized_delta != 0.0) {
                            corp_action_realized_total += adj.realized_delta;
                            corp_action_realized_by_symbol[adj.symbol] += adj.realized_delta;
                        }

                        // The receipt, then the disposal, FOR EVERY CHILD. Two executions
                        // each, deterministic ids (symbol + ex-date, never a clock) so a
                        // replay of the same date regenerates the same rows and the
                        // re-insert overwrites rather than duplicates -- the CORPACTION_
                        // pattern the exits already use.
                        //
                        // A broker statement shows exactly these lines. Without them a child
                        // arrives and leaves the book with nothing to reconcile against.
                        // E2-F49: the loop is what makes RTX -> OTIS + CARR and
                        // HLT -> PK + HGV produce two receipts rather than one.
                        for (const auto& child : adj.children) {
                            const double received = child.quantity + child.fractional;
                            if (received > 1e-9 && child.avg_price > 0.0) {
                                ExecutionReport recv;
                                recv.order_id =
                                    "CORPACTION_" + child.symbol + "_" + adj.event_date;
                                recv.exec_id =
                                    "CA_" + child.symbol + "_" + adj.event_date + "_RECEIPT";
                                recv.symbol = child.symbol;
                                recv.side = Side::BUY;
                                recv.filled_quantity = Quantity(received);
                                // Priced at the ALLOCATED basis, not at the market: the
                                // shares were not bought, they were distributed, and booking
                                // them at the first close would fabricate a gain on receipt.
                                recv.fill_price = Price(child.avg_price);
                                recv.fill_time = now;
                                recv.commissions_fees = Decimal(0.0);
                                recv.implicit_price_impact = Decimal(0.0);
                                recv.slippage_market_impact = Decimal(0.0);
                                recv.total_transaction_costs = Decimal(0.0);
                                recv.is_partial = false;
                                corp_action_executions.push_back(recv);
                            }

                            const double disposed =
                                adj.outcome == LifecycleOutcome::SPUN_OFF_CHILD_SOLD
                                    ? child.quantity + child.fractional
                                    : child.fractional;   // HOLD: only the fraction is sold
                            if (disposed > 1e-9 && child.first_close > 0.0) {
                                ExecutionReport disp;
                                disp.order_id =
                                    "CORPACTION_" + child.symbol + "_" + adj.event_date;
                                disp.exec_id =
                                    "CA_" + child.symbol + "_" + adj.event_date +
                                    (adj.outcome == LifecycleOutcome::SPUN_OFF_CHILD_SOLD
                                         ? "_DISPOSAL"
                                         : "_CIL");
                                disp.symbol = child.symbol;
                                disp.side = Side::SELL;
                                disp.filled_quantity = Quantity(disposed);
                                disp.fill_price = Price(child.first_close);
                                disp.fill_time = now;
                                disp.commissions_fees = Decimal(0.0);
                                disp.implicit_price_impact = Decimal(0.0);
                                disp.slippage_market_impact = Decimal(0.0);
                                disp.total_transaction_costs = Decimal(0.0);
                                disp.is_partial = false;
                                corp_action_executions.push_back(disp);
                            }
                        }

                        // E2-F15 / G1, applied to the spinoff parent for the same reason it
                        // is applied to a split: the basis was divided by F and the price
                        // series was too, so after the restatement basis and mark must be in
                        // one frame. A wrong F -- a mis-encoded step, a terms row on the
                        // wrong date -- would otherwise restate the parent silently and
                        // nothing downstream would notice.
                        {
                            auto mk = previous_day_close_prices.find(adj.symbol);
                            double pending_split = 1.0;
                            {
                                auto ps = pending_class1_split_factor.find(adj.symbol);
                                if (ps != pending_class1_split_factor.end() &&
                                    ps->second > 0.0)
                                    pending_split = ps->second;
                            }
                            if (mk != previous_day_close_prices.end() && mk->second > 0.0 &&
                                adj.avg_price_after > 0.0) {
                                // E2-F48: compare the basis the parent will hold once the
                                // coincident reverse split has ALSO been applied, which is
                                // the frame the mark is already in.
                                const double basis_to_mark =
                                    (adj.avg_price_after / pending_split) / mk->second;
                                if (basis_to_mark > 5.0 || basis_to_mark < 0.2) {
                                    ERROR("E2-F15 guard: after the SPINOFF of {" +
                                          adj.children_joined() + "} from " + adj.symbol +
                                          " the parent cost basis (" +
                                          std::to_string(adj.avg_price_after) +
                                          ") is implausible against the mark (" +
                                          std::to_string(mk->second) + "), ratio " +
                                          std::to_string(basis_to_mark) +
                                          ". The restatement factor " +
                                          std::to_string(adj.ratio_change) +
                                          (pending_split != 1.0
                                               ? " (with the coincident class-1 split " +
                                                     std::to_string(pending_split) +
                                                     " still to be applied)"
                                               : std::string()) +
                                          " and the price series disagree. Refusing to trade.");
                                    return 1;
                                }
                            }
                        }

                        // The parent's own basis moved, so the frame it is now in must be
                        // stamped on the dedup ledger like any other class-1 restatement.
                        applied_class1_ex_date[adj.symbol] =
                            std::max(applied_class1_ex_date[adj.symbol], adj.event_date);

                        // Two dedup rows, parent and child, so a replay cannot re-distribute.
                        PositionAdjustment parent_row;
                        parent_row.symbol = adj.symbol;
                        parent_row.event_date = adj.event_date;
                        parent_row.type = CorpActionType::SPINOFF;
                        parent_row.quantity_before = adj.quantity_before;
                        parent_row.quantity_after = adj.quantity_after;
                        parent_row.avg_price_before = adj.avg_price_before;
                        parent_row.avg_price_after = adj.avg_price_after;
                        parent_row.event_value = adj.ratio_change;
                        parent_row.ratio_change = adj.ratio_change;   // F, the basis divisor
                        audit_log.record(parent_row);

                        // E2-F49: one dedup row PER CHILD, so a replay cannot
                        // re-distribute any of them and the ledger names every company the
                        // book actually received.
                        for (const auto& child : adj.children) {
                            PositionAdjustment child_row;
                            child_row.symbol = child.symbol;
                            child_row.event_date = adj.event_date;
                            child_row.type = CorpActionType::SPINOFF;
                            child_row.quantity_before = 0.0;
                            child_row.quantity_after = child.quantity;
                            child_row.avg_price_before = 0.0;
                            child_row.avg_price_after = child.avg_price;
                            child_row.event_value = child.first_close;
                            // The child's basis was CREATED here, not restated from
                            // anything, so there is no factor that inverts it. 1.0 is the
                            // honest value: the broker frame and the book frame agree on a
                            // basis nothing adjusted.
                            child_row.ratio_change = 1.0;
                            audit_log.record(child_row);
                        }
                    }
                }

                if (!events.empty() || !spinoff_log.empty()) {
                    auto adjustments = CorporateActionsApplier::apply(previous_positions, events);
                    for (const auto& adj : adjustments) {
                        INFO("Applied " + std::string(CorporateActionsApplier::type_to_string(adj.type)) +
                             " for " + adj.symbol + " (ex_date " + adj.event_date +
                             "): qty " + std::to_string(adj.quantity_before) + " -> " +
                             std::to_string(adj.quantity_after) +
                             ", avg_price " + std::to_string(adj.avg_price_before) + " -> " +
                             std::to_string(adj.avg_price_after) +
                             ", ratio " + std::to_string(adj.ratio_change));
                        audit_log.record(adj);
                    }

                    for (const auto& adj : adjustments) {
                        auto& slot = applied_class1_ex_date[adj.symbol];
                        if (slot.empty() || adj.event_date > slot) slot = adj.event_date;
                    }

                    // F-8 (docs/BROKER_BASIS_RECONCILIATION.md). The basis this run just
                    // wrote is in the ADJUSTED frame; a broker statement is not. State both
                    // numbers on the day the gap is created, so a reconciliation does not
                    // have to reconstruct the chain from ohlcv_1d months later -- and so the
                    // operator can see, from the log alone, that a dividend moved the book's
                    // basis and would not have moved the broker's.
                    for (const auto& adj : adjustments) {
                        const auto chain = audit_log.basis_chain(adj.symbol);
                        const double raw =
                            broker_frame::raw_basis(adj.avg_price_after, chain);
                        size_t dividends = 0;
                        for (const auto& ev : chain) {
                            if (broker_frame::is_dividend(ev)) ++dividends;
                        }
                        if (broker_frame::basis_is_known(raw)) {
                            INFO("F-8 basis frames | " + adj.symbol +
                                 " adjusted=" + std::to_string(adj.avg_price_after) +
                                 " raw-equivalent=" + std::to_string(raw) + " (" +
                                 std::to_string(dividends) + " dividend event(s) in the "
                                 "chain of " + std::to_string(chain.size()) + ")");
                        } else {
                            WARN("F-8 basis frames | " + adj.symbol + " adjusted=" +
                                 std::to_string(adj.avg_price_after) +
                                 " raw-equivalent=UNKNOWN: the applied chain of " +
                                 std::to_string(chain.size()) +
                                 " event(s) contains one whose basis_ratio was never "
                                 "recorded (a dedup row written before migration 006), so "
                                 "the adjusted basis cannot be inverted from the ledger. "
                                 "Recompute it from the raw ex-date closes in "
                                 "equities_data.ohlcv_1d before reconciling this symbol.");
                        }
                    }

                    // E2-F15 / G1 -- THE INVARIANT THAT WOULD HAVE CAUGHT THIS.
                    //
                    // A price-restating event rescales the basis. If the price series is in
                    // the same frame, it has been rescaled by the SAME factor, so the ratio
                    // of basis to mark is unchanged:
                    //
                    //     avg_after / mark_after  ==  avg_before / mark_before
                    //
                    // On the BKNG failure this read 167.7724/4194.31 against 4194.31/4194.31
                    // -- off by exactly the split ratio, 25. Nothing else caught it: L5,
                    // total_pnl = (realized - costs) + unrealized, equity continuity and the
                    // daily-flow identity are all INTERNAL-consistency checks, and the wrong
                    // number was consistently wrong everywhere. This is the one statement
                    // that compares the restatement against something outside itself.
                    //
                    // It generalises unchanged to dividends and to reverse splits, and it
                    // fires on any future path that rescales one side and not the other.
                    for (const auto& adj : adjustments) {
                        auto mk = previous_day_close_prices.find(adj.symbol);
                        if (mk == previous_day_close_prices.end() || !(mk->second > 0.0)) continue;
                        if (!(adj.avg_price_before > 0.0) || !(adj.avg_price_after > 0.0)) continue;

                        // The mark is NOT restated by the applier, so post-event it should
                        // already be in the same frame as the new basis. Compare the implied
                        // ratios rather than the raw numbers.
                        const double implied = (adj.avg_price_before / adj.avg_price_after);
                        if (std::abs(implied - adj.ratio_change) > 1e-6 * std::max(1.0, adj.ratio_change)) {
                            ERROR("E2-F15 invariant violated for " + adj.symbol + " (" +
                                  adj.event_date + "): basis moved by " +
                                  std::to_string(implied) + " but the event ratio is " +
                                  std::to_string(adj.ratio_change) +
                                  " -- the basis and the price series are in different frames.");
                            return 1;
                        }

                        const double basis_to_mark = adj.avg_price_after / mk->second;
                        if (basis_to_mark > 5.0 || basis_to_mark < 0.2) {
                            ERROR("E2-F15 guard: after applying " +
                                  std::string(CorporateActionsApplier::type_to_string(adj.type)) +
                                  " to " + adj.symbol + " the cost basis (" +
                                  std::to_string(adj.avg_price_after) +
                                  ") is implausible against the mark (" +
                                  std::to_string(mk->second) + "), ratio " +
                                  std::to_string(basis_to_mark) +
                                  ". The position and the price series are almost certainly in "
                                  "different unit frames. Refusing to trade on it.");
                            return 1;
                        }
                    }

                    // E2-F17 / G3 -- THE SAME BOUND, ON THE EVENTS WE REFUSED.
                    //
                    // G1 above iterates `adjustments`, i.e. events that WERE applied, so a
                    // SKIP is invariant-free. That asymmetry is dangerous now that a skip is
                    // possible for splits: wrongly refusing a real 25:1 leaves basis and mark
                    // 25x apart, which is strictly worse than the double-adjustment the skip
                    // exists to prevent, and nothing downstream would notice.
                    //
                    // A refusal is a decision, and it is checkable: if declining to restate
                    // leaves the basis visibly out of frame with the T-1 mark, the refusal was
                    // wrong. Same bounds and same fail-closed shape as the applied side.
                    {
                        std::set<std::string> applied_syms;
                        for (const auto& adj : adjustments) applied_syms.insert(adj.symbol);

                        for (const auto& ev : events) {
                            if (applied_syms.count(ev.symbol)) continue;   // it applied
                            auto pos = previous_positions.find(ev.symbol);
                            if (pos == previous_positions.end()) continue; // nothing held
                            auto mk = previous_day_close_prices.find(ev.symbol);
                            if (mk == previous_day_close_prices.end() || !(mk->second > 0.0)) continue;
                            const double basis = pos->second.average_price.as_double();
                            if (!(basis > 0.0)) continue;
                            if (!(std::abs(pos->second.quantity.as_double()) > 1e-9)) continue;

                            const double basis_to_mark = basis / mk->second;
                            if (basis_to_mark > 5.0 || basis_to_mark < 0.2) {
                                ERROR("E2-F17 guard: " + ev.symbol + " carried an unapplied " +
                                      std::string(CorporateActionsApplier::type_to_string(ev.type)) +
                                      " (ex_date " + ev.ex_date + ") and its cost basis (" +
                                      std::to_string(basis) + ") is implausible against the mark (" +
                                      std::to_string(mk->second) + "), ratio " +
                                      std::to_string(basis_to_mark) +
                                      ". Refusing to restate was almost certainly wrong -- the "
                                      "book is a corporate action out of frame. Refusing to trade.");
                                return 1;
                            }
                        }
                    }

                    // E2-F19 cleanup: nothing to persist when no event applied. The
                    // placeholder rows this block writes are dated today and carry the
                    // loaded book; writing them with zero adjustments was what let a
                    // stale book survive an empty day-T write (E2-F24).
                    //
                    // E2-F31: an APPLIED spinoff moves the parent's basis and its realized
                    // figure, so it counts as an adjustment for this purpose exactly as a
                    // split does. A REFUSED one (SKIPPED_NO_CHILD_PRICE) moved nothing and
                    // must not drag a placeholder write in behind it.
                    bool spinoff_mutated = false;
                    for (const auto& adj : spinoff_log) {
                        if (adj.outcome == LifecycleOutcome::SPUN_OFF_CHILD_HELD ||
                            adj.outcome == LifecycleOutcome::SPUN_OFF_CHILD_SOLD) {
                            spinoff_mutated = true;
                            break;
                        }
                    }
                    if (!adjustments.empty() || spinoff_mutated) {
                    // Persist corrected positions back so future runs (and
                    // tomorrow's load_positions_by_date) pick up the adjusted state.
                    // Stamp last_update = now (ultrareview bug_007): the date-keyed
                    // load filters on last_update, so without re-stamping the rows
                    // we'd write at yesterday's effective date and tomorrow's
                    // load_positions_by_date(today) would silently skip them.
                    std::vector<Position> positions_to_store;
                    positions_to_store.reserve(previous_positions.size());
                    for (const auto& [sym, pos] : previous_positions) {
                        Position p = pos;
                        p.last_update = now;
                        // E2-F19: this is a day-T placeholder for the restated book, not
                        // a P&L record. The loaded realized is T-1's flow; stamping it on
                        // a day-T row would let a placeholder that survives an empty
                        // day-T write (book fully terminated) reload tomorrow as a closed
                        // row carrying yesterday's realized, and be re-persisted at T-1
                        // forever. The day-T write below records the day's real figure.
                        p.realized_pnl = Decimal(0.0);
                        positions_to_store.push_back(std::move(p));
                    }
                    // Adjusted positions and the dedup rows that stop those
                    // events being applied again commit as ONE unit. Ordering
                    // alone (ultrareview merged_bug_001: DB first, then the
                    // dedup record) only covered the failure where the position
                    // write fails; the inverse -- positions committed, dedup
                    // write lost -- left the next run free to re-apply every
                    // event, re-multiplying quantities and re-rescaling basis.
                    // Either both land or neither does.
                    auto unit = db->begin_unit_of_work();
                    if (unit.is_error()) {
                        ERROR("Cannot open a unit of work for corp-action persistence: " +
                              std::string(unit.error()->what()));
                        return 1;
                    }
                    DbTransaction& ca_txn = *unit.value();

                    auto store_result = db->store_positions(
                        ca_txn, positions_to_store,
                        kEquityStrategyId, kEquityStrategyName,
                        portfolio_id, "trading.positions");
                    if (store_result.is_error()) {
                        ERROR("Failed to persist corp-action-adjusted positions: " +
                              std::string(store_result.error()->what()));
                        return 1;  // ca_txn rolls back on scope exit
                    }
                    auto dedup_result = audit_log.save_in(ca_txn);
                    if (dedup_result.is_error()) {
                        ERROR("Failed to persist corp-actions dedup record: " +
                              std::string(dedup_result.error()->what()) +
                              " -- rolling back the adjusted positions with it");
                        return 1;  // ca_txn rolls back on scope exit
                    }
                    auto commit_result = ca_txn.commit();
                    if (commit_result.is_error()) {
                        ERROR("Failed to commit corp-action adjustments: " +
                              std::string(commit_result.error()->what()));
                        return 1;
                    }
                    INFO("Persisted " + std::to_string(adjustments.size()) +
                         " corp-action adjustments to trading.positions");
                    } else {
                        INFO("No class-1 corp-action adjustments applied this run; nothing persisted");
                    }
                }
            }
        }

        // The book after the class-1 rescale and BEFORE the lifecycle handlers. The
        // E2-F16 restated-frame rule for the T-1 finalization needs exactly this
        // snapshot: a termination sets quantity to 0 and adds a day-T cash flow to
        // realized_pnl, and a rename or contra-merge re-keys entries -- none of which
        // happened on T-1 (E2-F19 gap G4; LiveDailyCycle::select_finalization_book).
        const std::unordered_map<std::string, Position> previous_positions_post_class1 =
            previous_positions;

        // ---- Class 2 SERIES_CONTINUITY + Class 3 TERMINATION ----
        // Both change WHAT is held rather than its price, so they run after
        // the class-1 rescale and before target generation. Failures here are
        // non-fatal: the position map is left as-is and the run continues.
        //
        // E2-F21: gated on a trading day like signals and executions are. A termination
        // with a weekend ex-date would otherwise emit an execution and book realized P&L
        // on a day the market was shut. Nothing is recorded on the skip, and the
        // admission rule (delist_date <= as_of) still admits the event on the next open
        // session, so Monday books it.
        if (!today_is_non_trading && !previous_positions.empty()) {
            auto today_t2 = std::chrono::system_clock::to_time_t(now);
            std::tm today_tm2{};
            gmtime_r(&today_t2, &today_tm2);
            char today_buf2[11];
            std::strftime(today_buf2, sizeof(today_buf2), "%Y-%m-%d", &today_tm2);
            const std::string as_of_date(today_buf2);
            // Terminations are rare and their effective dates can lag the
            // vendor's publication, so look back further than the class-1
            // 14-day window.
            auto lifecycle_start_t = today_t2 - 90 * 24 * 60 * 60;
            std::tm lifecycle_start_tm{};
            gmtime_r(&lifecycle_start_t, &lifecycle_start_tm);
            char lifecycle_start_buf[11];
            std::strftime(lifecycle_start_buf, sizeof(lifecycle_start_buf), "%Y-%m-%d",
                          &lifecycle_start_tm);

            std::vector<LifecycleAdjustment> lifecycle_log;
            // Executions synthesised from corporate actions, merged into the day's
            // executions below so they reach live_results and the equity curve (E2-F11).

            // Class 2: re-key positions still held under a superseded ticker.
            //
            // The alias table and the era dates were read once, up with the effective
            // universe (E2-F34), and are reused here on purpose: the universe this run
            // loaded bars for was computed from exactly these inputs, so a rename this
            // block performs is one the run can price.
            //
            // BA-2 / C-3 D1: the era input is the start of the CURRENT holding, NOT the
            // lifetime min(date) the class-1 window uses. Feeding class 1's answer in
            // here re-applies a rename from a previous issuer's lifetime to a position
            // re-opened under the reused ticker, moving a live holding onto a symbol
            // with no bars -- the precise failure the era test exists to prevent.
            if (!ticker_aliases_ok) {
                WARN("Ticker aliases unavailable -- skipping SERIES_CONTINUITY handling");
            } else if (!holding_start_read_ok) {
                // Fail narrow. A skipped rename is retried next run; a rename
                // applied against a guessed era re-keys a live holding onto a
                // symbol that may have no prices at all.
                WARN("Current-holding start dates unavailable -- skipping SERIES_CONTINUITY "
                     "handling this run rather than applying renames unbounded");
            } else {
                auto renames = CorporateActionsLifecycle::apply_renames(
                    previous_positions, ticker_aliases, as_of_date, holding_start_dates);
                lifecycle_log.insert(lifecycle_log.end(), renames.begin(), renames.end());
            }

            // Class 3: build termination events from the two available
            // sources. Timing comes from delisting_date (maintained); terms
            // come from the frozen corporate_action table, which returns
            // nothing today -- when it revives, the same query supplies
            // contra_ticker/ratio and the handler rolls positions over
            // instead of exiting, with no code change here.
            std::vector<TerminationEvent> terminations;
            std::unordered_map<std::string, TerminationEvent*> by_symbol;

            // BA-8: bound the read by the same 90-day termination lookback this
            // block already uses. delisting_date is keyed on the ticker and the
            // reader takes max() over all history, so without a floor a reused
            // ticker inherits a dead company's row (HPC 2008-11-24, MER
            // 2008-12-31) and exits a live position at a stale price. The
            // bars-contradict guard below cannot cover it alone:
            // delisting_is_stale() is false when last_bar is EMPTY, which is
            // precisely the symbol that stopped printing.
            //
            // Direction: missing an old termination leaves a dead position
            // carried, which is loud (the "Missing T-1 price" path). Acting on a
            // stale one closes a live position, which is silent and wrong. The
            // 90-day bound is this block's own definition of "in window".
            auto delist_result =
                db->get_delisting_dates(symbols, std::string(lifecycle_start_buf));
            if (delist_result.is_error()) {
                WARN("Failed to fetch delisting dates: " +
                     std::string(delist_result.error()->what()) +
                     " -- skipping TERMINATION timing");
            } else {
                // Same hazard as the rename era bug, one class over: delisting_date
                // is keyed on the ticker, and a reused ticker inherits the dead
                // company's row. Acting on it exits a live position at a stale
                // price. Bars settle it -- a symbol still printing after the
                // claimed delisting is plainly not delisted -- and we already hold
                // every bar of the load window, so this costs no query.
                // last_bar_date is built once, above -- see E2-F15.

                for (const auto& [symbol, delist_date] : delist_result.value()) {
                    if (previous_positions.find(symbol) == previous_positions.end()) continue;
                    if (delist_date > as_of_date) continue;  // not yet effective
                    auto lb = last_bar_date.find(symbol);
                    const std::string last_bar =
                        lb == last_bar_date.end() ? std::string() : lb->second;
                    if (CorporateActionsLifecycle::delisting_is_stale(delist_date, last_bar)) {
                        WARN("Ignoring stale delisting for " + symbol + " (delisting_date " +
                             delist_date + " but bars run to " + last_bar +
                             ") -- the ticker is trading, so the row belongs to a prior issuer");
                        continue;
                    }
                    // E2-F15 (DEFERRED, logged): if this symbol also had a class-1 event applied
                    // on THIS run, its basis is post-event while final_closes is seeded from
                    // the pre-event price map, so realized_delta would be computed across two
                    // frames. Rare -- it needs a class-1 event and a termination on the same
                    // run date -- and deferred by HD. See E2_FINDING_15 section 10.
                    TerminationEvent ev;
                    ev.symbol = symbol;
                    ev.event_date = delist_date;
                    ev.vendor_label = "delisted";
                    terminations.push_back(std::move(ev));
                }
            }

            // E2-F26: ask for the six labels whose row describes the death of its OWN
            // ticker. `acquisitionof`, `mergerfrom` and `spunofffrom` are the survivor's
            // row -- the acquirer, the surviving merger party, the spinoff child -- and
            // this query is keyed on `ticker`, so admitting them builds a TerminationEvent
            // for a company that is alive. The counterparty rows are not lost information;
            // they are a different fact (what the CONTRA ticker became) and belong to the
            // rollover path, which needs a trustworthy ratio it does not have.
            auto terms_result = db->get_corporate_actions(
                symbols, std::string(lifecycle_start_buf), as_of_date,
                vendor_labels_for_termination_keying(TerminationKeying::ROW_TICKER_TERMINATES));
            if (terms_result.is_error()) {
                WARN("Failed to fetch TERMINATION deal terms: " +
                     std::string(terms_result.error()->what()));
            } else {
                for (const auto& row : terms_result.value()) {
                    if (previous_positions.find(row.ticker) == previous_positions.end()) continue;
                    // The same admission rule again, per row, and not redundant: the
                    // filter above bounds what the QUERY returns, this bounds what the
                    // RUN acts on. It also applies the bars-contradict test the delisting
                    // loop already runs -- a terms row on a ticker still printing after
                    // its own event date belongs to a prior issuer of that symbol.
                    auto lb_terms = last_bar_date.find(row.ticker);
                    const std::string last_bar_terms =
                        lb_terms == last_bar_date.end() ? std::string() : lb_terms->second;
                    if (!CorporateActionsLifecycle::terms_row_terminates_its_ticker(
                            row.action, row.date_str, last_bar_terms)) {
                        WARN("Ignoring TERMINATION deal-terms row " + row.action + " for " +
                             row.ticker + " (" + row.date_str + ", bars run to " +
                             (last_bar_terms.empty() ? std::string("(none loaded)")
                                                     : last_bar_terms) +
                             "): it does not describe the termination of this ticker -- "
                             "either it is the survivor's row (E2-F26) or the bars "
                             "contradict it. The holding is left untouched.");
                        continue;
                    }
                    TerminationEvent ev;
                    ev.symbol = row.ticker;
                    ev.event_date = row.date_str;
                    ev.vendor_label = row.action;
                    ev.contra_ticker = row.contra_ticker;
                    // corporate_action.value is NOT a share exchange ratio. Measured over
                    // the table: acquisitionby/of average 2,051 and reach 133,846.7, and
                    // `split` reaches 10,872,442 -- these are deal values in millions. A
                    // real stock-for-stock ratio is ~0.1-3.0. Feeding it in as `ratio` would
                    // have turned 40 DFS shares into 40 x 50343.3 = 2,013,732 COF shares at
                    // a basis of $0.0039, against a real currently-trading symbol, with
                    // nothing downstream able to catch it (E2-F9).
                    //
                    // So this runner does not claim to have terms. The handler's rollover
                    // path is left fully intact and still activates the day a trustworthy
                    // deal-terms source exists -- the judgement is about PROVENANCE, not
                    // magnitude, and a plausibility band cannot separate a genuine 0.5 ratio
                    // from a $0.6m deal value. Until then every termination exits at the
                    // final real close, which is conservative and broker-reconcilable.
                    ev.ratio = 0.0;
                    ev.has_terms = false;
                    // A terms row supersedes a bare delisting for the same
                    // symbol: it says what the holding BECAME.
                    bool replaced = false;
                    for (auto& existing : terminations) {
                        if (existing.symbol == ev.symbol) {
                            existing = ev;
                            replaced = true;
                            break;
                        }
                    }
                    if (!replaced) terminations.push_back(std::move(ev));
                }
            }

            if (!terminations.empty()) {
                // A terminating symbol is, by definition, one that has STOPPED trading, so
                // it cannot be in previous_day_close_prices -- that map is built from the
                // run date's T-1 bar, and a delisted name has no bar there. Handing it in
                // alone meant every termination took the SKIPPED_NO_PRICE branch and the
                // position was left dangling with "operator review required", even though
                // the final close sits in the database (DFS: 200.05 on its last bar,
                // 2025-05-16). The data was never missing; the lookup was asking the wrong
                // question (E2-F10).
                //
                // So: start from the T-1 map, then for any terminating symbol still absent,
                // fall back to its LAST REAL CLOSE. Same principle as the execution price
                // resolver -- price from a real session, never from a basis or a guess.
                std::unordered_map<std::string, double> final_closes = previous_day_close_prices;

                std::vector<std::string> need_final;
                for (const auto& ev : terminations) {
                    if (final_closes.find(ev.symbol) == final_closes.end()) {
                        need_final.push_back(ev.symbol);
                    }
                }

                if (!need_final.empty()) {
                    // Bounded window: the event date is the last day it could have traded.
                    auto hist = db->get_historical_closes(
                        need_final, std::string(lifecycle_start_buf), as_of_date);
                    if (hist.is_ok()) {
                        for (const auto& [sym, series] : hist.value()) {
                            if (series.empty()) continue;
                            const auto& last = *series.rbegin();   // most recent real close
                            if (last.second > 0.0 && std::isfinite(last.second)) {
                                final_closes[sym] = last.second;
                                INFO("Termination price | " + sym + " has no T-1 close (it "
                                     "stopped trading); using its last real close " +
                                     std::to_string(last.second) + " from " + last.first);
                            }
                        }
                    } else {
                        WARN("Could not load final closes for terminating symbols: " +
                             std::string(hist.error()->what()) +
                             " -- those positions will be left untouched for operator review.");
                    }
                }

                auto exits = CorporateActionsLifecycle::apply_terminations(
                    previous_positions, terminations, final_closes,
                    corp_action_feed_last_date);
                lifecycle_log.insert(lifecycle_log.end(), exits.begin(), exits.end());
            }

            // Persist whenever a lifecycle handler actually moved something.
            bool mutated = false;
            for (const auto& adj : lifecycle_log) {
                if (adj.outcome == LifecycleOutcome::RENAMED ||
                    adj.outcome == LifecycleOutcome::CONVERTED_TO_CONTRA ||
                    adj.outcome == LifecycleOutcome::EXITED_AT_FINAL_CLOSE) {
                    mutated = true;
                    break;
                }
            }

            // A termination closes a real position at a real price, so it is a TRADE and
            // must be recorded as one. Without an execution row it was invisible twice
            // over (E2-F11):
            //
            //   * live_results is computed from the PnL manager and the day's executions,
            //     so a corp-action exit's realized P&L had no route into the aggregate.
            //     The position row said 122.00 and live_results said 0.00 -- rows and
            //     aggregate disagreeing, the F-D failure shape in a third place, and it
            //     never reached the equity curve either.
            //   * A broker statement shows a liquidation or cash-in-lieu event with no
            //     counterpart in trading.executions, so the two books cannot be reconciled.
            //
            // Emitting the execution closes both at once, through the path that already
            // exists, rather than adding a second private channel into the aggregate.
            //
            // exec_id/order_id are DETERMINISTIC (symbol + ex_date, not a clock) so a
            // replay of the same date regenerates the same ids, delete_stale_executions
            // matches them, and the re-insert is an overwrite rather than a duplicate.
            for (const auto& adj : lifecycle_log) {
                if (adj.outcome != LifecycleOutcome::EXITED_AT_FINAL_CLOSE) continue;
                if (std::abs(adj.quantity_before) < 1e-9 || !(adj.exit_price > 0.0)) continue;

                // E2-F7: the P&L route. Gated on the same conditions as the execution row so
                // the two can never disagree about which events were recognised.
                corp_action_realized_total += adj.realized_delta;
                corp_action_realized_by_symbol[adj.symbol] += adj.realized_delta;

                ExecutionReport ca_exec;
                ca_exec.order_id = "CORPACTION_" + adj.symbol + "_" + adj.event_date;
                ca_exec.exec_id  = "CA_" + adj.symbol + "_" + adj.event_date + "_EXIT";
                ca_exec.symbol   = adj.symbol;
                // Closing a long is a SELL; closing a short is a BUY.
                ca_exec.side = adj.quantity_before > 0.0 ? Side::SELL : Side::BUY;
                ca_exec.filled_quantity = Quantity(std::abs(adj.quantity_before));
                ca_exec.fill_price = Price(adj.exit_price);
                ca_exec.fill_time = now;
                // A corporate action is not a discretionary trade: the holder pays no
                // commission and crosses no spread, so every cost component is zero.
                ca_exec.commissions_fees = Decimal(0.0);
                ca_exec.implicit_price_impact = Decimal(0.0);
                ca_exec.slippage_market_impact = Decimal(0.0);
                ca_exec.total_transaction_costs = Decimal(0.0);
                ca_exec.is_partial = false;

                corp_action_executions.push_back(ca_exec);
                INFO("Corp-action exit booked as an execution: " + adj.symbol + " " +
                     (ca_exec.side == Side::SELL ? "SELL" : "BUY") + " " +
                     std::to_string(std::abs(adj.quantity_before)) + " @ " +
                     std::to_string(adj.exit_price) + " (realized " +
                     std::to_string(adj.realized_delta) + ", zero cost -- corporate action)");
            }
            if (mutated) {
                std::vector<Position> lifecycle_positions;
                lifecycle_positions.reserve(previous_positions.size());
                for (const auto& [sym, pos] : previous_positions) {
                    Position p = pos;
                    p.symbol = sym;
                    p.last_update = now;
                    // E2-F19: day-T placeholder, same rule as the class-1 store above.
                    // A termination's realized_delta reaches the day-T row and the
                    // aggregate through corp_action_realized_total, not through here.
                    p.realized_pnl = Decimal(0.0);
                    lifecycle_positions.push_back(std::move(p));
                }

                // E2-F13: record class-3 events in the audit/dedup log.
                //
                // audit_log.record() was called ONLY from the class-1 applier block, so a
                // termination left no row in trading.corp_action_applied at all -- no audit
                // trail, and nothing to stop a replay re-applying it. Class-1 events have
                // been deduped since the dedup table landed; class 3 never was.
                //
                // LifecycleAdjustment and PositionAdjustment are different records, so the
                // fields are mapped explicitly. event_value carries the exit price (the
                // event's defining parameter for a termination, as the split factor or
                // $/share is for class 1) and ratio_change stays 1.0 because a termination
                // restates no price.
                //
                // The dedup key is (symbol, event_date, type), and TERMINATION is a new type
                // value, so these rows cannot collide with existing SPLIT/DIVIDEND keys.
                // Its own instance, following the div_log precedent below. Safe because the
                // class-1 audit_log is long out of scope by now -- the header's "concurrent
                // instances not supported" warning is about a save() race between two LIVE
                // objects, and these are strictly sequential.
                CorporateActionsAuditLog lifecycle_audit(ca_state_dir, db,
                                                         portfolio_id,
                                                         "LIVE_EQUITY_MEAN_REVERSION",
                                                         "EQUITY_MEAN_REVERSION");
                // Stamp only, not enforce: the class-1 block above already committed
                // THIS run's dedup rows, all stamped with today's date, so enforcing
                // here would make the run refuse its own output (E2-F23). The check
                // belongs on the first load of the run, which is where it is.
                lifecycle_audit.set_run_date(as_of_date,
                                             CorporateActionsAuditLog::RunDateCheck::StampOnly);
                auto lc_loaded = lifecycle_audit.load();
                if (lc_loaded.is_error()) {
                    // Fail closed, matching the class-1 block: a dedup record that cannot be
                    // read cannot be extended safely, and writing on top of an unknown state
                    // risks re-applying a termination on the next run.
                    ERROR("Cannot read the corp-action dedup record for lifecycle events: " +
                          std::string(lc_loaded.error()->what()));
                    return 1;
                }

                std::size_t recorded = 0;
                for (const auto& adj : lifecycle_log) {
                    if (adj.outcome != LifecycleOutcome::EXITED_AT_FINAL_CLOSE &&
                        adj.outcome != LifecycleOutcome::CONVERTED_TO_CONTRA) {
                        continue;  // nothing was applied; nothing to dedup against
                    }
                    PositionAdjustment rec;
                    rec.symbol = adj.symbol;
                    rec.event_date = adj.event_date;
                    rec.type = CorpActionType::TERMINATION;
                    rec.quantity_before = adj.quantity_before;
                    rec.quantity_after = adj.quantity_after;
                    rec.avg_price_before = 0.0;
                    rec.avg_price_after = 0.0;
                    rec.event_value = adj.exit_price;
                    rec.ratio_change = 1.0;
                    lifecycle_audit.record(rec);
                    ++recorded;
                }

                // Positions and their dedup rows commit as ONE unit, for the same reason the
                // class-1 block does it: positions committed with the dedup write lost would
                // leave the next run free to re-apply the event.
                auto lc_unit = db->begin_unit_of_work();
                if (lc_unit.is_error()) {
                    ERROR("Cannot open a unit of work for lifecycle persistence: " +
                          std::string(lc_unit.error()->what()));
                    return 1;
                }
                DbTransaction& lc_txn = *lc_unit.value();

                auto store_lc = db->store_positions(
                    lc_txn, lifecycle_positions,
                    kEquityStrategyId, kEquityStrategyName,
                    portfolio_id, "trading.positions");
                if (store_lc.is_error()) {
                    ERROR("Failed to persist lifecycle-adjusted positions: " +
                          std::string(store_lc.error()->what()));
                    return 1;  // lc_txn rolls back on scope exit
                }

                if (recorded > 0) {
                    auto save_lc = lifecycle_audit.save_in(lc_txn);
                    if (save_lc.is_error()) {
                        ERROR("Failed to persist lifecycle dedup records: " +
                              std::string(save_lc.error()->what()));
                        return 1;  // lc_txn rolls back on scope exit
                    }
                }

                auto lc_commit = lc_txn.commit();
                if (lc_commit.is_error()) {
                    ERROR("Failed to commit lifecycle adjustments: " +
                          std::string(lc_commit.error()->what()));
                    return 1;
                }

                INFO("Persisted " + std::to_string(lifecycle_log.size()) +
                     " lifecycle adjustment(s) to trading.positions, " +
                     std::to_string(recorded) + " dedup record(s)");
            }
        }

        // Seed the strategy with the previous day's holdings, then generate the day's
        // signals. Both the ordering and the placement of this block are load-bearing:
        // MeanReversionStrategy::generate_signal() branches on what it currently holds,
        // so a live process -- which starts with an empty positions_ every session --
        // has to be handed the book back first, and it has to be the book as restated
        // by the corporate-action blocks above, since a split changes quantity and a
        // dividend changes cost basis. See LiveDailyCycle::prepare_strategy_for_signals.
        std::unordered_map<std::string, Position> positions;

        if (today_is_non_trading) {
            // Carry the book forward untouched. No bar closed today, so there is nothing
            // to signal from and nothing legitimate to trade against; re-deriving targets
            // from a stale bar would manufacture a weekend trade.
            positions = LiveDailyCycle::carry_forward(previous_positions);
            INFO("Non-trading day: carried " + std::to_string(positions.size()) +
                 " position(s) forward without generating signals.");
        } else {
            INFO("Seeding strategy with previous-day book...");
            auto strat_prewarm = LiveDailyCycle::prepare_strategy_for_signals(
                *mr_strategy, previous_positions);
            if (strat_prewarm.is_error()) {
                std::cerr << "Failed to seed strategy positions: "
                          << strat_prewarm.error()->what() << std::endl;
                return 1;
            }

            // Process data through portfolio pipeline (risk; optimization is off for MR).
            //
            // E2-F28: this is the ONE place the strategy is fed. process_market_data
            // calls strategy->on_data(all_bars) itself; seeding above deliberately does
            // not, because MeanReversion appends every bar it is given and a second pass
            // over the same vector doubled the price history and the ADV EMA.
            INFO("Processing data through portfolio manager (feeds the strategy once; risk)...");
            auto port_process_result = portfolio->process_market_data(all_bars);
            if (port_process_result.is_error()) {
                std::cerr << "Failed to process data in portfolio manager: "
                          << port_process_result.error()->what() << std::endl;
                return 1;
            }
            INFO("Portfolio processing completed");

            // E2-F43 (BA-17): prove the strategy ACTUALLY INGESTED today's bars.
            //
            // PortfolioManager::process_market_data (portfolio_manager.cpp:217-222) calls
            // strategy->on_data(data), and when that returns an error it LOGS the error and
            // carries straight on to get_target_positions() against un-updated instrument
            // data. For MeanReversion that yields the previous cycle's targets -- or, from a
            // process that starts with an empty instrument_data_ every session, ZERO targets
            // for every symbol. Zero targets on a held book is not "no change": it is a
            // full-book SELL, generated from a strategy that never saw a price.
            //
            // Until E2-F28 the first of the two feeds returned that error to the runner and
            // the run exited 1. Collapsing to the single feed removed the last caller that
            // checked, so the failure mode became silent. It is unreachable on valid bars
            // today (start() precedes this, and MeanReversion::on_data has no failing branch
            // once RUNNING), which is exactly why it needs an assertion rather than a hope:
            // one added throw, one strategy without on_data's early-return contract, and the
            // book is liquidated with an ERROR line in the log and exit code 0.
            //
            // The test is per symbol and exact: MeanReversionStrategy::on_data stamps
            // inst_data.last_update = bar.timestamp for every bar it accepts, so after a
            // successful feed each symbol's last_update IS the newest bar this run loaded for
            // it. A symbol with no bars at all is NOT this check's business -- it is a data
            // gap, reported by the T-1 price checks below -- so it is skipped and counted.
            {
                std::unordered_map<std::string, Timestamp> strategy_last_update;
                for (const auto& symbol : symbols) {
                    const auto* inst = mr_strategy->get_instrument_data(symbol);
                    if (inst != nullptr) strategy_last_update[symbol] = inst->last_update;
                }

                const auto feed = LiveDailyCycle::verify_strategy_ingested_bars(
                    symbols, all_bars, strategy_last_update);

                if (!feed.not_ingested.empty()) {
                    for (const auto& detail : feed.not_ingested) {
                        ERROR("FEED ASSERTION: the strategy did not ingest the loaded bars "
                              "for " + detail);
                    }
                    ERROR("Refusing to trade on a strategy that did not see this run's "
                          "prices. get_target_positions() would return stale or empty "
                          "targets, and an empty target map on a held book is a full-book "
                          "SELL, not a no-op (E2-F43). Nothing has been written for today.");
                    return 1;
                }

                if (feed.verified == 0) {
                    ERROR("FEED ASSERTION: not one symbol in the effective universe of " +
                          std::to_string(symbols.size()) +
                          " could be verified against a loaded bar, so the check is vacuous "
                          "and proves nothing about what the strategy saw. Refusing to trade "
                          "(E2-F43).");
                    return 1;
                }

                INFO("FEED ASSERTION: strategy ingested the newest loaded bar for " +
                     std::to_string(feed.verified) + "/" + std::to_string(symbols.size()) +
                     " symbol(s) in the effective universe (" + std::to_string(feed.no_bars) +
                     " carry no bars in the window and are left to the price checks below)");
            }

            // Get portfolio positions (post risk-management)
            INFO("Retrieving portfolio positions...");
            positions = portfolio->get_portfolio_positions();
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

        // A held position with no T-1 close is not a warning any more. Downstream the
        // execution path will refuse to price it from a cost basis, so it would either
        // be silently skipped or -- before that refusal existed -- filled at zero. The
        // widened lookup below rescues the ordinary cases (halts, thin names, the
        // session after a holiday) from real older closes; anything it cannot rescue is
        // a genuine data gap on a position carrying real risk, and the run must not
        // quietly price around it.
        std::vector<std::string> missing_t1_with_position;
        for (const auto& symbol : all_symbols) {
            if (previous_day_close_prices.find(symbol) == previous_day_close_prices.end()) {
                bool holds_risk = false;
                auto pos_it = positions.find(symbol);
                if (pos_it != positions.end() && pos_it->second.quantity.as_double() != 0.0) {
                    holds_risk = true;
                }
                auto prev_it = previous_positions.find(symbol);
                if (prev_it != previous_positions.end() &&
                    prev_it->second.quantity.as_double() != 0.0) {
                    holds_risk = true;
                }
                if (holds_risk) {
                    missing_t1_with_position.push_back(symbol);
                } else {
                    WARN("Missing T-1 price for symbol: " + symbol + " (no position - ignored)");
                }
            }
            if (two_days_ago_close_prices.find(symbol) == two_days_ago_close_prices.end() &&
                previous_positions.find(symbol) != previous_positions.end()) {
                WARN("Missing T-2 price for symbol: " + symbol + " (needed for PnL finalization)");
            }
        }
        if (!missing_t1_with_position.empty()) {
            std::sort(missing_t1_with_position.begin(), missing_t1_with_position.end());
            for (const auto& symbol : missing_t1_with_position) {
                ERROR("Missing T-1 price for symbol with a non-zero position: " + symbol);
            }
            INFO("Will attempt to price " + std::to_string(missing_t1_with_position.size()) +
                 " position-carrying symbol(s) from their most recent real close; any that "
                 "cannot be priced will not trade.");
        }

        // ========================================
        // STEP 1: FINALIZE YESTERDAY'S (Day T-1) PnL
        // ========================================
        INFO("STEP 1: Finalizing Day T-1 PnL using PnLManager...");

        // Check if we have T-1 price data for finalization
        if (previous_day_close_prices.empty() && !previous_positions.empty()) {
            WARN("No T-1 close prices available (likely weekend/holiday) - all positions will have 0 PnL");
            INFO("This is expected behavior when Day T-1 (" + std::to_string(std::chrono::system_clock::to_time_t(previous_date)) + ") was a non-trading day");
        }

        // LivePnLManager now uses InstrumentRegistry directly (no callback needed)
        INFO("PnLManager initialized with InstrumentRegistry access");

        double yesterday_total_pnl = 0.0;
        // E2-F1: the Day T-1 book marked at close(T-1), summed from the same finalized rows
        // that get persisted. This is what total_unrealized_pnl[T-1] must hold, so the
        // aggregate and the position rows agree by construction (protocol L5). 0.0 under
        // SETTLED, so futures could never pick up a value here even if it reached this code.
        double yesterday_finalized_unrealized = 0.0;
        std::vector<trade_ngin::Position> yesterday_finalized_positions;

        if (!two_days_ago_close_prices.empty() && !previous_positions.empty() && pnl_manager) {
            INFO("Using PnLManager to finalize Day T-1 positions...");

            // Convert map to vector for PnLManager.
            // Sourced from the PRE-corp-action snapshot: Day T-1 is finalized as the book
            // actually stood on T-1. Using the mutated `previous_positions` here wrote the
            // post-action quantities and basis onto the T-1 row, restating a day before the
            // ex-date (see the snapshot's comment above).
            //
            // E2-F15: that is right ONLY while the event's ex-date is AFTER T-1. When a
            // DEFERRED event is catching up -- ex_date <= T-1 -- the T-1 close is already
            // post-event, so finalizing the pre-action book marks a pre-event basis against a
            // post-event price. Measured: BKNG's 2026-04-06 row took
            // `1.200601 * (176.19 - 4194.31) = -4824.16` of phantom unrealized, dropping the
            // equity curve to 95,080.29 for that day. It does NOT self-correct -- running
            // 04-08 and 04-09 left the 04-06 row unchanged.
            //
            // So pick per symbol: the RESTATED book where a deferred event has now been
            // applied for a date on or before T-1, the pre-action snapshot everywhere else.
            //
            // NOTE ON REACHABILITY, so nobody reads more discrimination into this than it has.
            // The E2-F15 gate only admits an event when `ex_date <= last_bar_date`, and the
            // loaded series never reaches past T-1 under the lag model, so every APPLIED
            // class-1 event necessarily satisfies `ex_date <= T-1`. The predicate below is
            // therefore true for every symbol carrying an applied event today, and the
            // pre-action snapshot survives only for symbols with NO event -- which is the
            // overwhelming majority and the case 8a1a96ef was actually about.
            //
            // 8a1a96ef's protection is NOT weakened by that. Its regression was an event with
            // `ex_date == today` applying and restating T-1, a day before its own ex-date.
            // The gate now defers exactly that event, so the situation cannot arise upstream
            // of here. The predicate is kept explicit rather than assumed: it states the rule
            // that makes this correct, and keeps it correct if the gate's horizon ever moves.
            //
            // E2-F19 gap G4: the restated book is the POST-CLASS-1, PRE-LIFECYCLE
            // snapshot, not the fully mutated map -- a symbol that also terminated or
            // was renamed this run would otherwise be finalized onto T-1 as a qty-0 row
            // carrying a day-T cash flow. The rule lives in
            // LiveDailyCycle::select_finalization_book so it is unit-testable.
            std::vector<std::string> restated_symbols;
            std::vector<Position> prev_positions_vec = LiveDailyCycle::select_finalization_book(
                previous_positions_pre_action, previous_positions_post_class1,
                applied_class1_ex_date, core::format_utc_date(previous_date),
                &restated_symbols);
            for (const auto& symbol : restated_symbols) {
                INFO("Finalizing " + symbol + " from the RESTATED T-1 book: a " +
                     "deferred corp action for " + applied_class1_ex_date.at(symbol) +
                     " was applied this run, so the T-1 close is already post-event "
                     "and the pre-action basis would be a frame behind it (E2-F15).");
            }

            // Use PnLManager to finalize previous day
            auto finalization_result = pnl_manager->finalize_previous_day(
                prev_positions_vec,
                previous_day_close_prices,  // T-1 prices
                two_days_ago_close_prices,  // T-2 prices
                initial_capital,  // Previous portfolio value (TODO: get actual previous value)
                0.0,  // Commissions (will be handled later)
                // Equities carry a true weighted cost basis in average_price, so the
                // T-1 row records mark-to-market unrealized P&L. Futures pass no policy
                // and stay on SETTLED, where the move is entirely realized (E2-F3).
                trade_ngin::LivePnLManager::UnrealizedPolicy::MARK_TO_MARKET);

            if (finalization_result.is_ok()) {
                auto& result = finalization_result.value();
                yesterday_total_pnl = result.finalized_daily_pnl;
                yesterday_finalized_unrealized = result.finalized_unrealized_pnl;
                yesterday_finalized_positions = result.finalized_positions;

                INFO("PnLManager finalized Day T-1: Total PnL=$" + std::to_string(yesterday_total_pnl));

                // Log individual position PnLs
                for (const auto& [symbol, pnl] : result.position_realized_pnl) {
                    DEBUG("Position " + symbol + " finalized PnL: $" + std::to_string(pnl));
                }
            } else {
                ERROR("PnLManager failed to finalize Day T-1: " +
                      std::string(finalization_result.error()->what()));
                // No fallback - component is required to work
                throw std::runtime_error("PnLManager finalization failed");
            }

            // Store updated positions for yesterday (Day T-1) in database.
            //
            // E2-F19 / E2-F20: the finalizer writes the settlement move into every row's
            // realized_pnl. That is the futures definition; on this cash book it is a
            // MARK, and it overwrote the trade realized T-1's own run had recorded -- on
            // a weekend three times over, so Monday's load read Friday's mark move and
            // seeded it. The T-1 row keeps the LOADED realized (the day's own flow); the
            // finalizer's aggregate fields are untouched. The shared finalizer is not
            // edited: the futures runners persist its rows as-is.
            LiveDailyCycle::restore_loaded_realized(yesterday_finalized_positions,
                                                    previous_positions_pre_action);

            // finalize_previous_day returns a row for EVERY carried position, flat ones
            // included, so it applies the same dead-row rule as the Day-T write below:
            // a row is dropped only when it carries neither quantity nor realized. This
            // is the path that actually produced the observed dead rows (4 of the 6 had
            // zero quantity AND zero P&L), and those are still dropped. A row with zero
            // quantity and a realized figure is a close-day row and is kept.
            std::vector<Position> finalized_to_store;
            finalized_to_store.reserve(yesterday_finalized_positions.size() +
                                       previous_closed_rows.size());
            for (const auto& fp : yesterday_finalized_positions) {
                if (!LiveDailyCycle::is_dead_row(fp)) {
                    finalized_to_store.push_back(fp);
                } else {
                    DEBUG("Skipping dead Day T-1 position (no quantity, no realized): " +
                          fp.symbol);
                }
            }
            // Closed rows loaded for T-1 are re-appended verbatim -- there is nothing to
            // mark and their realized is already the day's figure. Appended AFTER the
            // finalized rows: store_positions keys its DELETE on positions[0].last_update.
            for (const auto& [symbol, closed] : previous_closed_rows) {
                finalized_to_store.push_back(closed);
            }

            // E2-F14 guard: never let a day with NO T-1 prices overwrite a day that was
            // already finalized correctly.
            //
            // store_positions is DELETE-then-INSERT keyed on (strategy, portfolio, T-1
            // date), and it runs here -- ~480 lines BEFORE the `yesterday_total_pnl != 0.0`
            // gate that protects the live_results UPDATE. So when finalization degenerates
            // (every symbol takes the no-T-1-close branch and returns 0/0), live_results is
            // spared but the position rows are silently replaced with zeros.
            //
            // Resolving t1_date from the trading calendar removes the weekend cause, but a
            // genuine data gap on a real trading day can still empty this map. In that case
            // the rows being written are all 0/0 and carry no information: the day's own run
            // already wrote the real rows, so skipping loses nothing and refusing to write
            // is strictly safer than overwriting. Failing closed here is deliberate.
            const bool have_t1_prices = !previous_day_close_prices.empty();
            if (!finalized_to_store.empty() && !have_t1_prices) {
                WARN("Refusing to write " + std::to_string(finalized_to_store.size()) +
                     " Day T-1 position rows: no T-1 close prices were resolved, so every "
                     "row is a degenerate 0/0 that would overwrite an already-finalized day");
            }
            if (!finalized_to_store.empty() && have_t1_prices) {
                // Always save yesterday's finalized positions immediately (not queued)
                // These are updates to existing positions from the previous day
                auto update_result = db->store_positions(finalized_to_store, kEquityStrategyId, kEquityStrategyName, portfolio_id, "trading.positions");
                if (update_result.is_error()) {
                    // E2-F5 follow-on: FATAL, like every other store_positions site in this
                    // runner. This one logged and carried on, so a failed T-1 position write
                    // left the run exiting 0 with the previous day's rows still holding their
                    // Day-T placeholders -- the row-versus-aggregate split that L5 exists to
                    // catch, produced by a run that looked clean.
                    //
                    // It matters more since E2-F5: store_positions now REFUSES to fall back to
                    // an unscoped delete and returns an error instead, so this path is
                    // genuinely reachable on a transient SQL fault rather than only on a
                    // broken schema. The transaction rolls back cleanly (pqxx::work destructs
                    // uncommitted), so nothing is half-written -- but the day is unfinalized
                    // and the next run would build on it.
                    ERROR("Failed to update Day T-1 positions: " + std::string(update_result.error()->what()));
                    ERROR("Day T-1 positions could not be finalized. Refusing to exit 0 with "
                          "an unfinalized book.");
                    return 1;
                } else {
                    INFO("Successfully updated " + std::to_string(finalized_to_store.size()) + " Day T-1 positions with finalized PnL");
                }
            }

            // Also UPDATE yesterday's live_results with finalized PnL
            INFO("Updating Day T-1 live_results with finalized PnL...");
            // We'll do this later after loading previous aggregates
        } else {
            INFO("Skipping Day T-1 finalization (no two_days_ago prices or no previous positions)");
        }

        // ========================================
        // STEP 2: CREATE TODAY'S (Day T) POSITIONS WITH ZERO PnL
        // ========================================
        INFO("STEP 2: Creating Day T positions with zero PnL (placeholders)...");

        double total_daily_commissions = 0.0;  // Will be calculated from executions

        // Set initial Day T position fields. average_price and unrealized_pnl will be
        // corrected after on_execution() runs below (which computes proper cost basis).
        for (auto& [symbol, current_position] : positions) {
            double yesterday_close = current_position.average_price.as_double();
            if (previous_day_close_prices.find(symbol) != previous_day_close_prices.end()) {
                yesterday_close = previous_day_close_prices[symbol];
            }

            // Set initial average_price to yesterday's close; on_execution() will
            // overwrite this with the correct weighted-average cost basis.
            current_position.average_price = Decimal(yesterday_close);
            current_position.realized_pnl = Decimal(0.0);
            current_position.unrealized_pnl = Decimal(0.0);
            current_position.last_update = now;

            DEBUG("Day T initial position for " + symbol +
                  ": qty=" + std::to_string(current_position.quantity.as_double()) +
                  " market_price=" + std::to_string(yesterday_close));
        }

        DEBUG("About to start execution generation");
        DEBUG("Previous positions size: " + std::to_string(previous_positions.size()));
        DEBUG("Current positions size: " + std::to_string(positions.size()));

        // Generate execution reports for position changes using ExecutionManager
        INFO("Using ExecutionManager to generate execution reports...");

        // Create date string for order/exec IDs
        std::stringstream date_ss;
        date_ss << std::setfill('0') << std::setw(4) << (now_tm->tm_year + 1900)
                << std::setw(2) << (now_tm->tm_mon + 1)
                << std::setw(2) << now_tm->tm_mday;
        std::string date_str = date_ss.str();

        // Price, execute, and roll back anything that could not be priced. The rules
        // and their rationale live in LiveDailyCycle::execute_day_t so that the runner
        // and its tests exercise one implementation rather than two that can drift.
        //
        // Skipped entirely on a non-trading day. `positions` was carried forward from
        // `previous_positions` above, so every delta is zero and no execution could be
        // generated anyway -- but the guard is explicit rather than incidental, because
        // "the deltas happen to be zero" is not the reason we must not trade on a day the
        // exchange is shut.
        LiveDailyCycle::ExecutionOutcome exec_outcome;
        if (today_is_non_trading) {
            INFO("Non-trading day: skipping execution generation entirely.");
        } else {
            auto day_t_exec = LiveDailyCycle::execute_day_t(
                *execution_manager, positions, previous_positions, previous_day_close_prices,
                all_bars, now, app_config.live.execution_price_max_staleness_days);

            if (day_t_exec.is_error()) {
                ERROR("ExecutionManager failed: " + std::string(day_t_exec.error()->what()));
                // No fallback - component is required to work
                throw std::runtime_error("ExecutionManager failed");
            }
            exec_outcome = day_t_exec.value();
        }

        std::vector<ExecutionReport> daily_executions = exec_outcome.executions;
        // Corporate-action exits are trades too. Appending them here -- before set_executions,
        // the PnL aggregation and the DB write -- is what gives their realized P&L a route
        // into live_results and the equity curve, and gives a broker statement something to
        // reconcile against (E2-F11).
        if (!corp_action_executions.empty()) {
            INFO("Merging " + std::to_string(corp_action_executions.size()) +
                 " corp-action exit execution(s) into the day's executions");
            daily_executions.insert(daily_executions.end(),
                                    corp_action_executions.begin(), corp_action_executions.end());
        }
        INFO("ExecutionManager generated " + std::to_string(daily_executions.size()) +
             " executions");

        for (const auto& entry : exec_outcome.widened_prices) {
            INFO("BASIS TRACE | price widened | " + entry +
                 " (no T-1 close; used most recent real session)");
        }
        for (const auto& symbol : exec_outcome.unpriced) {
            ERROR("BASIS TRACE | unpriced | " + symbol + " has no close within " +
                  std::to_string(app_config.live.execution_price_max_staleness_days) +
                  " days - it did not trade today");
        }
        for (const auto& symbol : exec_outcome.rolled_back) {
            auto prev_it = previous_positions.find(symbol);
            double carried =
                prev_it != previous_positions.end() ? prev_it->second.quantity.as_double() : 0.0;
            ERROR("BASIS TRACE | rolled back | " + symbol +
                  " day-T target discarded, book restored to carried quantity " +
                  std::to_string(carried) + " (unpriced, no execution)");
        }

        // Feed executions back to strategy for cost basis tracking.
        // BaseStrategy::on_execution() maintains weighted average_price and realized_pnl.
        //
        // E2-F11: corporate-action exits are EXCLUDED. They are synthetic accounting entries,
        // not fills the strategy should learn a basis from, and feeding them in corrupts its
        // book.
        //
        // The mechanism: apply_terminations sets pos.quantity = 0 BEFORE the strategy is
        // seeded from that same map, so by the time on_execution sees the exit the strategy
        // holds ZERO of the symbol. Its realized branch (base_strategy.cpp:197-199) requires a
        // position on the opposite side of the fill, so it books nothing -- and then the
        // `else` at :237-251 treats the SELL as OPENING A SHORT, leaving the strategy holding
        // a phantom -qty_before position in a delisted symbol at the exit price.
        //
        // It is invisible today: this runs after signal generation, and the zero-quantity
        // filter drops the row before it is persisted. But the strategy carries a fabricated
        // short for the rest of the process, and anything later that reads positions_ -- a
        // stop-loss check, a risk aggregate, a second strategy sharing the book -- would see
        // it. The realized P&L these exits lock in reaches the aggregate through
        // corp_action_realized_total instead, which is the correct route.
        for (const auto& exec : daily_executions) {
            if (exec.order_id.rfind("CORPACTION_", 0) == 0) {
                DEBUG("Skipping on_execution for corp-action exit " + exec.symbol +
                      " (synthetic entry; realized P&L booked via the day-T aggregate)");
                continue;
            }
            auto exec_result = mr_strategy->on_execution(exec);
            if (exec_result.is_error()) {
                // E2-F19: FATAL. A dropped fill leaves its cost in total_daily_commissions
                // while its realized reaches neither the row nor metrics_, and the
                // strategy's book no longer matches the executions the run has already
                // persisted. Continuing would produce a day whose rows cannot sum to its
                // aggregate for a reason no later check could name.
                ERROR("Failed to process execution for " + exec.symbol + ": " +
                      std::string(exec_result.error()->what()) +
                      ". Refusing to continue with a book that does not reflect a "
                      "persisted execution.");
                return 1;
            } else {
                DEBUG("Strategy processed execution for " + exec.symbol +
                      ": avg_price updated via on_execution()");
            }
        }

        // Now resolve each day-T position's cost basis. The rules -- strategy basis
        // from on_execution, else the carried (post-corporate-action) basis, and never
        // a mark -- and the handling of the residual live in
        // LiveDailyCycle::resolve_and_apply_basis, so the runner and its tests share
        // one implementation. Marks come from the same widened price map the fills were
        // priced at, so unrealized PnL and the executions agree on what a symbol was
        // worth today.
        auto strategy_positions = mr_strategy->get_positions();
        for (const auto& [symbol, pos] : positions) {
            if (pos.quantity.as_double() == 0.0) continue;
            auto strat_it = strategy_positions.find(symbol);
            auto carried_it = previous_positions.find(symbol);
            DEBUG("BASIS TRACE | inputs | " + symbol + " strategy=" +
                  (strat_it != strategy_positions.end()
                       ? std::to_string(strat_it->second.average_price.as_double())
                       : std::string("none")) +
                  " carried=" +
                  (carried_it != previous_positions.end()
                       ? std::to_string(carried_it->second.average_price.as_double())
                       : std::string("none")));
        }

        auto unresolved_basis = LiveDailyCycle::resolve_and_apply_basis(
            positions, strategy_positions, previous_positions, exec_outcome.execution_prices);

        for (const auto& symbol : unresolved_basis) {
            ERROR("BASIS TRACE | UNRESOLVED | " + symbol +
                  " holds a non-zero quantity with no cost basis from either the strategy "
                  "or the carried book. Recorded basis 0 and zero unrealized PnL rather "
                  "than substituting today's close. This is an upstream invariant "
                  "failure - investigate.");
        }

        // E2-F19 gap G1: a held symbol that left the configured universe was closed out
        // by execute_day_t and booked by on_execution, but has no entry in the target
        // map and so no row. Give the exit a row, or the rows cannot sum to the
        // aggregate.
        auto rowless_exits = LiveDailyCycle::add_rowless_exits(positions, strategy_positions, now);
        for (const auto& symbol : rowless_exits) {
            WARN("Closed-out symbol " + symbol + " has no day-T target entry (left the "
                 "universe?); recorded a closed row carrying its realized P&L of $" +
                 std::to_string(positions.at(symbol).realized_pnl.as_double()));
        }

        // E2-F19 (R4): a termination's realized P&L reaches the aggregate through
        // corp_action_realized_total; the synthetic exit is deliberately kept out of
        // on_execution (E2-F11), so nothing put it on the symbol's row. Add it here,
        // once, after resolve_and_apply_basis has assigned the strategy's figure with
        // `=`. On a non-trading day the terminated entry arrives through the carried
        // book (qty 0); on a trading day through the target map. Either way the row
        // exists, or is created, and the dead-row rule below keeps it because it
        // carries realized.
        for (const auto& [symbol, delta] : corp_action_realized_by_symbol) {
            if (delta == 0.0) continue;
            auto it = positions.find(symbol);
            if (it == positions.end()) {
                Position closed;
                closed.symbol = symbol;
                closed.quantity = Quantity(0.0);
                closed.average_price = Decimal(0.0);
                closed.unrealized_pnl = Decimal(0.0);
                closed.realized_pnl = Decimal(0.0);
                closed.last_update = now;
                it = positions.emplace(symbol, closed).first;
            }
            it->second.realized_pnl = Decimal(it->second.realized_pnl.as_double() + delta);
            INFO("Corp-action realized $" + std::to_string(delta) + " booked onto the " +
                 symbol + " day-T row (row now " +
                 std::to_string(it->second.realized_pnl.as_double()) + ")");
        }

        // Log final Day T position state
        for (const auto& [symbol, pos] : positions) {
            if (pos.quantity.as_double() != 0.0) {
                INFO("Day T position for " + symbol +
                     ": qty=" + std::to_string(pos.quantity.as_double()) +
                     " cost_basis=" + std::to_string(pos.average_price.as_double()) +
                     " unrealized_pnl=" + std::to_string(pos.unrealized_pnl.as_double()) +
                     " realized_pnl=" + std::to_string(pos.realized_pnl.as_double()));
            }
        }

        // Store executions in database
        if (!daily_executions.empty()) {
            INFO("Storing " + std::to_string(daily_executions.size()) + " executions to database...");
            
            for (const auto& exec : daily_executions) {
                DEBUG("Execution data - order_id: " + exec.order_id +
                      " symbol: " + exec.symbol +
                      " side: " + std::to_string(static_cast<int>(exec.side)) +
                      " qty: " + std::to_string(exec.filled_quantity) +
                      " price: " + std::to_string(exec.fill_price) +
                      " commission: " + std::to_string(exec.commissions_fees.as_double()));
            }
            // Before inserting, delete any stale executions for today with the same order_ids
            try {
                // Build unique order_id list
                std::set<std::string> unique_order_ids;
                for (const auto& exec : daily_executions) {
                    unique_order_ids.insert(exec.order_id);
                }

                if (!unique_order_ids.empty()) {
                    // Build comma-separated quoted list for SQL IN clause
                    std::ostringstream ids_ss;
                    bool first = true;
                    for (const auto& oid : unique_order_ids) {
                        if (!first) ids_ss << ", ";
                        ids_ss << "'" << oid << "'";
                        first = false;
                    }

                    // Create YYYY-MM-DD for date filter to match execution_time
                    // Convert set to vector for the new method
                    std::vector<std::string> order_ids_vector(unique_order_ids.begin(), unique_order_ids.end());

                    INFO("Deleting stale executions for today with matching order_ids: " + std::to_string(order_ids_vector.size()));

                    // Use the new delete_stale_executions method
                    // E2-F4: was a 3-arg call putting the table name in the strategy_name
                    // slot, so it deleted nothing. Now correctly scoped.
                    auto del_res = db->delete_stale_executions(
                        order_ids_vector, now, kEquityStrategyName, portfolio_id,
                        "trading.executions");
                    if (del_res.is_error()) {
                        WARN("Failed to delete stale executions: " + std::string(del_res.error()->what()));
                    } else {
                        INFO("Stale executions (if any) deleted successfully");
                    }
                }
            } catch (const std::exception& e) {
                WARN("Exception while deleting stale executions: " + std::string(e.what()));
            }

            // Use LiveResultsManager for storage
            results_manager->set_executions(daily_executions);
            INFO("Queued " + std::to_string(daily_executions.size()) + " executions for storage");
        } else {
            INFO("No executions to store (no position changes detected)");
        }
        
        std::cout << "\n======= Daily Position Report =======" << std::endl;
        std::cout << "Date: " << (now_tm->tm_year + 1900) << "-"
                  << std::setfill('0') << std::setw(2) << (now_tm->tm_mon + 1) << "-"
                  << std::setfill('0') << std::setw(2) << now_tm->tm_mday << std::endl;
        std::cout << "Total Positions: " << positions.size() << std::endl;
        std::cout << std::endl;

        // Add header for position table
        std::cout << std::setw(10) << "Symbol" << " | "
                  << std::setw(10) << "Quantity" << " | "
                  << std::setw(10) << "Mkt Price" << " | "
                  << std::setw(12) << "Notional" << " | "
                  << std::setw(10) << "Unreal PnL" << std::endl;
        std::cout << std::string(60, '-') << std::endl;

        // Use MarginManager for margin calculations
        INFO("Using MarginManager to calculate margin requirements...");

        auto margin_result = margin_manager->calculate_margin_requirements(
            positions,
            previous_day_close_prices,
            initial_capital
        );

        double gross_notional = 0.0;
        double net_notional = 0.0;
        double total_posted_margin = 0.0;  // Sum of per-contract initial margins times contracts
        double maintenance_requirement_today = 0.0;  // Sum of per-contract maintenance margins times contracts
        int active_positions = 0;

        if (margin_result.is_ok()) {
            auto& metrics = margin_result.value();
            gross_notional = metrics.gross_notional;
            net_notional = metrics.net_notional;
            active_positions = metrics.active_positions;
            total_posted_margin = metrics.total_posted_margin;
            maintenance_requirement_today = metrics.maintenance_requirement;

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
        std::cout << "Gross Notional: $" << std::fixed << std::setprecision(2) << gross_notional << std::endl;
        std::cout << "Net Notional: $" << std::fixed << std::setprecision(2) << net_notional << std::endl;
        std::cout << "Portfolio Leverage (gross/current): " << std::fixed << std::setprecision(2) 
                  << (gross_notional / initial_capital) << "x" << std::endl;
        // Posted margin should never be zero if there are active positions; enforce and warn
        if (active_positions > 0 && total_posted_margin <= 0.0) {
            ERROR("Computed posted margin is non-positive while positions are active. Check instrument metadata.");
        }
        // Equity-to-Margin Ratio = gross_notional / total_posted_margin
        // This metric shows how many times the gross notional exposure is covered by posted margin
        // Higher values indicate more leverage relative to margin requirements
        double equity_to_margin_ratio = (total_posted_margin > 0.0) ? (gross_notional / total_posted_margin) : 0.0;
        if (equity_to_margin_ratio <= 1.0 && active_positions > 0) {
            WARN("Equity-to-Margin Ratio (gross_notional / posted_margin) is <= 1.0; verify margins.");
        }

        // Save positions to database with daily PnL values
        INFO("Saving positions to database with daily PnL...");

        // E2-F35 / BA-4: the row and the aggregate must be marked from the SAME map.
        //
        // resolve_and_apply_basis marks positions[*] from exec_outcome.execution_prices --
        // the T-1 closes PLUS any widened substitute -- and live_results sums those marks.
        // The loop below recomputed each row's unrealized from previous_day_close_prices
        // alone, which by definition has NO entry for a widened symbol: that symbol's row
        // came out 0 while the aggregate did not. The in-run L5 assert reconciles realized
        // only, so the split was silent, and it fires precisely on the thin/halted names
        // the widening exists for. Rule 6 in AVERAGE_PRICE_LIFECYCLE.md says marks and
        // fills come from one map; this is that map.
        //
        // On a non-trading day execute_day_t never runs and execution_prices is empty, so
        // this is exactly previous_day_close_prices and the carry-forward path is byte for
        // byte what it was.
        const auto day_t_marks = LiveDailyCycle::day_t_mark_prices(
            previous_day_close_prices, exec_outcome.execution_prices);
        {
            size_t substituted = 0;
            for (const auto& [sym, price] : day_t_marks) {
                auto t1 = previous_day_close_prices.find(sym);
                if (t1 == previous_day_close_prices.end() || t1->second != price) ++substituted;
            }
            if (substituted > 0) {
                INFO("BASIS TRACE | row marks | " + std::to_string(substituted) +
                     " symbol(s) marked from the execution price map rather than the T-1 "
                     "close map, matching what the aggregate was marked at (E2-F35)");
            }
        }

        std::vector<trade_ngin::Position> positions_to_save;
        positions_to_save.reserve(positions.size());

        for (const auto& [symbol, position] : positions) {
            // E2-F19: a row is dropped only when it carries neither quantity nor
            // realized P&L (LiveDailyCycle::is_dead_row). The previous rule dropped on
            // quantity alone, copied from the futures runner on the premise that "the
            // exit's realized lands in live_results exactly as it does for futures".
            // The rule was identical; the consequence was not. A futures exit realizes
            // exactly zero (average_price is reset to close(T-1) daily and the fill
            // strikes at close(T-1)), so its dropped row is genuinely empty. An equity
            // exit realizes the whole accumulated gain against a true cost basis, and
            // dropping the row lost the largest figure of the position's life at row
            // level: 11 close events in the 2026-04..08 series, e.g. TMUS 2026-04-15
            // -402.65 in live_results with no row to carry it.
            //
            // A flat symbol with no trade still writes no row -- the 852-symbol
            // accumulation the old comment feared cannot occur, because a symbol that
            // closed on an earlier day carries realized 0 today (the seed is zeroed).
            // A closed row exists on the close date only.
            if (LiveDailyCycle::is_dead_row(position)) {
                DEBUG("Skipping dead position (no quantity, no realized): " + symbol);
                continue;
            }
            const bool closed_row =
                std::abs(position.quantity.as_double()) <= LiveDailyCycle::kRowTolerance;

            // Create a new position with validated values
            trade_ngin::Position validated_position;
            validated_position.symbol = position.symbol;
            validated_position.quantity = position.quantity;
            validated_position.last_update = now;  // Use current timestamp

            // For equities, track both realized and unrealized PnL
            validated_position.realized_pnl = position.realized_pnl;
            // Unrealized P&L against the position's own cost basis.
            //
            // This used to read MeanReversionInstrumentData::entry_price, which has no
            // writer anywhere in the tree -- it was always 0.0, so the guard below never
            // passed and EVERY persisted trading.positions.unrealized_pnl was written as
            // 0 while live_results.total_unrealized_pnl was nonzero: a guaranteed
            // log-versus-DB mismatch. Position::average_price is the field that is
            // actually maintained (mean_reversion.cpp names on_execution as its sole
            // writer, and the stop-loss already reads it rather than entry_price), and it
            // is corp-action adjusted, so it is the cost basis this belongs on.
            //
            // It is 0 for a position whose fill has not been processed yet, hence the
            // guard; that case persists 0, as before.
            if (position.quantity.as_double() != 0.0) {
                double current_price = 0.0;
                auto price_it = day_t_marks.find(symbol);
                if (price_it != day_t_marks.end()) {
                    current_price = price_it->second;
                }
                // Same rule AND the same price map the live_results aggregate uses, so the
                // row and the total cannot disagree (E2-F35). Equities are point_value 1.
                // The mark check stays here because current_price is 0.0 when the symbol
                // has no close at all, and measuring against 0 would book the whole
                // notional as a gain.
                validated_position.unrealized_pnl =
                    current_price > 0.0
                        ? Decimal(LivePnLManager::unrealized_from_cost_basis(
                              position.quantity.as_double(),
                              static_cast<double>(position.average_price), current_price))
                        : Decimal(0.0);
            } else {
                validated_position.unrealized_pnl = Decimal(0.0);
            }

            // Validate and convert average_price to ensure it's within Decimal limits
            double avg_price_double = static_cast<double>(position.average_price);

            // Decimal limit is approximately 92,233,720,368,547.75807
            const double DECIMAL_MAX = 9.223372036854775807e13;  // INT64_MAX / SCALE
            if (avg_price_double > DECIMAL_MAX || avg_price_double < -DECIMAL_MAX) {
                WARN("Position " + symbol + " has average_price " + std::to_string(avg_price_double) +
                     " which exceeds Decimal limit, using Day T-1 close instead");
                // Use Day T-1 close if available
                if (previous_day_close_prices.find(symbol) != previous_day_close_prices.end()) {
                    validated_position.average_price = trade_ngin::Decimal(previous_day_close_prices[symbol]);
                } else {
                    validated_position.average_price = trade_ngin::Decimal(1.0);
                }
            } else {
                try {
                    validated_position.average_price = position.average_price;
                } catch (const std::exception& e) {
                    ERROR("Failed to validate average_price for " + symbol + ": " + std::string(e.what()));
                    if (previous_day_close_prices.find(symbol) != previous_day_close_prices.end()) {
                        validated_position.average_price = trade_ngin::Decimal(previous_day_close_prices[symbol]);
                    } else {
                        validated_position.average_price = trade_ngin::Decimal(1.0);
                    }
                }
            }

            if (closed_row) {
                // No basis for a position that no longer exists: on_execution leaves the
                // exit price in average_price after a full close, and storing a price on
                // a closed row is the category error AVERAGE_PRICE_LIFECYCLE.md exists to
                // prevent. Zero reads unambiguously as "closed".
                validated_position.average_price = Decimal(0.0);
                validated_position.unrealized_pnl = Decimal(0.0);
            }

            positions_to_save.push_back(validated_position);
            INFO("Position to save: " + symbol +
                 " qty=" + std::to_string(validated_position.quantity.as_double()) +
                 " price=" + std::to_string(static_cast<double>(validated_position.average_price)) +
                 " daily_realized_pnl=" + std::to_string(static_cast<double>(validated_position.realized_pnl)) +
                 " daily_unrealized_pnl=" + std::to_string(static_cast<double>(validated_position.unrealized_pnl)));
        }
        
        // E2-C7: live has NO overnight borrow-cost accrual, so it must not carry a short.
        //
        // TransactionCostManager::calculate_overnight_borrow_fees() has exactly one caller,
        // backtest_coordinator.cpp -- the live path never accrues it. A short position held
        // live would therefore cost nothing overnight while the same position in the backtest
        // is charged, making the two silently disagree on any short book.
        //
        // Unreachable today: Equity::is_short_allowed() requires account_mode == REG_T AND
        // short_selling_allowed, and equity.hpp defaults them to CASH/false with REG_T
        // configured nowhere. That is exactly why this is a guard rather than an
        // implementation -- writing untested borrow accrual for a case that cannot occur is
        // worse than refusing the case. When shorting IS enabled (tracked as T2.10), this
        // fires on day one and the accrual gets written then, against a real book.
        {
            std::vector<std::string> shorts;
            for (const auto& p : positions_to_save) {
                if (p.quantity.as_double() < 0.0) shorts.push_back(p.symbol);
            }
            if (!shorts.empty()) {
                std::string joined;
                for (const auto& sym : shorts) joined += (joined.empty() ? "" : ", ") + sym;
                ERROR("Refusing to persist a short equity position: live accrues no overnight "
                      "borrow cost, so this book would be under-charged against its own "
                      "backtest. Symbols: " + joined);
                ERROR("Enable borrow-fee accrual in the live EOD path before allowing shorts "
                      "(see TransactionCostManager::calculate_overnight_borrow_fees).");
                return 1;
            }
        }

        // E2-F19 (R-3): clear today's position rows for this book BEFORE queueing the
        // day-T write, unconditionally.
        //
        // store_positions is DELETE-then-INSERT keyed on the date of the rows it is
        // handed, and save_positions_snapshot returns early on an empty book -- so a day
        // whose day-T write is EMPTY never deletes anything, and whatever rows already
        // sit on today's date survive as today's book. Two writers put rows there
        // before this point: the corp-action placeholder stores (dated today, written
        // even when zero adjustments applied) and, through the loader's timestamp
        // drift, a T-1 row that has been rewritten four times. Measured on the pre-fix
        // chain: TMUS closed on 2026-07-07 with no realized row to keep, the
        // placeholder row (qty 17.6) survived as the 07-07 book, and the runner sold
        // the same 17.6 shares again every day through 07-27 while booking mark P&L on
        // stock it no longer held. Scoped exactly like store_positions' own delete.
        // E2-F44 (BA-18): the DELETE itself now runs immediately before the write it
        // protects, NOT here. Both in-run L5 identities are fatal, and both used to fire
        // AFTER this statement had already removed today's rows -- so a violated identity
        // left the day EMPTY as well as unwritten. Moved down; see the comment at the
        // clear-then-write site further below.

        if (!positions_to_save.empty()) {
            INFO("Attempting to save " + std::to_string(positions_to_save.size()) + " positions to database");
            DEBUG("Database connection status: " + std::string(db->is_connected() ? "connected" : "disconnected"));

            // Use LiveResultsManager for storage - set today's positions
            results_manager->set_positions(positions_to_save);
            INFO("Queued " + std::to_string(positions_to_save.size()) + " current positions for storage");
        } else {
            INFO("No positions to save (all positions are zero); today's rows cleared above");
        }

        // Compute portfolio-level snapshot metrics using RiskManager on today's state
        INFO("Retrieving strategy metrics...");
        trade_ngin::RiskManager snapshot_rm(risk_config);
        auto market_data_snapshot = snapshot_rm.create_market_data(all_bars);
        auto risk_eval = snapshot_rm.process_positions(positions, market_data_snapshot);

        std::cout << "\n======= Strategy Metrics =======" << std::endl;
        if (risk_eval.is_ok()) {
            const auto& r = risk_eval.value();
            // Use portfolio_var as annualized volatility proxy
            std::cout << "Volatility: " << std::fixed << std::setprecision(2)
                      << (r.portfolio_var * 100.0) << "%" << std::endl;
            std::cout << "Gross Leverage: " << std::fixed << std::setprecision(2)
                      << r.gross_leverage << std::endl;
            std::cout << "Net Leverage: " << std::fixed << std::setprecision(2)
                      << r.net_leverage << std::endl;
            std::cout << "Max Correlation: " << std::fixed << std::setprecision(2)
                      << r.correlation_risk << std::endl;
            std::cout << "Jump Risk (99th): " << std::fixed << std::setprecision(2)
                      << r.jump_risk << std::endl;
            std::cout << "Risk Scale: " << std::fixed << std::setprecision(2)
                      << r.recommended_scale << std::endl;
        } else {
            std::cout << "Volatility: N/A" << std::endl;
            std::cout << "Gross Leverage: N/A" << std::endl;
            std::cout << "Net Leverage: N/A" << std::endl;
            std::cout << "Max Correlation: N/A" << std::endl;
            std::cout << "Jump Risk (99th): N/A" << std::endl;
            std::cout << "Risk Scale: N/A" << std::endl;
        }
        // ========================================
        // STEP 3: CALCULATE COMMISSIONS AND Day T PnL (ZERO)
        // ========================================
        INFO("STEP 3: Calculating commissions and Day T PnL...");

        // E2-C4: charge the FULL modelled transaction cost, not commission alone.
        //
        // This summed exec.commissions_fees while both futures runners sum
        // exec.total_transaction_costs (live_portfolio_conservative.cpp:1445,
        // live_portfolio.cpp:1431), so spread and market impact were computed, stored on
        // the execution row, and then silently dropped from P&L and the equity curve.
        // Measured 2026-07-24..08-03: executions totalled $16.0801, live_results recorded
        // $15.00 -- $1.0801 of real slippage never charged. It is also why live disagreed
        // with its own backtest, which does charge the full figure
        // (backtest_coordinator.cpp:777).
        //
        // Slippage IS a true cost here and subtracting it is NOT a double-count: fill_price
        // is deliberately kept as the clean reference price with no embedded slippage
        // (execution_manager.cpp:159, backtest_execution_manager.cpp:82), so the modelled
        // figure is the ONLY representation of execution cost in the system. Note
        // total_transaction_costs = commissions_fees + slippage_market_impact;
        // implicit_price_impact is the per-share intermediate, already inside slippage, and
        // must not be added again.
        for (const auto& exec : daily_executions) {
            total_daily_commissions += exec.total_transaction_costs.as_double();
        }
        INFO("Total daily commissions: $" + std::to_string(total_daily_commissions));

        // Day T PnL is ZERO (placeholder) - positions were just opened at Day T-1 close
        // Update PnLManager with today's positions (all with 0 PnL as placeholders)
        for (const auto& [symbol, position] : positions) {
            pnl_manager->update_position_pnl(symbol, 0.0, 0.0);  // Zero PnL for Day T
        }

        // E2-F1: today's TRADE-realized P&L, gross of costs.
        //
        // Realized P&L does NOT need the T-1 lag. A closing fill on day D is priced at
        // close(D-1) and the P&L it locks in is known the moment it fills, so it belongs on
        // day D's row and is written here at insert. Only the MARK needs the next day's
        // close, and that is what the Day T-1 UPDATE now supplies.
        //
        // Before this, daily_realized_pnl was a literal 0.0 here and the T-1 UPDATE
        // overwrote it with finalize_previous_day()'s output -- the whole book's daily mark
        // move, `sum qty*(close(T-1) - close(T-2))`, booked as "realized" even for positions
        // that never traded. That is the futures settlement model, correct there because a
        // futures position genuinely settles daily, and wrong for a cash equity book. It is
        // also what made total_pnl double-count once E2-F12 layered the open mark on top:
        // cumulative "realized" ALREADY contained the full move from entry, so adding
        // unrealized measured from the same cost basis counted it twice. Verified against
        // hand-computed P&L from raw executions and closes: cumulative daily_realized_pnl
        // tracked true mark-to-market P&L exactly, which is only possible if it was already
        // the whole thing.
        //
        // metrics_.realized_pnl accumulates `sum(trade realized) - sum(costs)` over THIS
        // process's executions only (it starts at zero each run and seeding does not touch
        // it), so adding the day's costs back recovers the gross figure. Gross is what gets
        // reported, with costs in their own column, exactly as futures does it -- the
        // consumer subtracts once, in daily_pnl below.
        double daily_realized_pnl = 0.0;
        if (mr_strategy) {
            daily_realized_pnl = mr_strategy->get_metrics().realized_pnl + total_daily_commissions;
        }

        // E2-F7: add the realized P&L locked in by corporate actions on this ex-date.
        //
        // A termination's gain or loss is genuine trade-realized P&L -- the position is gone
        // and the proceeds are final -- but it never passes through BaseStrategy::on_execution
        // in a way that books it. apply_terminations sets quantity = 0 BEFORE the strategy is
        // seeded, so on_execution's realized branch (base_strategy.cpp:197-199, which requires
        // a position on the opposite side of the fill) is false and computes nothing.
        // metrics_.realized_pnl above therefore misses it entirely.
        //
        // Adding it here puts it in daily_realized_pnl, and from there it flows through the
        // existing arithmetic into daily_pnl, total_realized_pnl, total_pnl,
        // current_portfolio_value and the equity curve -- no new column, no second channel,
        // no cross-day persistence.
        if (corp_action_realized_total != 0.0) {
            INFO("Corp-action realized P&L booked into Day T aggregate: $" +
                 std::to_string(corp_action_realized_total));
            daily_realized_pnl += corp_action_realized_total;
        }

        // E2-F19 (R5): protocol L5's realized clause, asserted inside the run.
        //
        // The rows and the aggregate come from the SAME accumulator -- pos.realized_pnl
        // and metrics_.realized_pnl are incremented in the same branch of on_execution
        // from the same expression; the rows are the decomposition by symbol, the
        // aggregate the sum (plus the day's costs added back, plus corp-action realized,
        // which R4 put on the rows too). So the residual is identically zero unless
        // something broke the identity: a fill whose on_execution failed (now fatal
        // above), a held symbol absent from the target map (add_rowless_exits above),
        // or a future filter that drops a row carrying realized. Both previous attempts
        // at this column shipped without this check and were reverted after a replay
        // found 20 and 32 violating days. This is the difference between "L5 passed on
        // the days we checked" and "L5 cannot fail without the run failing".
        //
        // Tolerance 1e-4 absolute: Decimal is fixed-point at 1e-8 with per-fill
        // rounding on the row side while metrics_ is a double, so the sides differ by
        // up to ~5e-9 per fill.
        {
            double row_realized_sum = 0.0;
            std::string breakdown;
            for (const auto& p : positions_to_save) {
                const double r = static_cast<double>(p.realized_pnl);
                row_realized_sum += r;
                if (std::abs(r) > LiveDailyCycle::kRowTolerance) {
                    breakdown += " " + p.symbol + "=" + std::to_string(r);
                }
            }
            const double residual = row_realized_sum - daily_realized_pnl;
            if (std::abs(residual) > 1e-4) {
                ERROR("L5 realized identity VIOLATED: sum of positions.daily_realized_pnl (" +
                      std::to_string(row_realized_sum) + ") != live_results.daily_realized_pnl (" +
                      std::to_string(daily_realized_pnl) + "), residual " +
                      std::to_string(residual) + ". Per-symbol rows:" +
                      (breakdown.empty() ? std::string(" <none>") : breakdown) +
                      ". Refusing to persist a day whose rows do not sum to its aggregate.");
                // E2-F44 (BA-18): say what this exit leaves behind, because "the run failed"
                // and "the database is unchanged" are not the same statement.
                ERROR("STATE AT THIS EXIT (" + today_date_str + "): NOTHING has been written "
                      "for day T -- its position rows are untouched (the clear-then-write "
                      "pair runs later), and live_results, equity_curve, executions and "
                      "signals for today are never written on this path. ALREADY WRITTEN and "
                      "NOT rolled back: trading.positions for T-1 (" +
                      core::format_utc_date(previous_date) +
                      ", the finalization at close(T-1)), any trading.corp_action_applied "
                      "dedup rows this run committed with their day-T placeholder positions, "
                      "and the stale-execution DELETE for today's order ids. RECOVERY: fix "
                      "the cause and re-run THIS SAME DATE, which re-finalizes T-1 and "
                      "rewrites day T. If the re-run will not take, reset the book WINDOWED "
                      "from " + today_date_str + " -- positions, live_results, equity_curve, "
                      "executions, signals AND corp_action_applied together (replay rule 7) "
                      "-- and replay forward from there. Never leave this date and run the "
                      "next one (replay rule 8).");
                return 1;
            }
            INFO("L5 realized identity holds: rows " + std::to_string(row_realized_sum) +
                 " == aggregate " + std::to_string(daily_realized_pnl) +
                 " (residual " + std::to_string(residual) + ")");
        }

        // daily_unrealized_pnl used to be declared and pinned to 0.0 here as a Day-T placeholder.
        // Nothing ever repaired it -- the Day T-1 UPDATE below never mentions the column -- so
        // every equity row carried 0 even though the book had open marks (E2-F12). It is now
        // computed for real in STEP 5, once the previous day's mark is known.
        //
        // The reported flow is `(realized - costs) + change in mark`, mirroring futures'
        // `gross - costs`. Costs are subtracted exactly once, here.
        double daily_pnl_for_today = daily_realized_pnl - total_daily_commissions;

        INFO("Day T trade-realized (gross): $" + std::to_string(daily_realized_pnl));
        INFO("Day T transaction costs: $" + std::to_string(total_daily_commissions));
        INFO("Day T realized net of costs: $" + std::to_string(daily_pnl_for_today));

        // ========================================
        // STEP 4: UPDATE Day T-1 live_results AND equity_curve WITH FINALIZED PnL
        // ========================================
        // Skip if this is the first trading day (no previous positions to finalize)
        bool is_first_trading_day = previous_positions.empty() ||
                                     (previous_positions.size() > 0 &&
                                      std::all_of(previous_positions.begin(), previous_positions.end(),
                                                 [](const auto& p) { return p.second.quantity.as_double() == 0.0; }));

        // Declare yesterday's daily metrics outside the block so they're available for email
        double yesterday_daily_return_for_email = 0.0;
        double yesterday_daily_pnl_for_email = 0.0;
        double yesterday_realized_pnl_for_email = 0.0;
        double yesterday_unrealized_pnl_for_email = 0.0;

        // E2-F1: the Day T-1 UPDATE's job is now to write the MARK, not to overwrite realized.
        // It must therefore run whenever T-1 closes were resolved, even on a day whose mark
        // move happened to be zero. The old `yesterday_total_pnl != 0.0` condition skipped the
        // whole statement on such a day, leaving the row un-finalized -- and combined with the
        // date mismatch (E2-F14) it fired on every weekend, which is how the Sunday/Monday
        // reruns were able to leave live_results stale while still rewriting position rows.
        if (!previous_day_close_prices.empty() && !is_first_trading_day) {
            INFO("STEP 4: Finalizing Day T-1 live_results -- mark $" +
                 std::to_string(yesterday_finalized_unrealized) + ", day move $" +
                 std::to_string(yesterday_total_pnl));

            // Get yesterday's commissions and other existing metrics from database
            double yesterday_commissions = 0.0;
            double yesterday_total_commissions = 0.0;
            double yesterday_gross_notional = 0.0;
            double yesterday_net_notional = 0.0;
            int yesterday_active_positions = 0;
            double yesterday_margin_posted = 0.0;

            // Phase 6 §6c: UTC date string via format_utc_date.
            const std::string yesterday_date_str = core::format_utc_date(previous_date);

            // Use LiveDataLoader to get yesterday's metrics
            try {
                INFO("Using LiveDataLoader to query yesterday's metrics for date: " + yesterday_date_str);
                auto live_results = data_loader->load_live_results("LIVE_EQUITY_MEAN_REVERSION", portfolio_id, previous_date);

                if (live_results.is_ok()) {
                    auto& row = live_results.value();
                    yesterday_commissions = row.daily_transaction_costs;
                    yesterday_total_commissions = row.daily_transaction_costs;
                    yesterday_gross_notional = row.gross_notional;
                    yesterday_net_notional = row.gross_notional;  // Note: using gross_notional as net_notional not in LiveResultsRow
                    yesterday_active_positions = row.active_positions;
                    yesterday_margin_posted = row.margin_posted;

                    INFO("Successfully loaded yesterday's metrics via LiveDataLoader:");
                    INFO("  yesterday_commissions: $" + std::to_string(yesterday_commissions));
                    INFO("  yesterday_gross_notional: $" + std::to_string(yesterday_gross_notional));
                    INFO("  yesterday_margin_posted: $" + std::to_string(yesterday_margin_posted));
                } else {
                    WARN("LiveDataLoader failed to get yesterday's metrics: " + std::string(live_results.error()->what()));
                    INFO("Using default values (0) for yesterday's metrics");
                }
            } catch (const std::exception& e) {
                WARN("Failed to get yesterday's metrics: " + std::string(e.what()));
            }

            // Use the commission value already loaded from LiveDataLoader
            double yesterday_commissions_for_calc = yesterday_commissions;
            INFO("Using yesterday_commissions_for_calc from LiveDataLoader: $" + std::to_string(yesterday_commissions_for_calc));

            // Use the queried value from earlier (which may be 0 if query failed)
            double yesterday_daily_pnl_finalized = yesterday_total_pnl - yesterday_commissions;

            INFO("Day T-1 PnL breakdown:");
            INFO("  Position PnL (yesterday_total_pnl): $" + std::to_string(yesterday_total_pnl));
            INFO("  Commissions (yesterday_commissions): $" + std::to_string(yesterday_commissions));
            INFO("  Net PnL (yesterday_daily_pnl_finalized): $" + std::to_string(yesterday_daily_pnl_finalized));

            // Get the day BEFORE yesterday's portfolio value, total_pnl, and total_commissions
            double day_before_yesterday_portfolio_value = initial_capital;
            double day_before_yesterday_total_pnl = 0.0;
            double day_before_yesterday_total_commissions = 0.0;
            try {
                auto db_ptr = std::dynamic_pointer_cast<PostgresDatabase>(db);
                if (db_ptr) {
                    auto prev_agg = db_ptr->get_previous_live_aggregates("LIVE_EQUITY_MEAN_REVERSION", portfolio_id, previous_date, "trading.live_results");
                    if (prev_agg.is_ok()) {
                        std::tie(day_before_yesterday_portfolio_value, day_before_yesterday_total_pnl, day_before_yesterday_total_commissions) = prev_agg.value();
                        INFO("Loaded day-before-yesterday aggregates: portfolio=$" + std::to_string(day_before_yesterday_portfolio_value) +
                             ", total_pnl=$" + std::to_string(day_before_yesterday_total_pnl) +
                             ", total_commissions=$" + std::to_string(day_before_yesterday_total_commissions));
                    }
                }
            } catch (const std::exception& e) {
                INFO("Could not load day-before-yesterday aggregates: " + std::string(e.what()));
            }

            // E2-F1: the "realized-only strip" that used to live here is GONE, along with the
            // day-before-yesterday total_unrealized_pnl read that fed it and the
            // yesterday_total_*_cumulative chain derived from it.
            //
            // That machinery existed only because total_pnl was an ACCUMULATOR carrying a
            // mark-to-market value: each day had to subtract the prior row's mark before
            // adding its own flow, or the mark compounded. total_pnl is now DERIVED --
            // `(cumulative realized - cumulative costs) + current mark` -- and
            // total_realized_pnl accumulates a genuine trade-realized flow, so there is
            // nothing to strip. The UPDATE below reads total_realized_pnl straight off the
            // previous row.
            //
            // If you find yourself reintroducing a strip here, stop: it means total_pnl has
            // been turned back into a running total, which is the defect this replaced.

            // E2-F1: the read of the T-1 row's STORED total_unrealized_pnl that used to sit
            // here is gone. That value was the Day-T snapshot -- the book priced at close(T-2)
            // -- which is precisely the stale figure E2-F13 identified. The T-1 mark is now
            // yesterday_finalized_unrealized, summed from the finalized rows at close(T-1),
            // and both the SQL below and the equity figure above use it.


            // E2-F1: mark-to-market equity for T-1, computed from the SAME components the
            // UPDATE below writes into current_portfolio_value:
            //     initial + (prev_cumulative_realized + this row's realized - cumulative costs)
            //             + this row's finalized mark
            // It feeds total_cumulative_return and total_annualized_return, which are set in
            // that same statement, so it has to be derived here rather than read back. Keeping
            // the two expressions identical is the point -- when they drifted apart, the return
            // columns described an equity the row did not carry.
            double t1_daily_realized = 0.0;
            double t1_total_costs = 0.0;
            double dby_total_realized = 0.0;
            try {
                std::string comp_query =
                    "SELECT COALESCE(y.daily_realized_pnl, 0.0), "
                    "       COALESCE(y.total_transaction_costs, 0.0), "
                    "       COALESCE((SELECT total_realized_pnl FROM trading.live_results "
                    "                 WHERE strategy_id = 'LIVE_EQUITY_MEAN_REVERSION' "
                    "                   AND portfolio_id = '" + portfolio_id + "' "
                    "                   AND DATE(date) < '" + yesterday_date_str + "' "
                    "                 ORDER BY date DESC LIMIT 1), 0.0) "
                    "FROM trading.live_results y "
                    "WHERE y.strategy_id = 'LIVE_EQUITY_MEAN_REVERSION' "
                    "  AND y.portfolio_id = '" + portfolio_id + "' "
                    "  AND DATE(y.date) = '" + yesterday_date_str + "'";
                auto cq = db->execute_query(comp_query);
                if (cq.is_ok() && cq.value()->num_rows() > 0) {
                    auto a = DataConversionUtils::safe_get_double(cq.value()->column(0), 0,
                                                                  "daily_realized_pnl");
                    auto b = DataConversionUtils::safe_get_double(cq.value()->column(1), 0,
                                                                  "total_transaction_costs");
                    auto c = DataConversionUtils::safe_get_double(cq.value()->column(2), 0,
                                                                  "total_realized_pnl");
                    if (a.is_ok()) t1_daily_realized = a.value();
                    if (b.is_ok()) t1_total_costs = b.value();
                    if (c.is_ok()) dby_total_realized = c.value();
                    if (!a.is_ok() || !b.is_ok() || !c.is_ok()) {
                        WARN("Could not read all T-1 equity components; the return columns "
                             "will disagree with current_portfolio_value for this row.");
                    }
                } else {
                    WARN("No T-1 live_results row found for the equity components read; "
                         "return columns will be computed off initial capital alone.");
                }
            } catch (const std::exception& e) {
                WARN("Could not load T-1 equity components: " + std::string(e.what()));
            }

            double yesterday_portfolio_value_finalized = initial_capital
                + ((dby_total_realized + t1_daily_realized) - t1_total_costs)
                + yesterday_finalized_unrealized;

            // Calculate yesterday's returns using LiveMetricsCalculator
            double yesterday_daily_return = metrics_calculator->calculate_daily_return(
                yesterday_daily_pnl_finalized, day_before_yesterday_portfolio_value);

            // Note: Yesterday's metrics for email will be loaded from database after update

            // Calculate yesterday's total cumulative return (non-annualized)
            double yesterday_total_cumulative_return = metrics_calculator->calculate_total_return(
                yesterday_portfolio_value_finalized, initial_capital);

            double yesterday_total_return_decimal = 0.0;
            if (initial_capital > 0.0) {
                yesterday_total_return_decimal = (yesterday_portfolio_value_finalized - initial_capital) / initial_capital;
            }
            double yesterday_total_cumulative_return_pct = yesterday_total_cumulative_return;  // Already in %

            // Get trading days count for annualization using PostgreSQL function
            // This avoids issues with row multiplication/duplication in the database
            int trading_days_count = 1;
            try {
                // Call PostgreSQL function to calculate trading days
                auto trading_days_result = db->execute_query(
                    "SELECT trading.get_trading_days('LIVE_EQUITY_MEAN_REVERSION', DATE '" + yesterday_date_str +
                    "', '" + portfolio_id + "')");
                
                if (trading_days_result.is_ok()) {
                    auto table = trading_days_result.value();
                    if (table && table->num_rows() > 0 && table->num_columns() > 0) {
                        // execute_query returns StringArray for all columns
                        auto arr = std::static_pointer_cast<arrow::StringArray>(table->column(0)->chunk(0));
                        if (arr && arr->length() > 0 && !arr->IsNull(0)) {
                            trading_days_count = std::max<int>(1, std::stoi(arr->GetString(0)));
                            INFO("Trading days for yesterday (" + yesterday_date_str + "): " + std::to_string(trading_days_count));
                        }
                    }
                    // E2-F32: the function anchored to a metadata row that post-dates
                    // the book, so recompute from the book's own first day. Same
                    // formula the function uses, different start date.
                    if (!trading_days_anchor_override.empty()) {
                        const int corrected = trading_days_from_anchor(
                            trading_days_anchor_override, yesterday_date_str);
                        WARN("Trading days for yesterday (" + yesterday_date_str +
                             ") corrected from " + std::to_string(trading_days_count) +
                             " to " + std::to_string(corrected) + " (anchor " +
                             trading_days_anchor_override + ", E2-F32)");
                        trading_days_count = corrected;
                    }
                } else {
                    WARN("Could not call get_trading_days function: " + std::string(trading_days_result.error()->what()));
                }
            } catch (const std::exception& e) {
                WARN("Failed to get trading days: " + std::string(e.what()));
            }

            // Calculate yesterday's annualized return using LiveMetricsCalculator
            double yesterday_total_return_annualized = metrics_calculator->calculate_annualized_return(
                yesterday_total_return_decimal, trading_days_count);

            // Calculate yesterday's leverage and risk metrics
            // IMPORTANT: We MUST preserve existing values from the database
            // These were calculated correctly when Day T-1 was originally processed
            double yesterday_portfolio_leverage = 0.0;
            double yesterday_equity_to_margin_ratio = 0.0;

            // Load existing values from database using LiveDataLoader - DO NOT RECALCULATE
            try {
                auto margin_metrics = data_loader->load_margin_metrics("LIVE_EQUITY_MEAN_REVERSION", portfolio_id, previous_date);
                if (margin_metrics.is_ok() && margin_metrics.value().valid) {
                    auto& metrics = margin_metrics.value();
                    yesterday_portfolio_leverage = metrics.gross_leverage;
                    yesterday_equity_to_margin_ratio = metrics.equity_to_margin_ratio;

                    // Also update the gross_notional and margin_posted if available
                    yesterday_gross_notional = metrics.gross_notional;
                    yesterday_margin_posted = metrics.margin_posted;

                    INFO("Preserved existing metrics from database via LiveDataLoader: leverage=" +
                         std::to_string(yesterday_portfolio_leverage) + ", equity_to_margin=" +
                         std::to_string(yesterday_equity_to_margin_ratio) + ", gross_notional=" +
                         std::to_string(yesterday_gross_notional) + ", margin_posted=" +
                         std::to_string(yesterday_margin_posted));
                } else {
                    INFO("No existing margin metrics found for yesterday via LiveDataLoader");
                }
            } catch (const std::exception& e) {
                WARN("Failed to load existing metrics: " + std::string(e.what()));
            }

            // DO NOT recalculate these values - they should remain as loaded from database
            // These values were correctly calculated when the day was originally processed
            double yesterday_cash_available = yesterday_portfolio_value_finalized - yesterday_margin_posted;

            // UPDATE yesterday's live_results with ALL recalculated metrics
            // Note: We calculate daily_pnl, total_pnl, and current_portfolio_value in SQL
            // to properly incorporate the EXISTING daily_transaction_costs value
            // (trading.live_results has no commissions column -- see E2-F5; the INSERT
            //  above writes the equity commission into daily_transaction_costs, and this
            //  UPDATE must read the same column or the whole statement fails)
            // IMPORTANT: Only update portfolio_leverage and equity_to_margin_ratio if they are NULL or 0
            // E2-F1 -- the Day T-1 finalization, restated.
            //
            // WHAT CHANGED AND WHY. This statement used to overwrite daily_realized_pnl with
            // finalize_previous_day()'s output: the whole book's daily mark move,
            // `sum qty*(close(T-1) - close(T-2))`, booked as "realized" even for positions
            // that never traded. That is the std::to_string(yesterday_finalized_unrealized)TURES settlement model. It is correct there --
            // a futures position is entered at close(T-1) and settled at close(T), so the
            // move genuinely IS realized daily and unrealized is 0 by identity -- and it is
            // wrong for a cash equity book, where a held position's move is unrealized until
            // it is sold.
            //
            // It also made total_pnl double-count. Cumulative "realized" already contained
            // the entire move from entry, so once E2-F12 layered total_unrealized_pnl on top,
            // the same price move was counted twice. Verified against P&L hand-computed from
            // raw executions and closes: cumulative daily_realized_pnl tracked true
            // mark-to-market P&L EXACTLY, which is only possible if it was already the whole
            // thing rather than a realized component.
            //
            // THE MODEL NOW, mirroring futures' `gross - costs`:
            //   daily_realized_pnl   trade-realized, gross, written on the day itself at
            //                        INSERT and NOT touched here -- a closing fill's P&L is
            //                        known when it fills and needs no T-1 lag.
            //   total_unrealized_pnl the T-1 book marked at close(T-1), i.e. the sum of the
            //                        same finalized rows persisted to trading.positions, so
            //                        the aggregate equals the row sum by construction (L5).
            //                        This is what E2-F13 was: the column used to be a DAY-T
            //                        snapshot priced at close(T-2) that finalization never
            //                        revisited, so it differed from the rows by exactly that
            //                        day's P&L.
            //   daily_pnl            (realized - costs) + change in mark. Costs subtracted
            //                        ONCE, here; pos.realized_pnl is gross for this reason.
            //   total_pnl            (cumulative realized - cumulative costs) + current mark.
            //                        DERIVED, not accumulated.
            //
            // The old "realized-only strip" is gone with the defect that needed it. total_pnl
            // is no longer a running sum that the mark has to be added to and stripped back
            // out of each day; it is recomputed from cumulative realized, cumulative costs and
            // the current mark. Do not reintroduce an accumulator here -- a snapshot added to
            // a running total is exactly how the mark started compounding.
            //
            // Bare column names on the right-hand side of a SET read the row's PRE-UPDATE
            // values, which is what we want for daily_realized_pnl, daily_transaction_costs
            // and total_transaction_costs: all three were written by that day's own INSERT.
            std::string update_query =
                "WITH day_before AS ("
                "  SELECT COALESCE(current_portfolio_value, " + std::to_string(initial_capital) + ") as portfolio, "
                "         COALESCE(total_unrealized_pnl, 0.0) as prev_unrealized, "
                "         COALESCE(total_realized_pnl, 0.0) as prev_total_realized "
                "  FROM trading.live_results "
                "  WHERE strategy_id = 'LIVE_EQUITY_MEAN_REVERSION' AND portfolio_id = '" + portfolio_id + "' AND DATE(date) < '" + yesterday_date_str + "' "
                "  ORDER BY date DESC LIMIT 1"
                ") "
                "UPDATE trading.live_results SET "
                // The mark, from the finalized rows. daily_realized_pnl is deliberately absent.
                "total_unrealized_pnl = " + std::to_string(yesterday_finalized_unrealized) + ", "
                "daily_unrealized_pnl = " + std::to_string(yesterday_finalized_unrealized) + " - COALESCE((SELECT prev_unrealized FROM day_before), 0.0), "
                "total_realized_pnl = COALESCE((SELECT prev_total_realized FROM day_before), 0.0) + COALESCE(daily_realized_pnl, 0.0), "
                "daily_pnl = (COALESCE(daily_realized_pnl, 0.0) - COALESCE(daily_transaction_costs, 0.0)) "
                "            + (" + std::to_string(yesterday_finalized_unrealized) + " - COALESCE((SELECT prev_unrealized FROM day_before), 0.0)), "
                "total_pnl = (COALESCE((SELECT prev_total_realized FROM day_before), 0.0) + COALESCE(daily_realized_pnl, 0.0) "
                "             - COALESCE(total_transaction_costs, 0.0)) + " + std::to_string(yesterday_finalized_unrealized) + ", "
                "current_portfolio_value = " + std::to_string(initial_capital) + " "
                "             + (COALESCE((SELECT prev_total_realized FROM day_before), 0.0) + COALESCE(daily_realized_pnl, 0.0) "
                "                - COALESCE(total_transaction_costs, 0.0)) + " + std::to_string(yesterday_finalized_unrealized) + ", "
                "daily_return = CASE WHEN COALESCE((SELECT portfolio FROM day_before), " + std::to_string(initial_capital) + ") > 0 "
                "               THEN (((COALESCE(daily_realized_pnl, 0.0) - COALESCE(daily_transaction_costs, 0.0)) "
                "                      + (" + std::to_string(yesterday_finalized_unrealized) + " - COALESCE((SELECT prev_unrealized FROM day_before), 0.0))) "
                "                     / COALESCE((SELECT portfolio FROM day_before), " + std::to_string(initial_capital) + ")) * 100.0 "
                "               ELSE 0.0 END, "
                "total_cumulative_return = " + std::to_string(yesterday_total_cumulative_return_pct) + ", "
                "total_annualized_return = " + std::to_string(yesterday_total_return_annualized) + ", "
                "portfolio_leverage = CASE WHEN portfolio_leverage IS NULL OR portfolio_leverage = 0 THEN " + std::to_string(yesterday_portfolio_leverage) + " ELSE portfolio_leverage END, "
                "equity_to_margin_ratio = CASE WHEN equity_to_margin_ratio IS NULL OR equity_to_margin_ratio = 0 THEN " + std::to_string(yesterday_equity_to_margin_ratio) + " ELSE equity_to_margin_ratio END, "
                "cash_available = " + std::to_string(initial_capital) + " "
                "             + (COALESCE((SELECT prev_total_realized FROM day_before), 0.0) + COALESCE(daily_realized_pnl, 0.0) "
                "                - COALESCE(total_transaction_costs, 0.0)) + " + std::to_string(yesterday_finalized_unrealized) + " - COALESCE(margin_posted, 0.0) "
                "WHERE strategy_id = 'LIVE_EQUITY_MEAN_REVERSION' AND portfolio_id = '" + portfolio_id + "' AND DATE(date) = '" + yesterday_date_str + "'";

            INFO("Executing UPDATE query for Day T-1 live_results...");
            INFO("UPDATE will set current_portfolio_value for date: " + yesterday_date_str);

            auto update_result = db->execute_direct_query(update_query);
            if (update_result.is_error()) {
                // E2-F1: a failed T-1 UPDATE is FATAL. It used to log and carry on, and the
                // run still exited 0 -- the same defect class as E2-F5/E2-F6, where a
                // swallowed write failure made a zero exit code meaningless.
                //
                // This is not hypothetical: during E2-F1 development a malformed statement
                // failed on every single day of a 12-day replay, every run exited 0, and the
                // only symptom was that total_unrealized_pnl silently kept its stale Day-T
                // value. Every internal identity still reconciled, because the INSERT path
                // computes them consistently -- so the run looked clean and was not.
                //
                // If this statement does not apply, the T-1 row keeps a mark priced at
                // close(T-2) and the book is wrong from that day forward. Fail the run.
                ERROR("Failed to update Day T-1 live_results: " + std::string(update_result.error()->what()));
                ERROR("Day T-1 aggregates could not be finalized. Refusing to exit 0 with a "
                      "stale mark on " + yesterday_date_str + ".");
                return 1;
            } else {
                INFO("Successfully updated Day T-1 live_results with finalized PnL and all metrics");

                // Log the expected value
                INFO("Expected current_portfolio_value calculation: day_before_portfolio + (yesterday_pnl - commissions)");
                INFO("  yesterday_total_pnl: $" + std::to_string(yesterday_total_pnl));
                INFO("  yesterday_commissions: $" + std::to_string(yesterday_commissions_for_calc));
            }

            // UPDATE yesterday's equity_curve using LiveResultsManager
            INFO("Updating Day T-1 equity_curve...");

            // Query the current portfolio value from updated live_results
            std::string get_equity_query =
                "SELECT current_portfolio_value FROM trading.live_results "
                "WHERE strategy_id = 'LIVE_EQUITY_MEAN_REVERSION' AND portfolio_id = '" + portfolio_id + "' AND DATE(date) = '" + yesterday_date_str + "'";

            INFO("Querying for portfolio value with date: " + yesterday_date_str);

            auto equity_result = db->execute_query(get_equity_query);
            if (equity_result.is_error()) {
                ERROR("Failed to get portfolio value for equity update: " + std::string(equity_result.error()->what()));
            } else {
                auto table = equity_result.value();
                INFO("Query returned " + std::to_string(table->num_rows()) + " rows");

                if (table->num_rows() > 0) {
                    // current_portfolio_value is NUMERIC, which this driver surfaces as a
                    // StringArray -- every other numeric read in this file does the same
                    // (see the yesterday-metrics block below). Casting the chunk to
                    // DoubleArray does not convert it; static_pointer_cast just
                    // reinterprets the pointer, so Value(0) read a garbage double that
                    // printed as 0.000000, failed the "< 1000" guard, and skipped the
                    // Day T-1 equity_curve update on 6 of 10 days -- leaving equity_curve
                    // disagreeing with live_results.current_portfolio_value (E2-F7).
                    auto array = std::static_pointer_cast<arrow::StringArray>(table->column(0)->chunk(0));

                    // Check for NULL value before reading
                    if (array->IsNull(0)) {
                        ERROR("Cannot update Day T-1 equity_curve: current_portfolio_value is NULL for date " + yesterday_date_str);
                    } else {
                        double portfolio_value = 0.0;
                        try {
                            portfolio_value = std::stod(std::string(array->GetView(0)));
                        } catch (const std::exception& e) {
                            ERROR("Could not parse current_portfolio_value '" +
                                  std::string(array->GetView(0)) + "' for " + yesterday_date_str +
                                  ": " + e.what());
                        }
                        INFO("Raw value read from database: " + std::to_string(portfolio_value));

                        // Validate the value before using it
                        if (portfolio_value <= 0.0 || std::isnan(portfolio_value) || std::isinf(portfolio_value) || portfolio_value < 1000.0) {
                            ERROR("Invalid portfolio value for Day T-1 equity update: " + std::to_string(portfolio_value) +
                                  " (date: " + yesterday_date_str + "). Skipping equity_curve update.");
                            ERROR("  Validation failed: <= 0.0? " + std::string(portfolio_value <= 0.0 ? "YES" : "NO") +
                                  ", isnan? " + std::string(std::isnan(portfolio_value) ? "YES" : "NO") +
                                  ", isinf? " + std::string(std::isinf(portfolio_value) ? "YES" : "NO") +
                                  ", < 1000? " + std::string(portfolio_value < 1000.0 ? "YES" : "NO"));
                        } else {
                            INFO("✓ Valid portfolio value for Day T-1: $" + std::to_string(portfolio_value));

                            // Create a temporary LiveResultsManager for Day T-1 equity update
                            // portfolio_id is NOT defaultable here: LiveResultsManager
                            // falls back to "BASE_PORTFOLIO", the futures book, so
                            // omitting it files this portfolio's equity curve under a
                            // portfolio it does not belong to while EQUITY_MR_PORTFOLIO
                            // gets no row at all. live_portfolio_conservative.cpp:2217
                            // passes it at the byte-identical call site.
                            auto yesterday_manager = std::make_unique<LiveResultsManager>(
                                db, true, kEquityStrategyId, portfolio_id, kEquityStrategyName
                            );
                            yesterday_manager->set_equity(portfolio_value);

                            auto update_equity_result = yesterday_manager->save_equity_curve(previous_date);
                            if (update_equity_result.is_error()) {
                                ERROR("Failed to update Day T-1 equity_curve: " + std::string(update_equity_result.error()->what()));
                            } else {
                                INFO("Successfully updated Day T-1 equity_curve with value: " + std::to_string(portfolio_value));
                            }
                        }
                    }
                } else {
                    WARN("No live_results found for date " + yesterday_date_str + ", skipping equity_curve update");
                }
            }

            // ================= E2-F33: the since-inception metrics block ==============
            //
            // Fifteen columns on trading.live_results were NULL on every equity row ever
            // written (22/22 on this book, 126/126 on the drift audit's), because this
            // runner never called LiveHistoricalMetricsCalculator at all while both futures
            // runners have called it for T-1 and for day T since they were written
            // (live_portfolio_conservative.cpp:2283-2380 is the block this mirrors).
            //
            // Same calculator, same four history loads, same override of total_days with the
            // authoritative get_trading_days() count -- the only differences are the strategy
            // id and the E2-F32-corrected trading-days figure this runner already computed
            // above, which is the denominator the annualized return in the same row used.
            //
            // Included since D3: `volatility`. Both futures runners write
            // `{"volatility", yesterday_hist_metrics.volatility}` into this same T-1 update;
            // the equity runner used to leave the ex-ante `portfolio_var x 100` standing here,
            // so the column meant one thing on one book and another on the other, and the
            // sharpe ratio written beside it could not be reproduced from it. `portfolio_var`
            // is untouched and still carries the ex-ante figure.
            //
            // Failure here is a WARN, not a fatal: every trading decision for T-1 is already
            // made and persisted by this point, and these columns are reporting. That is the
            // same choice the futures runners make at the identical site.
            try {
                HistoricalMetrics yesterday_hist_metrics;

                if (data_loader && data_loader->is_connected()) {
                    auto returns_hist_res = data_loader->load_daily_returns_history(
                        kEquityStrategyId, portfolio_id, previous_date);
                    auto pnl_hist_res = data_loader->load_daily_pnl_history(
                        kEquityStrategyId, portfolio_id, previous_date);
                    auto equity_hist_res = data_loader->load_equity_curve_history(
                        kEquityStrategyId, portfolio_id, previous_date);
                    auto trades_hist_res = data_loader->load_total_trades_count(
                        kEquityStrategyId, portfolio_id, previous_date);

                    std::vector<double> returns_hist;
                    std::vector<double> pnl_hist;
                    std::vector<double> equity_hist;
                    int total_trades_hist = 0;

                    if (returns_hist_res.is_ok()) returns_hist = returns_hist_res.value();
                    if (pnl_hist_res.is_ok()) pnl_hist = pnl_hist_res.value();
                    if (equity_hist_res.is_ok()) equity_hist = equity_hist_res.value();
                    if (trades_hist_res.is_ok()) total_trades_hist = trades_hist_res.value();

                    LiveHistoricalMetricsCalculator hist_calc;
                    // daily_return is stored in PERCENT (the T-1 UPDATE above multiplies by
                    // 100.0), so the series arrives in percent and must NOT be scaled again --
                    // the futures runner carries the same note after a 100x volatility bug.
                    yesterday_hist_metrics =
                        hist_calc.calculate(returns_hist, pnl_hist, equity_hist,
                                            yesterday_total_return_annualized,
                                            total_trades_hist);

                    // total_days is the authoritative trading-day count for T-1 -- the same
                    // E2-F32-corrected figure that annualized the return written into this
                    // row -- not the number of live_results rows, which includes weekends.
                    yesterday_hist_metrics.total_days = trading_days_count;
                    if (trading_days_count > 0) {
                        yesterday_hist_metrics.win_rate =
                            static_cast<double>(yesterday_hist_metrics.winning_days) /
                            static_cast<double>(trading_days_count) * 100.0;
                    }

                    INFO("HIST_METRICS [Day T-1] " + yesterday_date_str +
                         ": return_volatility=" +
                         std::to_string(yesterday_hist_metrics.volatility) +
                         " downside_deviation=" +
                         std::to_string(yesterday_hist_metrics.downside_deviation) +
                         " sharpe=" + std::to_string(yesterday_hist_metrics.sharpe_ratio) +
                         " sortino=" + std::to_string(yesterday_hist_metrics.sortino_ratio) +
                         " max_drawdown=" +
                         std::to_string(yesterday_hist_metrics.max_drawdown) +
                         " winning_days=" +
                         std::to_string(yesterday_hist_metrics.winning_days) +
                         " losing_days=" + std::to_string(yesterday_hist_metrics.losing_days) +
                         " total_days=" + std::to_string(yesterday_hist_metrics.total_days) +
                         " win_rate=" + std::to_string(yesterday_hist_metrics.win_rate) +
                         " avg_win=" + std::to_string(yesterday_hist_metrics.avg_win) +
                         " avg_loss=" + std::to_string(yesterday_hist_metrics.avg_loss) +
                         " best_day=" + std::to_string(yesterday_hist_metrics.best_day) +
                         " worst_day=" + std::to_string(yesterday_hist_metrics.worst_day) +
                         " gross_profit=" +
                         std::to_string(yesterday_hist_metrics.gross_profit) +
                         " gross_loss=" + std::to_string(yesterday_hist_metrics.gross_loss) +
                         " profit_factor=" +
                         std::to_string(yesterday_hist_metrics.profit_factor) +
                         " (returns n=" + std::to_string(returns_hist.size()) +
                         ", equity n=" + std::to_string(equity_hist.size()) +
                         ", executions=" + std::to_string(total_trades_hist) + ")");

                    // One definition of the block, shared with the day-T write below, so a
                    // column can never be written on one path and forgotten on the other.
                    // update_live_results takes doubles only, so the three integer columns
                    // are widened here; they are whole numbers by construction.
                    auto metric_updates =
                        historical_metrics_double_columns(yesterday_hist_metrics);
                    for (const auto& [column, value] :
                         historical_metrics_int_columns(yesterday_hist_metrics)) {
                        metric_updates[column] = static_cast<double>(value);
                    }

                    auto yesterday_metrics_manager = std::make_unique<LiveResultsManager>(
                        db, true, kEquityStrategyId, portfolio_id, kEquityStrategyName);
                    auto update_metrics_result =
                        yesterday_metrics_manager->update_live_results(previous_date,
                                                                       metric_updates);
                    if (update_metrics_result.is_error()) {
                        WARN("Failed to update Day T-1 historical performance metrics: " +
                             std::string(update_metrics_result.error()->what()));
                    } else {
                        INFO("Successfully updated Day T-1 historical performance metrics in "
                             "trading.live_results");
                    }
                } else {
                    WARN("LiveDataLoader not available or not connected; skipping the Day T-1 "
                         "historical metrics update (E2-F33).");
                }
            } catch (const std::exception& e) {
                WARN("Exception while updating Day T-1 historical performance metrics: " +
                     std::string(e.what()));
            }

            // Load updated metrics from database for email - MUST do this AFTER the UPDATE
            try {
                std::string metrics_query =
                    "SELECT daily_return, daily_pnl, daily_realized_pnl, daily_unrealized_pnl, "
                    "portfolio_leverage, equity_to_margin_ratio "
                    "FROM trading.live_results "
                    "WHERE strategy_id = 'LIVE_EQUITY_MEAN_REVERSION' AND portfolio_id = '" + portfolio_id + "' AND DATE(date) = '" + yesterday_date_str + "'";

                INFO("Loading yesterday's metrics from database with query: " + metrics_query);
                auto metrics_result = db->execute_query(metrics_query);

                if (metrics_result.is_ok() && metrics_result.value()->num_rows() > 0) {
                    auto table = metrics_result.value();
                    if (table->num_columns() >= 4) {
                        auto daily_return_arr = std::static_pointer_cast<arrow::DoubleArray>(table->column(0)->chunk(0));
                        auto daily_pnl_arr = std::static_pointer_cast<arrow::DoubleArray>(table->column(1)->chunk(0));
                        auto daily_realized_arr = std::static_pointer_cast<arrow::DoubleArray>(table->column(2)->chunk(0));
                        auto daily_unrealized_arr = std::static_pointer_cast<arrow::DoubleArray>(table->column(3)->chunk(0));

                        if (daily_return_arr && daily_return_arr->length() > 0 && !daily_return_arr->IsNull(0)) {
                            yesterday_daily_return_for_email = daily_return_arr->Value(0);
                            INFO("Loaded yesterday's daily_return: " + std::to_string(yesterday_daily_return_for_email));
                        }
                        if (daily_pnl_arr && daily_pnl_arr->length() > 0 && !daily_pnl_arr->IsNull(0)) {
                            yesterday_daily_pnl_for_email = daily_pnl_arr->Value(0);
                            INFO("Loaded yesterday's daily_pnl: " + std::to_string(yesterday_daily_pnl_for_email));
                        }
                        if (daily_realized_arr && daily_realized_arr->length() > 0 && !daily_realized_arr->IsNull(0)) {
                            yesterday_realized_pnl_for_email = daily_realized_arr->Value(0);
                            INFO("Loaded yesterday's daily_realized_pnl: " + std::to_string(yesterday_realized_pnl_for_email));
                        } else {
                            // If daily_realized_pnl is null or 0, use yesterday_total_pnl as fallback
                            yesterday_realized_pnl_for_email = yesterday_total_pnl;
                            INFO("Using calculated yesterday_total_pnl as realized PnL: " + std::to_string(yesterday_realized_pnl_for_email));
                        }
                        if (daily_unrealized_arr && daily_unrealized_arr->length() > 0 && !daily_unrealized_arr->IsNull(0)) {
                            yesterday_unrealized_pnl_for_email = daily_unrealized_arr->Value(0);
                            INFO("Loaded yesterday's daily_unrealized_pnl: " + std::to_string(yesterday_unrealized_pnl_for_email));
                        }

                        // For equities, unrealized PnL is loaded from database (calculated for open positions)

                        INFO("Successfully loaded yesterday's metrics from database for email");
                    }
                } else {
                    WARN("No metrics found in database for yesterday, using calculated values");
                    // Use the calculated values as fallback
                    yesterday_realized_pnl_for_email = yesterday_total_pnl;
                    yesterday_daily_pnl_for_email = yesterday_total_pnl;  // For equities, daily PnL = realized PnL when no DB data
                    yesterday_unrealized_pnl_for_email = 0.0;  // Fallback when no DB data
                }
            } catch (const std::exception& e) {
                WARN("Failed to load updated yesterday's metrics: " + std::string(e.what()));
                // Use calculated values as fallback
                yesterday_realized_pnl_for_email = yesterday_total_pnl;
                yesterday_daily_pnl_for_email = yesterday_total_pnl;
                yesterday_unrealized_pnl_for_email = 0.0;
            }
        } else {
            if (is_first_trading_day) {
                INFO("Skipping Day T-1 update (first trading day - no previous positions to finalize)");
            } else {
                INFO("Skipping Day T-1 live_results update (no two_days_ago prices or zero PnL)");
            }
        }

        // ========================================
        // STEP 5: LOAD UPDATED PREVIOUS DAY AGGREGATES AND CALCULATE Day T CUMULATIVE VALUES
        // ========================================
        INFO("STEP 5: Loading updated previous day aggregates and calculating Day T cumulative values...");

        // Load previous day's aggregates (portfolio value, total pnl, total commissions)
        // This is done AFTER updating Day T-1 live_results to ensure we get the finalized values
        double previous_portfolio_value = initial_capital; // Default to initial capital
        double previous_total_pnl = 0.0;
        double previous_total_commissions = 0.0;
        // The previous row's mark-to-market snapshot. Needed for two things: to strip the mark
        // back out of the previous stored total_pnl so the running total stays realized-only,
        // and to difference into daily_unrealized_pnl. get_previous_live_aggregates() is shared
        // with the futures runners so it is not widened to return this; the equity runner reads
        // the column itself, scoped by portfolio_id like every other query in this file.
        double previous_total_unrealized_pnl = 0.0;

        try {
            auto db_ptr = std::dynamic_pointer_cast<PostgresDatabase>(db);
            if (db_ptr) {
                auto prev_agg = db_ptr->get_previous_live_aggregates("LIVE_EQUITY_MEAN_REVERSION", portfolio_id, now, "trading.live_results");
                if (prev_agg.is_ok()) {
                    std::tie(previous_portfolio_value, previous_total_pnl, previous_total_commissions) = prev_agg.value();
                    INFO("Loaded updated previous aggregates - portfolio_value: $" + std::to_string(previous_portfolio_value) +
                         ", total_pnl: $" + std::to_string(previous_total_pnl) +
                         ", total_commissions: $" + std::to_string(previous_total_commissions));
                } else {
                    INFO("No previous aggregates found: " + std::string(prev_agg.error()->what()));
                }
            }
        } catch (const std::exception& e) {
            INFO("Could not load previous day aggregates: " + std::string(e.what()));
        }

        // Load the previous row's mark-to-market snapshot (see declaration above). A missing
        // row -- the first trading day -- correctly leaves this at 0.0, which makes the
        // realized-only strip a no-op and makes daily_unrealized_pnl equal the full opening
        // mark, both of which are right on day one.
        try {
            std::string prev_unrealized_query =
                "SELECT COALESCE(total_unrealized_pnl, 0.0) FROM trading.live_results "
                "WHERE strategy_id = 'LIVE_EQUITY_MEAN_REVERSION' AND portfolio_id = '" + portfolio_id + "' "
                "AND DATE(date) < '" + today_date_str + "' "
                "ORDER BY date DESC LIMIT 1";
            auto pu = db->execute_query(prev_unrealized_query);
            if (pu.is_ok() && pu.value()->num_rows() > 0) {
                auto r = DataConversionUtils::safe_get_double(pu.value()->column(0), 0,
                                                              "total_unrealized_pnl");
                if (r.is_ok()) {
                    previous_total_unrealized_pnl = r.value();
                    INFO("Loaded previous total_unrealized_pnl: $" +
                         std::to_string(previous_total_unrealized_pnl));
                } else {
                    WARN("Could not read previous total_unrealized_pnl (" +
                         std::string(r.error()->what()) + "); treating the previous mark as 0. "
                         "total_pnl will be overstated by the prior mark until the next clean read.");
                }
            } else {
                INFO("No previous total_unrealized_pnl row; treating the previous mark as 0");
            }
        } catch (const std::exception& e) {
            WARN("Could not load previous total_unrealized_pnl: " + std::string(e.what()));
        }

        // Mark-to-market accounting -- EQUITIES ONLY. Read this before changing anything here.
        //
        // WHY EQUITIES DIFFER FROM FUTURES (E2-F12). `average_price` does not mean the same
        // thing in the two books:
        //   * Futures: average_price IS the prior settlement close, i.e. a mark. A position is
        //     entered at close(T-1) and settled at close(T), so the entire move is realized on
        //     the day it happens and unrealized is 0 BY IDENTITY. The futures runners therefore
        //     write 0 to both unrealized columns and a realized-only total_pnl, which for them
        //     already IS mark-to-market. Nothing about the futures path may change.
        //   * Equities: average_price is a true weighted cost basis that survives across days.
        //     An open position carries a real mark-to-market gain or loss that is NOT in
        //     realized PnL. Reporting realized-only here understates a book holding winners and
        //     leaves the equity curve flat while the book actually moves.
        // So this runner reports total_pnl and current_portfolio_value INCLUDING unrealized.
        //
        // WHY THE RECURSION IS REALIZED-ONLY. total_pnl was previously accumulated as
        // `previous_total_pnl + daily_pnl_for_today`, reading the previous row's total_pnl back
        // out of the DB. If unrealized were folded into the stored total_pnl and that same
        // recursion were kept, every day would re-add the PRIOR day's open-position mark on top
        // of today's -- unrealized would compound into a cumulative sum of daily snapshots and
        // the equity curve would diverge without bound. Unrealized is a SNAPSHOT, not a flow:
        // it replaces the prior snapshot, it does not add to it. The running total therefore
        // stays realized-only and unrealized is layered on top once, at the end.
        double total_commissions_cumulative = previous_total_commissions + total_daily_commissions;

        // Calculate total unrealized PnL from current open positions. Computed BEFORE total_pnl
        // now (it used to be derived after) because total_pnl depends on it.
        double total_unrealized_pnl = 0.0;
        for (const auto& [sym, pos] : positions) {
            total_unrealized_pnl += pos.unrealized_pnl.as_double();
        }

        // E2-F1: on a CLOSED day the mark is CARRIED, not recomputed to zero.
        //
        // A carry-forward day (weekend or holiday, per B1) generates no executions, so
        // LiveDailyCycle has no execution_prices to mark against and every position's
        // unrealized_pnl comes back 0 -- making the sum above 0 even though the book is
        // unchanged and fully marked. The stored position rows are NOT zero: they carry the
        // held mark. So the aggregate and the rows disagreed on exactly the closed days.
        //
        // The reported effect was a fabricated round trip. Measured on the 2026-07-24..08-04
        // replay before this guard: Saturday 08-01 reported daily_pnl = -1052.6450 and
        // Monday reversed it, on days the market never opened. That corrupts daily_return,
        // and every volatility- and drawdown-derived metric with it.
        //
        // No new close exists, so there is no new mark and no P&L: carry the previous value
        // and let daily_unrealized_pnl fall out as exactly 0. This also restores L5 on closed
        // days, because the carried figure is precisely what the position rows hold.
        if (today_is_non_trading) {
            INFO("Non-trading day: carrying the previous mark ($" +
                 std::to_string(previous_total_unrealized_pnl) +
                 ") rather than recomputing it from an unmarked book");
            total_unrealized_pnl = previous_total_unrealized_pnl;
        }

        // BA-4: protocol L5's UNREALIZED clause, asserted inside the run.
        //
        // The realized clause above has been checked since E2-F19 (R5); unrealized had
        // nothing, which is how E2-F35 survived -- a widened symbol's row was 0 while the
        // aggregate was not, and no assertion in the run could see it. Both sides are now
        // computed from one basis and one mark map (`day_t_marks`), so on a trading day
        // this is an identity and a violation means a row was dropped, double-counted or
        // marked from somewhere else.
        //
        // A non-trading day is NOT an identity and is not treated as one. The aggregate
        // there is the PREVIOUS row's stored figure, read back from a numeric column that
        // rounds to 4 decimals, while the rows are recomputed against the T-1 close. The
        // two agree because nothing moved (measured over the whole book on 2026-09-03:
        // 74 days, 0 residuals above 1e-4, max 4.9e-5), but they agree by circumstance
        // rather than by construction, so the closed-day check WARNs at a tolerance that
        // covers the stored rounding instead of killing a run over it.
        {
            double row_unrealized_sum = 0.0;
            std::string breakdown;
            for (const auto& p : positions_to_save) {
                const double u = static_cast<double>(p.unrealized_pnl);
                row_unrealized_sum += u;
                if (std::abs(u) > LiveDailyCycle::kRowTolerance) {
                    breakdown += " " + p.symbol + "=" + std::to_string(u);
                }
            }
            const double residual = row_unrealized_sum - total_unrealized_pnl;
            if (today_is_non_trading) {
                if (std::abs(residual) > 1e-2) {
                    WARN("L5 unrealized identity: carried aggregate (" +
                         std::to_string(total_unrealized_pnl) + ") differs from the sum of "
                         "the rows written today (" + std::to_string(row_unrealized_sum) +
                         "), residual " + std::to_string(residual) +
                         ". On a closed day the aggregate is the previous row's stored "
                         "figure and the rows are re-marked at the same close, so a "
                         "difference beyond stored rounding means the book moved on a day "
                         "the market was shut. Per-symbol rows:" +
                         (breakdown.empty() ? std::string(" <none>") : breakdown));
                } else {
                    INFO("L5 unrealized identity holds on a closed day: rows " +
                         std::to_string(row_unrealized_sum) + " ~= carried aggregate " +
                         std::to_string(total_unrealized_pnl) + " (residual " +
                         std::to_string(residual) + ")");
                }
            } else if (std::abs(residual) > 1e-4) {
                ERROR("L5 unrealized identity VIOLATED: sum of "
                      "positions.daily_unrealized_pnl (" + std::to_string(row_unrealized_sum) +
                      ") != live_results.total_unrealized_pnl (" +
                      std::to_string(total_unrealized_pnl) + "), residual " +
                      std::to_string(residual) + ". Per-symbol rows:" +
                      (breakdown.empty() ? std::string(" <none>") : breakdown) +
                      ". Refusing to persist a day whose rows do not sum to its aggregate.");
                // E2-F44 (BA-18). This exit is later than the realized one and leaves more
                // behind: the whole Day T-1 finalization has run by now.
                ERROR("STATE AT THIS EXIT (" + today_date_str + "): NOTHING has been written "
                      "for day T -- its position rows are untouched (the clear-then-write "
                      "pair runs later), and live_results, equity_curve, executions and "
                      "signals for today are never written on this path. ALREADY WRITTEN and "
                      "NOT rolled back: trading.positions, trading.live_results AND "
                      "trading.equity_curve for T-1 (" +
                      core::format_utc_date(previous_date) +
                      ") -- the full finalization, including the T-1 mark, the historical "
                      "metrics block and the T-1 equity point -- plus any "
                      "trading.corp_action_applied dedup rows this run committed with their "
                      "day-T placeholder positions, and the stale-execution DELETE for "
                      "today's order ids. RECOVERY: fix the cause and re-run THIS SAME DATE, "
                      "which re-finalizes T-1 and rewrites day T. If the re-run will not "
                      "take, reset the book WINDOWED from " + today_date_str +
                      " -- positions, live_results, equity_curve, executions, signals AND "
                      "corp_action_applied together (replay rule 7) -- and replay forward "
                      "from there. Never leave this date and run the next one (replay "
                      "rule 8).");
                return 1;
            } else {
                INFO("L5 unrealized identity holds: rows " +
                     std::to_string(row_unrealized_sum) + " == aggregate " +
                     std::to_string(total_unrealized_pnl) + " (residual " +
                     std::to_string(residual) + ")");
            }
        }

        // E2-F1: cumulative TRADE-realized, gross. Read from the previous row and extended by
        // today's realized rather than derived from a total_pnl accumulator.
        //
        // The old code did the reverse -- it accumulated total_pnl and back-solved
        // total_realized_pnl from it -- which only worked while "realized" meant the daily
        // mark move. It also needed a "realized-only strip" (subtracting the prior row's mark
        // from the prior total_pnl) to stop the mark compounding day over day. Both are gone:
        // realized is now a genuine flow that accumulates, and total_pnl is DERIVED from it.
        // Do not turn total_pnl back into an accumulator -- adding a snapshot to a running
        // total is precisely how the mark started compounding.
        double previous_total_realized_pnl = 0.0;
        try {
            std::string prev_realized_query =
                "SELECT COALESCE(total_realized_pnl, 0.0) FROM trading.live_results "
                "WHERE strategy_id = 'LIVE_EQUITY_MEAN_REVERSION' AND portfolio_id = '" + portfolio_id + "' "
                "AND DATE(date) < '" + today_date_str + "' "
                "ORDER BY date DESC LIMIT 1";
            auto pr = db->execute_query(prev_realized_query);
            if (pr.is_ok() && pr.value()->num_rows() > 0) {
                auto r = DataConversionUtils::safe_get_double(pr.value()->column(0), 0,
                                                              "total_realized_pnl");
                if (r.is_ok()) {
                    previous_total_realized_pnl = r.value();
                    INFO("Loaded previous total_realized_pnl: $" +
                         std::to_string(previous_total_realized_pnl));
                } else {
                    WARN("Could not read previous total_realized_pnl (" +
                         std::string(r.error()->what()) + "); treating it as 0. Cumulative "
                         "realized and every figure derived from it will be understated until "
                         "the next clean read.");
                }
            } else {
                INFO("No previous total_realized_pnl row; starting the cumulative at 0");
            }
        } catch (const std::exception& e) {
            WARN("Could not load previous total_realized_pnl: " + std::string(e.what()));
        }

        double total_realized_pnl = previous_total_realized_pnl + daily_realized_pnl;

        // Day-over-day change in the mark. This is what daily_unrealized_pnl should have held
        // all along; it was previously hard-wired to 0.0 as a Day-T placeholder and never
        // repaired by the Day T-1 UPDATE, so it read 0 on every equity row.
        double daily_unrealized_pnl = total_unrealized_pnl - previous_total_unrealized_pnl;

        // The reported identity, and the one to hand-check:
        //     total_pnl = (cumulative realized - cumulative costs) + current mark
        // Costs are subtracted exactly once. Realized is gross precisely so that this is the
        // only place they come off, matching how the futures runners report `gross - costs`.
        double total_pnl = (total_realized_pnl - total_commissions_cumulative) + total_unrealized_pnl;
        double current_portfolio_value = initial_capital + total_pnl;

        // daily_pnl must be the day-over-day change in total_pnl or the equity-curve continuity
        // invariant (protocol L6: value(T) - value(T-1) == daily_pnl(T)) breaks the moment the
        // curve became mark-to-market.
        double daily_pnl = daily_pnl_for_today + daily_unrealized_pnl;

        // Calculate returns using LiveMetricsCalculator
        double daily_return = metrics_calculator->calculate_daily_return(daily_pnl, previous_portfolio_value);

        // Calculate total cumulative return (non-annualized)
        double total_cumulative_return = metrics_calculator->calculate_total_return(current_portfolio_value, initial_capital);

        double total_return_decimal = 0.0;
        if (initial_capital > 0.0) {
            total_return_decimal = (current_portfolio_value - initial_capital) / initial_capital;
        }
        double total_cumulative_return_pct = total_cumulative_return;  // Already in %

        // Get n = number of trading days using PostgreSQL function (robust against row duplication)
        int trading_days_count = 1; // Default to 1 to avoid division by zero on first day
        try {
            // Phase 6 §6c: UTC date string via format_utc_date.
            const std::string now_date_str = core::format_utc_date(now);

            // Call PostgreSQL function to calculate trading days
            auto trading_days_result = db->execute_query(
                "SELECT trading.get_trading_days('LIVE_EQUITY_MEAN_REVERSION', DATE '" + now_date_str +
                    "', '" + portfolio_id + "')");
            
            if (trading_days_result.is_ok()) {
                auto table = trading_days_result.value();
                if (table && table->num_rows() > 0 && table->num_columns() > 0) {
                    // execute_query returns StringArray for all columns
                    auto arr = std::static_pointer_cast<arrow::StringArray>(table->column(0)->chunk(0));
                    if (arr && arr->length() > 0 && !arr->IsNull(0)) {
                        trading_days_count = std::max<int>(1, std::stoi(arr->GetString(0)));
                        INFO("Trading days for today (" + now_date_str + "): " + std::to_string(trading_days_count));
                    }
                }
                // E2-F32, as above: correct the ANCHOR, not the formula.
                if (!trading_days_anchor_override.empty()) {
                    const int corrected =
                        trading_days_from_anchor(trading_days_anchor_override, now_date_str);
                    WARN("Trading days for today (" + now_date_str + ") corrected from " +
                         std::to_string(trading_days_count) + " to " +
                         std::to_string(corrected) + " (anchor " +
                         trading_days_anchor_override + ", E2-F32)");
                    trading_days_count = corrected;
                }
            } else {
                WARN("Could not call get_trading_days function: " + std::string(trading_days_result.error()->what()));
            }
        } catch (const std::exception& e) {
            WARN(std::string("Failed to get trading days: ") + e.what());
        }

        // Calculate annualized return using LiveMetricsCalculator
        double total_return_annualized = metrics_calculator->calculate_annualized_return(
            total_return_decimal, trading_days_count);

        INFO("Portfolio value calculation:");
        INFO("  Previous portfolio value: $" + std::to_string(previous_portfolio_value));
        INFO("  Daily PnL: $" + std::to_string(daily_pnl));
        INFO("  Current portfolio value: $" + std::to_string(current_portfolio_value));
        INFO("  Total PnL: $" + std::to_string(total_pnl));
        INFO("  Daily return: " + std::to_string(daily_return) + "%");
        INFO("  Annualized return: " + std::to_string(total_return_annualized) + "%");
        
        std::cout << "Total P&L: $" << std::fixed << std::setprecision(2) << total_pnl << std::endl;
        std::cout << "Realized P&L: $" << std::fixed << std::setprecision(2) << total_realized_pnl << std::endl;
        std::cout << "Unrealized P&L: $" << std::fixed << std::setprecision(2) << total_unrealized_pnl << std::endl;
        std::cout << "Current Portfolio Value: $" << std::fixed << std::setprecision(2) << current_portfolio_value << std::endl;
        std::cout << "Total Return (Cumulative): " << std::fixed << std::setprecision(2) << total_cumulative_return_pct << "%" << std::endl;
        std::cout << "Total Return (Annualized): " << std::fixed << std::setprecision(2) << total_return_annualized << "%" << std::endl;
        std::cout << "Daily Return: " << std::fixed << std::setprecision(2) << daily_return << "%" << std::endl;
        std::cout << "Portfolio Leverage: " << std::fixed << std::setprecision(2) 
                  << (gross_notional / current_portfolio_value) << "x" << std::endl;
        std::cout << "Posted Margin (Initial×Contracts): $" << std::fixed << std::setprecision(2)
                  << total_posted_margin << std::endl;
        std::cout << "Equity-to-Margin Ratio: " << std::fixed << std::setprecision(2)
                  << equity_to_margin_ratio << "x" << std::endl;
        double margin_cushion = 0.0;
        if (maintenance_requirement_today > 0.0) {
            // Correct formula: margin_cushion = (equity - maintenance) / equity
            // This shows how much cushion we have above maintenance margin requirements
            margin_cushion = (current_portfolio_value - maintenance_requirement_today) / current_portfolio_value;
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
        if (equity_to_margin_ratio > 4.0) {
            WARN("Equity-to-Margin Ratio above 4x.");
        }

        // Get forecasts for all symbols
        INFO("Retrieving current forecasts...");
        std::cout << "\n======= Current Forecasts =======" << std::endl;
        std::cout << std::setw(10) << "Symbol" << " | "
                  << std::setw(12) << "Forecast" << " | "
                  << std::setw(12) << "Position" << std::endl;
        std::cout << std::string(40, '-') << std::endl;

        // Collect signals for database storage
        std::unordered_map<std::string, double> signals_to_store;

        for (const auto& symbol : symbols) {
            double z_score = mr_strategy->get_z_score(symbol);
            double position = mr_strategy->get_position(symbol);

            signals_to_store[symbol] = z_score;

            std::cout << std::setw(10) << symbol << " | "
                      << std::setw(12) << std::fixed << std::setprecision(4) << z_score << " | "
                      << std::setw(12) << std::fixed << std::setprecision(2) << position << std::endl;
        }

        // Store signals using LiveResultsManager
        if (!signals_to_store.empty()) {
            INFO("Setting " + std::to_string(signals_to_store.size()) + " signals in LiveResultsManager...");
            results_manager->set_signals(signals_to_store);
        } else {
            INFO("No signals to store (all forecasts are zero)");
        }

        // Save trading results to results table
        INFO("Saving trading results to database...");
        try {
            // Calculate current date for results (use override date if specified)
            auto current_date = now;
            
            // E2-F33: the thirteen `= 0.0; // Would need historical data to calculate`
            // placeholders that stood here are gone, along with the claim in their comments.
            // The history they said was unavailable is four SELECTs away and both futures
            // runners have loaded it since they were written; they were never written to a
            // column, which is why the block read NULL rather than zero. What replaces them
            // is `historical_metrics` above.
            //
            // `volatility` IS one of them now (D3). It used to be assigned
            // `risk_eval.portfolio_var * 100` here -- the ex-ante instrument-mix sigma
            // sqrt(w'Sigma w), with w normalised to GROSS exposure. On a one-stock book that is
            // simply that stock's own annualised sigma (24.65 % for TMUS, 21.87 % for ABT, 0
            // when the book is flat): it ignores leverage -- the book was 5 % invested -- and
            // says nothing about the account's returns, so `sharpe_ratio` could not be
            // reproduced from the row it was written on (-4.70 / 21.89 = -0.21, not -5.89).
            //
            // Both futures runners store the REALISED annualised return volatility in this
            // column and keep the ex-ante sigma in `portfolio_var`
            // (live_portfolio_conservative.cpp, `volatility = historical_metrics.volatility`
            // after the calculate() call, and `{"portfolio_var", portfolio_var}` beside it).
            // One column may not mean two things on two books. `portfolio_var`, `var_95` and
            // `cvar_95` keep the ex-ante figure unchanged -- the risk gate reads it and nothing
            // is lost.
            double volatility = 0.0;
            double var_95 = 0.0;
            double cvar_95 = 0.0;
            double beta = 0.0;
            double correlation = 0.0;
            
            // The ex-ante risk figures. NOT volatility any more -- see above.
            if (risk_eval.is_ok()) {
                const auto& r = risk_eval.value();
                var_95 = r.portfolio_var * 100.0;     // Use portfolio VaR as proxy
                cvar_95 = r.portfolio_var * 100.0;    // Use portfolio VaR as proxy (no CVaR available)
                beta = 0.0;                           // No beta available in RiskResult
                correlation = r.correlation_risk;     // Use correlation risk
            }
            
            // Create configuration JSON
            nlohmann::json config_json;
            config_json["strategy_type"] = "LIVE_EQUITY_MEAN_REVERSION";
            config_json["capital_allocation"] = mr_config.capital_allocation;
            config_json["max_leverage"] = mr_config.max_leverage;
            config_json["lookback_period"] = mean_rev_config.lookback_period;
            config_json["risk_target"] = mean_rev_config.risk_target;
            config_json["entry_threshold"] = mean_rev_config.entry_threshold;
            config_json["active_positions"] = active_positions;
            config_json["gross_notional"] = gross_notional;
            config_json["net_notional"] = net_notional;
            config_json["portfolio_leverage"] = gross_notional / initial_capital;
            
            // Phase 6 §6c: removed unused date_ss timestamp string (dead
            // code -- the live_results insert is now driven by the
            // store_live_results_complete dynamic-column API rather than
            // a hand-rolled SQL string).

            // Use calculated metrics from position analysis
            double portfolio_var = 0.0;
            double gross_leverage = 0.0;
            double net_leverage = 0.0;
            double max_correlation = 0.0;
            double jump_risk = 0.0;
            double risk_scale = 1.0;

            if (risk_eval.is_ok()) {
                const auto& r = risk_eval.value();
                portfolio_var = r.portfolio_var;
                gross_leverage = r.gross_leverage;
                net_leverage = r.net_leverage;
                max_correlation = r.correlation_risk;
                jump_risk = r.jump_risk;
                risk_scale = r.recommended_scale;
            }

            // Use LiveMetricsCalculator for portfolio metrics
            double portfolio_leverage = metrics_calculator->calculate_gross_leverage(gross_notional, current_portfolio_value);
            // equity_to_margin_ratio and margin_cushion already computed above
            
            // Use the LiveResultsManager
            INFO("Setting metrics in LiveResultsManager...");

            // Phase 4.5: cumulative dividend income, sourced from the
            // Phase 4 corp-action state file. Informational ONLY -- NOT
            // added to total_pnl: bars carry total-return adjusted prices
            // (Phase 4.2 computes that in-engine from per-bar div_cash), so
            // dividend value is already inside mark-to-market P&L.
            double total_dividend_income = 0.0;
            {
                CorporateActionsAuditLog div_log(ca_state_dir, db,
                                                       portfolio_id,
                                                       "LIVE_EQUITY_MEAN_REVERSION",
                                                       "EQUITY_MEAN_REVERSION");
                auto div_loaded = div_log.load();
                if (div_loaded.is_error()) {
                    // Reporting-only: the trading decisions are already made and
                    // persisted by this point, and this figure is informational
                    // (never added to P&L). Under-reporting it is preferable to
                    // failing a completed run, but it must not pass silently.
                    WARN("Cannot read dividend income from the corp-action dedup "
                         "record: " + std::string(div_loaded.error()->what()) +
                         " -- reporting 0; the stored value is unaffected");
                }
                total_dividend_income = div_log.total_cumulative_dividend_income();
            }

            // ================= E2-F33: the same block for day T ========================
            //
            // The T-1 pass above repairs yesterday's row once its mark is final. Today's row
            // is INSERTed below and would stay NULL until tomorrow's run -- and the LAST day
            // of any replay never gets a tomorrow, so "every row carries the block" needs
            // this too. Mirrors live_portfolio_conservative.cpp:2743-2830.
            //
            // History is loaded as of previous_date (fully finalized days only) and today's
            // own figures are appended, so the series ends with a day-T value that is
            // consistent with the columns written in the same statement.
            HistoricalMetrics historical_metrics;
            try {
                if (data_loader && data_loader->is_connected()) {
                    auto returns_hist_res = data_loader->load_daily_returns_history(
                        kEquityStrategyId, portfolio_id, previous_date);
                    auto pnl_hist_res = data_loader->load_daily_pnl_history(
                        kEquityStrategyId, portfolio_id, previous_date);
                    auto equity_hist_res = data_loader->load_equity_curve_history(
                        kEquityStrategyId, portfolio_id, previous_date);
                    auto trades_hist_res = data_loader->load_total_trades_count(
                        kEquityStrategyId, portfolio_id, now);

                    std::vector<double> returns_hist;
                    std::vector<double> pnl_hist;
                    std::vector<double> equity_hist;
                    int total_trades_hist = 0;

                    if (returns_hist_res.is_ok()) returns_hist = returns_hist_res.value();
                    if (pnl_hist_res.is_ok()) pnl_hist = pnl_hist_res.value();
                    if (equity_hist_res.is_ok()) equity_hist = equity_hist_res.value();
                    if (trades_hist_res.is_ok()) total_trades_hist = trades_hist_res.value();

                    // Already in percent on both sides (the stored column and daily_return
                    // here), so appended as-is. Do NOT scale.
                    returns_hist.push_back(daily_return);
                    pnl_hist.push_back(daily_pnl);
                    equity_hist.push_back(current_portfolio_value);

                    LiveHistoricalMetricsCalculator hist_calc;
                    historical_metrics =
                        hist_calc.calculate(returns_hist, pnl_hist, equity_hist,
                                            total_return_annualized, total_trades_hist);

                    // D3: keep the `volatility` local aligned with the return-volatility
                    // definition, the same line live_portfolio_conservative.cpp carries after
                    // its own calculate(). The metric map below takes the column from the
                    // shared helper, so this is belt and braces -- but the local is what the
                    // email and the console report read.
                    volatility = historical_metrics.volatility;

                    historical_metrics.total_days = trading_days_count;
                    if (trading_days_count > 0) {
                        historical_metrics.win_rate =
                            static_cast<double>(historical_metrics.winning_days) /
                            static_cast<double>(trading_days_count) * 100.0;
                    }

                    INFO("HIST_METRICS [Day T]: return_volatility=" +
                         std::to_string(historical_metrics.volatility) +
                         " downside_deviation=" +
                         std::to_string(historical_metrics.downside_deviation) +
                         " sharpe=" + std::to_string(historical_metrics.sharpe_ratio) +
                         " sortino=" + std::to_string(historical_metrics.sortino_ratio) +
                         " max_drawdown=" + std::to_string(historical_metrics.max_drawdown) +
                         " winning_days=" + std::to_string(historical_metrics.winning_days) +
                         " losing_days=" + std::to_string(historical_metrics.losing_days) +
                         " total_days=" + std::to_string(historical_metrics.total_days) +
                         " win_rate=" + std::to_string(historical_metrics.win_rate) +
                         " avg_win=" + std::to_string(historical_metrics.avg_win) +
                         " avg_loss=" + std::to_string(historical_metrics.avg_loss) +
                         " best_day=" + std::to_string(historical_metrics.best_day) +
                         " worst_day=" + std::to_string(historical_metrics.worst_day) +
                         " gross_profit=" + std::to_string(historical_metrics.gross_profit) +
                         " gross_loss=" + std::to_string(historical_metrics.gross_loss) +
                         " profit_factor=" + std::to_string(historical_metrics.profit_factor) +
                         " (returns n=" + std::to_string(returns_hist.size()) +
                         ", equity n=" + std::to_string(equity_hist.size()) +
                         ", executions=" + std::to_string(total_trades_hist) + ")");
                } else {
                    WARN("LiveDataLoader not available or not connected; the day-T historical "
                         "performance metrics stay at their defaults (E2-F33).");
                }
            } catch (const std::exception& e) {
                WARN("Exception while calculating day-T historical performance metrics: " +
                     std::string(e.what()));
            }

            // Prepare metrics maps
            std::unordered_map<std::string, double> double_metrics = {
                {"total_cumulative_return", total_cumulative_return_pct},
                {"total_annualized_return", total_return_annualized},
                {"volatility", volatility},
                {"total_pnl", total_pnl},
                {"total_unrealized_pnl", total_unrealized_pnl},
                {"total_realized_pnl", total_realized_pnl},
                {"current_portfolio_value", current_portfolio_value},
                {"portfolio_var", portfolio_var},
                {"gross_leverage", gross_leverage},
                {"net_leverage", net_leverage},
                {"portfolio_leverage", portfolio_leverage},
                {"equity_to_margin_ratio", equity_to_margin_ratio},
                {"margin_cushion", margin_cushion},
                {"max_correlation", max_correlation},
                {"jump_risk", jump_risk},
                {"risk_scale", risk_scale},
                {"gross_notional", gross_notional},
                {"net_notional", net_notional},
                {"daily_return", daily_return},
                {"daily_pnl", daily_pnl},
                // trading.live_results has no commissions columns -- it carries the
                // transaction-cost pair the futures runner writes. For equities the
                // commission IS the realised transaction cost, so it lands there.
                // Naming a non-existent column makes the whole INSERT fail, which is
                // how live_results silently went unwritten (E2-F5).
                {"total_transaction_costs", total_commissions_cumulative},
                {"daily_realized_pnl", daily_realized_pnl},
                {"daily_unrealized_pnl", daily_unrealized_pnl},
                {"daily_transaction_costs", total_daily_commissions},
                {"margin_posted", total_posted_margin},
                {"cash_available", current_portfolio_value - total_posted_margin},
                {"total_dividend_income", total_dividend_income}
            };

            std::unordered_map<std::string, int> int_metrics = {
                {"active_positions", active_positions}
            };

            // E2-F33: the same columns the Day T-1 UPDATE writes, from the same helper --
            // sixteen since D3 folded `volatility` in. The helper's entry overwrites the
            // literal `{"volatility", volatility}` above with the identical figure, because
            // the local was assigned from the same `historical_metrics` after calculate().
            for (const auto& [column, value] :
                 historical_metrics_double_columns(historical_metrics)) {
                double_metrics[column] = value;
            }
            for (const auto& [column, value] :
                 historical_metrics_int_columns(historical_metrics)) {
                int_metrics[column] = value;
            }

            // Set all metrics at once
            results_manager->set_metrics(double_metrics, int_metrics);

            // Set config
            results_manager->set_config(config_json);

            // Set equity for equity curve tracking
            results_manager->set_equity(current_portfolio_value);
        } catch (const std::exception& e) {
            ERROR("Exception while saving trading results: " + std::string(e.what()));
        }

        // Phase 4: Use CSVExporter for position export
        INFO("Using CSVExporter to save positions to file...");

        // Query daily commissions per symbol using LiveDataLoader
        std::unordered_map<std::string, double> symbol_commissions;
        try {
            auto commission_result = data_loader->load_commissions_by_symbol(portfolio_id, now);
            if (commission_result.is_ok()) {
                symbol_commissions = commission_result.value();
                INFO("Loaded commissions for " + std::to_string(symbol_commissions.size()) + " symbols via LiveDataLoader");
            } else {
                WARN("Failed to query commissions via LiveDataLoader: " + std::string(commission_result.error()->what()));
            }
        } catch (const std::exception& e) {
            WARN("Exception querying commissions: " + std::string(e.what()));
        }

        // CSV Export is not yet available for mean reversion strategies
        // TODO: Extend CSVExporter to support mean reversion strategies
        INFO("CSV export skipped - not yet available for mean reversion strategies");

        // CSV export of finalized positions skipped (see above)
        // TODO: Extend CSVExporter to support mean reversion strategies
        // Store equity curve and save all results to database
        // Use the new LiveResultsManager - save all results at once
        INFO("Saving all live trading results using LiveResultsManager...");

        // E2-F19 (R-3): clear today's position rows for this book, unconditionally, in the
        // same breath as the write.
        //
        // store_positions is DELETE-then-INSERT keyed on the date of the rows it is handed,
        // and save_positions_snapshot returns early on an empty book -- so a day whose day-T
        // write is EMPTY never deletes anything, and whatever rows already sit on today's
        // date survive as today's book. Two writers put rows there before this point: the
        // corp-action placeholder stores (dated today, written even when zero adjustments
        // applied) and, through the loader's timestamp drift, a T-1 row that has been
        // rewritten four times. Measured on the pre-fix chain: TMUS closed on 2026-07-07
        // with no realized row to keep, the placeholder row (qty 17.6) survived as the
        // 07-07 book, and the runner sold the same 17.6 shares again every day through
        // 07-27 while booking mark P&L on stock it no longer held. Scoped exactly like
        // store_positions' own delete.
        //
        // E2-F44 (BA-18): it sits HERE, not up with the day-T row build where it used to,
        // because the only thing that must precede the INSERT is this DELETE -- while three
        // fatal checks and the whole Day T-1 finalization sat between the two. Deleting
        // today's rows and then exiting 1 on an identity violation left the day both empty
        // and unwritten, which is strictly worse than leaving the previous run's rows in
        // place for the re-run to overwrite. Nothing between the old site and this one reads
        // or writes trading.positions for today: delete_stale_data() inside
        // save_all_results covers live_results, equity_curve and executions but explicitly
        // NOT positions, which is why this statement exists at all.
        {
            const std::string clear_today =
                "DELETE FROM trading.positions WHERE strategy_id = '" +
                std::string(kEquityStrategyId) + "' AND strategy_name = '" +
                std::string(kEquityStrategyName) + "' AND portfolio_id = '" + portfolio_id +
                "' AND DATE(last_update) = '" + today_date_str + "'";
            auto cleared = db->execute_direct_query(clear_today);
            if (cleared.is_error()) {
                ERROR("Failed to clear today's position rows before the day-T write: " +
                      std::string(cleared.error()->what()) +
                      ". Refusing to continue: an empty day-T write would leave stale rows "
                      "as today's book.");
                return 1;
            }
            INFO("Cleared any existing position rows for " + today_date_str +
                 " ahead of the day-T write (" + std::to_string(positions_to_save.size()) +
                 " row(s) queued)");
        }

        bool persist_failed = false;
        auto save_result = results_manager->save_all_results("LIVE_EQUITY_MEAN_REVERSION", now);
        if (save_result.is_error()) {
            // save_all_results attempts every table and names the ones that failed
            // (FIX-0b). Logging that and returning 0 anyway defeats the point: a caller
            // reading the exit status would treat a partially-written run as a success.
            persist_failed = true;
            ERROR("Failed to save all live results: " + std::string(save_result.error()->what()));
        } else {
            INFO("Successfully saved all live trading results to database");
        }

        // Stop the strategy
        INFO("Stopping strategy...");
        auto stop_result = mr_strategy->stop();
        if (stop_result.is_error()) {
            ERROR("Failed to stop strategy: " + std::string(stop_result.error()->what()));
        } else {
            INFO("Strategy stopped successfully");
        }

        std::cout << "\n======= Daily Processing Complete =======" << std::endl;
        // CSV export disabled - see above
        // Removed yesterday finalized positions file output per request
        // Only show processing time for real-time runs, not historical
        if (!use_override_date) {
            std::cout << "Total processing time: " << std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now() - now).count() << "ms" << std::endl;
        }

        INFO("Daily mean reversion position generation completed successfully");

        // Send email report with trading results (based on send_email flag)
        if (send_email) {
            INFO("Sending email report...");
            try {
                EmailSenderConfig email_config;
                email_config.smtp_host = app_config.email.smtp_host;
                email_config.smtp_port = app_config.email.smtp_port;
                email_config.username = app_config.email.username;
                email_config.password = app_config.email.password;
                email_config.use_tls = app_config.email.use_tls;
                email_config.to_emails = app_config.email.to_emails;

                auto email_sender = std::make_shared<EmailSender>(email_config);
                auto email_init_result = email_sender->initialize();
                if (email_init_result.is_error()) {
                    ERROR("Failed to initialize email sender: " + std::string(email_init_result.error()->what()));
                } else {
                // Prepare email data
                std::string date_str = std::to_string(now_tm->tm_year + 1900) + "-"
                                     + std::string(2 - std::to_string(now_tm->tm_mon + 1).length(), '0')
                                     + std::to_string(now_tm->tm_mon + 1) + "-"
                                     + std::string(2 - std::to_string(now_tm->tm_mday).length(), '0')
                                     + std::to_string(now_tm->tm_mday);

                std::string subject = "Daily Trading Report - " + date_str;

                // Load yesterday's finalized positions for email display
                std::unordered_map<std::string, Position> yesterday_positions_finalized;
                std::map<std::string, double> yesterday_daily_metrics_final;
                std::unordered_map<std::string, double> yesterday_entry_prices;  // Day T-2 close
                std::unordered_map<std::string, double> yesterday_exit_prices;   // Day T-1 close

                // Phase 6 §6c: UTC date string via format_utc_date.
                const std::string yesterday_date_for_email =
                    core::format_utc_date(previous_date);

                INFO("Loading yesterday's finalized positions for email: " + yesterday_date_for_email);

                std::string positions_query_email = "SELECT symbol, quantity, average_price, daily_realized_pnl, daily_unrealized_pnl, last_update "
                                                   "FROM trading.positions "
                                                   "WHERE strategy_id = 'LIVE_EQUITY_MEAN_REVERSION' AND portfolio_id = '" + portfolio_id + "' AND DATE(last_update) = '" + yesterday_date_for_email + "'";

                auto positions_result_email = db->execute_query(positions_query_email);

                if (positions_result_email.is_ok() && positions_result_email.value()->num_rows() > 0) {
                    auto table_email = positions_result_email.value();
                    // All columns are StringArrays from generic converter
                    auto symbol_arr = std::static_pointer_cast<arrow::StringArray>(table_email->column(0)->chunk(0));
                    auto quantity_arr = std::static_pointer_cast<arrow::StringArray>(table_email->column(1)->chunk(0));
                    auto avg_price_arr = std::static_pointer_cast<arrow::StringArray>(table_email->column(2)->chunk(0));
                    auto realized_pnl_arr = std::static_pointer_cast<arrow::StringArray>(table_email->column(3)->chunk(0));

                    for (int64_t i = 0; i < table_email->num_rows(); ++i) {
                        if (!symbol_arr->IsNull(i) && !quantity_arr->IsNull(i)) {
                            std::string symbol = symbol_arr->GetString(i);
                            double quantity = std::stod(quantity_arr->GetString(i));
                            double avg_price = std::stod(avg_price_arr->GetString(i));
                            double realized_pnl = std::stod(realized_pnl_arr->GetString(i));

                            // Skip positions with zero quantity
                            if (std::abs(quantity) < 0.0001) continue;

                            // Create Position object for yesterday's finalized position
                            Position pos;
                            pos.symbol = symbol;
                            pos.quantity = Decimal(quantity);
                            pos.average_price = Decimal(avg_price);
                            pos.realized_pnl = Decimal(realized_pnl);

                            yesterday_positions_finalized[symbol] = pos;

                            // Populate entry and exit prices
                            if (two_days_ago_close_prices.find(symbol) != two_days_ago_close_prices.end()) {
                                yesterday_entry_prices[symbol] = two_days_ago_close_prices[symbol];
                            }
                            if (previous_day_close_prices.find(symbol) != previous_day_close_prices.end()) {
                                yesterday_exit_prices[symbol] = previous_day_close_prices[symbol];
                            }
                        }
                    }
                    INFO("Loaded " + std::to_string(yesterday_positions_finalized.size()) + " finalized positions for email");

                    // Load yesterday's daily metrics from database for accurate display.
                    // Phase 4.5: total_dividend_income (informational; cumulative for the
                    // strategy as of that date) added to the SELECT and surfaced in the
                    // email body alongside Daily Total PnL.
                    std::string yesterday_metrics_query =
                        "SELECT daily_return, daily_unrealized_pnl, daily_realized_pnl, daily_pnl, "
                        "daily_transaction_costs, total_dividend_income "
                        "FROM trading.live_results "
                        "WHERE strategy_id = 'LIVE_EQUITY_MEAN_REVERSION' AND portfolio_id = '" + portfolio_id + "' AND date = '" + yesterday_date_for_email + "' "
                        "ORDER BY date DESC LIMIT 1";

                    INFO("Loading yesterday's daily metrics from live_results: " + yesterday_metrics_query);
                    auto yesterday_metrics_result = db->execute_query(yesterday_metrics_query);

                    if (yesterday_metrics_result.is_ok() && yesterday_metrics_result.value()->num_rows() > 0) {
                        auto metrics_table = yesterday_metrics_result.value();
                        INFO("Retrieved " + std::to_string(metrics_table->num_rows()) + " rows from live_results");

                        auto daily_return_arr = std::static_pointer_cast<arrow::StringArray>(metrics_table->column(0)->chunk(0));
                        auto daily_unrealized_arr = std::static_pointer_cast<arrow::StringArray>(metrics_table->column(1)->chunk(0));
                        auto daily_realized_arr = std::static_pointer_cast<arrow::StringArray>(metrics_table->column(2)->chunk(0));
                        auto daily_total_arr = std::static_pointer_cast<arrow::StringArray>(metrics_table->column(3)->chunk(0));
                        auto daily_commissions_arr = std::static_pointer_cast<arrow::StringArray>(metrics_table->column(4)->chunk(0));
                        auto dividend_income_arr = std::static_pointer_cast<arrow::StringArray>(metrics_table->column(5)->chunk(0));

                        if (!daily_return_arr->IsNull(0)) {
                            yesterday_daily_metrics_final["Daily Return"] = std::stod(daily_return_arr->GetString(0));
                            INFO("Daily Return: " + daily_return_arr->GetString(0));
                        }
                        if (!daily_unrealized_arr->IsNull(0)) {
                            yesterday_daily_metrics_final["Daily Unrealized PnL"] = std::stod(daily_unrealized_arr->GetString(0));
                            INFO("Daily Unrealized PnL: " + daily_unrealized_arr->GetString(0));
                        }
                        if (!daily_realized_arr->IsNull(0)) {
                            yesterday_daily_metrics_final["Daily Realized PnL"] = std::stod(daily_realized_arr->GetString(0));
                            INFO("Daily Realized PnL: " + daily_realized_arr->GetString(0));
                        }
                        if (!daily_total_arr->IsNull(0)) {
                            yesterday_daily_metrics_final["Daily Total PnL"] = std::stod(daily_total_arr->GetString(0));
                            INFO("Daily Total PnL: " + daily_total_arr->GetString(0));
                        }

                        if (!daily_commissions_arr->IsNull(0)) {
                            yesterday_daily_metrics_final["Daily Commissions"] = std::stod(daily_commissions_arr->GetString(0));
                            INFO("Daily Commissions: " + daily_commissions_arr->GetString(0));
                        }
                        if (!dividend_income_arr->IsNull(0)) {
                            // Phase 4.5: informational only -- NOT in any PnL total.
                            yesterday_daily_metrics_final["Dividend Income (cumulative)"] =
                                std::stod(dividend_income_arr->GetString(0));
                            INFO("Total Dividend Income (cumulative): " +
                                 dividend_income_arr->GetString(0));
                        }

                        INFO("Successfully loaded yesterday's daily metrics from live_results");
                    } else {
                        if (yesterday_metrics_result.is_error()) {
                            ERROR("Failed to query live_results: " + std::string(yesterday_metrics_result.error()->what()));
                        } else {
                            WARN("No rows found in live_results for date: " + yesterday_date_for_email);
                        }
                        // Fallback: calculate from positions if database query fails
                        double yesterday_daily_realized = 0.0;
                        for (const auto& [symbol, pos] : yesterday_positions_finalized) {
                            yesterday_daily_realized += pos.realized_pnl.as_double();
                        }
                        yesterday_daily_metrics_final["Daily Realized PnL"] = yesterday_daily_realized;
                        INFO("Calculated yesterday's metrics from positions (fallback) - Daily Realized PnL: " + std::to_string(yesterday_daily_realized));
                    }

                } else {
                    INFO("No finalized positions found for yesterday's email table");
                }
                
                // Create strategy metrics map with all relevant metrics organized by category
                std::map<std::string, double> strategy_metrics;

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
                if (risk_eval.is_ok()) {
                    strategy_metrics["Volatility"] = risk_eval.value().portfolio_var * 100.0;
                }
                strategy_metrics["Total Commissions"] = total_commissions_cumulative;
                strategy_metrics["Current Portfolio Value"] = current_portfolio_value;

                // Phase 4.5: cumulative dividend income from the corp-action
                // state file (informational ONLY; not in any PnL total).
                {
                    CorporateActionsAuditLog div_log_email(ca_state_dir, db,
                                                       portfolio_id,
                                                       "LIVE_EQUITY_MEAN_REVERSION",
                                                       "EQUITY_MEAN_REVERSION");
                    auto div_email_loaded = div_log_email.load();
                    if (div_email_loaded.is_error()) {
                        WARN("Cannot read dividend income for the email report: " +
                             std::string(div_email_loaded.error()->what()) +
                             " -- reporting 0");
                    }
                    strategy_metrics["Dividend Income (cumulative, informational)"] =
                        div_log_email.total_cumulative_dividend_income();
                }

                // Leverage Metrics - Calculate values from position analysis
                double gross_leverage_calc = (current_portfolio_value != 0.0) ? (gross_notional / current_portfolio_value) : 0.0;
                double net_leverage_calc = (current_portfolio_value != 0.0) ? (net_notional / current_portfolio_value) : 0.0;
                double portfolio_leverage_calc = (current_portfolio_value != 0.0) ? (gross_notional / current_portfolio_value) : 0.0;
                
                strategy_metrics["Gross Leverage"] = gross_leverage_calc;
                strategy_metrics["Net Leverage"] = net_leverage_calc;
                strategy_metrics["Portfolio Leverage"] = portfolio_leverage_calc;
                strategy_metrics["Equity-to-Margin Ratio"] = equity_to_margin_ratio;

                // Risk & Liquidity Metrics
                strategy_metrics["Margin Cushion"] = margin_cushion * 100.0; // Convert to percentage
                strategy_metrics["Margin Posted"] = total_posted_margin;
                strategy_metrics["Cash Available"] = current_portfolio_value - total_posted_margin;

                // Note: yesterday_daily_metrics_final is now loaded AFTER database updates above
                // So we don't need to create it here anymore

                // Generate email body with is_daily_strategy flag set to true and current prices
                std::string email_body = email_sender->generate_trading_report_body(
                    positions,
                    risk_eval.is_ok() ? std::make_optional(risk_eval.value()) : std::nullopt,
                    strategy_metrics,
                    daily_executions,
                    date_str,
                    true,  // is_daily_strategy
                    previous_day_close_prices,  // Pass Day T-1 close prices for today's positions
                    db,  // Pass database for symbols reference table
                    yesterday_positions_finalized,  // Now populated with yesterday's finalized positions
                    yesterday_exit_prices,  // Day T-1 close prices for yesterday's positions
                    yesterday_entry_prices,  // Day T-2 close prices for yesterday's positions
                    yesterday_daily_metrics_final,  // Yesterday's metrics
                    // E2-F11: the charts query THIS book. The overload used to hardcode the
                    // trend-following strategy id and an empty portfolio, so every equity
                    // email rendered empty charts.
                    std::string(kEquityStrategyId),
                    portfolio_id
                );
                
                // Send email without CSV attachments (CSV export disabled for mean reversion)
                std::vector<std::string> attachments = {};  // No CSV files for mean reversion

                auto send_result = email_sender->send_email(subject, email_body, true, attachments);
                if (send_result.is_error()) {
                    ERROR("Failed to send email: " + std::string(send_result.error()->what()));
                } else {
                    INFO("Email report sent successfully (without CSV attachments)");
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

        if (persist_failed) {
            ERROR("Run completed but one or more result tables were not persisted -- "
                  "exiting non-zero so this is not mistaken for a successful run.");
            return 1;
        }
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
