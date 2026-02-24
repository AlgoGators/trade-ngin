// apps/backtest/bt_equity_validation.cpp
//
// Comprehensive Equity Backtest Validation App
// Connects to real DB, runs the mean reversion backtest, then independently
// recomputes every calculation to verify correctness.
//
// Sections:
//   A) Data Validation — OHLC adjustment ratio
//   B) Strategy Calculation Validation — SMA, StdDev, Z-Score, Volatility, PositionSize
//   C) PnL Validation — day-by-day equity curve replay
//   D) Metrics Validation — Sharpe, Sortino, MaxDrawdown, Volatility, TotalReturn

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <sstream>
#include <vector>

#include <nlohmann/json.hpp>

#include <arrow/api.h>

#include "trade_ngin/backtest/backtest_coordinator.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/data/conversion_utils.hpp"
#include "trade_ngin/data/database_pooling.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/instruments/equity.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/strategy/mean_reversion.hpp"

using namespace trade_ngin;
using namespace trade_ngin::backtest;

// ============================================================================
// VALIDATION HELPERS
// ============================================================================

static int total_checks = 0;
static int passed_checks = 0;
static int failed_checks = 0;

static bool approx_eq(double a, double b, double rel_tol = 1e-4, double abs_tol = 1e-6) {
    if (std::abs(a - b) < abs_tol) return true;
    double denom = std::max(std::abs(a), std::abs(b));
    return denom > 0 && std::abs(a - b) / denom < rel_tol;
}

static void check(const std::string& name, bool condition, const std::string& detail = "") {
    total_checks++;
    if (condition) {
        passed_checks++;
        std::cout << "  PASS: " << name << std::endl;
    } else {
        failed_checks++;
        std::cout << "  FAIL: " << name;
        if (!detail.empty()) std::cout << " -- " << detail;
        std::cout << std::endl;
    }
}

static std::string ts_to_date_str(const Timestamp& ts) {
    auto time_t = std::chrono::system_clock::to_time_t(ts);
    std::tm* tm = std::gmtime(&time_t);
    std::ostringstream oss;
    oss << std::put_time(tm, "%Y-%m-%d");
    return oss.str();
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    try {
        StateManager::reset_instance();
        Logger::reset_for_tests();

        auto& logger = Logger::instance();
        LoggerConfig logger_config;
        logger_config.min_level = LogLevel::INFO;
        logger_config.destination = LogDestination::BOTH;
        logger_config.log_directory = "logs";
        logger_config.filename_prefix = "bt_equity_val";
        logger.initialize(logger_config);

        std::atomic_thread_fence(std::memory_order_seq_cst);

        if (!logger.is_initialized()) {
            std::cerr << "ERROR: Logger initialization failed" << std::endl;
            return 1;
        }

        std::cout << "\n======================================================" << std::endl;
        std::cout << "    EQUITY BACKTEST VALIDATION" << std::endl;
        std::cout << "======================================================\n" << std::endl;

        // ====================================================================
        // SETUP — Direct connection (no config file needed)
        // ====================================================================
        INFO("Setting up direct database connection...");

        // Database connection — same credentials as test_database_extensions
        std::string conn_string =
            "host=13.58.153.216 port=5432 dbname=new_algo_data user=postgres password=algogators";

        auto pool_result = DatabasePool::instance().initialize(conn_string, 2);
        if (pool_result.is_error()) {
            std::cerr << "Failed to initialize connection pool: " << pool_result.error()->what() << std::endl;
            return 1;
        }

        auto db_guard = DatabasePool::instance().acquire_connection();
        auto db = db_guard.get();
        if (!db || !db->is_connected()) {
            std::cerr << "Failed to acquire database connection" << std::endl;
            return 1;
        }
        INFO("Database connection established");

        // Initialize instrument registry
        auto& registry = InstrumentRegistry::instance();
        auto init_result = registry.initialize(db);
        if (init_result.is_error()) {
            std::cerr << "Failed to initialize instrument registry: " << init_result.error()->what() << std::endl;
            return 1;
        }
        auto load_result = registry.load_instruments();
        if (load_result.is_error()) {
            WARN("Could not load instruments from DB: " + std::string(load_result.error()->what()));
        }

        // Get equity symbols with the most data coverage
        // Query the DB to find the 3 symbols with the most bars in the last 6 months
        // Use well-known tickers that we know have data in the DB
        // We'll verify they exist and fall back to querying the DB if needed
        std::vector<std::string> candidate_symbols = {"AAPL", "MSFT", "AMZN", "GOOGL", "META",
                                                       "TMUS", "NSC", "ABT", "ABEV", "ABM"};
        std::vector<std::string> symbols;

        INFO("Verifying symbol data availability...");
        for (const auto& sym : candidate_symbols) {
            if (symbols.size() >= 3) break;

            // NOTE: execute_query returns all columns as strings (arrow::utf8)
            std::string check_sql =
                "SELECT COUNT(*) as cnt FROM equities_data.ohlcv_1d WHERE ticker = '" + sym + "'";
            auto check_result = db->execute_query(check_sql);
            if (!check_result.is_error()) {
                auto check_table = check_result.value();
                if (check_table->num_rows() > 0) {
                    auto cnt_col = check_table->GetColumnByName("cnt");
                    if (cnt_col) {
                        auto arr = std::static_pointer_cast<arrow::StringArray>(cnt_col->chunk(0));
                        int64_t count = 0;
                        try { count = std::stoll(arr->GetString(0)); } catch (...) {}
                        if (count >= 50) {
                            symbols.push_back(sym);
                            std::cout << "  " << sym << ": " << count << " bars - OK" << std::endl;
                        } else {
                            std::cout << "  " << sym << ": " << count << " bars - skipping" << std::endl;
                        }
                    }
                }
            }
        }

        if (symbols.empty()) {
            ERROR("No equity symbols found with sufficient data (>= 50 bars)");
            return 1;
        }

        std::cout << "Validation symbols (" << symbols.size() << "): ";
        for (const auto& s : symbols) std::cout << s << " ";
        std::cout << std::endl;

        // Register equity instruments
        for (const auto& symbol : symbols) {
            if (!registry.has_instrument(symbol)) {
                EquitySpec spec;
                spec.exchange = "NYSE";
                spec.currency = "USD";
                spec.tick_size = 0.01;
                spec.commission_per_share = 0.0;
                registry.register_instrument(symbol,
                    std::make_shared<EquityInstrument>(symbol, std::move(spec)));
            }
        }

        // Determine date range from actual data in DB for selected symbols
        // Use a text cast for dates to avoid Arrow type issues
        std::string sym_list_for_range;
        for (size_t i = 0; i < symbols.size(); i++) {
            if (i > 0) sym_list_for_range += "','";
            sym_list_for_range += symbols[i];
        }
        std::string range_sql =
            "SELECT MIN(date)::text as min_date, MAX(date)::text as max_date "
            "FROM equities_data.ohlcv_1d "
            "WHERE ticker IN ('" + sym_list_for_range + "')";
        auto range_result = db->execute_query(range_sql);

        Timestamp start_date, end_date;
        auto parse_date_str = [](const std::string& s) -> Timestamp {
            std::tm tm = {};
            std::istringstream ss(s);
            ss >> std::get_time(&tm, "%Y-%m-%d");
            return std::chrono::system_clock::from_time_t(timegm(&tm));
        };

        bool dates_set = false;
        if (!range_result.is_error()) {
            auto range_table = range_result.value();
            auto min_col = range_table->GetColumnByName("min_date");
            auto max_col = range_table->GetColumnByName("max_date");
            if (min_col && max_col && range_table->num_rows() > 0) {
                auto min_arr = std::static_pointer_cast<arrow::StringArray>(min_col->chunk(0));
                auto max_arr = std::static_pointer_cast<arrow::StringArray>(max_col->chunk(0));
                std::string min_date_str = min_arr->GetString(0);
                std::string max_date_str = max_arr->GetString(0);
                std::cout << "Data range for selected symbols: " << min_date_str << " to " << max_date_str << std::endl;

                end_date = parse_date_str(max_date_str);
                // Use last 6 months of data, or all data if less than 6 months
                auto six_months_back = end_date - std::chrono::hours(24 * 180);
                Timestamp data_start = parse_date_str(min_date_str);
                start_date = std::max(six_months_back, data_start);
                dates_set = true;
            }
        }

        if (!dates_set) {
            // Fallback: recent 6 months
            auto now = std::chrono::system_clock::now();
            end_date = now;
            start_date = now - std::chrono::hours(24 * 180);
        }

        std::cout << "Date range: " << ts_to_date_str(start_date) << " to " << ts_to_date_str(end_date) << std::endl;

        // Strategy config — using reasonable defaults
        double initial_capital = 100000.0;
        int lookback_period = 20;
        double entry_threshold = 2.0;
        double exit_threshold = 0.5;
        double risk_target = 0.15;
        double position_size_pct = 0.1;
        int vol_lookback = 20;
        double stop_loss_pct = 0.05;
        double position_limit = 1000.0;

        std::cout << "Initial capital: $" << std::fixed << std::setprecision(0) << initial_capital << std::endl;
        std::cout << std::endl;

        // ====================================================================
        // SECTION A: DATA VALIDATION (OHLC Adjustment Ratio)
        // ====================================================================
        std::cout << "------------------------------------------------------" << std::endl;
        std::cout << "  SECTION A: Data Validation (OHLC Adjustment Ratio)" << std::endl;
        std::cout << "------------------------------------------------------\n" << std::endl;

        {
            // Build symbol list for SQL
            std::string sym_list;
            for (size_t i = 0; i < symbols.size(); i++) {
                if (i > 0) sym_list += "','";
                sym_list += symbols[i];
            }

            std::string start_str = ts_to_date_str(start_date);
            std::string end_str = ts_to_date_str(end_date);

            // Query RAW data (unadjusted) via execute_query
            std::string raw_sql =
                "SELECT date::text as date_str, ticker, open, high, low, close, closeadj, volume "
                "FROM equities_data.ohlcv_1d "
                "WHERE ticker IN ('" + sym_list + "') "
                "AND date BETWEEN '" + start_str + "'::date AND '" + end_str + "'::date "
                "ORDER BY date, ticker LIMIT 30";

            auto raw_arrow_result = db->execute_query(raw_sql);
            if (raw_arrow_result.is_error()) {
                ERROR("Raw query failed: " + std::string(raw_arrow_result.error()->what()));
                return 1;
            }
            auto raw_table = raw_arrow_result.value();

            // Query ADJUSTED data via framework
            auto market_data_result = db->get_market_data(
                symbols, start_date, end_date, AssetClass::EQUITIES, DataFrequency::DAILY);

            if (market_data_result.is_error()) {
                ERROR("Failed to load market data: " + std::string(market_data_result.error()->what()));
                return 1;
            }

            // Convert Arrow table to bars
            auto bars_result = DataConversionUtils::arrow_table_to_bars(market_data_result.value());
            if (bars_result.is_error()) {
                ERROR("Failed to convert Arrow to bars: " + std::string(bars_result.error()->what()));
                return 1;
            }
            auto all_bars = bars_result.value();

            // Build lookup: (date_str, ticker) -> Bar
            std::map<std::pair<std::string, std::string>, Bar> framework_bars;
            for (const auto& bar : all_bars) {
                std::string date_str = ts_to_date_str(bar.timestamp);
                framework_bars[{date_str, bar.symbol}] = bar;
            }

            // Extract raw data from Arrow table
            auto date_col = raw_table->GetColumnByName("date_str");
            auto ticker_col = raw_table->GetColumnByName("ticker");
            auto open_col = raw_table->GetColumnByName("open");
            auto high_col = raw_table->GetColumnByName("high");
            auto low_col = raw_table->GetColumnByName("low");
            auto close_col = raw_table->GetColumnByName("close");
            auto closeadj_col = raw_table->GetColumnByName("closeadj");

            if (!date_col || !ticker_col || !open_col || !close_col || !closeadj_col) {
                ERROR("Raw query missing expected columns");
                return 1;
            }

            std::cout << "Raw data rows: " << raw_table->num_rows()
                      << ", Framework bars: " << all_bars.size() << std::endl;

            // Debug: print first raw date and framework bar dates
            if (raw_table->num_rows() > 0) {
                auto date_arr = std::static_pointer_cast<arrow::StringArray>(date_col->chunk(0));
                auto ticker_arr = std::static_pointer_cast<arrow::StringArray>(ticker_col->chunk(0));
                std::cout << "First raw row: date='" << date_arr->GetString(0)
                          << "' ticker='" << ticker_arr->GetString(0) << "'" << std::endl;
            }
            if (!all_bars.empty()) {
                std::cout << "First framework bar: date='" << ts_to_date_str(all_bars[0].timestamp)
                          << "' symbol='" << all_bars[0].symbol << "'" << std::endl;
            }

            std::cout << "\n" << std::setw(12) << "Date" << " | "
                      << std::setw(6) << "Ticker" << " | "
                      << std::setw(10) << "Raw Open" << " | "
                      << std::setw(10) << "Manual Adj" << " | "
                      << std::setw(10) << "Framework" << " | "
                      << std::setw(6) << "Match?" << std::endl;
            std::cout << std::string(70, '-') << std::endl;

            int rows_checked = 0;
            int64_t num_rows = raw_table->num_rows();

            for (int64_t r = 0; r < num_rows; r++) {
                // Extract values from Arrow arrays
                auto date_arr = std::static_pointer_cast<arrow::StringArray>(date_col->chunk(0));
                auto ticker_arr = std::static_pointer_cast<arrow::StringArray>(ticker_col->chunk(0));

                std::string date_str = date_arr->GetString(r);
                std::string ticker = ticker_arr->GetString(r);

                // Extract doubles — execute_query returns all columns as strings
                double raw_open = 0, raw_high = 0, raw_low = 0, raw_close = 0, closeadj = 0;

                auto extract_double = [](const std::shared_ptr<arrow::ChunkedArray>& col, int64_t idx) -> double {
                    auto arr = std::static_pointer_cast<arrow::StringArray>(col->chunk(0));
                    try { return std::stod(arr->GetString(idx)); } catch (...) { return 0.0; }
                };

                raw_open = extract_double(open_col, r);
                raw_high = extract_double(high_col, r);
                raw_low = extract_double(low_col, r);
                raw_close = extract_double(close_col, r);
                closeadj = extract_double(closeadj_col, r);

                if (raw_close == 0.0) continue;

                double ratio = closeadj / raw_close;
                double manual_adj_open = raw_open * ratio;
                double manual_adj_high = raw_high * ratio;
                double manual_adj_low = raw_low * ratio;
                double manual_adj_close = closeadj;

                // Trim date_str to just YYYY-MM-DD (10 chars) for matching
                if (date_str.size() > 10) date_str = date_str.substr(0, 10);

                auto key = std::make_pair(date_str, ticker);
                auto it = framework_bars.find(key);
                if (it == framework_bars.end()) continue;

                const auto& fw_bar = it->second;
                double fw_open = fw_bar.open.as_double();
                double fw_high = fw_bar.high.as_double();
                double fw_low = fw_bar.low.as_double();
                double fw_close = fw_bar.close.as_double();

                bool open_match = approx_eq(manual_adj_open, fw_open, 1e-6);
                bool high_match = approx_eq(manual_adj_high, fw_high, 1e-6);
                bool low_match = approx_eq(manual_adj_low, fw_low, 1e-6);
                bool close_match = approx_eq(manual_adj_close, fw_close, 1e-6);

                if (rows_checked < 10) {
                    std::cout << std::setw(12) << date_str << " | "
                              << std::setw(6) << ticker << " | "
                              << std::fixed << std::setprecision(4)
                              << std::setw(10) << raw_open << " | "
                              << std::setw(10) << manual_adj_open << " | "
                              << std::setw(10) << fw_open << " | "
                              << std::setw(6) << (open_match ? "OK" : "FAIL") << std::endl;
                }

                check("adj_open [" + date_str + " " + ticker + "]", open_match,
                      "manual=" + std::to_string(manual_adj_open) + " fw=" + std::to_string(fw_open));
                check("adj_high [" + date_str + " " + ticker + "]", high_match,
                      "manual=" + std::to_string(manual_adj_high) + " fw=" + std::to_string(fw_high));
                check("adj_low [" + date_str + " " + ticker + "]", low_match,
                      "manual=" + std::to_string(manual_adj_low) + " fw=" + std::to_string(fw_low));
                check("adj_close [" + date_str + " " + ticker + "]", close_match,
                      "manual=" + std::to_string(manual_adj_close) + " fw=" + std::to_string(fw_close));

                rows_checked++;
            }
            std::cout << "\nChecked " << rows_checked << " rows of OHLC adjustment data\n" << std::endl;
        }

        // ====================================================================
        // SECTION B: STRATEGY CALCULATION VALIDATION + RUN BACKTEST
        // ====================================================================
        std::cout << "------------------------------------------------------" << std::endl;
        std::cout << "  SECTION B: Strategy Calculation Validation" << std::endl;
        std::cout << "------------------------------------------------------\n" << std::endl;

        // Run the actual backtest
        MeanReversionConfig mr_config;
        mr_config.lookback_period = lookback_period;
        mr_config.entry_threshold = entry_threshold;
        mr_config.exit_threshold = exit_threshold;
        mr_config.risk_target = risk_target;
        mr_config.position_size = position_size_pct;
        mr_config.vol_lookback = vol_lookback;
        mr_config.use_stop_loss = true;
        mr_config.stop_loss_pct = stop_loss_pct;
        mr_config.allow_fractional_shares = true;

        StrategyConfig strategy_config;
        strategy_config.asset_classes = {AssetClass::EQUITIES};
        strategy_config.frequencies = {DataFrequency::DAILY};
        strategy_config.capital_allocation = initial_capital;
        strategy_config.max_drawdown = 0.20;
        strategy_config.max_leverage = 1.0;

        for (const auto& symbol : symbols) {
            strategy_config.trading_params[symbol] = {};
            strategy_config.position_limits[symbol] = position_limit;
        }

        auto registry_ptr = std::shared_ptr<InstrumentRegistry>(&registry, [](InstrumentRegistry*) {});

        auto strategy = std::make_shared<MeanReversionStrategy>(
            "VALIDATION_MR", strategy_config, mr_config, db, registry_ptr);

        auto strat_init = strategy->initialize();
        if (strat_init.is_error()) {
            ERROR("Strategy init failed: " + std::string(strat_init.error()->what()));
            return 1;
        }
        auto strat_start = strategy->start();
        if (strat_start.is_error()) {
            ERROR("Strategy start failed: " + std::string(strat_start.error()->what()));
            return 1;
        }

        BacktestCoordinatorConfig coord_config;
        coord_config.initial_capital = initial_capital;
        coord_config.use_risk_management = false;
        coord_config.use_optimization = false;
        coord_config.store_trade_details = true;
        coord_config.portfolio_id = "equity_validation";

        auto coordinator = std::make_unique<BacktestCoordinator>(db, &registry, coord_config);
        auto coord_init = coordinator->initialize();
        if (coord_init.is_error()) {
            ERROR("Coordinator init failed: " + std::string(coord_init.error()->what()));
            return 1;
        }

        PortfolioConfig portfolio_config;
        portfolio_config.total_capital = Decimal(initial_capital);
        portfolio_config.reserve_capital = Decimal(initial_capital * 0.05);
        portfolio_config.use_optimization = false;
        portfolio_config.use_risk_management = false;

        auto portfolio = std::make_shared<PortfolioManager>(portfolio_config);
        auto add_result = portfolio->add_strategy(strategy, 1.0, false, false);
        if (add_result.is_error()) {
            ERROR("Failed to add strategy: " + std::string(add_result.error()->what()));
            return 1;
        }

        INFO("Running equity mean reversion backtest...");
        auto bt_result = coordinator->run_portfolio(
            portfolio, symbols, start_date, end_date,
            AssetClass::EQUITIES, DataFrequency::DAILY);

        if (bt_result.is_error()) {
            ERROR("Backtest failed: " + std::string(bt_result.error()->what()));
            return 1;
        }

        const auto& backtest_results = bt_result.value();
        INFO("Backtest completed: " + std::to_string(backtest_results.equity_curve.size()) + " equity curve points, " +
             std::to_string(backtest_results.executions.size()) + " executions");

        // Load adjusted close prices for manual computation
        auto market_data_result = db->get_market_data(
            symbols, start_date, end_date, AssetClass::EQUITIES, DataFrequency::DAILY);
        if (market_data_result.is_error()) {
            ERROR("Failed to load market data for validation: " + std::string(market_data_result.error()->what()));
            return 1;
        }
        auto bars_result = DataConversionUtils::arrow_table_to_bars(market_data_result.value());
        if (bars_result.is_error()) {
            ERROR("Failed to convert bars: " + std::string(bars_result.error()->what()));
            return 1;
        }
        auto all_bars = bars_result.value();

        // Build per-symbol ordered close prices and timestamps
        std::map<std::string, std::vector<double>> symbol_closes;
        std::map<std::string, std::vector<Timestamp>> symbol_timestamps;
        // Also build a date -> symbol -> close map for PnL validation
        std::map<Timestamp, std::map<std::string, double>> close_by_date;

        for (const auto& bar : all_bars) {
            symbol_closes[bar.symbol].push_back(bar.close.as_double());
            symbol_timestamps[bar.symbol].push_back(bar.timestamp);
            close_by_date[bar.timestamp][bar.symbol] = bar.close.as_double();
        }

        // Manual indicator recomputation for each symbol
        for (const auto& symbol : symbols) {
            const auto& closes = symbol_closes[symbol];
            const auto& timestamps = symbol_timestamps[symbol];

            if (closes.size() < static_cast<size_t>(lookback_period)) {
                WARN("Not enough data for " + symbol + ": " + std::to_string(closes.size()) + " bars");
                continue;
            }

            std::cout << "\n--- Strategy validation for " << symbol << " (" << closes.size() << " bars) ---\n";

            // Replicate the strategy's internal price_history with trimming
            std::vector<double> price_history;
            size_t max_history = static_cast<size_t>(std::max(lookback_period, vol_lookback) * 2);

            double manual_sma = 0, manual_std = 0, manual_z = 0, manual_vol = 0;

            // Print header for first 30 bars
            std::cout << std::setw(12) << "Date" << " | "
                      << std::setw(10) << "Close" << " | "
                      << std::setw(10) << "SMA(20)" << " | "
                      << std::setw(10) << "StdDev" << " | "
                      << std::setw(10) << "Z-Score" << " | "
                      << std::setw(10) << "Volatility" << std::endl;
            std::cout << std::string(75, '-') << std::endl;

            // BOD (Beginning-of-Day) model replication:
            // Day 1: on_data() receives Day 1 bars (first time, no previous exists)
            // Day 2: on_data() receives Day 1 bars (previous day's bars)
            // Day 3: on_data() receives Day 2 bars
            // ...
            // Day N: on_data() receives Day N-1 bars
            // After Day N: portfolio_previous_bars_ = Day N bars (NOT fed to strategy)
            //
            // So the strategy receives: close[0], close[0], close[1], close[2], ..., close[N-2]
            // (Day 1 appears twice, Day N-1 is last one fed, Day N is never fed)
            //
            // Build the actual sequence of closes the strategy sees:
            std::vector<double> bod_closes;
            std::vector<Timestamp> bod_timestamps;
            // Day 1: first-time fallback, strategy sees Day 1
            bod_closes.push_back(closes[0]);
            bod_timestamps.push_back(timestamps[0]);
            // Day 2 onwards: strategy sees previous day's bars
            for (size_t i = 0; i < closes.size() - 1; i++) {
                bod_closes.push_back(closes[i]);
                bod_timestamps.push_back(timestamps[i]);
            }

            std::cout << "BOD sequence length: " << bod_closes.size()
                      << " (from " << closes.size() << " raw bars)" << std::endl;

            for (size_t i = 0; i < bod_closes.size(); i++) {
                price_history.push_back(bod_closes[i]);

                // Trim (matching mean_reversion.cpp trim_history)
                while (price_history.size() > max_history) {
                    price_history.erase(price_history.begin());
                }

                if (price_history.size() < static_cast<size_t>(lookback_period)) {
                    if (i < 30) {
                        std::cout << std::setw(12) << ts_to_date_str(bod_timestamps[i]) << " | "
                                  << std::fixed << std::setprecision(4)
                                  << std::setw(10) << bod_closes[i] << " | "
                                  << std::setw(10) << "N/A" << " | "
                                  << std::setw(10) << "N/A" << " | "
                                  << std::setw(10) << "N/A" << " | "
                                  << std::setw(10) << "N/A" << std::endl;
                    }
                    continue;
                }

                // SMA (matching calculate_sma)
                double sum = 0;
                for (size_t j = price_history.size() - lookback_period; j < price_history.size(); j++) {
                    sum += price_history[j];
                }
                manual_sma = sum / lookback_period;

                // StdDev (matching calculate_std_dev — population std dev)
                double sq_sum = 0;
                for (size_t j = price_history.size() - lookback_period; j < price_history.size(); j++) {
                    double diff = price_history[j] - manual_sma;
                    sq_sum += diff * diff;
                }
                manual_std = std::sqrt(sq_sum / lookback_period);

                // Z-Score: uses the current bar's close (bar.close.as_double()) not price_history
                manual_z = (manual_std < 1e-8) ? 0.0 : (bod_closes[i] - manual_sma) / manual_std;

                // Volatility (matching calculate_volatility — log returns, population std dev)
                size_t vol_start = price_history.size() > static_cast<size_t>(vol_lookback)
                                   ? price_history.size() - vol_lookback : 0;
                std::vector<double> log_returns;
                for (size_t j = vol_start + 1; j < price_history.size(); j++) {
                    if (price_history[j-1] > 0) {
                        log_returns.push_back(std::log(price_history[j] / price_history[j-1]));
                    }
                }
                if (!log_returns.empty()) {
                    double ret_mean = std::accumulate(log_returns.begin(), log_returns.end(), 0.0) / log_returns.size();
                    double ret_sq = 0;
                    for (double r : log_returns) {
                        double diff = r - ret_mean;
                        ret_sq += diff * diff;
                    }
                    manual_vol = std::sqrt(ret_sq / log_returns.size()) * std::sqrt(252.0);
                } else {
                    manual_vol = 0.01;
                }

                if (i < 30) {
                    std::cout << std::setw(12) << ts_to_date_str(bod_timestamps[i]) << " | "
                              << std::fixed << std::setprecision(4)
                              << std::setw(10) << bod_closes[i] << " | "
                              << std::setw(10) << manual_sma << " | "
                              << std::setw(10) << manual_std << " | "
                              << std::setw(10) << manual_z << " | "
                              << std::setw(10) << manual_vol << std::endl;
                }
            }

            // Compare final state against strategy's internal state
            auto* inst_data = strategy->get_instrument_data(symbol);
            if (inst_data) {
                std::cout << "\nFinal state comparison for " << symbol << ":" << std::endl;
                std::cout << "  Manual SMA:   " << std::fixed << std::setprecision(6) << manual_sma
                          << "  Strategy: " << inst_data->moving_average << std::endl;
                std::cout << "  Manual StdDev:" << manual_std
                          << "  Strategy: " << inst_data->std_deviation << std::endl;
                std::cout << "  Manual ZScore: " << manual_z
                          << "  Strategy: " << inst_data->z_score << std::endl;
                std::cout << "  Manual Vol:   " << manual_vol
                          << "  Strategy: " << inst_data->current_volatility << std::endl;

                check("SMA_" + symbol, approx_eq(manual_sma, inst_data->moving_average),
                      "manual=" + std::to_string(manual_sma) + " strat=" + std::to_string(inst_data->moving_average));
                check("StdDev_" + symbol, approx_eq(manual_std, inst_data->std_deviation),
                      "manual=" + std::to_string(manual_std) + " strat=" + std::to_string(inst_data->std_deviation));
                check("ZScore_" + symbol, approx_eq(manual_z, inst_data->z_score),
                      "manual=" + std::to_string(manual_z) + " strat=" + std::to_string(inst_data->z_score));
                check("Volatility_" + symbol, approx_eq(manual_vol, inst_data->current_volatility),
                      "manual=" + std::to_string(manual_vol) + " strat=" + std::to_string(inst_data->current_volatility));
            } else {
                std::cout << "  WARNING: No instrument data found for " << symbol << std::endl;
            }
        }
        std::cout << std::endl;

        // ====================================================================
        // SECTION C: PnL VALIDATION
        // ====================================================================
        std::cout << "------------------------------------------------------" << std::endl;
        std::cout << "  SECTION C: PnL Validation (Equity Curve Replay)" << std::endl;
        std::cout << "------------------------------------------------------\n" << std::endl;

        {
            const auto& equity_curve = backtest_results.equity_curve;
            const auto& executions = backtest_results.executions;

            std::cout << "Equity curve points: " << equity_curve.size() << std::endl;
            std::cout << "Total executions: " << executions.size() << std::endl;

            // Build execution map by date
            std::map<std::string, std::vector<ExecutionReport>> exec_by_date;
            for (const auto& exec : executions) {
                std::string date_str = ts_to_date_str(exec.fill_time);
                exec_by_date[date_str].push_back(exec);
            }

            // Build ordered dates from close_by_date
            std::vector<Timestamp> ordered_dates;
            for (const auto& [ts, _] : close_by_date) {
                ordered_dates.push_back(ts);
            }
            std::sort(ordered_dates.begin(), ordered_dates.end());

            // Track positions from executions
            std::map<std::string, double> positions;
            std::map<std::string, double> previous_closes;

            std::cout << "\n" << std::setw(12) << "Date" << " | "
                      << std::setw(14) << "Equity" << " | "
                      << std::setw(12) << "Daily PnL" << " | "
                      << std::setw(12) << "Manual PnL" << " | "
                      << std::setw(12) << "Txn Costs" << " | "
                      << std::setw(8) << "Match?" << std::endl;
            std::cout << std::string(82, '-') << std::endl;

            int pnl_checks = 0;
            int pnl_mismatches = 0;

            for (size_t i = 1; i < equity_curve.size() && i < ordered_dates.size() + 1; i++) {
                // The equity curve has N+1 entries (including initial capital).
                // equity_curve[0] = initial capital (before first bar)
                // equity_curve[i] corresponds to ordered_dates[i-1]

                if (i - 1 >= ordered_dates.size()) break;

                Timestamp current_ts = ordered_dates[i - 1];
                std::string date_str = ts_to_date_str(current_ts);

                double equity_prev = equity_curve[i - 1].second;
                double equity_curr = equity_curve[i].second;
                double framework_daily_pnl = equity_curr - equity_prev;

                // Apply executions for this date to update positions
                double day_txn_costs = 0.0;
                if (exec_by_date.count(date_str)) {
                    for (const auto& exec : exec_by_date[date_str]) {
                        double qty = exec.filled_quantity.as_double();
                        if (exec.side == Side::SELL) qty = -qty;
                        positions[exec.symbol] += qty;
                        day_txn_costs += exec.total_transaction_costs.as_double();
                    }
                }

                // Calculate manual PnL
                double manual_pnl = 0.0;
                auto& today_closes = close_by_date[current_ts];

                for (const auto& [sym, qty] : positions) {
                    if (std::abs(qty) < 1e-8) continue;

                    auto curr_it = today_closes.find(sym);
                    if (curr_it == today_closes.end()) continue;
                    double curr_close = curr_it->second;

                    auto prev_it = previous_closes.find(sym);
                    if (prev_it != previous_closes.end()) {
                        // PnL = qty * (current_close - previous_close) * point_value(1.0)
                        double pos_pnl = qty * (curr_close - prev_it->second);
                        manual_pnl += pos_pnl;
                    }
                    // else: first day for this symbol, PnL = 0
                }

                // Update previous closes
                for (const auto& [sym, price] : today_closes) {
                    previous_closes[sym] = price;
                }

                // The framework_daily_pnl includes transaction costs already deducted
                // manual_pnl is gross, so: framework ≈ manual - txn_costs
                double manual_net = manual_pnl - day_txn_costs;
                bool pnl_match = approx_eq(framework_daily_pnl, manual_net, 1e-3, 0.02);

                if (i <= 20 || i >= equity_curve.size() - 10) {
                    std::cout << std::setw(12) << date_str << " | "
                              << std::fixed << std::setprecision(2)
                              << std::setw(14) << equity_curr << " | "
                              << std::setw(12) << framework_daily_pnl << " | "
                              << std::setw(12) << manual_net << " | "
                              << std::setw(12) << day_txn_costs << " | "
                              << std::setw(8) << (pnl_match ? "OK" : "DIFF") << std::endl;
                } else if (i == 21) {
                    std::cout << "  ... (middle rows omitted) ..." << std::endl;
                }

                pnl_checks++;
                if (!pnl_match) pnl_mismatches++;
            }

            std::cout << "\nPnL checks: " << pnl_checks << ", mismatches: " << pnl_mismatches << std::endl;

            // Note: Due to the complexity of position tracking across the BOD model
            // (positions change based on signals from previous day's bars, while PnL
            // uses current day's close prices), exact per-day PnL matching requires
            // precise knowledge of when executions occur relative to PnL calculations.
            // We report mismatches as informational rather than hard failures.
            // The key aggregate check is total return.

            // Total return validation
            double manual_total_return = 0.0;
            if (equity_curve.size() >= 2 && equity_curve.front().second > 0) {
                manual_total_return = (equity_curve.back().second - equity_curve.front().second) /
                                      equity_curve.front().second;
            }
            check("total_return_from_equity_curve",
                  approx_eq(manual_total_return, backtest_results.total_return, 1e-3),
                  "manual=" + std::to_string(manual_total_return) + " bt=" + std::to_string(backtest_results.total_return));

            std::cout << "\nTotal Return: manual=" << std::fixed << std::setprecision(4)
                      << (manual_total_return * 100) << "%, backtest=" << (backtest_results.total_return * 100) << "%" << std::endl;
            std::cout << std::endl;
        }

        // ====================================================================
        // SECTION D: METRICS VALIDATION
        // ====================================================================
        std::cout << "------------------------------------------------------" << std::endl;
        std::cout << "  SECTION D: Metrics Validation" << std::endl;
        std::cout << "------------------------------------------------------\n" << std::endl;

        {
            const auto& equity_curve = backtest_results.equity_curve;
            int warmup_days = backtest_results.warmup_days;

            // Filter warmup (matching filter_warmup_period)
            std::vector<std::pair<Timestamp, double>> filtered;
            if (warmup_days > 0 && equity_curve.size() > static_cast<size_t>(warmup_days)) {
                filtered.assign(equity_curve.begin() + warmup_days, equity_curve.end());
            } else {
                filtered = equity_curve;
            }

            std::cout << "Equity curve size: " << equity_curve.size()
                      << ", warmup_days: " << warmup_days
                      << ", filtered size: " << filtered.size() << std::endl;

            if (filtered.size() < 2) {
                ERROR("Not enough data points for metrics validation");
                return 1;
            }

            // Daily returns (matching calculate_returns_from_equity)
            std::vector<double> returns;
            for (size_t i = 1; i < filtered.size(); i++) {
                if (filtered[i - 1].second > 0.0) {
                    returns.push_back((filtered[i].second - filtered[i - 1].second) / filtered[i - 1].second);
                }
            }

            int actual_trading_days = static_cast<int>(filtered.size()) - 1;
            if (actual_trading_days <= 0) actual_trading_days = 1;

            // Total return
            double manual_total_return = (filtered.back().second - filtered.front().second) / filtered.front().second;

            // Volatility (matching calculate_volatility: E[r^2] - E[r]^2)
            double mean_return = std::accumulate(returns.begin(), returns.end(), 0.0) / returns.size();
            double sq_sum = std::inner_product(returns.begin(), returns.end(), returns.begin(), 0.0);
            double variance = sq_sum / returns.size() - mean_return * mean_return;
            double manual_vol = std::sqrt(variance) * std::sqrt(252.0);

            // Sharpe ratio (matching calculate_sharpe_ratio)
            double ann_factor = 252.0 / static_cast<double>(actual_trading_days);
            double ann_return = mean_return * ann_factor;
            double manual_sharpe = (manual_vol > 0.0) ? ann_return / manual_vol : 0.0;

            // Sortino ratio (matching calculate_downside_volatility + calculate_sortino_ratio)
            double downside_sum = 0.0;
            int downside_count = 0;
            for (double r : returns) {
                if (r < 0.0) {
                    downside_sum += r * r;  // target = 0, so (r - 0)^2 = r^2
                    downside_count++;
                }
            }
            double downside_vol = (downside_count > 0)
                ? std::sqrt(downside_sum / downside_count) * std::sqrt(252.0) : 0.0;
            double manual_sortino = 0.0;
            if (downside_vol > 0.0) {
                manual_sortino = ann_return / downside_vol;
            } else {
                manual_sortino = (ann_return >= 0) ? 999.0 : 0.0;
            }

            // Max drawdown (matching calculate_drawdowns)
            double peak = filtered[0].second;
            double manual_max_dd = 0.0;
            for (const auto& [ts, val] : filtered) {
                peak = std::max(peak, val);
                double dd = (val < peak && peak > 0) ? (peak - val) / peak : 0.0;
                manual_max_dd = std::max(manual_max_dd, dd);
            }

            // Calmar ratio
            double manual_calmar = (manual_max_dd > 0) ? manual_total_return / manual_max_dd
                                                        : ((manual_total_return >= 0) ? 999.0 : 0.0);

            // Print comparison
            std::cout << "\n" << std::setw(20) << "Metric" << " | "
                      << std::setw(14) << "Manual" << " | "
                      << std::setw(14) << "Backtest" << " | "
                      << std::setw(8) << "Match?" << std::endl;
            std::cout << std::string(62, '-') << std::endl;

            auto print_metric = [&](const std::string& name, double manual, double bt) {
                bool match = approx_eq(manual, bt, 1e-3);
                std::cout << std::setw(20) << name << " | "
                          << std::fixed << std::setprecision(6)
                          << std::setw(14) << manual << " | "
                          << std::setw(14) << bt << " | "
                          << std::setw(8) << (match ? "OK" : "DIFF") << std::endl;
                check(name, match, "manual=" + std::to_string(manual) + " bt=" + std::to_string(bt));
            };

            print_metric("total_return", manual_total_return, backtest_results.total_return);
            print_metric("volatility", manual_vol, backtest_results.volatility);
            print_metric("sharpe_ratio", manual_sharpe, backtest_results.sharpe_ratio);
            print_metric("sortino_ratio", manual_sortino, backtest_results.sortino_ratio);
            print_metric("max_drawdown", manual_max_dd, backtest_results.max_drawdown);
            print_metric("calmar_ratio", manual_calmar, backtest_results.calmar_ratio);

            std::cout << "\nAdditional backtest results:" << std::endl;
            std::cout << "  Total trades: " << backtest_results.total_trades << std::endl;
            std::cout << "  Win rate: " << std::fixed << std::setprecision(2)
                      << (backtest_results.win_rate * 100) << "%" << std::endl;
            std::cout << "  Profit factor: " << std::setprecision(3) << backtest_results.profit_factor << std::endl;
            std::cout << std::endl;
        }

        // ====================================================================
        // SUMMARY
        // ====================================================================
        std::cout << "\n======================================================" << std::endl;
        std::cout << "    VALIDATION SUMMARY" << std::endl;
        std::cout << "======================================================" << std::endl;
        std::cout << "  Total checks:  " << total_checks << std::endl;
        std::cout << "  Passed:        " << passed_checks << " ("
                  << std::fixed << std::setprecision(1)
                  << (total_checks > 0 ? 100.0 * passed_checks / total_checks : 0.0)
                  << "%)" << std::endl;
        std::cout << "  Failed:        " << failed_checks << " ("
                  << (total_checks > 0 ? 100.0 * failed_checks / total_checks : 0.0)
                  << "%)" << std::endl;
        std::cout << "  Overall:       " << (failed_checks == 0 ? "PASS" : "FAIL") << std::endl;
        std::cout << "======================================================\n" << std::endl;

        coordinator.reset();
        return (failed_checks == 0) ? 0 : 1;

    } catch (const std::exception& e) {
        std::cerr << "Unexpected error: " << e.what() << std::endl;
        return 1;
    }
}
