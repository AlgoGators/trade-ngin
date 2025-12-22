// src/backtest/backtest_engine.cpp
#include "trade_ngin/backtest/backtest_engine.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <numeric>
#include <set>
#include <sstream>
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/run_id_generator.hpp"
#include "trade_ngin/core/state_manager.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/data/conversion_utils.hpp"
#include "trade_ngin/data/database_interface.hpp"
#include "trade_ngin/data/database_pooling.hpp"
#include "trade_ngin/data/market_data_bus.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/storage/backtest_results_manager.hpp"
#include "trade_ngin/strategy/trend_following.hpp"

namespace {
std::string join(const std::vector<std::string>& elements, const std::string& delimiter) {
    std::ostringstream os;
    if (!elements.empty()) {
        os << elements[0];
        for (size_t i = 1; i < elements.size(); ++i) {
            os << delimiter << elements[i];
        }
    }
    return os.str();
}
}  // anonymous namespace

namespace trade_ngin {
namespace backtest {

BacktestEngine::BacktestEngine(BacktestConfig config, std::shared_ptr<PostgresDatabase> db)
    : config_(std::move(config)), db_(std::move(db)) {
    // Generate a unique component for the backtest engine
    std::string unique_id =
        "BACKTEST_ENGINE_" +
        std::to_string(std::chrono::system_clock::now().time_since_epoch().count());

    Logger::register_component("BacktestEngine");

    // Register component with state manager
    ComponentInfo info{
        ComponentType::BACKTEST_ENGINE,
        ComponentState::INITIALIZED,
        unique_id,
        "",
        std::chrono::system_clock::now(),
        {{"total_capital", static_cast<double>(config_.portfolio_config.initial_capital)}}};

    auto register_result = StateManager::instance().register_component(info);
    if (register_result.is_error()) {
        ERROR("Failed to register backtest engine with state manager: " +
              std::string(register_result.error()->what()) +
              ". Continuing without state management.");
    } else {
        // Store registered component ID for future use
        backtest_component_id_ = unique_id;
    }

    // Initialize risk manager if enabled
    if (config_.portfolio_config.use_risk_management) {
        try {
            risk_manager_ = std::make_unique<RiskManager>(config_.portfolio_config.risk_config);
            if (!risk_manager_) {
                throw std::runtime_error("Failed to create risk manager");
            }
        } catch (const std::exception& e) {
            ERROR("Error initializing risk manager: " + std::string(e.what()));
            throw;
        }
    }

    // Initialize optimizer if enabled
    if (config_.portfolio_config.use_optimization) {
        try {
            optimizer_ = std::make_unique<DynamicOptimizer>(config_.portfolio_config.opt_config);
            if (!optimizer_) {
                throw std::runtime_error("Failed to create optimizer");
            }
        } catch (const std::exception& e) {
            ERROR("Error initializing optimizer: " + std::string(e.what()));
            throw;
        }
    }

    // Initialize slippage model
    if (static_cast<double>(config_.strategy_config.slippage_model) > 0.0) {
        SpreadSlippageConfig slippage_config;
        slippage_config.min_spread_bps =
            static_cast<double>(config_.strategy_config.slippage_model);
        slippage_config.spread_multiplier = 1.2;
        slippage_config.market_impact_multiplier = 1.5;

        slippage_model_ = SlippageModelFactory::create_spread_model(slippage_config);
    }

    INFO("Backtest engine initialized successfully with " +
         std::to_string(config_.strategy_config.symbols.size()) + " symbols and " +
         config_.portfolio_config.initial_capital.to_string() + " initial capital");
}

BacktestEngine::~BacktestEngine() {
    // TEMPORARILY DISABLED: StateManager unregistration causing segfault
    // if (!backtest_component_id_.empty()) {
    //     try {
    //         auto unreg_result = StateManager::instance().unregister_component(backtest_component_id_);
    //         if (unreg_result.is_error()) {
    //             // Log but don't throw from destructor
    //             std::cerr << "Error unregistering from StateManager: " << unreg_result.error()->what() << std::endl;
    //         }
    //     } catch (const std::exception& e) {
    //         // Log but don't throw from destructor
    //         std::cerr << "Exception during StateManager unregistration: " << e.what() << std::endl;
    //     }
    // }
    
    // Clear any remaining references to prevent use-after-free
    backtest_component_id_.clear();
}

// Run single strategy with portfolio-level constraints
Result<BacktestResults> BacktestEngine::run(std::shared_ptr<StrategyInterface> strategy) {
    // Check if BACKTEST_ENGINE component exists in StateManager and register if not
    auto component_result = StateManager::instance().get_state(backtest_component_id_);
    if (component_result.is_error()) {
        // Component doesn't exist, register it
        ComponentInfo info{
            ComponentType::BACKTEST_ENGINE,
            ComponentState::INITIALIZED,
            backtest_component_id_,
            "",
            std::chrono::system_clock::now(),
            {{"total_capital", static_cast<double>(config_.portfolio_config.initial_capital)}}};

        auto register_result = StateManager::instance().register_component(info);
        if (register_result.is_error()) {
            ERROR("Failed to register backtest engine with state manager: " +
                  std::string(register_result.error()->what()));
        } else {
            INFO("Registered backtest engine with state manager");
        }
    }

    // Update state to running
    auto state_result =
        StateManager::instance().update_state(backtest_component_id_, ComponentState::RUNNING);

    if (state_result.is_error()) {
        ERROR("Failed to update backtest engine state: " +
              std::string(state_result.error()->what()));
    }

    try {
        // Load historical market data
        auto data_result = load_market_data();
        if (data_result.is_error()) {
            ERROR("Failed to load market data: " + std::string(data_result.error()->what()));

            (void)StateManager::instance().update_state(
                backtest_component_id_, ComponentState::ERR_STATE, data_result.error()->what());

            return make_error<BacktestResults>(data_result.error()->code(),
                                               data_result.error()->what(), "BacktestEngine");
        }

            // Initialize tracking variables
    std::vector<ExecutionReport> executions;
    std::map<std::string, Position> current_positions;
    std::vector<std::pair<Timestamp, double>> equity_curve;
    std::vector<RiskResult> risk_metrics;
    std::map<std::pair<Timestamp, std::string>, double> signals;  // Track signals with timestamps for database storage
    double current_equity = static_cast<double>(config_.portfolio_config.initial_capital);

        // Initialize equity curve with starting point
        equity_curve.emplace_back(config_.strategy_config.start_date, current_equity);

        // Initialize strategy
        INFO("Initializing strategy for backtest");
        auto init_result = strategy->initialize();
        if (init_result.is_error()) {
            ERROR("Strategy initialization failed: " + std::string(init_result.error()->what()));

            (void)StateManager::instance().update_state(
                backtest_component_id_, ComponentState::ERR_STATE, init_result.error()->what());

            return make_error<BacktestResults>(init_result.error()->code(),
                                               init_result.error()->what(), "BacktestEngine");
        }

        // Start strategy
        INFO("Starting strategy for backtest");
        auto start_result = strategy->start();
        if (start_result.is_error()) {
            ERROR("Strategy start failed: " + std::string(start_result.error()->what()));

            (void)StateManager::instance().update_state(
                backtest_component_id_, ComponentState::ERR_STATE, start_result.error()->what());

            return make_error<BacktestResults>(start_result.error()->code(),
                                               start_result.error()->what(), "BacktestEngine");
        }

        // Generate run_id at the start for daily position storage
        // This will be used for daily position saves and can be passed to save_results_to_db
        // Use strategy metadata ID or default to "TREND_FOLLOWING"
        std::string strategy_id = "TREND_FOLLOWING";  // Default
        try {
            const auto& metadata = strategy->get_metadata();
            if (!metadata.id.empty()) {
                strategy_id = metadata.id;
            }
        } catch (...) {
            // Use default if metadata access fails
        }
        std::string backtest_run_id = BacktestResultsManager::generate_run_id(strategy_id);
        INFO("Generated backtest run_id: " + backtest_run_id + " for daily position storage");

        // Track last date we saved positions for (to avoid saving multiple times per day)
        std::string last_saved_date = "";

        // Process each bar
        INFO("Starting backtest simulation with " + std::to_string(data_result.value().size()) +
             " bars");

        size_t processed_bars = 0;

        // Group bars by timestamp for realistic simulation
        std::map<Timestamp, std::vector<Bar>> bars_by_time;
        std::unordered_map<std::string, int> symbol_counts;
        std::map<Timestamp, std::set<std::string>> symbols_per_timestamp;

        for (const auto& bar : data_result.value()) {
            bars_by_time[bar.timestamp].push_back(bar);
            symbol_counts[bar.symbol]++;
            symbols_per_timestamp[bar.timestamp].insert(bar.symbol);
        }

        // Debug: Log data organization
        INFO("Data grouping results: " + std::to_string(bars_by_time.size()) +
             " unique timestamps");
        for (const auto& [symbol, count] : symbol_counts) {
            DEBUG("Symbol " + symbol + " has " + std::to_string(count) + " bars");
        }

        // Check if we have proper cross-sectional data
        int perfect_timestamps = 0;
        int expected_symbols = symbol_counts.size();
        for (const auto& [timestamp, symbols] : symbols_per_timestamp) {
            if (symbols.size() == expected_symbols) {
                perfect_timestamps++;
            }
            if (symbols_per_timestamp.size() <= 5) {  // Only log first 5 to avoid spam
                DEBUG("Timestamp " +
                      std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                                         timestamp.time_since_epoch())
                                         .count()) +
                      " has " + std::to_string(symbols.size()) + " symbols");
            }
        }
        INFO("Perfect cross-sectional timestamps: " + std::to_string(perfect_timestamps) + "/" +
             std::to_string(bars_by_time.size()));

        // Process bars in chronological order
        for (const auto& [timestamp, bars] : bars_by_time) {
            // Update slippage model
            if (slippage_model_) {
                for (const auto& bar : bars) {
                    slippage_model_->update(bar);
                }
            }

            // Process strategy signals first
            auto signal_result = process_strategy_signals(bars, strategy, current_positions,
                                                          executions, equity_curve, signals);

            if (signal_result.is_error()) {
                ERROR("Bar processing failed: " + std::string(signal_result.error()->what()));

                (void)StateManager::instance().update_state(backtest_component_id_,
                                                            ComponentState::ERR_STATE,
                                                            signal_result.error()->what());

                return make_error<BacktestResults>(signal_result.error()->code(),
                                                   signal_result.error()->what(), "BacktestEngine");
            }

            // Then apply portfolio-level constraints
            if (config_.portfolio_config.use_risk_management ||
                config_.portfolio_config.use_optimization) {
                auto constraint_result = apply_portfolio_constraints(bars, current_positions,
                                                                     equity_curve, risk_metrics);

                if (constraint_result.is_error()) {
                    ERROR("Portfolio constraint application failed: " +
                          std::string(constraint_result.error()->what()));

                    (void)StateManager::instance().update_state(backtest_component_id_,
                                                                ComponentState::ERR_STATE,
                                                                constraint_result.error()->what());

                    return make_error<BacktestResults>(constraint_result.error()->code(),
                                                       constraint_result.error()->what(),
                                                       "BacktestEngine");
                }
            }

            // Save positions daily if storage is enabled (once per unique date)
            if (config_.store_trade_details && db_ && !bars.empty()) {
                // Extract date from current timestamp
                Timestamp current_timestamp = bars[0].timestamp;
                auto time_t = std::chrono::system_clock::to_time_t(current_timestamp);
                std::stringstream date_ss;
                std::tm time_info;
                trade_ngin::core::safe_gmtime(&time_t, &time_info);
                date_ss << std::put_time(&time_info, "%Y-%m-%d");
                std::string current_date = date_ss.str();
                
                // Only save positions if this is a new date (avoid saving multiple times per day)
                if (current_date != last_saved_date) {
                    // Convert positions map to vector
                    std::vector<Position> positions_vec;
                    positions_vec.reserve(current_positions.size());
                    
                    for (const auto& [symbol, pos] : current_positions) {
                        // Create a copy with updated timestamp
                        Position pos_with_date = pos;
                        pos_with_date.last_update = current_timestamp;
                        positions_vec.push_back(pos_with_date);
                    }
                    
                    // Save positions for this day using the run_id generated at the start
                    if (!positions_vec.empty()) {
                        auto save_result = db_->store_backtest_positions(positions_vec, backtest_run_id, 
                                                                          "backtest.final_positions");
                        if (save_result.is_error()) {
                            // Log error but don't fail the backtest
                            ERROR("Failed to save daily positions for date " + current_date + 
                                  ", run_id: " + backtest_run_id + 
                                  ", error: " + std::string(save_result.error()->what()));
                        } else {
                            INFO("Saved " + std::to_string(positions_vec.size()) + 
                                  " positions for run_id: " + backtest_run_id + 
                                  " on date: " + current_date);
                            last_saved_date = current_date;  // Update last saved date
                        }
                    } else {
                        DEBUG("No positions to save for date " + current_date);
                    }
                }
            }

            processed_bars += bars.size();

            // Periodically log progress
            if (processed_bars % 1000 == 0) {
                INFO("Processed " + std::to_string(processed_bars) + " bars");
            }
        }

        // Stop strategy
        INFO("Backtest complete, stopping strategy");
        strategy->stop();

        // Filter out warmup period from equity curve
        if (config_.strategy_config.warmup_days > 0 &&
            equity_curve.size() > static_cast<size_t>(config_.strategy_config.warmup_days)) {
            INFO("Filtering out " + std::to_string(config_.strategy_config.warmup_days) +
                 " warmup days from equity curve (total points: " +
                 std::to_string(equity_curve.size()) + ")");

            // Get the equity value at the end of warmup to use as the new starting value
            double warmup_end_equity = equity_curve[config_.strategy_config.warmup_days].second;

            // Erase the warmup period
            equity_curve.erase(equity_curve.begin(),
                              equity_curve.begin() + config_.strategy_config.warmup_days);

            // Normalize the equity curve to start at initial capital
            double initial_capital = static_cast<double>(config_.portfolio_config.initial_capital);
            double scale_factor = initial_capital / warmup_end_equity;

            for (auto& point : equity_curve) {
                point.second *= scale_factor;
            }

            INFO("Equity curve after warmup filter: " + std::to_string(equity_curve.size()) +
                 " points, starting at $" + std::to_string(equity_curve.front().second));
        }

        // Sort executions by timestamp to ensure chronological order
        std::sort(executions.begin(), executions.end(), 
                  [](const ExecutionReport& a, const ExecutionReport& b) {
                      return a.fill_time < b.fill_time;
                  });

        // Calculate final results
        INFO("Calculating backtest metrics");
        auto results = calculate_metrics(equity_curve, executions);

        // Add position and execution history
        results.executions = std::move(executions);
        results.positions.reserve(current_positions.size());
        for (const auto& [_, pos] : current_positions) {
            results.positions.push_back(pos);
        }
        results.equity_curve = std::move(equity_curve);

        // Calculate drawdown curve
        results.drawdown_curve = calculate_drawdowns(results.equity_curve);

        // Add risk metrics history
        results.risk_metrics.reserve(risk_metrics.size());
        for (size_t i = 0; i < risk_metrics.size(); ++i) {
            // Match timestamps from equity curve if available
            Timestamp ts =
                i < equity_curve.size() ? equity_curve[i].first : std::chrono::system_clock::now();

            results.risk_metrics.emplace_back(ts, risk_metrics[i]);
        }

        // Add collected signals to results
        results.signals = signals;

        // DEBUG: Log what we generated during backtest
        INFO("=== BACKTEST DEBUG INFO ===");
        INFO("Generated " + std::to_string(results.executions.size()) + " executions (all position changes)");
        INFO("Generated " + std::to_string(results.actual_trades.size()) + " actual trades (position closings)");
        INFO("Collected " + std::to_string(results.signals.size()) + " signals");
        INFO("Total trades in metrics: " + std::to_string(results.total_trades));
        INFO("Equity curve points: " + std::to_string(results.equity_curve.size()));
        if (!results.actual_trades.empty()) {
            INFO("First actual trade: " + results.actual_trades[0].symbol + " " + 
                 (results.actual_trades[0].side == Side::BUY ? "BUY" : "SELL") + " " +
                 std::to_string(static_cast<double>(results.actual_trades[0].filled_quantity)));
        }
        if (!results.signals.empty()) {
            auto first_signal = results.signals.begin();
            INFO("First signal: " + first_signal->first.second + " = " + 
                 std::to_string(first_signal->second));
        }
        INFO("=== END DEBUG INFO ===");

        // Update final state
        (void)StateManager::instance().update_state(backtest_component_id_,
                                                    ComponentState::STOPPED);

        INFO("Backtest completed successfully with " + std::to_string(signals.size()) + " signals collected");

        return Result<BacktestResults>(results);

    } catch (const std::exception& e) {
        ERROR("Unexpected error during backtest: " + std::string(e.what()));

        (void)StateManager::instance().update_state(backtest_component_id_,
                                                    ComponentState::ERR_STATE,
                                                    std::string("Unexpected error: ") + e.what());

        return make_error<BacktestResults>(ErrorCode::UNKNOWN_ERROR,
                                           std::string("Error running backtest: ") + e.what(),
                                           "BacktestEngine");
    }
}

Result<BacktestResults> BacktestEngine::run_portfolio(std::shared_ptr<PortfolioManager> portfolio) {
    // Update state to running
    auto state_result =
        StateManager::instance().update_state(backtest_component_id_, ComponentState::RUNNING);

    if (state_result.is_error()) {
        ERROR("Failed to update backtest engine state: " +
              std::string(state_result.error()->what()));
    }

    try {
        if (!portfolio) {
            return make_error<BacktestResults>(ErrorCode::INVALID_ARGUMENT,
                                               "Null portfolio manager provided for backtest",
                                               "BacktestEngine");
        }

        // Share the risk manager with the portfolio manager if available
        if (risk_manager_ && portfolio) {
            portfolio->set_risk_manager(
                std::shared_ptr<RiskManager>(risk_manager_.get(), [](RiskManager*) {}));
        }

        // CRITICAL: Disable MarketDataBus publishing during backtest data loading
        // This prevents the PortfolioManager callback from processing data twice
        // (once during loading, once during the main backtest loop)
        INFO("Disabling MarketDataBus publishing during data loading");
        MarketDataBus::instance().set_publish_enabled(false);

        // Load historical market data
        auto data_result = load_market_data();

        // Re-enable publishing after loading (though not strictly needed for backtests)
        MarketDataBus::instance().set_publish_enabled(true);
        INFO("Re-enabled MarketDataBus publishing");

        if (data_result.is_error()) {
            ERROR("Failed to load market data: " + std::string(data_result.error()->what()));

            (void)StateManager::instance().update_state(
                backtest_component_id_, ComponentState::ERR_STATE, data_result.error()->what());

            return make_error<BacktestResults>(data_result.error()->code(),
                                               data_result.error()->what(), "BacktestEngine");
        }

        // Initialize tracking variables
        std::vector<ExecutionReport> executions;
        std::vector<std::pair<Timestamp, double>> equity_curve;
        std::vector<RiskResult> risk_metrics;

        // Get initial portfolio config
        const auto& portfolio_config = portfolio->get_config();
        double initial_capital =
            static_cast<double>(portfolio_config.total_capital - portfolio_config.reserve_capital);

        // Initialize equity curve with starting point
        equity_curve.emplace_back(config_.strategy_config.start_date, initial_capital);

        // Generate run_id at the start for daily position storage
        // Use the same format as RunIdGenerator::generate_portfolio_run_id for consistency
        std::vector<std::string> strategy_names_for_id;
        for (const auto& strategy : portfolio->get_strategies()) {
            try {
                const auto& metadata = strategy->get_metadata();
                if (!metadata.id.empty()) {
                    strategy_names_for_id.push_back(metadata.id);
                } else {
                    strategy_names_for_id.push_back("TREND_FOLLOWING");  // Default
                }
            } catch (...) {
                strategy_names_for_id.push_back("TREND_FOLLOWING");  // Default
            }
        }
        
        // Use RunIdGenerator to match the format used in save_portfolio_results_to_db
        std::string backtest_run_id = RunIdGenerator::generate_portfolio_run_id(
            strategy_names_for_id, config_.strategy_config.end_date);
        current_run_id_ = backtest_run_id;  // Store for use in save_portfolio_results_to_db
        INFO("Generated portfolio backtest run_id: " + backtest_run_id + " for daily position storage");

        // Track last saved date (to avoid saving multiple times per day)
        std::string last_saved_date = "";

        // Group bars by timestamp for realistic simulation
        std::map<Timestamp, std::vector<Bar>> bars_by_time;
        for (const auto& bar : data_result.value()) {
            bars_by_time[bar.timestamp].push_back(bar);
        }

        INFO("Starting portfolio backtest simulation with " +
             std::to_string(portfolio->get_strategies().size()) + " strategies and " +
             std::to_string(data_result.value().size()) + " bars");

        size_t processed_bars = 0;

        // Process bars in chronological order
        int loop_count = 0;  // NOT static - reset each backtest run
        for (const auto& [timestamp, bars] : bars_by_time) {
            if (++loop_count <= 3) {
                auto time_t = std::chrono::system_clock::to_time_t(timestamp);
                std::tm tm = *std::gmtime(&time_t);
                char time_str[64];
                std::strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm);
                INFO("LOOP_ITER #" + std::to_string(loop_count) + ": processing timestamp=" + std::string(time_str));
            }
            try {
                // Process portfolio time step
                auto result = process_portfolio_data(timestamp, bars, portfolio, executions,
                                                     equity_curve, risk_metrics);

                if (result.is_error()) {
                    WARN("Portfolio data processing failed for timestamp " +
                         std::to_string(std::chrono::system_clock::to_time_t(timestamp)) + ": " +
                         std::string(result.error()->what()) +
                         ". Continuing with next time period.");

                    // Don't fail the entire backtest due to a single time period failure
                    // Instead, add a placeholder to maintain continuity
                    if (!equity_curve.empty()) {
                        // Use previous value for equity curve
                        equity_curve.emplace_back(timestamp, equity_curve.back().second);
                    }
                }
            } catch (const std::exception& e) {
                WARN("Exception processing portfolio data for timestamp " +
                     std::to_string(std::chrono::system_clock::to_time_t(timestamp)) + ": " +
                     std::string(e.what()) + ". Continuing with next time period.");

                // Don't fail the entire backtest due to a single time period exception
                if (!equity_curve.empty()) {
                    // Use previous value for equity curve
                    equity_curve.emplace_back(timestamp, equity_curve.back().second);
                }
            }

            // Save positions daily if storage is enabled (once per unique date)
            if (config_.store_trade_details && db_ && !bars.empty()) {
                // Extract date from current timestamp
                auto time_t = std::chrono::system_clock::to_time_t(timestamp);
                std::stringstream date_ss;
                std::tm time_info;
                trade_ngin::core::safe_gmtime(&time_t, &time_info);
                date_ss << std::put_time(&time_info, "%Y-%m-%d");
                std::string current_date = date_ss.str();
                
                // Only save positions if this is a new date (avoid saving multiple times per day)
                if (current_date != last_saved_date) {
                    // Get portfolio positions
                    auto portfolio_positions = portfolio->get_portfolio_positions();
                    
                    // Convert positions map to vector
                    std::vector<Position> positions_vec;
                    positions_vec.reserve(portfolio_positions.size());
                    
                    for (const auto& [symbol, pos] : portfolio_positions) {
                        // Create a copy with updated timestamp
                        Position pos_with_date = pos;
                        pos_with_date.last_update = timestamp;
                        positions_vec.push_back(pos_with_date);
                    }
                    
                    // Save positions for this day using the run_id generated at the start
                    if (!positions_vec.empty()) {
                        auto save_result = db_->store_backtest_positions(positions_vec, backtest_run_id, 
                                                                          "backtest.final_positions");
                        if (save_result.is_error()) {
                            // Log error but don't fail the backtest
                            ERROR("Failed to save daily portfolio positions for date " + current_date + 
                                  ", run_id: " + backtest_run_id + 
                                  ", error: " + std::string(save_result.error()->what()));
                        } else {
                            INFO("Saved " + std::to_string(positions_vec.size()) + 
                                  " portfolio positions for run_id: " + backtest_run_id + 
                                  " on date: " + current_date);
                            last_saved_date = current_date;  // Update last saved date
                        }
                    } else {
                        DEBUG("No portfolio positions to save for date " + current_date);
                    }
                }
            }

            processed_bars += bars.size();

            // Periodically log progress and clean up memory
            if (processed_bars % 1000 == 0) {
                INFO("Processed " + std::to_string(processed_bars) + " bars across " +
                     std::to_string(portfolio->get_strategies().size()) + " strategies");
                
                // MEMORY FIX: Keep only last 1000 equity curve points to prevent memory buildup
                if (equity_curve.size() > 1000) {
                    equity_curve.erase(equity_curve.begin(), equity_curve.begin() + 500);
                }
                
                // MEMORY FIX: Keep only last 500 risk metrics
                if (risk_metrics.size() > 500) {
                    risk_metrics.erase(risk_metrics.begin(), risk_metrics.begin() + 250);
                }
            }
        }

        // Filter out warmup period from equity curve
        if (config_.strategy_config.warmup_days > 0 &&
            equity_curve.size() > static_cast<size_t>(config_.strategy_config.warmup_days)) {
            INFO("Filtering out " + std::to_string(config_.strategy_config.warmup_days) +
                 " warmup days from equity curve (total points: " +
                 std::to_string(equity_curve.size()) + ")");

            // Get the equity value at the end of warmup to use as the new starting value
            double warmup_end_equity = equity_curve[config_.strategy_config.warmup_days].second;

            // Erase the warmup period
            equity_curve.erase(equity_curve.begin(),
                              equity_curve.begin() + config_.strategy_config.warmup_days);

            // Normalize the equity curve to start at initial capital
            double initial_capital_norm = static_cast<double>(config_.portfolio_config.initial_capital);
            double scale_factor = initial_capital_norm / warmup_end_equity;

            for (auto& point : equity_curve) {
                point.second *= scale_factor;
            }

            INFO("Equity curve after warmup filter: " + std::to_string(equity_curve.size()) +
                 " points, starting at $" + std::to_string(equity_curve.front().second));
        }

        // Sort executions by timestamp to ensure chronological order
        std::sort(executions.begin(), executions.end(), 
                  [](const ExecutionReport& a, const ExecutionReport& b) {
                      return a.fill_time < b.fill_time;
                  });

        // Calculate final results
        INFO("Calculating portfolio backtest metrics");
        auto results = calculate_metrics(equity_curve, executions);

        // Add position and execution history
        results.executions = std::move(executions);

        // Get final portfolio positions
        try {
            auto portfolio_positions = portfolio->get_portfolio_positions();
            results.positions.reserve(portfolio_positions.size());
            for (const auto& [_, pos] : portfolio_positions) {
                results.positions.push_back(pos);
            }
        } catch (const std::exception& e) {
            WARN("Exception getting final portfolio positions: " + std::string(e.what()) +
                 ". Backtest results will have incomplete position data.");
        }

        results.equity_curve = std::move(equity_curve);

        // Calculate drawdown curve
        results.drawdown_curve = calculate_drawdowns(results.equity_curve);

        // Add risk metrics history
        results.risk_metrics.reserve(risk_metrics.size());
        for (size_t i = 0; i < risk_metrics.size(); ++i) {
            // Match timestamps from equity curve if available
            Timestamp ts =
                i < equity_curve.size() ? equity_curve[i].first : std::chrono::system_clock::now();

            results.risk_metrics.emplace_back(ts, risk_metrics[i]);
        }

        // Update final state
        (void)StateManager::instance().update_state(backtest_component_id_,
                                                    ComponentState::STOPPED);

        INFO("Portfolio backtest completed successfully with " +
             std::to_string(portfolio->get_strategies().size()) + " strategies");

        return Result<BacktestResults>(results);

    } catch (const std::exception& e) {
        ERROR("Unexpected error during portfolio backtest: " + std::string(e.what()));

        (void)StateManager::instance().update_state(backtest_component_id_,
                                                    ComponentState::ERR_STATE,
                                                    std::string("Unexpected error: ") + e.what());

        return make_error<BacktestResults>(
            ErrorCode::UNKNOWN_ERROR, std::string("Error running portfolio backtest: ") + e.what(),
            "BacktestEngine");
    }
}

Result<void> BacktestEngine::process_bar(
    const std::vector<Bar>& bars, std::shared_ptr<StrategyInterface> strategy,
    std::map<std::string, Position>& current_positions,
    std::vector<std::pair<Timestamp, double>>& equity_curve,
    std::vector<RiskResult>& risk_metrics) {
    try {
        // PnL Lag Model: Store previous day's close prices for execution pricing
        static std::unordered_map<std::string, double> previous_day_close_prices;
        static std::unordered_map<std::string, double> two_days_ago_close_prices;
        
        // Update price history for PnL lag model
        for (const auto& bar : bars) {
            const std::string& symbol = bar.symbol;
            double current_close = static_cast<double>(bar.close);
            
            // Shift prices: T-2 becomes T-1, T-1 becomes current
            if (previous_day_close_prices.find(symbol) != previous_day_close_prices.end()) {
                two_days_ago_close_prices[symbol] = previous_day_close_prices[symbol];
            }
            previous_day_close_prices[symbol] = current_close;
        }

        // Pass market data to strategy
        auto data_result = strategy->on_data(bars);
        if (data_result.is_error()) {
            return data_result;
        }

        // Get updated positions from strategy
        const auto& new_positions = strategy->get_positions();
        std::vector<ExecutionReport> period_executions;

        // Process position changes
        for (const auto& [symbol, new_pos] : new_positions) {
            const auto current_it = current_positions.find(symbol);
            double current_qty = (current_it != current_positions.end())
                                     ? static_cast<double>(current_it->second.quantity)
                                     : 0.0;

            if (std::abs(static_cast<double>(new_pos.quantity) - current_qty) > 1e-4) {
                // PnL Lag Model: Use previous day's close for execution price (eliminate lookahead bias)
                double execution_price = 0.0;
                if (previous_day_close_prices.find(symbol) != previous_day_close_prices.end()) {
                    execution_price = previous_day_close_prices[symbol];
                    DEBUG("PnL Lag Model: Using previous day close for " + symbol + " execution: " + std::to_string(execution_price));
                } else {
                    // Fallback: if no previous price available, use current close (first day case)
                    for (const auto& bar : bars) {
                        if (bar.symbol == symbol) {
                            execution_price = static_cast<double>(bar.close);
                            break;
                        }
                    }
                    DEBUG("PnL Lag Model: First day fallback for " + symbol + " execution: " + std::to_string(execution_price));
                }

                if (execution_price == 0.0) {
                    continue;  // Skip if price not available
                }

                // Calculate trade size
                double trade_size = static_cast<double>(new_pos.quantity) - current_qty;
                Side side = trade_size > 0 ? Side::BUY : Side::SELL;

                // Apply slippage to price
                double fill_price;
                if (slippage_model_) {
                    // Find the bar for this symbol
                    std::optional<Bar> symbol_bar;
                    for (const auto& bar : bars) {
                        if (bar.symbol == symbol) {
                            symbol_bar = bar;
                            break;
                        }
                    }

                    fill_price = slippage_model_->calculate_slippage(
                        execution_price, std::abs(trade_size), side, symbol_bar);
                } else {
                    // Apply basic slippage model
                    double slip_factor =
                        static_cast<double>(config_.strategy_config.slippage_model) /
                        10000.0;  // bps to decimal
                    fill_price = side == Side::BUY ? execution_price * (1.0 + slip_factor)
                                                   : execution_price * (1.0 - slip_factor);
                }

                // Create execution report
                ExecutionReport exec;
                exec.order_id = "BT-" + std::to_string(equity_curve.size());
                exec.exec_id = "EX-" + std::to_string(equity_curve.size());
                exec.symbol = symbol;
                exec.side = side;
                exec.filled_quantity = Quantity(std::abs(trade_size));
                exec.fill_price = fill_price;
                exec.fill_time = bars[0].timestamp;  // Use timestamp of current batch
                exec.commission = Decimal(calculate_transaction_costs(exec));
                exec.is_partial = false;

                // Update position
                current_positions[symbol] = new_pos;

                // Add to executions for this period
                period_executions.push_back(exec);

                // Notify strategy of fill
                auto fill_result = strategy->on_execution(exec);
                if (fill_result.is_error()) {
                    return fill_result;
                }
            }
        }

        // Calculate current portfolio value using PnL Lag Model
        double portfolio_value = static_cast<double>(config_.portfolio_config.initial_capital);
        for (const auto& [symbol, pos] : current_positions) {
            if (static_cast<double>(pos.quantity) == 0.0) continue;
            
            // PnL Lag Model: Daily PnL = (current_day_close - previous_day_close) * quantity * point_value
            double current_price = 0.0;
            for (const auto& bar : bars) {
                if (bar.symbol == symbol) {
                    current_price = static_cast<double>(bar.close);
                    break;
                }
            }
            
            if (current_price > 0.0 && previous_day_close_prices.find(symbol) != previous_day_close_prices.end()) {
                // Get point value multiplier from strategy (correct symbol-specific value)
                double point_value = 1.0;
                auto trend_strategy = std::dynamic_pointer_cast<TrendFollowingStrategy>(strategy);
                if (trend_strategy) {
                    try {
                        point_value = trend_strategy->get_point_value_multiplier(symbol);
                    } catch (const std::exception& e) {
                        WARN("Failed to get point value for " + symbol + ": " + e.what() + ". Using 1.0");
                        point_value = 1.0;
                    }
                }

                // Calculate daily PnL: quantity * (current_close - previous_close) * point_value
                double daily_pnl = static_cast<double>(pos.quantity) *
                                  (current_price - previous_day_close_prices[symbol]) * point_value;
                portfolio_value += daily_pnl;
            }
        }

        // Update equity curve
        if (!bars.empty()) {
            equity_curve.emplace_back(bars[0].timestamp, portfolio_value);
        }

        // Apply risk management if enabled
        if (config_.portfolio_config.use_risk_management && risk_manager_) {
            MarketData market_data = risk_manager_->create_market_data(bars);
            // Convert map to unordered_map for risk manager compatibility
            std::unordered_map<std::string, Position> positions_for_risk(current_positions.begin(), current_positions.end());
            auto risk_result = risk_manager_->process_positions(positions_for_risk, market_data);
            if (risk_result.is_error()) {
                return make_error<void>(risk_result.error()->code(), risk_result.error()->what(),
                                        "BacktestEngine");
            }

            // Store risk metrics for analysis
            risk_metrics.push_back(risk_result.value());

            // Scale positions if risk limits exceeded
            if (risk_result.value().risk_exceeded) {
                double scale = risk_result.value().recommended_scale;
                WARN("Risk limits exceeded: scaling positions by " + std::to_string(scale));

                for (auto& [symbol, pos] : current_positions) {
                    pos.quantity = Quantity(static_cast<double>(pos.quantity) * scale);
                }
            }
        }

        // Apply optimization if enabled
        if (config_.portfolio_config.use_optimization && optimizer_ &&
            current_positions.size() > 1) {
            // Prepare inputs for optimization
            std::vector<std::string> symbols;
            std::vector<double> current_pos, target_pos, costs, weights;

            // Extract positions and costs
            for (const auto& [symbol, pos] : current_positions) {
                symbols.push_back(symbol);
                current_pos.push_back(static_cast<double>(pos.quantity));
                target_pos.push_back(
                    static_cast<double>(pos.quantity));  // Use current as starting point

                // Default cost is 1.0, can be refined with specific costs per symbol
                costs.push_back(1.0);

                // Equal weights to start, could be refined based on market cap, etc.
                weights.push_back(1.0);
            }

            // Simple diagonal covariance matrix as placeholder
            // In production, would use actual market data to calculate
            std::vector<std::vector<double>> covariance(symbols.size(),
                                                        std::vector<double>(symbols.size(), 0.0));

            // Set diagonal elements (variances)
            for (size_t i = 0; i < symbols.size(); ++i) {
                covariance[i][i] = 0.01;  // Default variance value
            }

            // Run optimization
            auto opt_result =
                optimizer_->optimize(current_pos, target_pos, costs, weights, covariance);

            if (opt_result.is_error()) {
                WARN("Optimization failed: " + std::string(opt_result.error()->what()));
            } else {
                // Apply optimized positions
                const auto& optimized = opt_result.value().positions;
                for (size_t i = 0; i < symbols.size(); ++i) {
                    current_positions[symbols[i]].quantity = Quantity(optimized[i]);
                }

                DEBUG("Positions optimized with tracking error: " +
                      std::to_string(opt_result.value().tracking_error));
            }
        }

        INFO("Successfully saved backtest results to database");
        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::UNKNOWN_ERROR,
                                std::string("Error processing bar: ") + e.what(), "BacktestEngine");
    }
}

// Process market data through each strategy and collect their positions
// BEGINNING-OF-DAY MODEL (no lookahead in signals):
// - Use previous day's bars for signal generation (strategy->on_data)
// - Use today's bars for executions and PnL, priced off previous day's close
Result<void> BacktestEngine::process_strategy_signals(
    const std::vector<Bar>& bars, std::shared_ptr<StrategyInterface> strategy,
    std::map<std::string, Position>& current_positions,
    std::vector<ExecutionReport>& executions,
    std::vector<std::pair<Timestamp, double>>& equity_curve,
    std::map<std::pair<Timestamp, std::string>, double>& signals) {
    try {
        // Static state for PnL lag model (previous day's close) and signal lag (previous bars)
        static std::unordered_map<std::string, double> previous_day_close_prices;
        static bool has_previous_bars = false;
        static std::vector<Bar> previous_bars;

        if (bars.empty()) {
            return Result<void>();
        }

        // If this is the first bar set, initialize previous_day_close_prices and previous_bars
        // but do NOT trade yet – we don't have T-1 data to generate T positions.
        if (!has_previous_bars) {
            for (const auto& bar : bars) {
                previous_day_close_prices[bar.symbol] = static_cast<double>(bar.close);
            }
            previous_bars = bars;
            has_previous_bars = true;
            return Result<void>();
        }

        // BEGINNING-OF-DAY MODEL:
        // - Use previous day's bars (previous_bars) to generate signals / positions for "today".
        // - Use today's bars (bars) together with previous_day_close_prices to compute PnL.

        // 1) Pass previous day's market data to strategy for signal generation (no lookahead)
        auto data_result = strategy->on_data(previous_bars);
        if (data_result.is_error()) {
            return data_result;
        }

        // 2) Get updated positions from strategy
        const auto& new_positions = strategy->get_positions();
        std::vector<ExecutionReport> period_executions;

        // Collect signals - for now, use position changes as proxy for signals
        Timestamp current_time =
            bars.empty() ? std::chrono::system_clock::now() : bars[0].timestamp;

        // 3) Process position changes and generate executions priced at previous close
        for (const auto& [symbol, new_pos] : new_positions) {
            const auto current_it = current_positions.find(symbol);
            double current_qty = (current_it != current_positions.end())
                                     ? static_cast<double>(current_it->second.quantity)
                                     : 0.0;

            if (std::abs(static_cast<double>(new_pos.quantity) - current_qty) > 1e-4) {
                // Collect signal: position change represents a trading signal
                double signal_strength = static_cast<double>(new_pos.quantity) - current_qty;
                signals[{current_time, symbol}] = signal_strength;

                DEBUG("📊 Signal collected: " + symbol + " = " +
                      std::to_string(signal_strength) + " (pos change: " +
                      std::to_string(current_qty) + " -> " +
                      std::to_string(static_cast<double>(new_pos.quantity)) + ")");

                // Use previous day's close for execution price (BOD model, no lookahead)
                double execution_price = 0.0;
                auto price_it = previous_day_close_prices.find(symbol);
                if (price_it != previous_day_close_prices.end()) {
                    execution_price = price_it->second;
                    DEBUG("PnL Lag Model: Using previous day close for " + symbol +
                          " execution: " + std::to_string(execution_price));
                } else {
                    // Fallback: if no previous price available, use today's close (first trade case)
                    for (const auto& bar : bars) {
                        if (bar.symbol == symbol) {
                            execution_price = static_cast<double>(bar.close);
                            break;
                        }
                    }
                    DEBUG("PnL Lag Model: Fallback for " + symbol +
                          " execution: " + std::to_string(execution_price));
                }

                if (execution_price == 0.0) {
                    continue;  // Skip if price not available
                }

                // Calculate trade size
                double trade_size = static_cast<double>(new_pos.quantity) - current_qty;
                Side side = trade_size > 0 ? Side::BUY : Side::SELL;

                // Apply slippage to price
                double fill_price;
                if (slippage_model_) {
                    // Find the bar for this symbol (today's bar)
                    std::optional<Bar> symbol_bar;
                    for (const auto& bar : bars) {
                        if (bar.symbol == symbol) {
                            symbol_bar = bar;
                            break;
                        }
                    }

                    fill_price = slippage_model_->calculate_slippage(
                        execution_price, std::abs(trade_size), side, symbol_bar);
                } else {
                    // Apply basic slippage model
                    double slip_factor =
                        static_cast<double>(config_.strategy_config.slippage_model) /
                        10000.0;  // bps to decimal
                    fill_price = side == Side::BUY ? execution_price * (1.0 + slip_factor)
                                                   : execution_price * (1.0 - slip_factor);
                }

                // Create execution report
                ExecutionReport exec;
                exec.order_id = "BT-" + std::to_string(equity_curve.size());
                exec.exec_id = "EX-" + std::to_string(equity_curve.size());
                exec.symbol = symbol;
                exec.side = side;
                exec.filled_quantity = Quantity(std::abs(trade_size));
                exec.fill_price = fill_price;
                exec.fill_time = bars[0].timestamp;  // Use timestamp of current batch
                exec.commission = Decimal(calculate_transaction_costs(exec));
                exec.is_partial = false;

                // Update position
                current_positions[symbol] = new_pos;

                // Add to executions for this period
                executions.push_back(exec);
                period_executions.push_back(exec);

                // Notify strategy of fill
                auto fill_result = strategy->on_execution(exec);
                if (fill_result.is_error()) {
                    return fill_result;
                }
            }
        }

        // 4) Calculate current portfolio value using close-to-close PnL (no lookahead)
        double portfolio_value = static_cast<double>(config_.portfolio_config.initial_capital);
        for (const auto& [symbol, pos] : current_positions) {
            if (static_cast<double>(pos.quantity) == 0.0)
                continue;

            // Today's close
            double current_price = 0.0;
            for (const auto& bar : bars) {
                if (bar.symbol == symbol) {
                    current_price = static_cast<double>(bar.close);
                    break;
                }
            }

            auto prev_it = previous_day_close_prices.find(symbol);
            if (current_price > 0.0 && prev_it != previous_day_close_prices.end()) {
                double previous_close = prev_it->second;

                // Get point value multiplier from strategy (correct symbol-specific value)
                double point_value = 1.0;
                auto trend_strategy = std::dynamic_pointer_cast<TrendFollowingStrategy>(strategy);
                if (trend_strategy) {
                    try {
                        point_value = trend_strategy->get_point_value_multiplier(symbol);
                    } catch (const std::exception& e) {
                        WARN("Failed to get point value for " + symbol + ": " + e.what() +
                             ". Using 1.0");
                        point_value = 1.0;
                    }
                }

                // Calculate daily PnL: quantity * (current_close - previous_close) * point_value
                double daily_pnl = static_cast<double>(pos.quantity) *
                                   (current_price - previous_close) * point_value;
                portfolio_value += daily_pnl;
            }
        }

        // 5) Update equity curve for today
        equity_curve.emplace_back(bars[0].timestamp, portfolio_value);

        // 6) Update previous_day_close_prices and previous_bars for next day
        for (const auto& bar : bars) {
            previous_day_close_prices[bar.symbol] = static_cast<double>(bar.close);
        }
        previous_bars = bars;
        has_previous_bars = true;

        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::UNKNOWN_ERROR,
                                std::string("Error processing strategy signals: ") + e.what(),
                                "BacktestEngine");
    }
}

// Apply portfolio-level constraints (risk management and optimization)
Result<void> BacktestEngine::apply_portfolio_constraints(
    const std::vector<Bar>& bars, std::map<std::string, Position>& current_positions,
    std::vector<std::pair<Timestamp, double>>& equity_curve,
    std::vector<RiskResult>& risk_metrics) {
    try {
        // Apply risk management if enabled
        if (config_.portfolio_config.use_risk_management && risk_manager_) {
            MarketData market_data = risk_manager_->create_market_data(bars);
            // Convert map to unordered_map for risk manager compatibility
            std::unordered_map<std::string, Position> positions_for_risk(current_positions.begin(), current_positions.end());
            auto risk_result = risk_manager_->process_positions(positions_for_risk, market_data);
            if (risk_result.is_error()) {
                return make_error<void>(risk_result.error()->code(), risk_result.error()->what(),
                                        "BacktestEngine");
            }

            // Store risk metrics for analysis
            risk_metrics.push_back(risk_result.value());

            // Scale positions if risk limits exceeded
            if (risk_result.value().risk_exceeded) {
                double scale = risk_result.value().recommended_scale;
                WARN("Risk limits exceeded: scaling positions by " + std::to_string(scale));

                for (auto& [symbol, pos] : current_positions) {
                    pos.quantity = Quantity(static_cast<double>(pos.quantity) * scale);
                }
            }
        }

        // Apply optimization if enabled
        if (config_.portfolio_config.use_optimization && optimizer_ &&
            current_positions.size() > 1) {
            // Prepare inputs for optimization
            std::vector<std::string> symbols;
            std::vector<double> current_pos, target_pos, costs, weights;

            // Extract positions and costs
            for (const auto& [symbol, pos] : current_positions) {
                symbols.push_back(symbol);
                current_pos.push_back(static_cast<double>(pos.quantity));
                target_pos.push_back(
                    static_cast<double>(pos.quantity));  // Use current as starting point

                // Default cost is 1.0, can be refined with specific costs per symbol
                costs.push_back(1.0);

                // Equal weights to start, could be refined based on market cap, etc.
                weights.push_back(1.0);
            }

            // Simple diagonal covariance matrix as placeholder
            // In production, would use actual market data to calculate
            std::vector<std::vector<double>> covariance(symbols.size(),
                                                        std::vector<double>(symbols.size(), 0.0));

            // Set diagonal elements (variances)
            for (size_t i = 0; i < symbols.size(); ++i) {
                covariance[i][i] = 0.01;  // Default variance value
            }

            // Run optimization
            auto opt_result =
                optimizer_->optimize(current_pos, target_pos, costs, weights, covariance);

            if (opt_result.is_error()) {
                WARN("Optimization failed: " + std::string(opt_result.error()->what()));
            } else {
                // Apply optimized positions
                const auto& optimized = opt_result.value().positions;
                for (size_t i = 0; i < symbols.size(); ++i) {
                    current_positions[symbols[i]].quantity = Quantity(optimized[i]);
                }

                DEBUG("Positions optimized with tracking error: " +
                      std::to_string(opt_result.value().tracking_error));
            }
        }

        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::UNKNOWN_ERROR,
                                std::string("Error applying portfolio constraints: ") + e.what(),
                                "BacktestEngine");
    }
}

// Helper method for portfolio backtesting: combines positions from multiple strategies
void BacktestEngine::combine_positions(
    const std::vector<std::map<std::string, Position>>& strategy_positions,
    std::map<std::string, Position>& portfolio_positions) {
    
    // Clear current portfolio positions
    portfolio_positions.clear();

    // Combine positions from all strategies
    for (const auto& strategy_pos_map : strategy_positions) {
        // Since std::map is already sorted, no need for explicit sorting
        for (const auto& [symbol, pos] : strategy_pos_map) {
            if (portfolio_positions.find(symbol) == portfolio_positions.end()) {
                // New symbol, add to portfolio
                portfolio_positions[symbol] = pos;
            } else {
                // Existing symbol, update quantity
                portfolio_positions[symbol].quantity += pos.quantity;

                // Update average price based on quantities
                double total_quantity = static_cast<double>(portfolio_positions[symbol].quantity);
                if (std::abs(total_quantity) > 1e-4) {
                    portfolio_positions[symbol].average_price =
                        Decimal((static_cast<double>(portfolio_positions[symbol].average_price) *
                                     (total_quantity - static_cast<double>(pos.quantity)) +
                                 static_cast<double>(pos.average_price) *
                                     static_cast<double>(pos.quantity)) /
                                total_quantity);
                }
            }
        }
    }
}

// Helper method for portfolio backtesting: distributes portfolio positions back to strategies
void BacktestEngine::redistribute_positions(
    const std::map<std::string, Position>& portfolio_positions,
    std::vector<std::map<std::string, Position>>& strategy_positions,
    const std::vector<std::shared_ptr<StrategyInterface>>& strategies) {
    // Calculate the total quantity for each symbol across all strategies
    std::map<std::string, double> total_quantities;
    for (const auto& strategy_pos_map : strategy_positions) {
        for (const auto& [symbol, pos] : strategy_pos_map) {
            total_quantities[symbol] += std::abs(static_cast<double>(pos.quantity));
        }
    }

    // Distribute portfolio positions based on original allocation ratios
    for (size_t i = 0; i < strategy_positions.size(); ++i) {
        auto& strategy_pos_map = strategy_positions[i];

        // Since std::map is already sorted, no need for explicit sorting
        for (auto& [symbol, pos] : strategy_pos_map) {
            // Calculate the original ratio this strategy had of this symbol
            double original_ratio = 0.0;
            if (total_quantities[symbol] > 1e-4) {
                original_ratio =
                    std::abs(static_cast<double>(pos.quantity)) / total_quantities[symbol];
            }

            // Get the new portfolio position
            auto portfolio_it = portfolio_positions.find(symbol);
            if (portfolio_it != portfolio_positions.end()) {
                // Update the strategy position based on the ratio
                double new_quantity =
                    static_cast<double>(portfolio_it->second.quantity) * original_ratio;
                if (static_cast<double>(pos.quantity) < 0) {
                    // Maintain original direction (long/short)
                    new_quantity = -std::abs(new_quantity);
                }

                pos.quantity = Quantity(new_quantity);
            } else {
                // Symbol no longer in portfolio, zero out
                pos.quantity = Quantity(0.0);
            }
        }

        // Update the strategy with new positions
        if (i < strategies.size()) {
            for (const auto& [symbol, pos] : strategy_pos_map) {
                strategies[i]->update_position(symbol, pos);
            }
        }
    }
}

// Main portfolio backtesting loop
Result<void> BacktestEngine::process_portfolio_data(
    const Timestamp& timestamp, const std::vector<Bar>& bars,
    std::shared_ptr<PortfolioManager> portfolio, std::vector<ExecutionReport>& executions,
    std::vector<std::pair<Timestamp, double>>& equity_curve,
    std::vector<RiskResult>& risk_metrics) {
    try {
        // BEGINNING-OF-DAY MODEL FOR PORTFOLIO BACKTEST:
        // - Use previous day's bars for signal generation via PortfolioManager
        // - Use today's bars for executions' slippage/valuation and equity curve

        // Static storage for previous day's bars (for signal generation) and a flag
        static bool has_previous_bars = false;
        static std::vector<Bar> previous_bars;

        // Check for empty data
        if (bars.empty()) {
            ERROR("Empty market data provided for portfolio backtest");
            return make_error<void>(ErrorCode::MARKET_DATA_ERROR,
                                    "Empty market data provided for portfolio backtest",
                                    "BacktestEngine");
        }

        // Check for invalid portfolio manager
        if (!portfolio) {
            ERROR("Null portfolio manager provided for portfolio backtest");
            return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                    "Null portfolio manager provided for portfolio backtest",
                                    "BacktestEngine");
        }

        // If this is the first bar set, initialize previous_bars but do NOT trade yet
        // We still allow equity curve to be updated from existing positions,
        // but no new signals/positions are generated using same-day data.
        bool had_previous_bars = has_previous_bars;
        if (!has_previous_bars) {
            previous_bars = bars;
            has_previous_bars = true;
        }

        // Update slippage model
        if (slippage_model_) {
            for (const auto& bar : bars) {
                try {
                    slippage_model_->update(bar);
                } catch (const std::exception& e) {
                    WARN("Exception updating slippage model: " + std::string(e.what()) +
                         ". Continuing without slippage update for symbol " + bar.symbol);
                }
            }
        }

        // Process market data through portfolio manager
        // BEGINNING-OF-DAY: use previous day's bars for signal generation when available
        try {
            const auto& bars_for_signals = had_previous_bars ? previous_bars : bars;

            DEBUG("Processing market data for " +
                  std::to_string(bars_for_signals.size()) +
                  " symbols at timestamp " +
                  std::to_string(
                      std::chrono::duration_cast<std::chrono::seconds>(timestamp.time_since_epoch())
                          .count()));
            for (const auto& bar : bars_for_signals) {
                DEBUG("Market data: " + bar.symbol +
                      " close=" + std::to_string(static_cast<double>(bar.close)) +
                      " volume=" + std::to_string(static_cast<double>(bar.volume)));
            }

            // Use previous day's bars when available to avoid lookahead in signals
            auto data_result = portfolio->process_market_data(bars_for_signals);
            if (data_result.is_error()) {
                ERROR("Portfolio process_market_data failed: " +
                      std::string(data_result.error()->what()));
                return data_result;
            }
        } catch (const std::exception& e) {
            return make_error<void>(
                ErrorCode::UNKNOWN_ERROR,
                std::string("Error processing market data through portfolio: ") + e.what(),
                "BacktestEngine");
        }

        // Get executions from the portfolio manager
        std::vector<ExecutionReport> period_executions;
        // Only retrieve executions if we actually processed signals this period
        if (had_previous_bars) {
            try {
                period_executions = portfolio->get_recent_executions();
                DEBUG("Period executions count: " + std::to_string(period_executions.size()));
                for (const auto& exec : period_executions) {
                    DEBUG("Execution: symbol=" + exec.symbol +
                          ", side=" + (exec.side == Side::BUY ? "BUY" : "SELL") +
                          ", qty=" + std::to_string(static_cast<double>(exec.filled_quantity)) +
                          ", price=" + std::to_string(static_cast<double>(exec.fill_price)));
                }
                // CRITICAL FIX: Clear executions immediately after retrieval to prevent accumulation
                portfolio->clear_execution_history();
            } catch (const std::exception& e) {
                WARN("Exception getting recent executions: " + std::string(e.what()) +
                     ". Continuing with empty executions list.");
                period_executions.clear();
            }
        }

        // Apply slippage and transaction costs to executions
        for (auto& exec : period_executions) {
            try {
                // Apply slippage
                if (slippage_model_) {
                    // Find the bar for this symbol
                    std::optional<Bar> symbol_bar;
                    for (const auto& bar : bars) {
                        if (bar.symbol == exec.symbol) {
                            symbol_bar = bar;
                            break;
                        }
                    }

                    double adjusted_price = slippage_model_->calculate_slippage(
                        static_cast<double>(exec.fill_price),
                        static_cast<double>(exec.filled_quantity), exec.side, symbol_bar);

                    exec.fill_price = Price(adjusted_price);
                } else {
                    // Apply basic slippage model
                    double slip_factor =
                        static_cast<double>(config_.strategy_config.slippage_model) / 10000.0;
                    exec.fill_price =
                        exec.side == Side::BUY
                            ? Price(static_cast<double>(exec.fill_price) * (1.0 + slip_factor))
                            : Price(static_cast<double>(exec.fill_price) * (1.0 - slip_factor));
                }

                // Calculate and add commission
                exec.commission = Decimal(calculate_transaction_costs(exec));

                // Add to overall executions list
                executions.push_back(exec);
            } catch (const std::exception& e) {
                WARN("Exception processing execution for " + exec.symbol + ": " +
                     std::string(e.what()) + ". Skipping this execution.");
                // Skip this execution but continue with others
            }
        }

        // CRITICAL FIX: Feed executions back to strategies to update position P&L
        for (const auto& exec : period_executions) {
            try {
                // Get strategies from portfolio manager and update their positions
                auto strategies = portfolio->get_strategies();
                for (auto strategy_ptr : strategies) {
                    auto execution_result = strategy_ptr->on_execution(exec);
                    if (execution_result.is_error()) {
                        WARN("Failed to process execution for strategy, symbol " + exec.symbol +
                             ": " + execution_result.error()->to_string());
                    }
                }
            } catch (const std::exception& e) {
                WARN("Exception feeding execution back to strategies for symbol " + exec.symbol +
                     ": " + std::string(e.what()));
            }
        }

        // Note: Portfolio manager will now read positions directly from strategies
        // via the modified get_positions_internal() method

        // Create price map for portfolio value calculation
        std::unordered_map<std::string, double> current_prices;
        for (const auto& bar : bars) {
            current_prices[bar.symbol] = static_cast<double>(bar.close);
        }

        // Calculate portfolio value and update equity curve
        double portfolio_value = 0.0;
        try {
            portfolio_value = portfolio->get_portfolio_value(current_prices);
            equity_curve.emplace_back(timestamp, portfolio_value);

            // Log first few equity curve entries
            static int ec_count = 0;
            if (++ec_count <= 5) {
                auto time_t = std::chrono::system_clock::to_time_t(timestamp);
                std::tm tm = *std::gmtime(&time_t);
                char time_str[64];
                std::strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm);
                INFO("EQUITY_CURVE #" + std::to_string(ec_count) + ": timestamp=" + std::string(time_str) +
                     ", value=$" + std::to_string(portfolio_value));
            }

            // Debug: Log portfolio value periodically (every 10 periods)
            static int debug_counter = 0;
            if (++debug_counter % 10 == 0) {
                auto portfolio_positions = portfolio->get_portfolio_positions();
                DEBUG("Portfolio value at period " + std::to_string(debug_counter) + ": $" +
                      std::to_string(portfolio_value) +
                      ", positions: " + std::to_string(portfolio_positions.size()));

                // Log detailed position information
                for (const auto& [symbol, pos] : portfolio_positions) {
                    DEBUG(
                        "Position " + symbol +
                        ": qty=" + std::to_string(static_cast<double>(pos.quantity)) +
                        ", avg_price=" + std::to_string(static_cast<double>(pos.average_price)) +
                        ", realized_pnl=" + std::to_string(static_cast<double>(pos.realized_pnl)) +
                        ", unrealized_pnl=" +
                        std::to_string(static_cast<double>(pos.unrealized_pnl)));
                }
            }
        } catch (const std::exception& e) {
            WARN("Exception calculating portfolio value: " + std::string(e.what()) +
                 ". Using previous value for equity curve.");

            // Use previous value if available, otherwise use initial capital
            double last_value = equity_curve.empty()
                                    ? static_cast<double>(config_.portfolio_config.initial_capital)
                                    : equity_curve.back().second;

            equity_curve.emplace_back(timestamp, last_value);
        }

        // Get risk metrics
        if (config_.portfolio_config.use_risk_management && risk_manager_) {
            try {
                auto portfolio_positions = portfolio->get_portfolio_positions();

                if (!portfolio_positions.empty()) {
                    MarketData market_data = risk_manager_->create_market_data(bars);
                    // Convert map to unordered_map for risk manager compatibility
                    std::unordered_map<std::string, Position> positions_for_risk(portfolio_positions.begin(), portfolio_positions.end());
                    auto risk_result =
                        risk_manager_->process_positions(positions_for_risk, market_data);

                    if (risk_result.is_ok()) {
                        risk_metrics.push_back(risk_result.value());
                    }
                }
            } catch (const std::exception& e) {
                WARN("Exception calculating risk metrics: " + std::string(e.what()) +
                     ". Continuing without risk metrics for this period.");
            }
        }

        // Execution history already cleared after retrieval to prevent accumulation

        // Update previous_bars for next day
        previous_bars = bars;
        has_previous_bars = true;

        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::UNKNOWN_ERROR,
                                std::string("Error processing portfolio data: ") + e.what(),
                                "BacktestEngine");
    }
}

Result<std::vector<Bar>> BacktestEngine::load_market_data() const {
    try {
        INFO("Loading market data for backtest from " +
             std::to_string(config_.strategy_config.start_date.time_since_epoch().count()) +
             " to " + std::to_string(config_.strategy_config.end_date.time_since_epoch().count()));

        // Validate the database connection
        if (!db_) {
            ERROR("Database interface is null");
            return make_error<std::vector<Bar>>(ErrorCode::CONNECTION_ERROR,
                                                "Database interface is null", "BacktestEngine");
        }

        // Connect to the database if not already connected
        if (!db_->is_connected()) {
            auto connect_result = db_->connect();
            if (connect_result.is_error()) {
                ERROR("Failed to connect to database: " +
                      std::string(connect_result.error()->what()));
                return make_error<std::vector<Bar>>(
                    connect_result.error()->code(),
                    "Failed to connect to database: " + std::string(connect_result.error()->what()),
                    "BacktestEngine");
            }
        }

        // Validate symbols list
        if (config_.strategy_config.symbols.empty()) {
            ERROR("Empty symbols list provided for backtest");
            return make_error<std::vector<Bar>>(ErrorCode::INVALID_ARGUMENT,
                                                "Empty symbols list provided for backtest",
                                                "BacktestEngine");
        }

        // Load market data in batches
        std::vector<Bar> all_bars;
        constexpr size_t MAX_SYMBOLS_PER_BATCH = 5;

        for (size_t i = 0; i < config_.strategy_config.symbols.size(); i += MAX_SYMBOLS_PER_BATCH) {
            // Create a batch of symbols
            size_t end_idx =
                std::min(i + MAX_SYMBOLS_PER_BATCH, config_.strategy_config.symbols.size());
            std::vector<std::string> symbol_batch(
                config_.strategy_config.symbols.begin() + i,
                config_.strategy_config.symbols.begin() + end_idx);

            // Load this batch of data
            try {
                auto result = db_->get_market_data(
                    symbol_batch, config_.strategy_config.start_date,
                    config_.strategy_config.end_date, config_.strategy_config.asset_class,
                    config_.strategy_config.data_freq, config_.strategy_config.data_type);

                if (result.is_error()) {
                    WARN("Error loading data for symbols batch " + std::to_string(i) + "-" +
                         std::to_string(end_idx) + ": " + result.error()->what() +
                         ". Continuing with other batches.");
                    continue;
                }

                // Inspect data before conversion
                auto arrow_table = result.value();
                INFO("Loaded Arrow table with " + std::to_string(arrow_table->num_rows()) +
                     " rows and " + std::to_string(arrow_table->num_columns()) + " columns");

                if (arrow_table->num_rows() == 0) {
                    ERROR(
                        "Market data query returned an empty table - no data for the specified "
                        "date range");
                    return make_error<std::vector<Bar>>(ErrorCode::DATA_NOT_FOUND,
                                                        "Market data query returned an empty table "
                                                        "- no data for the specified date range",
                                                        "BacktestEngine");
                }

                // Convert Arrow table to Bars
                auto conversion_result = DataConversionUtils::arrow_table_to_bars(result.value());
                if (conversion_result.is_error()) {
                    ERROR("Failed to convert market data to bars: " +
                          std::string(conversion_result.error()->what()));
                    return make_error<std::vector<Bar>>(conversion_result.error()->code(),
                                                        conversion_result.error()->what(),
                                                        "BacktestEngine");
                }

                // Add these bars to our collection
                auto& batch_bars = conversion_result.value();
                if (batch_bars.empty()) {
                    ERROR("No market data loaded for symbols batch " + std::to_string(i) + "-" +
                          std::to_string(end_idx));
                    return make_error<std::vector<Bar>>(ErrorCode::MARKET_DATA_ERROR,
                                                        "No market data loaded for symbols batch " +
                                                            std::to_string(i) + "-" +
                                                            std::to_string(end_idx),
                                                        "BacktestEngine");
                }

                // Debug: Log first few bars to understand data ordering
                INFO("Loaded " + std::to_string(batch_bars.size()) + " bars for symbols batch " +
                     std::to_string(i) + "-" + std::to_string(end_idx));

                if (!batch_bars.empty()) {
                    DEBUG("First 3 bars from this batch:");
                    for (size_t j = 0; j < std::min(size_t(3), batch_bars.size()); ++j) {
                        const auto& bar = batch_bars[j];
                        DEBUG("  Bar " + std::to_string(j) + ": " + bar.symbol + " at " +
                              std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                                                 bar.timestamp.time_since_epoch())
                                                 .count()) +
                              " close=" + std::to_string(static_cast<double>(bar.close)));
                    }
                }

                all_bars.insert(all_bars.end(), batch_bars.begin(), batch_bars.end());
            } catch (const std::exception& e) {
                WARN("Exception loading data for symbols batch " + std::to_string(i) + "-" +
                     std::to_string(end_idx) + ": " + std::string(e.what()) +
                     ". Continuing with other batches.");
            }
        }

        // Check for empty data
        if (all_bars.empty()) {
            ERROR("No market data loaded for backtest");
            return make_error<std::vector<Bar>>(ErrorCode::MARKET_DATA_ERROR,
                                                "No market data loaded for backtest",
                                                "BacktestEngine");
        }

        // Verify data quality - check at least one symbol has price movement
        bool has_price_movement = false;
        std::unordered_map<std::string, double> min_prices, max_prices;

        for (const auto& bar : all_bars) {
            if (min_prices.find(bar.symbol) == min_prices.end()) {
                min_prices[bar.symbol] = static_cast<double>(bar.close);
                max_prices[bar.symbol] = static_cast<double>(bar.close);
            } else {
                min_prices[bar.symbol] =
                    std::min(min_prices[bar.symbol], static_cast<double>(bar.close));
                max_prices[bar.symbol] =
                    std::max(max_prices[bar.symbol], static_cast<double>(bar.close));
            }
        }

        for (const auto& [symbol, min_price] : min_prices) {
            double max_price = max_prices[symbol];
            double price_range_pct = (max_price - min_price) / min_price * 100.0;
            INFO("Symbol " + symbol + " price range: " + std::to_string(min_price) + " to " +
                 std::to_string(max_price) + " (" + std::to_string(price_range_pct) + "%)");

            if (price_range_pct > 1.0) {  // At least 1% price movement
                has_price_movement = true;
            }
        }

        if (!has_price_movement) {
            WARN(
                "No significant price movement detected in market data. Strategy may not generate "
                "signals.");
        }

        INFO("Loaded a total of " + std::to_string(all_bars.size()) + " bars for " +
             std::to_string(config_.strategy_config.symbols.size()) + " symbols");

        return Result<std::vector<Bar>>(all_bars);

    } catch (const std::exception& e) {
        ERROR("Unexpected error loading market data: " + std::string(e.what()));
        return make_error<std::vector<Bar>>(ErrorCode::UNKNOWN_ERROR,
                                            std::string("Error loading market data: ") + e.what(),
                                            "BacktestEngine");
    }
}

double BacktestEngine::calculate_transaction_costs(const ExecutionReport& execution) const {
    // Base commission
    double commission = static_cast<double>(execution.filled_quantity) *
                        static_cast<double>(config_.strategy_config.commission_rate);

    // Add market impact based on size (simplified model)
    double market_impact = static_cast<double>(execution.filled_quantity) *
                           static_cast<double>(execution.fill_price) * 0.0005;  // 5 basis points

    double fixed_cost = 1.0;  // Fixed cost per trade

    return commission + market_impact + fixed_cost;
}

double BacktestEngine::apply_slippage(double price, double quantity, Side side) const {
    // Apply basic slippage model
    double slip_factor = static_cast<double>(config_.strategy_config.slippage_model) /
                         10000.0;  // Convert bps to decimal

    if (side == Side::BUY) {
        return price * (1.0 + slip_factor);
    } else {
        return price * (1.0 - slip_factor);
    }
}

BacktestResults BacktestEngine::calculate_metrics(
    const std::vector<std::pair<Timestamp, double>>& equity_curve,
    const std::vector<ExecutionReport>& executions) const {
    BacktestResults results;

    if (equity_curve.empty())
        return results;

    // Debug: Log equity curve info
    DEBUG("Equity curve size: " + std::to_string(equity_curve.size()) + ", start value: $" +
          std::to_string(equity_curve.front().second) + ", end value: $" +
          std::to_string(equity_curve.back().second));

    // Calculate returns
    std::vector<double> returns;
    returns.reserve(equity_curve.size() - 1);

    for (size_t i = 1; i < equity_curve.size(); ++i) {
        double ret =
            (equity_curve[i].second - equity_curve[i - 1].second) / equity_curve[i - 1].second;
        returns.push_back(ret);
    }

    // Basic performance metrics
    results.total_return =
        (equity_curve.back().second - equity_curve.front().second) / equity_curve.front().second;

    // Calculate volatility
    double mean_return = std::accumulate(returns.begin(), returns.end(), 0.0) / returns.size();

    double sq_sum = std::inner_product(returns.begin(), returns.end(), returns.begin(), 0.0);
    results.volatility = std::sqrt(sq_sum / returns.size() - mean_return * mean_return) *
                         std::sqrt(252.0);  // Annualize

    // Calculate Sharpe ratio (assuming 0% risk-free)
    if (results.volatility > 0) {
        results.sharpe_ratio = (mean_return * 252.0) / results.volatility;
    }

    // Calculate Sortino ratio
    double downside_sum = 0.0;
    int downside_count = 0;
    for (double ret : returns) {
        if (ret < 0) {
            downside_sum += ret * ret;
            downside_count++;
        }
    }

    if (downside_count > 0) {
        double downside_dev = std::sqrt(downside_sum / downside_count) * std::sqrt(252.0);
        results.sortino_ratio = (mean_return * 252.0) / downside_dev;
    } else {
        // No negative returns means infinite Sortino ratio, but cap it at a reasonable value
        results.sortino_ratio = (mean_return * 252.0) >= 0 ? 999.0 : 0.0;
    }

    // Trading metrics - track positions to calculate proper P&L
    std::unordered_map<std::string, double> positions;   // symbol -> net position
    std::unordered_map<std::string, double> avg_prices;  // symbol -> average entry price
    std::vector<double> trade_pnls;                      // individual trade P&Ls
    std::vector<ExecutionReport> actual_trades;          // Only actual trades that close positions

    double total_profit = 0.0;
    double total_loss = 0.0;
    int winning_trades = 0;

    // Debug: Log first few executions to understand the data
    if (!executions.empty()) {
        DEBUG("Processing " + std::to_string(executions.size()) + " executions for trade metrics");
        for (size_t i = 0; i < std::min(size_t(5), executions.size()); ++i) {
            const auto& exec = executions[i];
            std::string side_str = (exec.side == Side::BUY) ? "BUY" : "SELL";
            DEBUG("Execution " + std::to_string(i) + ": " + exec.symbol + " " + side_str + 
                  " qty=" + std::to_string(static_cast<double>(exec.filled_quantity)) +
                  " price=" + std::to_string(static_cast<double>(exec.fill_price)));
        }
    }

    for (const auto& exec : executions) {
        const std::string& symbol = exec.symbol;
        double fill_price = static_cast<double>(exec.fill_price);
        double quantity = static_cast<double>(exec.filled_quantity);
        double commission = static_cast<double>(exec.commission);

        // Adjust quantity based on side (BUY = positive, SELL = negative)
        double signed_qty = (exec.side == Side::BUY) ? quantity : -quantity;

        double current_pos = positions[symbol];
        double trade_pnl = -commission;  // Start with commission cost

        if (current_pos == 0.0) {
            // Opening new position
            positions[symbol] = signed_qty;
            avg_prices[symbol] = fill_price;
        } else if ((current_pos > 0 && signed_qty > 0) || (current_pos < 0 && signed_qty < 0)) {
            // Adding to existing position - calculate new average price
            double total_value = current_pos * avg_prices[symbol] + signed_qty * fill_price;
            positions[symbol] = current_pos + signed_qty;
            if (positions[symbol] != 0.0) {
                avg_prices[symbol] = total_value / positions[symbol];
            }
        } else {
            // Reducing or closing position - realize P&L
            double close_qty = std::min(std::abs(signed_qty), std::abs(current_pos));
            trade_pnl +=
                close_qty * (fill_price - avg_prices[symbol]) * (current_pos > 0 ? 1.0 : -1.0);

            positions[symbol] = current_pos + signed_qty;
            // Keep same average price for remaining position
        }

        // Count all trades that close positions (have realized P&L)
        // A trade closes a position when:
        // 1. We have an existing position (current_pos != 0)
        // 2. The execution is in the opposite direction (reducing or closing the position)
        bool is_closing_trade = std::abs(signed_qty) > 1e-6 && current_pos != 0.0 &&
            ((current_pos > 0 && signed_qty < 0) || (current_pos < 0 && signed_qty > 0));
        
        if (is_closing_trade) {
            // Count all position-closing trades, regardless of P&L magnitude
            // This includes partial closes and full closes
            trade_pnls.push_back(trade_pnl);
            // Add to actual_trades for database logging (only position-closing trades)
            actual_trades.push_back(exec);
            if (trade_pnl > 0) {
                total_profit += trade_pnl;
                winning_trades++;
                results.max_win = std::max(results.max_win, trade_pnl);
            } else {
                total_loss -= trade_pnl;  // total_loss is positive
                results.max_loss = std::max(results.max_loss, -trade_pnl);
            }
        }
    }

    results.total_trades = static_cast<int>(trade_pnls.size());
    results.actual_trades = actual_trades;  // Save only the actual trades

    // Debug logging for trade metrics
    DEBUG("Trade metrics calculation: total_trades=" + std::to_string(results.total_trades) +
          ", winning_trades=" + std::to_string(winning_trades) +
          ", total_profit=" + std::to_string(total_profit) +
          ", total_loss=" + std::to_string(total_loss) +
          ", executions_count=" + std::to_string(executions.size()));

    if (results.total_trades > 0) {
        results.win_rate = static_cast<double>(winning_trades) / results.total_trades;
        results.avg_win = winning_trades > 0 ? total_profit / winning_trades : 0.0;
        results.avg_loss = (results.total_trades - winning_trades) > 0
                               ? total_loss / (results.total_trades - winning_trades)
                               : 0.0;
    } else {
        // If no closing trades, log a warning
        WARN("No position-closing trades found in " + std::to_string(executions.size()) + 
             " executions. All trades may be position-opening or position-increasing.");
    }

    if (total_loss > 0) {
        results.profit_factor = total_profit / total_loss;
    } else if (results.total_trades > 0 && total_profit > 0) {
        // If we have trades but no losses, profit factor is infinite (cap at reasonable value)
        results.profit_factor = 999.0;
    }

    // Calculate drawdown metrics
    auto drawdowns = calculate_drawdowns(equity_curve);
    if (!drawdowns.empty()) {
        results.max_drawdown =
            std::max_element(drawdowns.begin(), drawdowns.end(), [](const auto& a, const auto& b) {
                return a.second < b.second;
            })->second;
        results.drawdown_curve = drawdowns;
    } else {
        results.max_drawdown = 0.0;
    }

    // Calculate Calmar ratio
    if (results.max_drawdown > 0) {
        results.calmar_ratio = results.total_return / results.max_drawdown;
    }

    // Calculate risk metrics
    auto risk_metrics = calculate_risk_metrics(returns);
    results.var_95 = risk_metrics["var_95"];
    results.cvar_95 = risk_metrics["cvar_95"];
    results.downside_volatility = risk_metrics["downside_volatility"];

    // Calculate beta and correlation (simplified - using returns vs themselves as benchmark for now)
    // In a full implementation, you'd compare against market benchmark returns
    if (returns.size() > 1) {
        // For now, calculate correlation between consecutive periods
        double covariance = 0.0;
        double variance_benchmark = 0.0;
        double variance_strategy = 0.0;
        
        for (size_t i = 1; i < returns.size(); ++i) {
            double prev_return = returns[i-1];
            double curr_return = returns[i];
            covariance += (prev_return - mean_return) * (curr_return - mean_return);
            variance_benchmark += (prev_return - mean_return) * (prev_return - mean_return);
            variance_strategy += (curr_return - mean_return) * (curr_return - mean_return);
        }
        
        if (variance_benchmark > 0) {
            results.beta = covariance / variance_benchmark;
            results.correlation = covariance / std::sqrt(variance_benchmark * variance_strategy);
        }
    }

    // Calculate average holding period from trade executions
    if (!executions.empty()) {
        std::map<std::string, Timestamp> open_times;  // symbol -> first trade time
        std::vector<double> holding_periods;
        
        for (const auto& exec : executions) {
            const std::string& symbol = exec.symbol;
            
            if (open_times.find(symbol) == open_times.end()) {
                // First trade for this symbol - opening position
                open_times[symbol] = exec.fill_time;
            } else {
                // Subsequent trade - could be closing position
                auto duration = std::chrono::duration_cast<std::chrono::hours>(
                    exec.fill_time - open_times[symbol]);
                double hours = static_cast<double>(duration.count());
                if (hours > 0) {
                    holding_periods.push_back(hours / 24.0);  // Convert to days
                    open_times[symbol] = exec.fill_time;  // Reset for next position
                }
            }
        }
        
        if (!holding_periods.empty()) {
            results.avg_holding_period = std::accumulate(holding_periods.begin(), 
                                                       holding_periods.end(), 0.0) / holding_periods.size();
        }
    }

    // Calculate monthly returns
    std::map<std::string, double> monthly_returns_map;
    for (size_t i = 1; i < equity_curve.size(); ++i) {
        auto time_t = std::chrono::system_clock::to_time_t(equity_curve[i].first);
        std::tm tm;
        trade_ngin::core::safe_localtime(&time_t, &tm);

        std::ostringstream month_key;
        month_key << std::setw(4) << (tm.tm_year + 1900) << "-" << std::setw(2) << std::setfill('0')
                  << (tm.tm_mon + 1);

        double period_return =
            (equity_curve[i].second - equity_curve[i - 1].second) / equity_curve[i - 1].second;

        monthly_returns_map[month_key.str()] += period_return;
    }

    for (const auto& [month, ret] : monthly_returns_map) {
        results.monthly_returns[month] = ret;
    }

    // Calculate per-symbol P&L using the same position tracking logic
    std::unordered_map<std::string, double> symbol_positions;   // symbol -> net position
    std::unordered_map<std::string, double> symbol_avg_prices;  // symbol -> average entry price
    std::map<std::string, double> symbol_pnl_map;

    for (const auto& exec : executions) {
        const std::string& symbol = exec.symbol;
        double fill_price = static_cast<double>(exec.fill_price);
        double quantity = static_cast<double>(exec.filled_quantity);
        double commission = static_cast<double>(exec.commission);

        // Adjust quantity based on side (BUY = positive, SELL = negative)
        double signed_qty = (exec.side == Side::BUY) ? quantity : -quantity;

        double current_pos = symbol_positions[symbol];
        double trade_pnl = -commission;  // Start with commission cost

        if (current_pos == 0.0) {
            // Opening new position
            symbol_positions[symbol] = signed_qty;
            symbol_avg_prices[symbol] = fill_price;
        } else if ((current_pos > 0 && signed_qty > 0) || (current_pos < 0 && signed_qty < 0)) {
            // Adding to existing position - calculate new average price
            double total_value = current_pos * symbol_avg_prices[symbol] + signed_qty * fill_price;
            symbol_positions[symbol] = current_pos + signed_qty;
            if (symbol_positions[symbol] != 0.0) {
                symbol_avg_prices[symbol] = total_value / symbol_positions[symbol];
            }
        } else {
            // Reducing or closing position - realize P&L
            double close_qty = std::min(std::abs(signed_qty), std::abs(current_pos));
            trade_pnl += close_qty * (fill_price - symbol_avg_prices[symbol]) *
                         (current_pos > 0 ? 1.0 : -1.0);

            symbol_positions[symbol] = current_pos + signed_qty;
            // Keep same average price for remaining position
        }

        symbol_pnl_map[symbol] += trade_pnl;
    }

    for (const auto& [symbol, pnl] : symbol_pnl_map) {
        results.symbol_pnl[symbol] = pnl;
    }

    return results;
}

std::vector<std::pair<Timestamp, double>> BacktestEngine::calculate_drawdowns(
    const std::vector<std::pair<Timestamp, double>>& equity_curve) const {
    std::vector<std::pair<Timestamp, double>> drawdowns;
    drawdowns.reserve(equity_curve.size());

    if (equity_curve.empty())
        return drawdowns;

    double peak = equity_curve[0].second;

    for (const auto& [timestamp, equity] : equity_curve) {
        peak = std::max(peak, equity);
        double drawdown = equity < peak ? (peak - equity) / peak : 0.0;
        drawdowns.emplace_back(timestamp, drawdown);
    }

    return drawdowns;
}

std::unordered_map<std::string, double> BacktestEngine::calculate_risk_metrics(
    const std::vector<double>& returns) const {
    std::unordered_map<std::string, double> metrics;

    if (returns.empty())
        return metrics;

    // Sort returns for percentile calculations
    std::vector<double> sorted_returns = returns;
    std::sort(sorted_returns.begin(), sorted_returns.end());

    // Calculate VaR 95%
    size_t var_index = static_cast<size_t>(returns.size() * 0.05);
    metrics["var_95"] = -sorted_returns[var_index];

    // Calculate CVaR 95%
    double cvar_sum = 0.0;
    for (size_t i = 0; i < var_index; ++i) {
        cvar_sum += sorted_returns[i];
    }
    metrics["cvar_95"] = -cvar_sum / var_index;

    // Calculate downside volatility
    double downside_sum = 0.0;
    int downside_count = 0;

    for (double ret : returns) {
        if (ret < 0) {
            downside_sum += ret * ret;
            downside_count++;
        }
    }

    metrics["downside_volatility"] =
        downside_count > 0 ? std::sqrt(downside_sum / downside_count) * std::sqrt(252.0) : 0.0;

    return metrics;
}

Result<void> BacktestEngine::save_results_to_db(const BacktestResults& results,
                                                const std::string& strategy_id,
                                                const std::string& run_id) const {
    if (!config_.store_trade_details) {
        return Result<void>();
    }

    // Use the new BacktestResultsManager for storage (Phase 1 refactoring)
    INFO("Using BacktestResultsManager for storage");

    auto db_ptr = std::dynamic_pointer_cast<PostgresDatabase>(db_);
    if (!db_ptr) {
        ERROR("Database is not a PostgresDatabase instance");
        return make_error<void>(ErrorCode::DATABASE_ERROR,
                               "Invalid database type", "BacktestEngine");
    }

    // Create and configure the results manager with the provided strategy ID
    auto results_manager = std::make_unique<BacktestResultsManager>(
        db_ptr,
        config_.store_trade_details,
        strategy_id
    );

    // Set metadata with full config structure (matching previous format)
    // Use the config's to_json() method to get the complete structure
    nlohmann::json hyperparameters = config_.to_json();
    hyperparameters["version"] = config_.version;
    
    results_manager->set_metadata(
        config_.strategy_config.start_date,
        config_.strategy_config.end_date,
        hyperparameters,
        "Backtest Run: " + strategy_id,
        "Automated backtest run for strategy: " + strategy_id
    );

    // Convert performance metrics
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
        {"downside_volatility", results.downside_volatility}
    };
    results_manager->set_performance_metrics(metrics);

    // Set equity curve
    std::vector<std::pair<Timestamp, double>> equity_points;
    for (const auto& [timestamp, equity] : results.equity_curve) {
        equity_points.push_back({timestamp, equity});
    }
    results_manager->set_equity_curve(equity_points);

    // Set final positions
    results_manager->set_final_positions(results.positions);

    // Set executions
    results_manager->set_executions(results.executions);

    // Generate run_id if not provided
    std::string actual_run_id = run_id.empty()
        ? BacktestResultsManager::generate_run_id(strategy_id)
        : run_id;

    // Save all results
    auto save_result = results_manager->save_all_results(actual_run_id,
                                                        config_.strategy_config.end_date);

    if (save_result.is_error()) {
        ERROR("Failed to save results using BacktestResultsManager: " +
              std::string(save_result.error()->what()));
        return save_result;
    }

    INFO("Successfully saved backtest results using new storage manager");
    return Result<void>();
}

Result<void> BacktestEngine::save_portfolio_results_to_db(
    const BacktestResults& results,
    const std::vector<std::string>& strategy_names,
    const std::unordered_map<std::string, double>& strategy_allocations,
    std::shared_ptr<PortfolioManager> portfolio,
    const nlohmann::json& portfolio_config) const {
    
    if (!config_.store_trade_details) {
        return Result<void>();
    }

    INFO("Using BacktestResultsManager for portfolio-level storage");

    auto db_ptr = std::dynamic_pointer_cast<PostgresDatabase>(db_);
    if (!db_ptr) {
        ERROR("Database is not a PostgresDatabase instance");
        return make_error<void>(ErrorCode::DATABASE_ERROR,
                               "Invalid database type", "BacktestEngine");
    }

    // Use the run_id from daily position storage if available, otherwise generate a new one
    std::string portfolio_run_id;
    if (!current_run_id_.empty()) {
        portfolio_run_id = current_run_id_;
        INFO("Using run_id from daily position storage: " + portfolio_run_id);
    } else {
        // Fallback: Generate portfolio run_id (combined strategy names)
        portfolio_run_id = RunIdGenerator::generate_portfolio_run_id(
            strategy_names, config_.strategy_config.end_date);
        INFO("Generated new portfolio run_id: " + portfolio_run_id);
    }

    // Create results manager for portfolio-level storage
    // Use portfolio_run_id as the strategy_id for the manager
    auto results_manager = std::make_unique<BacktestResultsManager>(
        db_ptr,
        config_.store_trade_details,
        portfolio_run_id  // Use portfolio run_id as identifier
    );

    // Set metadata with full config structure
    nlohmann::json hyperparameters = config_.to_json();
    hyperparameters["version"] = config_.version;
    
    results_manager->set_metadata(
        config_.strategy_config.start_date,
        config_.strategy_config.end_date,
        hyperparameters,
        "Portfolio Backtest Run: " + portfolio_run_id,
        "Multi-strategy portfolio backtest"
    );

    // Convert performance metrics (portfolio-level)
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
        {"downside_volatility", results.downside_volatility}
    };
    results_manager->set_performance_metrics(metrics);

    // Set portfolio-level equity curve
    std::vector<std::pair<Timestamp, double>> equity_points;
    for (const auto& [timestamp, equity] : results.equity_curve) {
        equity_points.push_back({timestamp, equity});
    }
    results_manager->set_equity_curve(equity_points);

    // Collect per-strategy positions and executions
    // NOTE: We skip saving final positions here because positions are already saved daily
    // during the backtest run. The last day's positions are already in the database.
    if (portfolio) {
        // Get per-strategy executions from PortfolioManager
        auto strategy_executions_map = portfolio->get_strategy_executions();
        
        // Process each strategy - only save executions, not positions (positions saved daily)
        for (const auto& [strategy_id, executions] : strategy_executions_map) {
            results_manager->set_strategy_executions(strategy_id, executions);
        }
        
        // Don't set strategy positions - they're already saved daily
        // If we need final positions separately, we can add a flag to control this
        INFO("Skipping final positions save - positions already saved daily during backtest");
    }

    // Don't save portfolio-level executions - we save per-strategy executions instead
    // (Portfolio-level executions are aggregated and don't have strategy_id attribution)
    // results_manager->set_executions(results.executions);  // Skip for portfolio runs

    // Save portfolio-level results (summary, equity curve)
    // Note: save_all_results will skip executions since we didn't set them
    auto save_result = results_manager->save_all_results(portfolio_run_id,
                                                          config_.strategy_config.end_date);

    if (save_result.is_error()) {
        ERROR("Failed to save portfolio results: " + std::string(save_result.error()->what()));
        return save_result;
    }

    // Skip saving per-strategy positions - positions are already saved daily during backtest
    // The last day's positions are already in the database, so no need to save again
    INFO("Skipping save_strategy_positions - positions already saved daily during backtest run");

    // Save per-strategy executions (Approach B - multiple rows)
    // Executions are now tracked per strategy in PortfolioManager
    auto executions_result = results_manager->save_strategy_executions(portfolio_run_id);
    if (executions_result.is_error()) {
        WARN("Failed to save strategy executions: " + std::string(executions_result.error()->what()));
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

Result<void> BacktestEngine::save_results_to_csv(const BacktestResults& results,
                                                 const std::string& run_id) const {
    if (!config_.store_trade_details) {
        return Result<void>();
    }

    try {
        std::string actual_run_id =
            run_id.empty()
                ? "BT_" +
                      std::to_string(std::chrono::system_clock::now().time_since_epoch().count())
                : run_id;

        INFO("Saving backtest results to CSV with ID: " + actual_run_id);

        if (config_.csv_output_path.empty()) {
            return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                    "CSV output path not specified in configuration",
                                    "BacktestEngine");
        }

        // Create directory if it doesn't exist
        std::filesystem::path output_dir =
            std::filesystem::path(config_.csv_output_path) / actual_run_id;
        std::filesystem::create_directories(output_dir);

        // Save main results
        {
            std::ofstream results_file(output_dir / "results.csv");
            if (!results_file.is_open()) {
                return make_error<void>(ErrorCode::CONVERSION_ERROR,
                                        "Failed to open results CSV file for writing",
                                        "BacktestEngine");
            }

            // Write header
            results_file
                << "run_id,start_date,end_date,total_return,sharpe_ratio,sortino_ratio,max_"
                   "drawdown,"
                << "calmar_ratio,volatility,total_trades,win_rate,profit_factor,avg_win,avg_loss,"
                << "max_win,max_loss,avg_holding_period,var_95,cvar_95,beta,correlation,"
                << "downside_volatility,config\n";

            // Format the configuration as JSON for storage
            std::string config_json =
                "{\"initial_capital\": " +
                std::to_string(static_cast<double>(config_.portfolio_config.initial_capital)) +
                ", \"symbols\": [";

            for (size_t i = 0; i < config_.strategy_config.symbols.size(); ++i) {
                if (i > 0)
                    config_json += ", ";
                config_json += "\"" + config_.strategy_config.symbols[i] + "\"";
            }

            config_json += "]}";

            // Escape any commas in JSON
            std::string escaped_config = config_json;
            // Replace any double quotes with escaped double quotes
            size_t pos = 0;
            while ((pos = escaped_config.find("\"", pos)) != std::string::npos) {
                escaped_config.replace(pos, 1, "\"\"");
                pos += 2;
            }

            // Format dates as ISO 8601 strings
            auto to_iso_string = [](const std::chrono::system_clock::time_point& tp) {
                auto time_t = std::chrono::system_clock::to_time_t(tp);
                std::stringstream ss;
                ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%SZ");
                return ss.str();
            };

            // Write data row
            results_file << actual_run_id << ","
                         << to_iso_string(config_.strategy_config.start_date) << ","
                         << to_iso_string(config_.strategy_config.end_date) << ","
                         << results.total_return << "," << results.sharpe_ratio << ","
                         << results.sortino_ratio << "," << results.max_drawdown << ","
                         << results.calmar_ratio << "," << results.volatility << ","
                         << results.total_trades << "," << results.win_rate << ","
                         << results.profit_factor << "," << results.avg_win << ","
                         << results.avg_loss << "," << results.max_win << "," << results.max_loss
                         << "," << results.avg_holding_period << "," << results.var_95 << ","
                         << results.cvar_95 << "," << results.beta << "," << results.correlation
                         << "," << results.downside_volatility << ","
                         << "\"" << escaped_config << "\"" << "\n";
        }

        // Save equity curve if enabled
        if (config_.store_trade_details && !results.equity_curve.empty()) {
            std::ofstream equity_file(output_dir / "equity_curve.csv");
            if (!equity_file.is_open()) {
                WARN("Failed to open equity curve CSV file for writing");
            } else {
                // Write header
                equity_file << "run_id,timestamp,equity\n";

                // Write data rows
                for (const auto& [timestamp, equity] : results.equity_curve) {
                    auto time_t = std::chrono::system_clock::to_time_t(timestamp);
                    std::stringstream time_ss;
                    time_ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%SZ");

                    equity_file << actual_run_id << "," << time_ss.str() << "," << equity << "\n";
                }
            }

            // Save trade executions
            if (!results.executions.empty()) {
                std::ofstream executions_file(output_dir / "trade_executions.csv");
                if (!executions_file.is_open()) {
                    WARN("Failed to open trade executions CSV file for writing");
                } else {
                    // Write header
                    executions_file
                        << "run_id,execution_id,timestamp,symbol,side,quantity,price,commission\n";

                    // Write data rows
                    for (const auto& exec : results.executions) {
                        auto time_t = std::chrono::system_clock::to_time_t(exec.fill_time);
                        std::stringstream time_ss;
                        time_ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%SZ");

                        std::string side_str = exec.side == Side::BUY ? "BUY" : "SELL";

                        executions_file << actual_run_id << "," << exec.exec_id << ","
                                        << time_ss.str() << "," << exec.symbol << "," << side_str
                                        << "," << exec.filled_quantity << "," << exec.fill_price
                                        << "," << exec.commission << "\n";
                    }
                }
            }

            // Save final positions
            if (!results.positions.empty()) {
                std::ofstream positions_file(output_dir / "final_positions.csv");
                if (!positions_file.is_open()) {
                    WARN("Failed to open final positions CSV file for writing");
                } else {
                    // Write header
                    positions_file
                        << "run_id,symbol,quantity,average_price,unrealized_pnl,realized_pnl\n";

                    // Write data rows
                    for (const auto& pos : results.positions) {
                        // Don't skip zero-quantity positions if they have realized PnL
                        bool has_realized_pnl = std::abs(static_cast<double>(pos.realized_pnl)) > 1e-6;
                        bool has_quantity = std::abs(static_cast<double>(pos.quantity)) >= 1e-6;
                        
                        if (!has_quantity && !has_realized_pnl)
                            continue;  // Skip only truly empty positions with no PnL

                        positions_file << actual_run_id << "," << pos.symbol << "," << pos.quantity
                                       << "," << pos.average_price << "," << pos.unrealized_pnl
                                       << "," << pos.realized_pnl << "\n";
                    }
                }
            }

            // Save monthly returns
            if (!results.monthly_returns.empty()) {
                std::ofstream monthly_file(output_dir / "monthly_returns.csv");
                if (!monthly_file.is_open()) {
                    WARN("Failed to open monthly returns CSV file for writing");
                } else {
                    // Write header
                    monthly_file << "run_id,month,return\n";

                    // Write data rows
                    for (const auto& [month, ret] : results.monthly_returns) {
                        monthly_file << actual_run_id << "," << month << "," << ret << "\n";
                    }
                }
            }

            // Save symbol P&L
            if (!results.symbol_pnl.empty()) {
                std::ofstream symbol_pnl_file(output_dir / "symbol_pnl.csv");
                if (!symbol_pnl_file.is_open()) {
                    WARN("Failed to open symbol P&L CSV file for writing");
                } else {
                    // Write header
                    symbol_pnl_file << "run_id,symbol,pnl\n";

                    // Write data rows
                    for (const auto& [symbol, pnl] : results.symbol_pnl) {
                        symbol_pnl_file << actual_run_id << "," << symbol << "," << pnl << "\n";
                    }
                }
            }
        }

        INFO("Successfully saved backtest results to CSV files in: " + output_dir.string());
        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::CONVERSION_ERROR,
                                std::string("Error saving backtest results to CSV: ") + e.what(),
                                "BacktestEngine");
    }
}

// Helper for timestamp formatting
std::string BacktestEngine::format_timestamp(const Timestamp& ts) const {
    auto time_t = std::chrono::system_clock::to_time_t(ts);
    std::stringstream ss;
    std::tm tm;
    trade_ngin::core::safe_localtime(&time_t, &tm);
    ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

Result<BacktestResults> BacktestEngine::load_results(const std::string& run_id) const {
    try {
        // Query main results
        std::string query = "SELECT * FROM " + config_.results_db_schema +
                            ".results "
                            "WHERE run_id = $1";

        auto result = db_->execute_query(query);
        if (result.is_error()) {
            return make_error<BacktestResults>(result.error()->code(), result.error()->what(),
                                               "BacktestEngine");
        }

        // Get Arrow table result
        auto table = result.value();
        if (table->num_rows() == 0) {
            return make_error<BacktestResults>(ErrorCode::DATA_NOT_FOUND,
                                               "No results found for run_id: " + run_id,
                                               "BacktestEngine");
        }

        // Initialize results
        BacktestResults results;

        // Extract scalar fields from first row
        auto numeric_arrays = {std::make_pair("total_return", &results.total_return),
                               std::make_pair("sharpe_ratio", &results.sharpe_ratio),
                               std::make_pair("sortino_ratio", &results.sortino_ratio),
                               std::make_pair("max_drawdown", &results.max_drawdown),
                               std::make_pair("calmar_ratio", &results.calmar_ratio),
                               std::make_pair("volatility", &results.volatility),
                               std::make_pair("win_rate", &results.win_rate),
                               std::make_pair("profit_factor", &results.profit_factor),
                               std::make_pair("avg_win", &results.avg_win),
                               std::make_pair("avg_loss", &results.avg_loss),
                               std::make_pair("max_win", &results.max_win),
                               std::make_pair("max_loss", &results.max_loss),
                               std::make_pair("var_95", &results.var_95),
                               std::make_pair("cvar_95", &results.cvar_95),
                               std::make_pair("beta", &results.beta),
                               std::make_pair("correlation", &results.correlation),
                               std::make_pair("downside_volatility", &results.downside_volatility)};

        for (const auto& [field_name, value_ptr] : numeric_arrays) {
            auto column = table->GetColumnByName(field_name);
            if (column && column->num_chunks() > 0) {
                auto array = std::static_pointer_cast<arrow::DoubleArray>(column->chunk(0));
                if (!array->IsNull(0)) {
                    *value_ptr = array->Value(0);
                }
            }
        }

        // Extract integer fields
        auto int_arrays = {std::make_pair("total_trades", &results.total_trades)};

        for (const auto& [field_name, value_ptr] : int_arrays) {
            auto column = table->GetColumnByName(field_name);
            if (column && column->num_chunks() > 0) {
                auto array = std::static_pointer_cast<arrow::Int32Array>(column->chunk(0));
                if (!array->IsNull(0)) {
                    *value_ptr = array->Value(0);
                }
            }
        }

        // Load equity curve if available
        if (config_.store_trade_details) {
            query = "SELECT timestamp, equity FROM " + config_.results_db_schema +
                    ".equity_curve "
                    "WHERE run_id = $1 "
                    "ORDER BY timestamp";

            auto curve_result = db_->execute_query(query);
            if (curve_result.is_ok()) {
                auto curve_table = curve_result.value();
                auto timestamp_col = curve_table->GetColumnByName("timestamp");
                auto equity_col = curve_table->GetColumnByName("equity");

                if (timestamp_col && equity_col && timestamp_col->num_chunks() > 0 &&
                    equity_col->num_chunks() > 0) {
                    auto timestamps =
                        std::static_pointer_cast<arrow::TimestampArray>(timestamp_col->chunk(0));
                    auto equity_values =
                        std::static_pointer_cast<arrow::DoubleArray>(equity_col->chunk(0));

                    results.equity_curve.reserve(timestamps->length());
                    for (int64_t i = 0; i < timestamps->length(); ++i) {
                        if (!timestamps->IsNull(i) && !equity_values->IsNull(i)) {
                            results.equity_curve.emplace_back(
                                std::chrono::system_clock::time_point(
                                    std::chrono::seconds(timestamps->Value(i))),
                                equity_values->Value(i));
                        }
                    }
                }
            }

            // Load trade executions
            query = "SELECT * FROM " + config_.results_db_schema +
                    ".trade_executions "
                    "WHERE run_id = $1 "
                    "ORDER BY timestamp";

            auto exec_result = db_->execute_query(query);
            if (exec_result.is_ok()) {
                auto exec_table = exec_result.value();

                // Extract execution data into results.executions vector
                auto extract_executions = [&results](const std::shared_ptr<arrow::Table>& table) {
                    auto symbol_col = table->GetColumnByName("symbol");
                    auto side_col = table->GetColumnByName("side");
                    auto qty_col = table->GetColumnByName("quantity");
                    auto price_col = table->GetColumnByName("price");
                    auto time_col = table->GetColumnByName("timestamp");

                    if (!symbol_col || !side_col || !qty_col || !price_col || !time_col) {
                        return;
                    }

                    auto symbols =
                        std::static_pointer_cast<arrow::StringArray>(symbol_col->chunk(0));
                    auto sides = std::static_pointer_cast<arrow::StringArray>(side_col->chunk(0));
                    auto quantities =
                        std::static_pointer_cast<arrow::DoubleArray>(qty_col->chunk(0));
                    auto prices = std::static_pointer_cast<arrow::DoubleArray>(price_col->chunk(0));
                    auto timestamps =
                        std::static_pointer_cast<arrow::TimestampArray>(time_col->chunk(0));

                    for (int64_t i = 0; i < table->num_rows(); ++i) {
                        if (!symbols->IsNull(i) && !sides->IsNull(i) && !quantities->IsNull(i) &&
                            !prices->IsNull(i) && !timestamps->IsNull(i)) {
                            ExecutionReport exec;
                            exec.symbol = symbols->GetString(i);
                            exec.side = sides->GetString(i) == "BUY" ? Side::BUY : Side::SELL;
                            exec.filled_quantity = quantities->Value(i);
                            exec.fill_price = prices->Value(i);
                            exec.fill_time = std::chrono::system_clock::time_point(
                                std::chrono::seconds(timestamps->Value(i)));

                            results.executions.push_back(exec);
                        }
                    }
                };

                extract_executions(exec_table);
            }

            // Calculate drawdown curve from equity curve
            results.drawdown_curve = calculate_drawdowns(results.equity_curve);
        }

        return Result<BacktestResults>(results);

    } catch (const std::exception& e) {
        return make_error<BacktestResults>(
            ErrorCode::DATABASE_ERROR, std::string("Error loading backtest results: ") + e.what(),
            "BacktestEngine");
    }
}

Result<std::unordered_map<std::string, double>> BacktestEngine::compare_results(
    const std::vector<BacktestResults>& results) {
    std::unordered_map<std::string, double> comparison;

    if (results.empty()) {
        return Result<std::unordered_map<std::string, double>>(comparison);
    }

    // Calculate comparative metrics
    double avg_return = 0.0;
    double avg_sharpe = 0.0;
    double best_return = results[0].total_return;
    double worst_return = results[0].total_return;

    for (const auto& result : results) {
        avg_return += result.total_return;
        avg_sharpe += result.sharpe_ratio;
        best_return = std::max(best_return, result.total_return);
        worst_return = std::min(worst_return, result.total_return);
    }

    comparison["average_return"] = avg_return / results.size();
    comparison["average_sharpe"] = avg_sharpe / results.size();
    comparison["best_return"] = best_return;
    comparison["worst_return"] = worst_return;
    comparison["return_range"] = best_return - worst_return;

    // Calculate consistency metrics
    double return_variance = 0.0;
    for (const auto& result : results) {
        double diff = result.total_return - comparison["average_return"];
        return_variance += diff * diff;
    }
    comparison["return_stddev"] = std::sqrt(return_variance / results.size());

    return Result<std::unordered_map<std::string, double>>(comparison);
}

}  // namespace backtest
}  // namespace trade_ngin