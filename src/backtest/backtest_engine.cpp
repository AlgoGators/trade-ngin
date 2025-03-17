// src/backtest/backtest_engine.cpp
#include "trade_ngin/backtest/backtest_engine.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/state_manager.hpp"
#include "trade_ngin/data/database_interface.hpp"
#include "trade_ngin/data/conversion_utils.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include <algorithm>
#include <numeric>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <chrono>

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
} // anonymous namespace

namespace trade_ngin {
namespace backtest {

BacktestEngine::BacktestEngine(
    BacktestConfig config,
    std::shared_ptr<PostgresDatabase> db)
    : config_(std::move(config))
    , db_(std::move(db)) {

    // Register component with state manager
    ComponentInfo info{
        ComponentType::BACKTEST_ENGINE, 
        ComponentState::INITIALIZED,
        "BACKTEST_ENGINE",
        "",
        std::chrono::system_clock::now(),
        {{"total_capital", config_.portfolio_config.initial_capital}}
    };

    auto register_result = StateManager::instance().register_component(info);
    if (register_result.is_error()) {
        ERROR("Failed to register backtest engine with state manager: " + 
              std::string(register_result.error()->what()));
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
    if (config_.strategy_config.slippage_model > 0.0) {
        SpreadSlippageConfig slippage_config;
        slippage_config.min_spread_bps = config_.strategy_config.slippage_model;
        slippage_config.spread_multiplier = 1.2;
        slippage_config.market_impact_multiplier = 1.5;
        
        slippage_model_ = SlippageModelFactory::create_spread_model(slippage_config);
    }

    INFO("Backtest engine initialized successfully with " + 
         std::to_string(config_.strategy_config.symbols.size()) + " symbols and " +
         std::to_string(config_.portfolio_config.initial_capital) + " initial capital");
}

// Backward compatibility method - run single strategy with portfolio-level constraints
Result<BacktestResults> BacktestEngine::run(
    std::shared_ptr<StrategyInterface> strategy) {

    // Update state to running
    auto state_result = StateManager::instance().update_state(
        "BACKTEST_ENGINE", 
        ComponentState::RUNNING
    );
    
    if (state_result.is_error()) {
        ERROR("Failed to update backtest engine state: " + 
              std::string(state_result.error()->what()));
    }
    
    try {
        // Load historical market data
        auto data_result = load_market_data();
        if (data_result.is_error()) {
            ERROR("Failed to load market data: " + 
                  std::string(data_result.error()->what()));
            
            StateManager::instance().update_state(
                "BACKTEST_ENGINE", 
                ComponentState::ERR_STATE,
                data_result.error()->what()
            );
            
            return make_error<BacktestResults>(
                data_result.error()->code(),
                data_result.error()->what(),
                "BacktestEngine"
            );
        }

        // Initialize tracking variables
        std::vector<ExecutionReport> executions;
        std::unordered_map<std::string, Position> current_positions;
        std::vector<std::pair<Timestamp, double>> equity_curve;
        std::vector<RiskResult> risk_metrics;
        double current_equity = config_.portfolio_config.initial_capital;

        // Initialize equity curve with starting point
        equity_curve.emplace_back(config_.strategy_config.start_date, current_equity);

        // Initialize strategy
        INFO("Initializing strategy for backtest");
        auto init_result = strategy->initialize();
        if (init_result.is_error()) {
            ERROR("Strategy initialization failed: " + 
                  std::string(init_result.error()->what()));
            
            StateManager::instance().update_state(
                "BACKTEST_ENGINE", 
                ComponentState::ERR_STATE,
                init_result.error()->what()
            );
            
            return make_error<BacktestResults>(
                init_result.error()->code(),
                init_result.error()->what(),
                "BacktestEngine"
            );
        }

        // Start strategy
        INFO("Starting strategy for backtest");
        auto start_result = strategy->start();
        if (start_result.is_error()) {
            ERROR("Strategy start failed: " + 
                  std::string(start_result.error()->what()));
            
            StateManager::instance().update_state(
                "BACKTEST_ENGINE", 
                ComponentState::ERR_STATE,
                start_result.error()->what()
            );
            
            return make_error<BacktestResults>(
                start_result.error()->code(),
                start_result.error()->what(),
                "BacktestEngine"
            );
        }

        // Process each bar
        INFO("Starting backtest simulation with " + 
             std::to_string(data_result.value().size()) + " bars");
        
        size_t processed_bars = 0;
        
        // Group bars by timestamp for realistic simulation
        std::map<Timestamp, std::vector<Bar>> bars_by_time;
        for (const auto& bar : data_result.value()) {
            bars_by_time[bar.timestamp].push_back(bar);
        }
        
        // Process bars in chronological order
        for (const auto& [timestamp, bars] : bars_by_time) {
            // Update market data in risk manager
            if (risk_manager_) {
                auto update_result = risk_manager_->update_market_data(bars);
                if (update_result.is_error()) {
                    WARN("Failed to update risk manager market data: " + 
                         std::string(update_result.error()->what()));
                }
            }
            
            // Update slippage model
            if (slippage_model_) {
                for (const auto& bar : bars) {
                    slippage_model_->update(bar);
                }
            }
            
            // Process strategy signals first
            auto signal_result = process_strategy_signals(
                bars, strategy, current_positions, executions, equity_curve);
            
            if (signal_result.is_error()) {
                ERROR("Bar processing failed: " + 
                      std::string(signal_result.error()->what()));
                
                StateManager::instance().update_state(
                    "BACKTEST_ENGINE", 
                    ComponentState::ERR_STATE,
                    signal_result.error()->what()
                );
                
                return make_error<BacktestResults>(
                    signal_result.error()->code(),
                    signal_result.error()->what(),
                    "BacktestEngine"
                );
            }
            
            // Then apply portfolio-level constraints
            if (config_.portfolio_config.use_risk_management || 
                config_.portfolio_config.use_optimization) {
                
                auto constraint_result = apply_portfolio_constraints(
                    current_positions, equity_curve, risk_metrics);
                
                if (constraint_result.is_error()) {
                    ERROR("Portfolio constraint application failed: " + 
                          std::string(constraint_result.error()->what()));
                    
                    StateManager::instance().update_state(
                        "BACKTEST_ENGINE", 
                        ComponentState::ERR_STATE,
                        constraint_result.error()->what()
                    );
                    
                    return make_error<BacktestResults>(
                        constraint_result.error()->code(),
                        constraint_result.error()->what(),
                        "BacktestEngine"
                    );
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
            Timestamp ts = i < equity_curve.size() ? 
                equity_curve[i].first : std::chrono::system_clock::now();
                
            results.risk_metrics.emplace_back(ts, risk_metrics[i]);
        }

        // Update final state
        StateManager::instance().update_state(
            "BACKTEST_ENGINE", 
            ComponentState::STOPPED
        );
        
        INFO("Backtest completed successfully");

        return Result<BacktestResults>(results);

    } catch (const std::exception& e) {
        ERROR("Unexpected error during backtest: " + std::string(e.what()));
        
        StateManager::instance().update_state(
            "BACKTEST_ENGINE", 
            ComponentState::ERR_STATE,
            std::string("Unexpected error: ") + e.what()
        );

        return make_error<BacktestResults>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Error running backtest: ") + e.what(),
            "BacktestEngine"
        );
    }
}

Result<BacktestResults> BacktestEngine::run_portfolio(
    std::shared_ptr<PortfolioManager> portfolio) {
    
    // Update state to running
    auto state_result = StateManager::instance().update_state(
        "BACKTEST_ENGINE", 
        ComponentState::RUNNING
    );
    
    if (state_result.is_error()) {
        ERROR("Failed to update backtest engine state: " + 
              std::string(state_result.error()->what()));
    }
    
    try {
        if (!portfolio) {
            return make_error<BacktestResults>(
                ErrorCode::INVALID_ARGUMENT,
                "Null portfolio manager provided for backtest",
                "BacktestEngine"
            );
        }
        
        // Load historical market data
        auto data_result = load_market_data();
        if (data_result.is_error()) {
            ERROR("Failed to load market data: " + 
                  std::string(data_result.error()->what()));
            
            StateManager::instance().update_state(
                "BACKTEST_ENGINE", 
                ComponentState::ERR_STATE,
                data_result.error()->what()
            );
            
            return make_error<BacktestResults>(
                data_result.error()->code(),
                data_result.error()->what(),
                "BacktestEngine"
            );
        }

        // Initialize tracking variables
        std::vector<ExecutionReport> executions;
        std::vector<std::pair<Timestamp, double>> equity_curve;
        std::vector<RiskResult> risk_metrics;
        
        // Get initial portfolio config
        const auto& portfolio_config = portfolio->get_config();
        double initial_capital = portfolio_config.total_capital - portfolio_config.reserve_capital;

        // Initialize equity curve with starting point
        equity_curve.emplace_back(config_.strategy_config.start_date, initial_capital);
        
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
        for (const auto& [timestamp, bars] : bars_by_time) {
            // Clear previous executions from portfolio manager
            portfolio->clear_execution_history();

            // Process portfolio time step
            auto result = process_portfolio_data(
                timestamp,
                bars,
                portfolio,
                executions,
                equity_curve,
                risk_metrics
            );
            
            if (result.is_error()) {
                ERROR("Portfolio data processing failed: " + 
                      std::string(result.error()->what()));
                
                StateManager::instance().update_state(
                    "BACKTEST_ENGINE", 
                    ComponentState::ERR_STATE,
                    result.error()->what()
                );
                
                return make_error<BacktestResults>(
                    result.error()->code(),
                    result.error()->what(),
                    "BacktestEngine"
                );
            }
            
            processed_bars += bars.size();
            
            // Periodically log progress
            if (processed_bars % 1000 == 0) {
                INFO("Processed " + std::to_string(processed_bars) + " bars across " +
                     std::to_string(portfolio->get_strategies().size()) + " strategies");
            }
        }

        // Calculate final results
        INFO("Calculating portfolio backtest metrics");
        auto results = calculate_metrics(equity_curve, executions);

        // Add position and execution history
        results.executions = std::move(executions);

        // Get final portfolio positions
        auto portfolio_positions = portfolio->get_portfolio_positions();
        results.positions.reserve(portfolio_positions.size());
        for (const auto& [_, pos] : portfolio_positions) {
            results.positions.push_back(pos);
        }
        results.equity_curve = std::move(equity_curve);

        // Calculate drawdown curve
        results.drawdown_curve = calculate_drawdowns(results.equity_curve);

        // Add risk metrics history
        results.risk_metrics.reserve(risk_metrics.size());
        for (size_t i = 0; i < risk_metrics.size(); ++i) {
            // Match timestamps from equity curve if available
            Timestamp ts = i < equity_curve.size() ? 
                equity_curve[i].first : std::chrono::system_clock::now();
                
            results.risk_metrics.emplace_back(ts, risk_metrics[i]);
        }

        // Update final state
        StateManager::instance().update_state(
            "BACKTEST_ENGINE", 
            ComponentState::STOPPED
        );
        
        INFO("Portfolio backtest completed successfully with " + 
             std::to_string(portfolio->get_strategies().size()) + " strategies");

        return Result<BacktestResults>(results);

    } catch (const std::exception& e) {
        ERROR("Unexpected error during portfolio backtest: " + std::string(e.what()));
        
        StateManager::instance().update_state(
            "BACKTEST_ENGINE", 
            ComponentState::ERR_STATE,
            std::string("Unexpected error: ") + e.what()
        );

        return make_error<BacktestResults>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Error running portfolio backtest: ") + e.what(),
            "BacktestEngine"
        );
    }
}

Result<void> BacktestEngine::process_bar(
    const std::vector<Bar>& bars,
    std::shared_ptr<StrategyInterface> strategy,
    std::unordered_map<std::string, Position>& current_positions,
    std::vector<std::pair<Timestamp, double>>& equity_curve,
    std::vector<RiskResult>& risk_metrics) {
    
    try {
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
            double current_qty = (current_it != current_positions.end()) ? 
            current_it->second.quantity : 0.0;
            
            if (std::abs(new_pos.quantity - current_qty) > 1e-6) {
                // Find latest price for symbol
                double latest_price = 0.0;
                for (const auto& bar : bars) {
                    if (bar.symbol == symbol) {
                        latest_price = bar.close;
                        break;
                    }
                }
                
                if (latest_price == 0.0) {
                    continue; // Skip if price not available
                }
                
                // Calculate trade size
                double trade_size = new_pos.quantity - current_qty;
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
                        latest_price, 
                        std::abs(trade_size), 
                        side,
                        symbol_bar
                    );
                } else {
                    // Apply basic slippage model
                    double slip_factor = config_.strategy_config.slippage_model / 10000.0;  // bps to decimal
                    fill_price = side == Side::BUY ? 
                               latest_price * (1.0 + slip_factor) : 
                               latest_price * (1.0 - slip_factor);
                }

                // Create execution report
                ExecutionReport exec;
                exec.order_id = "BT-" + std::to_string(equity_curve.size());
                exec.exec_id = "EX-" + std::to_string(equity_curve.size());
                exec.symbol = symbol;
                exec.side = side;
                exec.filled_quantity = std::abs(trade_size);
                exec.fill_price = fill_price;
                exec.fill_time = bars[0].timestamp; // Use timestamp of current batch
                exec.commission = calculate_transaction_costs(exec);
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

        // Calculate current portfolio value
        double portfolio_value = config_.portfolio_config.initial_capital;
        for (const auto& [symbol, pos] : current_positions) {
            // Find latest price for symbol
            double latest_price = 0.0;
            for (const auto& bar : bars) {
                if (bar.symbol == symbol) {
                    latest_price = bar.close;
                    break;
                }
            }
            
            if (latest_price > 0.0) {
                portfolio_value += pos.quantity * latest_price;
            }
        }

        // Update equity curve
        if (!bars.empty()) {
            equity_curve.emplace_back(bars[0].timestamp, portfolio_value);
        }

        // Apply risk management if enabled
        if (config_.portfolio_config.use_risk_management && risk_manager_) {
            auto risk_result = risk_manager_->process_positions(current_positions);
            if (risk_result.is_error()) {
                return make_error<void>(
                    risk_result.error()->code(),
                    risk_result.error()->what(),
                    "BacktestEngine"
                );
            }

            // Store risk metrics for analysis
            risk_metrics.push_back(risk_result.value());

            // Scale positions if risk limits exceeded
            if (risk_result.value().risk_exceeded) {
                double scale = risk_result.value().recommended_scale;
                WARN("Risk limits exceeded: scaling positions by " + std::to_string(scale));
                
                for (auto& [symbol, pos] : current_positions) {
                    pos.quantity *= scale;
                }
            }
        }

        // Apply optimization if enabled
        if (config_.portfolio_config.use_optimization && optimizer_ && current_positions.size() > 1) {
            // Prepare inputs for optimization
            std::vector<std::string> symbols;
            std::vector<double> current_pos, target_pos, costs, weights;
            
            // Extract positions and costs
            for (const auto& [symbol, pos] : current_positions) {
                symbols.push_back(symbol);
                current_pos.push_back(pos.quantity);
                target_pos.push_back(pos.quantity); // Use current as starting point
                
                // Default cost is 1.0, can be refined with specific costs per symbol
                costs.push_back(1.0);
                
                // Equal weights to start, could be refined based on market cap, etc.
                weights.push_back(1.0);
            }
            
            // Simple diagonal covariance matrix as placeholder
            // In production, would use actual market data to calculate
            std::vector<std::vector<double>> covariance(
                symbols.size(), 
                std::vector<double>(symbols.size(), 0.0)
            );
            
            // Set diagonal elements (variances)
            for (size_t i = 0; i < symbols.size(); ++i) {
                covariance[i][i] = 0.01; // Default variance value
            }
            
            // Run optimization
            auto opt_result = optimizer_->optimize_single_period(
                current_pos, target_pos, costs, weights, covariance);
            
            if (opt_result.is_error()) {
                WARN("Optimization failed: " + 
                     std::string(opt_result.error()->what()));
            } else {
                // Apply optimized positions
                const auto& optimized = opt_result.value().optimized_positions;
                for (size_t i = 0; i < symbols.size(); ++i) {
                    current_positions[symbols[i]].quantity = optimized[i];
                }
                
                DEBUG("Positions optimized with tracking error: " + 
                      std::to_string(opt_result.value().tracking_error));
            }
        }

        INFO("Successfully saved backtest results to database");
        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Error processing bar: ") + e.what(),
            "BacktestEngine"
        );
    }
}

// Process market data through each strategy and collect their positions
Result<void> BacktestEngine::process_strategy_signals(
    const std::vector<Bar>& bars,
    std::shared_ptr<StrategyInterface> strategy,
    std::unordered_map<std::string, Position>& current_positions,
    std::vector<ExecutionReport>& executions,
    std::vector<std::pair<Timestamp, double>>& equity_curve) {
    
    try {
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
            double current_qty = (current_it != current_positions.end()) ? 
            current_it->second.quantity : 0.0;
            
            if (std::abs(new_pos.quantity - current_qty) > 1e-6) {
                // Find latest price for symbol
                double latest_price = 0.0;
                for (const auto& bar : bars) {
                    if (bar.symbol == symbol) {
                        latest_price = bar.close;
                        break;
                    }
                }
                
                if (latest_price == 0.0) {
                    continue; // Skip if price not available
                }
                
                // Calculate trade size
                double trade_size = new_pos.quantity - current_qty;
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
                        latest_price, 
                        std::abs(trade_size), 
                        side,
                        symbol_bar
                    );
                } else {
                    // Apply basic slippage model
                    double slip_factor = config_.strategy_config.slippage_model / 10000.0;  // bps to decimal
                    fill_price = side == Side::BUY ? 
                               latest_price * (1.0 + slip_factor) : 
                               latest_price * (1.0 - slip_factor);
                }

                // Create execution report
                ExecutionReport exec;
                exec.order_id = "BT-" + std::to_string(equity_curve.size());
                exec.exec_id = "EX-" + std::to_string(equity_curve.size());
                exec.symbol = symbol;
                exec.side = side;
                exec.filled_quantity = std::abs(trade_size);
                exec.fill_price = fill_price;
                exec.fill_time = bars[0].timestamp; // Use timestamp of current batch
                exec.commission = calculate_transaction_costs(exec);
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

        // Calculate current portfolio value
        double portfolio_value = config_.portfolio_config.initial_capital;
        for (const auto& [symbol, pos] : current_positions) {
            // Find latest price for symbol
            double latest_price = 0.0;
            for (const auto& bar : bars) {
                if (bar.symbol == symbol) {
                    latest_price = bar.close;
                    break;
                }
            }
            
            if (latest_price > 0.0) {
                portfolio_value += pos.quantity * latest_price;
            }
        }

        // Update equity curve
        if (!bars.empty()) {
            equity_curve.emplace_back(bars[0].timestamp, portfolio_value);
        }

        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Error processing strategy signals: ") + e.what(),
            "BacktestEngine"
        );
    }
}

// Apply portfolio-level constraints (risk management and optimization)
Result<void> BacktestEngine::apply_portfolio_constraints(
    std::unordered_map<std::string, Position>& current_positions,
    std::vector<std::pair<Timestamp, double>>& equity_curve,
    std::vector<RiskResult>& risk_metrics) {
    
    try {
        // Apply risk management if enabled
        if (config_.portfolio_config.use_risk_management && risk_manager_) {
            auto risk_result = risk_manager_->process_positions(current_positions);
            if (risk_result.is_error()) {
                return make_error<void>(
                    risk_result.error()->code(),
                    risk_result.error()->what(),
                    "BacktestEngine"
                );
            }

            // Store risk metrics for analysis
            risk_metrics.push_back(risk_result.value());

            // Scale positions if risk limits exceeded
            if (risk_result.value().risk_exceeded) {
                double scale = risk_result.value().recommended_scale;
                WARN("Risk limits exceeded: scaling positions by " + std::to_string(scale));
                
                for (auto& [symbol, pos] : current_positions) {
                    pos.quantity *= scale;
                }
            }
        }

        // Apply optimization if enabled
        if (config_.portfolio_config.use_optimization && optimizer_ && current_positions.size() > 1) {
            // Prepare inputs for optimization
            std::vector<std::string> symbols;
            std::vector<double> current_pos, target_pos, costs, weights;
            
            // Extract positions and costs
            for (const auto& [symbol, pos] : current_positions) {
                symbols.push_back(symbol);
                current_pos.push_back(pos.quantity);
                target_pos.push_back(pos.quantity); // Use current as starting point
                
                // Default cost is 1.0, can be refined with specific costs per symbol
                costs.push_back(1.0);
                
                // Equal weights to start, could be refined based on market cap, etc.
                weights.push_back(1.0);
            }
            
            // Simple diagonal covariance matrix as placeholder
            // In production, would use actual market data to calculate
            std::vector<std::vector<double>> covariance(
                symbols.size(), 
                std::vector<double>(symbols.size(), 0.0)
            );
            
            // Set diagonal elements (variances)
            for (size_t i = 0; i < symbols.size(); ++i) {
                covariance[i][i] = 0.01; // Default variance value
            }
            
            // Run optimization
            auto opt_result = optimizer_->optimize_single_period(
                current_pos, target_pos, costs, weights, covariance);
            
            if (opt_result.is_error()) {
                WARN("Optimization failed: " + 
                     std::string(opt_result.error()->what()));
            } else {
                // Apply optimized positions
                const auto& optimized = opt_result.value().optimized_positions;
                for (size_t i = 0; i < symbols.size(); ++i) {
                    current_positions[symbols[i]].quantity = optimized[i];
                }
                
                DEBUG("Positions optimized with tracking error: " + 
                      std::to_string(opt_result.value().tracking_error));
            }
        }

        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Error applying portfolio constraints: ") + e.what(),
            "BacktestEngine"
        );
    }
}

// Helper method for portfolio backtesting: combines positions from multiple strategies
void BacktestEngine::combine_strategy_positions(
    const std::vector<std::unordered_map<std::string, Position>>& strategy_positions,
    std::unordered_map<std::string, Position>& portfolio_positions) {
    
    // Clear current portfolio positions
    portfolio_positions.clear();
    
    // Combine positions from all strategies
    for (const auto& strategy_pos_map : strategy_positions) {
        for (const auto& [symbol, pos] : strategy_pos_map) {
            if (portfolio_positions.find(symbol) == portfolio_positions.end()) {
                // New symbol, add to portfolio
                portfolio_positions[symbol] = pos;
            } else {
                // Existing symbol, update quantity
                portfolio_positions[symbol].quantity += pos.quantity;
                
                // Update average price based on quantities
                double total_quantity = portfolio_positions[symbol].quantity;
                if (std::abs(total_quantity) > 1e-6) {
                    portfolio_positions[symbol].average_price = 
                        (portfolio_positions[symbol].average_price * (total_quantity - pos.quantity) +
                         pos.average_price * pos.quantity) / total_quantity;
                }
            }
        }
    }
}

// Helper method for portfolio backtesting: distributes portfolio positions back to strategies
void BacktestEngine::redistribute_positions(
    const std::unordered_map<std::string, Position>& portfolio_positions,
    std::vector<std::unordered_map<std::string, Position>>& strategy_positions,
    const std::vector<std::shared_ptr<StrategyInterface>>& strategies) {
    
    // Calculate the total quantity for each symbol across all strategies
    std::unordered_map<std::string, double> total_quantities;
    for (const auto& strategy_pos_map : strategy_positions) {
        for (const auto& [symbol, pos] : strategy_pos_map) {
            total_quantities[symbol] += std::abs(pos.quantity);
        }
    }
    
    // Distribute portfolio positions based on original allocation ratios
    for (size_t i = 0; i < strategy_positions.size(); ++i) {
        auto& strategy_pos_map = strategy_positions[i];
        
        for (auto& [symbol, pos] : strategy_pos_map) {
            // Calculate the original ratio this strategy had of this symbol
            double original_ratio = 0.0;
            if (total_quantities[symbol] > 1e-6) {
                original_ratio = std::abs(pos.quantity) / total_quantities[symbol];
            }
            
            // Get the new portfolio position
            auto portfolio_it = portfolio_positions.find(symbol);
            if (portfolio_it != portfolio_positions.end()) {
                // Update the strategy position based on the ratio
                double new_quantity = portfolio_it->second.quantity * original_ratio;
                if (pos.quantity < 0) {
                    // Maintain original direction (long/short)
                    new_quantity = -std::abs(new_quantity);
                }
                
                pos.quantity = new_quantity;
            } else {
                // Symbol no longer in portfolio, zero out
                pos.quantity = 0.0;
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
    const Timestamp& timestamp,
    const std::vector<Bar>& bars,
    std::shared_ptr<PortfolioManager> portfolio,
    std::vector<ExecutionReport>& executions,
    std::vector<std::pair<Timestamp, double>>& equity_curve,
    std::vector<RiskResult>& risk_metrics) {
    
    try {
        // Update risk manager market data
        if (risk_manager_) {
            auto update_result = risk_manager_->update_market_data(bars);
            if (update_result.is_error()) {
                WARN("Failed to update risk manager market data: " + 
                     std::string(update_result.error()->what()));
            }
        }
        
        // Update slippage model
        if (slippage_model_) {
            for (const auto& bar : bars) {
                slippage_model_->update(bar);
            }
        }
        
        // Process market data through portfolio manager
        auto data_result = portfolio->process_market_data(bars);
        if (data_result.is_error()) {
            return data_result;
        }
        
        // Get executions from the portfolio manager
        auto period_executions = portfolio->get_recent_executions();
        
        // Apply slippage and transaction costs to executions
        for (auto& exec : period_executions) {
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
                    exec.fill_price, 
                    exec.filled_quantity, 
                    exec.side,
                    symbol_bar
                );
                
                exec.fill_price = adjusted_price;
            } else {
                // Apply basic slippage model
                double slip_factor = config_.strategy_config.slippage_model / 10000.0;
                exec.fill_price = exec.side == Side::BUY ? 
                               exec.fill_price * (1.0 + slip_factor) : 
                               exec.fill_price * (1.0 - slip_factor);
            }
            
            // Calculate and add commission
            exec.commission = calculate_transaction_costs(exec);
            
            // Add to overall executions list
            executions.push_back(exec);
        }
        
        // Create price map for portfolio value calculation
        std::unordered_map<std::string, double> current_prices;
        for (const auto& bar : bars) {
            current_prices[bar.symbol] = bar.close;
        }
        
        // Calculate portfolio value and update equity curve
        double portfolio_value = portfolio->get_portfolio_value(current_prices);
        equity_curve.emplace_back(timestamp, portfolio_value);
        
        // Get risk metrics
        if (config_.portfolio_config.use_risk_management && risk_manager_) {
            auto portfolio_positions = portfolio->get_portfolio_positions();
            INFO("Starting risk management with " + std::to_string(portfolio_positions.size()) + " positions");
            auto risk_result = risk_manager_->process_positions(portfolio_positions);
            
            if (risk_result.is_ok()) {
                risk_metrics.push_back(risk_result.value());
            } else {
                ERROR("Risk management failed: " + std::string(risk_result.error()->what()) + 
                    " with code " + std::to_string(static_cast<int>(risk_result.error()->code())));
                return make_error<void>(
                    risk_result.error()->code(),
                    "Risk management failed: " + std::string(risk_result.error()->what()),
                    "PortfolioManager"
                );
            }
        }
        
        return Result<void>();
        
    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Error processing portfolio data: ") + e.what(),
            "BacktestEngine"
        );
    }
}

Result<std::vector<Bar>> BacktestEngine::load_market_data() const {
    try {
        INFO("Loading market data for backtest from " + 
            std::to_string(config_.strategy_config.start_date.time_since_epoch().count()) + 
             " to " + std::to_string(config_.strategy_config.end_date.time_since_epoch().count()));
        
        // Validate the database connection
        if (!db_) {
            return make_error<std::vector<Bar>>(
                ErrorCode::DATABASE_ERROR,
                "Database interface is null",
                "BacktestEngine"
            );
        }
        
        if (!db_->is_connected()) {
            auto connect_result = db_->connect();
            if (connect_result.is_error()) {
                return make_error<std::vector<Bar>>(
                    connect_result.error()->code(),
                    "Failed to connect to database: " + std::string(connect_result.error()->what()),
                    "BacktestEngine"
                );
            }
        }

        // Load market data directly using PostgresInterface
        auto result = db_->get_market_data(
            config_.strategy_config.symbols,
            config_.strategy_config.start_date,
            config_.strategy_config.end_date,
            config_.strategy_config.asset_class,
            config_.strategy_config.data_freq,
            config_.strategy_config.data_type
        );

        if (result.is_error()) {
            return make_error<std::vector<Bar>>(
                result.error()->code(),
                result.error()->what(),
                "BacktestEngine"
            );
        }

        // Convert Arrow table to Bars using your DataConversionUtils
        auto conversion_result = DataConversionUtils::arrow_table_to_bars(result.value());
        if (conversion_result.is_error()) {
            return make_error<std::vector<Bar>>(
                conversion_result.error()->code(),
                conversion_result.error()->what(),
                "BacktestEngine"
            );
        }
        
        auto& bars = conversion_result.value();
        INFO("Loaded " + std::to_string(bars.size()) + " bars for " + 
             std::to_string(config_.strategy_config.symbols.size()) + " symbols");
             
        return conversion_result;

    } catch (const std::exception& e) {
        return make_error<std::vector<Bar>>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Error loading market data: ") + e.what(),
            "BacktestEngine"
        );
    }
}

double BacktestEngine::calculate_transaction_costs(
    const ExecutionReport& execution) const {
    
    // Base commission
    double commission = execution.filled_quantity * 
                       config_.strategy_config.commission_rate;

    // Add market impact based on size (simplified model)
    double market_impact = execution.filled_quantity * 
                          execution.fill_price * 
                          0.0005;  // 5 basis points

    double fixed_cost = 1.0;  // Fixed cost per trade

    return commission + market_impact + fixed_cost;
}

double BacktestEngine::apply_slippage(
    double price,
    double quantity,
    Side side) const {
    
    // Apply basic slippage model
    double slip_factor = config_.strategy_config.slippage_model / 10000.0;  // Convert bps to decimal
    
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
    
    if (equity_curve.empty()) return results;

    // Calculate returns
    std::vector<double> returns;
    returns.reserve(equity_curve.size() - 1);
    
    for (size_t i = 1; i < equity_curve.size(); ++i) {
        double ret = (equity_curve[i].second - equity_curve[i-1].second) / 
                    equity_curve[i-1].second;
        returns.push_back(ret);
    }

    // Basic performance metrics
    results.total_return = (equity_curve.back().second - equity_curve.front().second) / 
                          equity_curve.front().second;

    // Calculate volatility
    double mean_return = std::accumulate(returns.begin(), returns.end(), 0.0) / 
                        returns.size();
    
    double sq_sum = std::inner_product(
        returns.begin(), returns.end(), returns.begin(), 0.0);
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

    double downside_dev = downside_count > 0 ? 
        std::sqrt(downside_sum / downside_count) * std::sqrt(252.0) : 1e-6;

    results.sortino_ratio = (mean_return * 252.0) / downside_dev;

    // Trading metrics
    results.total_trades = static_cast<int>(executions.size());
    
    double total_profit = 0.0;
    double total_loss = 0.0;
    int winning_trades = 0;
    
    for (const auto& exec : executions) {
        double pnl = exec.side == Side::BUY ? 
            -exec.fill_price * exec.filled_quantity - exec.commission :
            exec.fill_price * exec.filled_quantity - exec.commission;

        if (pnl > 0) {
            total_profit += pnl;
            winning_trades++;
            results.max_win = std::max(results.max_win, pnl);
        } else {
            total_loss -= pnl;
            results.max_loss = std::max(results.max_loss, -pnl);
        }
    }

    if (results.total_trades > 0) {
        results.win_rate = static_cast<double>(winning_trades) / results.total_trades;
        results.avg_win = winning_trades > 0 ? total_profit / winning_trades : 0.0;
        results.avg_loss = (results.total_trades - winning_trades) > 0 ? 
            total_loss / (results.total_trades - winning_trades) : 0.0;
    }

    if (total_loss > 0) {
        results.profit_factor = total_profit / total_loss;
    }

    // Calculate drawdown metrics
    auto drawdowns = calculate_drawdowns(equity_curve);
    if (!drawdowns.empty()) {
        results.max_drawdown = std::max_element(
            drawdowns.begin(),
            drawdowns.end(),
            [](const auto& a, const auto& b) { return a.second < b.second; }
        )->second;
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

    // Calculate monthly returns
    std::map<std::string, double> monthly_returns_map;
    for (size_t i = 1; i < equity_curve.size(); ++i) {
        auto time_t = std::chrono::system_clock::to_time_t(equity_curve[i].first);
        std::tm tm;
        trade_ngin::core::safe_localtime(&time_t, &tm);
        
        std::ostringstream month_key;
        month_key << std::setw(4) << (tm.tm_year + 1900) << "-" 
                  << std::setw(2) << std::setfill('0') << (tm.tm_mon + 1);
        
        double period_return = (equity_curve[i].second - equity_curve[i-1].second) / 
                             equity_curve[i-1].second;
        
        monthly_returns_map[month_key.str()] += period_return;
    }
    
    for (const auto& [month, ret] : monthly_returns_map) {
        results.monthly_returns[month] = ret;
    }
    
    // Calculate per-symbol P&L
    std::map<std::string, double> symbol_pnl_map;
    for (const auto& exec : executions) {
        double trade_pnl = exec.side == Side::BUY ? 
            -exec.fill_price * exec.filled_quantity - exec.commission :
            exec.fill_price * exec.filled_quantity - exec.commission;
            
        symbol_pnl_map[exec.symbol] += trade_pnl;
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

    if (equity_curve.empty()) return drawdowns;

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
    
    if (returns.empty()) return metrics;

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
    
    metrics["downside_volatility"] = downside_count > 0 ?
        std::sqrt(downside_sum / downside_count) * std::sqrt(252.0) : 0.0;

    return metrics;
}

Result<void> BacktestEngine::save_results(
    const BacktestResults& results,
    const std::string& run_id) const {
    
    try {
        std::string actual_run_id = run_id.empty() ? 
            "BT_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) : 
            run_id;
            
        INFO("Saving backtest results with ID: " + actual_run_id);

        if (!db_ || !db_->is_connected()) {
            return make_error<void>(
                ErrorCode::DATABASE_ERROR,
                "Database not connected",
                "BacktestEngine"
            );
        }
            
        // Create SQL query to save results
        std::string query = 
            "INSERT INTO " + config_.results_db_schema + ".backtest_results "
            "(run_id, total_return, sharpe_ratio, sortino_ratio, max_drawdown, "
            "calmar_ratio, volatility, total_trades, win_rate, profit_factor, "
            "var_95, cvar_95, beta, correlation, start_date, end_date, config) VALUES "
            "('" + actual_run_id + "', " +
            std::to_string(results.total_return) + ", " +
            std::to_string(results.sharpe_ratio) + ", " +
            std::to_string(results.sortino_ratio) + ", " +
            std::to_string(results.max_drawdown) + ", " +
            std::to_string(results.calmar_ratio) + ", " +
            std::to_string(results.volatility) + ", " +
            std::to_string(results.total_trades) + ", " +
            std::to_string(results.win_rate) + ", " +
            std::to_string(results.profit_factor) + ", " +
            std::to_string(results.var_95) + ", " +
            std::to_string(results.cvar_95) + ", " +
            std::to_string(results.beta) + ", " +
            std::to_string(results.correlation) + ", '" +
            std::to_string(std::chrono::system_clock::to_time_t(config_.strategy_config.start_date)) + "', '" +
            std::to_string(std::chrono::system_clock::to_time_t(config_.strategy_config.end_date)) + "', ";

            
        // Format the configuration as JSON for storage
        std::string config_json = "{\"initial_capital\": " + std::to_string(config_.portfolio_config.initial_capital) + 
                                 ", \"symbols\": [";
                                 
        for (size_t i = 0; i < config_.strategy_config.symbols.size(); ++i) {
            if (i > 0) config_json += ", ";
            config_json += "\"" + config_.strategy_config.symbols[i] + "\"";
        }
        
        config_json += "]}";

        query += "'" + config_json + "')";
        
        // Execute query
        auto result = db_->execute_query(query);
        if (result.is_error()) {
            return make_error<void>(
                result.error()->code(),
                result.error()->what(),
                "BacktestEngine"
            );
        }

        // Save equity curve if enabled
        if (config_.store_trade_details) {
            // Prepare batch for equity curve points
            std::vector<std::string> equity_values;
            for (const auto& [timestamp, equity] : results.equity_curve) {
                std::string timestamp_str = std::to_string(std::chrono::system_clock::to_time_t(timestamp));
                std::string value = "('" + actual_run_id + "', '" + timestamp_str + "', " + 
                                  std::to_string(equity) + ")";
                equity_values.push_back(value);
                
                // Insert in batches of 1000 to avoid too-large queries
                if (equity_values.size() >= 1000) {
                    query = "INSERT INTO " + config_.results_db_schema + ".equity_curve "
                           "(run_id, timestamp, equity) VALUES " + 
                           join(equity_values, ", ");
                           
                    auto curve_result = db_->execute_query(query);
                    if (curve_result.is_error()) {
                        WARN("Failed to save equity curve batch: " + 
                             std::string(curve_result.error()->what()));
                    }
                    equity_values.clear();
                }
            }
            
            // Insert any remaining equity curve points
            if (!equity_values.empty()) {
                query = "INSERT INTO " + config_.results_db_schema + ".equity_curve "
                       "(run_id, timestamp, equity) VALUES " + 
                       join(equity_values, ", ");
                       
                auto curve_result = db_->execute_query(query);
                if (curve_result.is_error()) {
                    WARN("Failed to save final equity curve batch: " + 
                         std::string(curve_result.error()->what()));
                }
            }
            
            // Save trade executions in batches
            std::vector<std::string> execution_values;
            for (const auto& exec : results.executions) {
                std::string timestamp_str = std::to_string(std::chrono::system_clock::to_time_t(exec.fill_time));
                std::string side_str = exec.side == Side::BUY ? "BUY" : "SELL";
                
                std::string value = "('" + actual_run_id + "', '" + 
                                  exec.order_id + "', '" + 
                                  exec.symbol + "', '" + 
                                  side_str + "', " + 
                                  std::to_string(exec.filled_quantity) + ", " + 
                                  std::to_string(exec.fill_price) + ", '" + 
                                  timestamp_str + "', " + 
                                  std::to_string(exec.commission) + ")";
                                  
                execution_values.push_back(value);
                
                // Insert in batches of 1000
                if (execution_values.size() >= 1000) {
                    query = "INSERT INTO " + config_.results_db_schema + ".trade_executions "
                           "(run_id, order_id, symbol, side, quantity, price, timestamp, commission) "
                           "VALUES " + join(execution_values, ", ");
                           
                    auto exec_result = db_->execute_query(query);
                    if (exec_result.is_error()) {
                        WARN("Failed to save executions batch: " + 
                             std::string(exec_result.error()->what()));
                    }
                    execution_values.clear();
                }
            }
            
            // Insert any remaining executions
            if (!execution_values.empty()) {
                query = "INSERT INTO " + config_.results_db_schema + ".trade_executions "
                       "(run_id, order_id, symbol, side, quantity, price, timestamp, commission) "
                       "VALUES " + join(execution_values, ", ");
                       
                auto exec_result = db_->execute_query(query);
                if (exec_result.is_error()) {
                    WARN("Failed to save final executions batch: " + 
                         std::string(exec_result.error()->what()));
                }
            }
            
            // Save final positions
            std::vector<std::string> position_values;
            for (const auto& pos : results.positions) {
                if (std::abs(pos.quantity) < 1e-6) continue; // Skip empty positions
                
                std::string value = "('" + actual_run_id + "', '" + 
                                  pos.symbol + "', " + 
                                  std::to_string(pos.quantity) + ", " + 
                                  std::to_string(pos.average_price) + ", " + 
                                  std::to_string(pos.unrealized_pnl) + ", " + 
                                  std::to_string(pos.realized_pnl) + ")";
                                  
                position_values.push_back(value);
            }
            
            if (!position_values.empty()) {
                query = "INSERT INTO " + config_.results_db_schema + ".final_positions "
                       "(run_id, symbol, quantity, average_price, unrealized_pnl, realized_pnl) "
                       "VALUES " + join(position_values, ", ");
                       
                auto pos_result = db_->execute_query(query);
                if (pos_result.is_error()) {
                    WARN("Failed to save positions: " + 
                         std::string(pos_result.error()->what()));
                }
            }
            
            // Save monthly returns
            std::vector<std::string> monthly_values;
            for (const auto& [month, ret] : results.monthly_returns) {
                std::string value = "('" + actual_run_id + "', '" + 
                                  month + "', " + 
                                  std::to_string(ret) + ")";
                                  
                monthly_values.push_back(value);
            }
            
            if (!monthly_values.empty()) {
                query = "INSERT INTO " + config_.results_db_schema + ".monthly_returns "
                       "(run_id, month, return) VALUES " + 
                       join(monthly_values, ", ");
                       
                auto month_result = db_->execute_query(query);
                if (month_result.is_error()) {
                    WARN("Failed to save monthly returns: " + 
                         std::string(month_result.error()->what()));
                }
            }
            
            // Save symbol P&L
            std::vector<std::string> symbol_pnl_values;
            for (const auto& [symbol, pnl] : results.symbol_pnl) {
                std::string value = "('" + actual_run_id + "', '" + 
                                  symbol + "', " + 
                                  std::to_string(pnl) + ")";
                                  
                symbol_pnl_values.push_back(value);
            }
            
            if (!symbol_pnl_values.empty()) {
                query = "INSERT INTO " + config_.results_db_schema + ".symbol_pnl "
                       "(run_id, symbol, pnl) VALUES " + 
                       join(symbol_pnl_values, ", ");
                       
                auto pnl_result = db_->execute_query(query);
                if (pnl_result.is_error()) {
                    WARN("Failed to save symbol P&L: " + 
                         std::string(pnl_result.error()->what()));
                }
            }
        }

        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::DATABASE_ERROR,
            std::string("Error saving backtest results: ") + e.what(),
            "BacktestEngine"
        );
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

Result<BacktestResults> BacktestEngine::load_results(
    const std::string& run_id) const {
    
    try {
        // Query main results
        std::string query = 
            "SELECT * FROM " + config_.results_db_schema + ".backtest_results "
            "WHERE run_id = $1";
        
        auto result = db_->execute_query(query);
        if (result.is_error()) {
            return make_error<BacktestResults>(
                result.error()->code(),
                result.error()->what(),
                "BacktestEngine"
            );
        }

        // Get Arrow table result
        auto table = result.value();
        if (table->num_rows() == 0) {
            return make_error<BacktestResults>(
                ErrorCode::DATA_NOT_FOUND,
                "No results found for run_id: " + run_id,
                "BacktestEngine"
            );
        }

        // Initialize results
        BacktestResults results;

        // Extract scalar fields from first row
        auto numeric_arrays = {
            std::make_pair("total_return", &results.total_return),
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
            std::make_pair("downside_volatility", &results.downside_volatility)
        };

        for (const auto& [field_name, value_ptr] : numeric_arrays) {
            auto column = table->GetColumnByName(field_name);
            if (column && column->num_chunks() > 0) {
                auto array = std::static_pointer_cast<arrow::DoubleArray>(
                    column->chunk(0));
                if (!array->IsNull(0)) {
                    *value_ptr = array->Value(0);
                }
            }
        }

        // Extract integer fields
        auto int_arrays = {
            std::make_pair("total_trades", &results.total_trades)
        };

        for (const auto& [field_name, value_ptr] : int_arrays) {
            auto column = table->GetColumnByName(field_name);
            if (column && column->num_chunks() > 0) {
                auto array = std::static_pointer_cast<arrow::Int32Array>(
                    column->chunk(0));
                if (!array->IsNull(0)) {
                    *value_ptr = array->Value(0);
                }
            }
        }

        // Load equity curve if available
        if (config_.store_trade_details) {
            query = 
                "SELECT timestamp, equity FROM " + 
                config_.results_db_schema + ".equity_curve "
                "WHERE run_id = $1 "
                "ORDER BY timestamp";
            
            auto curve_result = db_->execute_query(query);
            if (curve_result.is_ok()) {
                auto curve_table = curve_result.value();
                auto timestamp_col = curve_table->GetColumnByName("timestamp");
                auto equity_col = curve_table->GetColumnByName("equity");

                if (timestamp_col && equity_col && 
                    timestamp_col->num_chunks() > 0 && 
                    equity_col->num_chunks() > 0) {
                    
                    auto timestamps = std::static_pointer_cast<arrow::TimestampArray>(
                        timestamp_col->chunk(0));
                    auto equity_values = std::static_pointer_cast<arrow::DoubleArray>(
                        equity_col->chunk(0));

                    results.equity_curve.reserve(timestamps->length());
                    for (int64_t i = 0; i < timestamps->length(); ++i) {
                        if (!timestamps->IsNull(i) && !equity_values->IsNull(i)) {
                            results.equity_curve.emplace_back(
                                std::chrono::system_clock::time_point(
                                    std::chrono::seconds(timestamps->Value(i))),
                                equity_values->Value(i)
                            );
                        }
                    }
                }
            }

            // Load trade executions
            query = 
                "SELECT * FROM " + config_.results_db_schema + ".trade_executions "
                "WHERE run_id = $1 "
                "ORDER BY timestamp";
            
            auto exec_result = db_->execute_query(query);
            if (exec_result.is_ok()) {
                auto exec_table = exec_result.value();
                
                // Extract execution data into results.executions vector
                auto extract_executions = [&results](
                    const std::shared_ptr<arrow::Table>& table) {
                    
                    auto symbol_col = table->GetColumnByName("symbol");
                    auto side_col = table->GetColumnByName("side");
                    auto qty_col = table->GetColumnByName("quantity");
                    auto price_col = table->GetColumnByName("price");
                    auto time_col = table->GetColumnByName("timestamp");
                    
                    if (!symbol_col || !side_col || !qty_col || 
                        !price_col || !time_col) {
                        return;
                    }

                    auto symbols = std::static_pointer_cast<arrow::StringArray>(
                        symbol_col->chunk(0));
                    auto sides = std::static_pointer_cast<arrow::StringArray>(
                        side_col->chunk(0));
                    auto quantities = std::static_pointer_cast<arrow::DoubleArray>(
                        qty_col->chunk(0));
                    auto prices = std::static_pointer_cast<arrow::DoubleArray>(
                        price_col->chunk(0));
                    auto timestamps = std::static_pointer_cast<arrow::TimestampArray>(
                        time_col->chunk(0));

                    for (int64_t i = 0; i < table->num_rows(); ++i) {
                        if (!symbols->IsNull(i) && !sides->IsNull(i) && 
                            !quantities->IsNull(i) && !prices->IsNull(i) && 
                            !timestamps->IsNull(i)) {
                            
                            ExecutionReport exec;
                            exec.symbol = symbols->GetString(i);
                            exec.side = sides->GetString(i) == "BUY" ? 
                                Side::BUY : Side::SELL;
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
            ErrorCode::DATABASE_ERROR,
            std::string("Error loading backtest results: ") + e.what(),
            "BacktestEngine"
        );
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

} // namespace backtest
} // namespace trade_ngin