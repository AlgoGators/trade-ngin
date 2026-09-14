// apps/backtest/bt_spread.cpp
//
// Backtest a two-ticker mean-reverting spread.
//
//   bt_spread [SYMBOL_A SYMBOL_B] [options]
//
// With no arguments it runs a default pair so the binary is useful out of the
// box. Every strategy parameter carries a seed value; the options below only
// exist to override them.
//
//   --asset futures|equities   Asset class to load (default futures)
//   --years N          Lookback in years (default 2)
//   --type log|diff    Spread construction (default log)
//   --beta-period N    Rolling OLS window for the hedge ratio (default 60)
//   --z-period N       Rolling window for the spread z-score (default 20)
//   --entry-z X        |z| at which a position opens (default 2.0)
//   --exit-z X         |z| at which a position is flattened (default 0.5)
//   --stop-z X         |z| at which a position is stopped out (default 4.0)
//   --leg-pct X        Notional per leg as a fraction of capital (default 0.10)
//   --min-hold N       Bars a position is held before exiting (default 1)
//   --capital X        Starting capital (default 100000)
//   --save             Save results to the database (off by default)

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>
#include "trade_ngin/backtest/backtest_engine.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/data/credential_store.hpp"
#include "trade_ngin/data/database_pooling.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/strategy/spread.hpp"

using namespace trade_ngin;
using namespace trade_ngin::backtest;

namespace {

struct Options {
    // Defaults are a classic precious-metals pair from the futures dataset.
    std::string symbol_a{"GC.v.0"};
    std::string symbol_b{"SI.v.0"};
    AssetClass asset_class{AssetClass::FUTURES};
    int years{2};
    double capital{100000.0};
    bool save_to_db{false};
    SpreadConfig spread{};
};

void print_usage() {
    std::cout
        << "Usage: bt_spread [SYMBOL_A SYMBOL_B] [options]\n\n"
        << "  --asset CLASS      futures or equities (default futures)\n"
        << "  --years N          Lookback in years (default 2)\n"
        << "  --type log|diff    Spread construction (default log)\n"
        << "  --beta-period N    Rolling OLS window for the hedge ratio (default 60)\n"
        << "  --z-period N       Rolling window for the spread z-score (default 20)\n"
        << "  --entry-z X        |z| at which a position opens (default 2.0)\n"
        << "  --exit-z X         |z| at which a position is flattened (default 0.5)\n"
        << "  --stop-z X         |z| at which a position is stopped out (default 4.0)\n"
        << "  --leg-pct X        Notional per leg as a fraction of capital (default 0.10)\n"
        << "  --min-hold N       Bars a position is held before exiting (default 1)\n"
        << "  --capital X        Starting capital (default 100000)\n"
        << "  --save             Save results to the database\n"
        << "  -h, --help         Show this message\n";
}

// Returns false if the arguments are unusable; usage has already been printed.
bool parse_args(int argc, char** argv, Options& opts) {
    std::vector<std::string> positional;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            print_usage();
            return false;
        }

        if (arg == "--save") {
            opts.save_to_db = true;
            continue;
        }

        if (arg.rfind("--", 0) != 0) {
            positional.push_back(arg);
            continue;
        }

        if (i + 1 >= argc) {
            std::cerr << "Missing value for " << arg << "\n\n";
            print_usage();
            return false;
        }
        const std::string value = argv[++i];

        try {
            if (arg == "--asset") {
                if (value == "futures") {
                    opts.asset_class = AssetClass::FUTURES;
                } else if (value == "equities") {
                    opts.asset_class = AssetClass::EQUITIES;
                } else {
                    std::cerr << "--asset must be 'futures' or 'equities', got '" << value
                              << "'\n";
                    return false;
                }
            } else if (arg == "--years") {
                opts.years = std::stoi(value);
            } else if (arg == "--type") {
                if (value == "log") {
                    opts.spread.spread_type = SpreadType::LOG_RATIO;
                } else if (value == "diff") {
                    opts.spread.spread_type = SpreadType::PRICE_DIFF;
                } else {
                    std::cerr << "--type must be 'log' or 'diff', got '" << value << "'\n";
                    return false;
                }
            } else if (arg == "--beta-period") {
                opts.spread.beta_period = std::stoi(value);
            } else if (arg == "--z-period") {
                opts.spread.zscore_period = std::stoi(value);
            } else if (arg == "--entry-z") {
                opts.spread.entry_z = std::stod(value);
            } else if (arg == "--exit-z") {
                opts.spread.exit_z = std::stod(value);
            } else if (arg == "--stop-z") {
                opts.spread.stop_z = std::stod(value);
            } else if (arg == "--leg-pct") {
                opts.spread.capital_per_leg_pct = std::stod(value);
            } else if (arg == "--min-hold") {
                opts.spread.min_holding_period = std::stoi(value);
            } else if (arg == "--capital") {
                opts.capital = std::stod(value);
            } else {
                std::cerr << "Unknown option: " << arg << "\n\n";
                print_usage();
                return false;
            }
        } catch (const std::exception&) {
            std::cerr << "Invalid value for " << arg << ": '" << value << "'\n";
            return false;
        }
    }

    if (positional.size() == 1 || positional.size() > 2) {
        std::cerr << "Expected two ticker symbols, got " << positional.size() << "\n\n";
        print_usage();
        return false;
    }
    if (positional.size() == 2) {
        opts.symbol_a = positional[0];
        opts.symbol_b = positional[1];
    }

    opts.spread.symbol_a = opts.symbol_a;
    opts.spread.symbol_b = opts.symbol_b;

    if (opts.years < 1) {
        std::cerr << "--years must be at least 1\n";
        return false;
    }
    if (opts.capital <= 0.0) {
        std::cerr << "--capital must be positive\n";
        return false;
    }

    return true;
}

// Reads one database setting, reporting which one failed rather than just that
// something did.
bool read_setting(const std::shared_ptr<CredentialStore>& credentials, const std::string& key,
                  std::string& out) {
    auto result = credentials->get<std::string>("database", key);
    if (result.is_error()) {
        std::cerr << "Failed to read database." << key << " from config.json: "
                  << result.error()->what() << std::endl;
        return false;
    }
    out = result.value();
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        Options opts;
        if (!parse_args(argc, argv, opts)) {
            return 1;
        }

        StateManager::reset_instance();
        Logger::reset_for_tests();

        auto& logger = Logger::instance();
        LoggerConfig logger_config;
        logger_config.min_level = LogLevel::INFO;
        logger_config.destination = LogDestination::FILE;
        logger_config.log_directory = "logs";
        logger_config.filename_prefix = "bt_spread";
        logger.initialize(logger_config);

        if (!logger.is_initialized()) {
            std::cerr << "ERROR: Logger initialization failed" << std::endl;
            return 1;
        }

        // --- Database ---------------------------------------------------------
        auto credentials = std::make_shared<CredentialStore>("./config.json");
        std::string username;
        std::string password;
        std::string host;
        std::string port;
        std::string db_name;
        if (!read_setting(credentials, "username", username) ||
            !read_setting(credentials, "password", password) ||
            !read_setting(credentials, "host", host) ||
            !read_setting(credentials, "port", port) ||
            !read_setting(credentials, "name", db_name)) {
            return 1;
        }

        const std::string conn_string =
            "postgresql://" + username + ":" + password + "@" + host + ":" + port + "/" + db_name;

        auto pool_result = DatabasePool::instance().initialize(conn_string, 5);
        if (pool_result.is_error()) {
            std::cerr << "Failed to initialize connection pool: " << pool_result.error()->what()
                      << std::endl;
            return 1;
        }

        auto db_guard = DatabasePool::instance().acquire_connection();
        auto db = db_guard.get();
        if (!db || !db->is_connected()) {
            std::cerr << "Failed to acquire a database connection" << std::endl;
            return 1;
        }

        auto& registry = InstrumentRegistry::instance();
        auto registry_init = registry.initialize(db);
        if (registry_init.is_error()) {
            std::cerr << "Failed to initialize instrument registry: "
                      << registry_init.error()->what() << std::endl;
            return 1;
        }
        auto load_result = registry.load_instruments();
        if (load_result.is_error()) {
            std::cerr << "Failed to load instruments: " << load_result.error()->what() << std::endl;
            return 1;
        }

        // --- Confirm both legs exist before spending time on a backtest -------
        auto symbols_result = db->get_symbols(opts.asset_class);
        if (symbols_result.is_error()) {
            std::cerr << "Failed to list symbols: " << symbols_result.error()->what()
                      << std::endl;
            return 1;
        }
        const auto& available = symbols_result.value();
        std::vector<std::string> missing;
        for (const auto& symbol : {opts.symbol_a, opts.symbol_b}) {
            if (std::find(available.begin(), available.end(), symbol) == available.end()) {
                missing.push_back(symbol);
            }
        }
        if (!missing.empty()) {
            std::cerr << "Not in the database: ";
            for (const auto& symbol : missing) std::cerr << symbol << " ";
            std::cerr << "\nAvailable equity symbols (" << available.size() << " total): ";
            for (size_t i = 0; i < std::min<size_t>(25, available.size()); ++i) {
                std::cerr << available[i] << " ";
            }
            if (available.size() > 25) std::cerr << "...";
            std::cerr << std::endl;
            return 1;
        }

        // --- Backtest configuration ------------------------------------------
        BacktestConfig config;

        const auto now = std::chrono::system_clock::now();
        auto now_time_t = std::chrono::system_clock::to_time_t(now);
        std::tm start_tm = *std::localtime(&now_time_t);
        start_tm.tm_year -= opts.years;
        config.strategy_config.start_date =
            std::chrono::system_clock::from_time_t(std::mktime(&start_tm));
        config.strategy_config.end_date = now;

        config.strategy_config.asset_class = opts.asset_class;
        config.strategy_config.data_freq = DataFrequency::DAILY;
        config.strategy_config.commission_rate = 0.001;
        config.strategy_config.slippage_model = 0.5;
        // Both legs must fill their beta and z-score windows before the first signal.
        config.strategy_config.warmup_days =
            opts.spread.beta_period + opts.spread.zscore_period;
        config.strategy_config.symbols = {opts.symbol_a, opts.symbol_b};

        config.portfolio_config.initial_capital = opts.capital;
        config.portfolio_config.use_risk_management = false;
        config.portfolio_config.use_optimization = false;

        StrategyConfig strategy_config;
        strategy_config.capital_allocation = opts.capital;
        strategy_config.asset_classes = {opts.asset_class};
        strategy_config.frequencies = {config.strategy_config.data_freq};
        strategy_config.max_drawdown = 0.3;
        strategy_config.max_leverage = 2.0;  // Two legs, each up to leg-pct of capital
        strategy_config.save_positions = false;
        strategy_config.save_signals = false;
        strategy_config.save_executions = false;
        for (const auto& symbol : config.strategy_config.symbols) {
            strategy_config.position_limits[symbol] = 1e9;  // Sizing is controlled by leg-pct
            strategy_config.costs[symbol] = config.strategy_config.commission_rate.as_double();
        }

        std::cout << "\n======= Spread Backtest =======" << std::endl;
        std::cout << "Asset class:     "
                  << (opts.asset_class == AssetClass::FUTURES ? "FUTURES" : "EQUITIES")
                  << std::endl;
        std::cout << "Pair:            " << opts.symbol_a << " / " << opts.symbol_b << std::endl;
        std::cout << "Spread:          "
                  << (opts.spread.spread_type == SpreadType::LOG_RATIO
                          ? "log(A) - beta*log(B)"
                          : "A - beta*B")
                  << std::endl;
        std::cout << "Hedge ratio:     rolling OLS, " << opts.spread.beta_period << " bars"
                  << std::endl;
        std::cout << "Z-score window:  " << opts.spread.zscore_period << " bars" << std::endl;
        std::cout << "Entry/exit/stop: " << opts.spread.entry_z << " / " << opts.spread.exit_z
                  << " / " << opts.spread.stop_z << std::endl;
        std::cout << "Notional/leg:    " << (opts.spread.capital_per_leg_pct * 100.0) << "%"
                  << std::endl;
        std::cout << "Min holding:     " << opts.spread.min_holding_period << " bars" << std::endl;
        std::cout << "Capital:         $" << std::fixed << std::setprecision(2) << opts.capital
                  << std::endl;
        std::cout << "Lookback:        " << opts.years << " years" << std::endl;
        std::cout << "==============================\n" << std::endl;

        auto engine = std::make_unique<BacktestEngine>(config, db);

        auto registry_ptr = std::shared_ptr<InstrumentRegistry>(&registry, [](InstrumentRegistry*) {});
        auto strategy = std::make_shared<SpreadStrategy>(
            "SPREAD_" + opts.symbol_a + "_" + opts.symbol_b, strategy_config, opts.spread, db,
            registry_ptr);

        auto init_result = strategy->initialize();
        if (init_result.is_error()) {
            std::cerr << "Failed to initialize strategy: " << init_result.error()->what()
                      << std::endl;
            return 1;
        }

        auto start_result = strategy->start();
        if (start_result.is_error()) {
            std::cerr << "Failed to start strategy: " << start_result.error()->what() << std::endl;
            return 1;
        }

        PortfolioConfig portfolio_config;
        portfolio_config.total_capital = opts.capital;
        portfolio_config.reserve_capital = opts.capital * 0.1;
        portfolio_config.max_strategy_allocation = 1.0;
        portfolio_config.min_strategy_allocation = 0.1;
        portfolio_config.use_optimization = false;
        portfolio_config.use_risk_management = false;

        auto portfolio = std::make_shared<PortfolioManager>(portfolio_config);
        auto add_result = portfolio->add_strategy(strategy, 1.0, false, false);
        if (add_result.is_error()) {
            std::cerr << "Failed to add strategy to portfolio: " << add_result.error()->what()
                      << std::endl;
            return 1;
        }

        auto result = engine->run_portfolio(portfolio);
        if (result.is_error()) {
            std::cerr << "Backtest failed: " << result.error()->what() << std::endl;
            return 1;
        }

        const auto& results = result.value();
        const auto& state = strategy->get_state_snapshot();

        std::cout << "\n======= Results =======" << std::endl;
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "Total Return:  " << (results.total_return * 100.0) << "%" << std::endl;
        std::cout << "Sharpe Ratio:  " << results.sharpe_ratio << std::endl;
        std::cout << "Sortino Ratio: " << results.sortino_ratio << std::endl;
        std::cout << "Max Drawdown:  " << (results.max_drawdown * 100.0) << "%" << std::endl;
        std::cout << "Calmar Ratio:  " << results.calmar_ratio << std::endl;
        std::cout << "Volatility:    " << (results.volatility * 100.0) << "%" << std::endl;
        std::cout << "Win Rate:      " << (results.win_rate * 100.0) << "%" << std::endl;
        std::cout << "Total Trades:  " << results.total_trades << std::endl;

        std::cout << "\n======= Spread Diagnostics =======" << std::endl;
        std::cout << "Aligned bars:  " << state.observations << std::endl;
        std::cout << "Final beta:    " << std::setprecision(4) << state.beta << std::endl;
        std::cout << "Final z-score: "
                  << (state.zscore_valid ? std::to_string(state.current_zscore) : "n/a")
                  << std::endl;
        std::cout << "Final position: "
                  << (state.direction > 0   ? "LONG SPREAD"
                      : state.direction < 0 ? "SHORT SPREAD"
                                            : "FLAT")
                  << std::endl;

        if (state.observations == 0) {
            std::cout << "\nWARNING: no bar ever had a close for both legs on the same "
                         "timestamp, so the strategy could not trade. Check that both "
                         "tickers have overlapping daily history."
                      << std::endl;
        }

        if (!results.symbol_pnl.empty()) {
            std::cout << "\n======= Leg PnL =======" << std::endl;
            std::cout << std::setprecision(2);
            for (const auto& [symbol, pnl] : results.symbol_pnl) {
                std::cout << symbol << ": $" << pnl << std::endl;
            }
        }
        std::cout << "==============================\n" << std::endl;

        if (opts.save_to_db) {
            auto save_result = engine->save_results_to_db(results);
            if (save_result.is_error()) {
                std::cerr << "Failed to save results: " << save_result.error()->what() << std::endl;
            } else {
                std::cout << "Results saved to database." << std::endl;
            }
        }

        engine.reset();
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Unexpected error: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "Unknown error occurred" << std::endl;
        return 1;
    }
}
