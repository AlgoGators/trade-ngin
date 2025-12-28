// src/portfolio/portfolio_manager.cpp
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include <algorithm>
#include <climits>
#include <cmath>
#include <set>
#include <sstream>

namespace trade_ngin {

PortfolioManager::PortfolioManager(PortfolioConfig config, std::string id,
                                   std::shared_ptr<InstrumentRegistry> registry)
    : config_(std::move(config)),
      id_(std::move(id)),
      registry_(std::move(registry)),
      instance_id_(id_) {
    Logger::register_component("PortfolioManager");

    // Initialize optimizer if enabled
    if (config_.use_optimization) {
        optimizer_ = std::make_unique<DynamicOptimizer>(config_.opt_config);
    }

    // Initialize risk manager if enabled
    if (config_.use_risk_management) {
        try {
            risk_manager_ = std::make_unique<RiskManager>(config_.risk_config);
            if (!risk_manager_) {
                WARN("Failed to create risk manager, risk management will be disabled");
            } else {
                INFO("Risk manager initialized successfully with capital=" +
                     std::to_string(config_.risk_config.capital));
                Logger::register_component("PortfolioManager");
            }
        } catch (const std::exception& e) {
            ERROR("Failed to initialize risk manager: " + std::string(e.what()));
            risk_manager_ = nullptr;  // Explicitly set to nullptr
        }
    } else {
        INFO("Risk management is disabled in the configuration");
    }

    // Initialize with the provided ID
    ComponentInfo info{ComponentType::PORTFOLIO_MANAGER,
                       ComponentState::INITIALIZED,
                       id_,  // Use the provided ID
                       "",
                       std::chrono::system_clock::now(),
                       {{"total_capital", static_cast<double>(config_.total_capital)},
                        {"reserve_capital", static_cast<double>(config_.reserve_capital)}}};

    auto register_result = StateManager::instance().register_component(info);
    if (register_result.is_error()) {
        throw std::runtime_error(register_result.error()->what());
    }

    // Subscribe to market data and position updates
    MarketDataCallback callback = [this](const MarketDataEvent& event) {
        if (event.type == MarketDataEventType::POSITION_UPDATE) {
            // Handle position updates
            std::string strategy_id = event.string_fields.at("strategy_id");
            auto it = strategies_.find(strategy_id);
            if (it != strategies_.end()) {
                Position pos;
                pos.symbol = event.symbol;
                pos.quantity = event.numeric_fields.at("quantity");
                pos.average_price = event.numeric_fields.at("price");
                pos.last_update = event.timestamp;
                it->second.current_positions[event.symbol] = pos;
            }
        } else if (event.type == MarketDataEventType::BAR) {
            // Convert to Bar and process
            Bar bar;
            bar.timestamp = event.timestamp;
            bar.symbol = event.symbol;
            bar.open = event.numeric_fields.at("open");
            bar.high = event.numeric_fields.at("high");
            bar.low = event.numeric_fields.at("low");
            bar.close = event.numeric_fields.at("close");
            bar.volume = event.numeric_fields.at("volume");

            std::vector<Bar> bars{bar};
            auto result = this->process_market_data(bars);
            if (result.is_error()) {
                ERROR("Error processing market data: " + std::string(result.error()->what()));
            }
        }
    };

    SubscriberInfo sub_info{"PORTFOLIO_MANAGER",
                            {MarketDataEventType::BAR, MarketDataEventType::POSITION_UPDATE},
                            {},  // Subscribe to all symbols
                            callback};

    auto subscribe_result = MarketDataBus::instance().subscribe(sub_info);
    if (subscribe_result.is_error()) {
        throw std::runtime_error(subscribe_result.error()->what());
    }

    (void)StateManager::instance().update_state("PORTFOLIO_MANAGER", ComponentState::RUNNING);
}

Result<void> PortfolioManager::add_strategy(std::shared_ptr<StrategyInterface> strategy,
                                            double initial_allocation, bool use_optimization,
                                            bool use_risk_management) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!strategy) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT, "Strategy cannot be null",
                                "PortfolioManager");
    }

    const auto& metadata = strategy->get_metadata();

    if (strategies_.find(metadata.id) != strategies_.end()) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Strategy with ID " + metadata.id + " already exists",
                                "PortfolioManager");
    }

    if (initial_allocation < config_.min_strategy_allocation ||
        initial_allocation > config_.max_strategy_allocation) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT, "Initial allocation out of bounds",
                                "PortfolioManager");
    }

    // Validate total allocation doesn't exceed 1
    double total_allocation = initial_allocation;
    for (const auto& [_, info] : strategies_) {
        total_allocation += info.allocation;
    }

    if (total_allocation > 1.0) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT, "Total allocation would exceed 1.0",
                                "PortfolioManager");
    }

    // Create strategy info
    StrategyInfo info{
        strategy,
        initial_allocation,
        use_optimization && config_.use_optimization,
        use_risk_management && config_.use_risk_management,
        {},  // current positions
        {}   // target positions
    };

    strategies_[metadata.id] = std::move(info);

    INFO("Added strategy " + metadata.id + " with allocation " +
         std::to_string(initial_allocation));

    return Result<void>();
}

Result<void> PortfolioManager::process_market_data(const std::vector<Bar>& data, bool skip_execution_generation, 
                                                    std::optional<Timestamp> current_timestamp) {
    std::vector<std::string> processed_strategies;

    try {
        std::unordered_map<std::string, std::unordered_map<std::string, Position>> prev_positions;
        std::unordered_map<std::string, Position> prev_portfolio_positions;
        {
            std::lock_guard<std::mutex> lock(mutex_);

            // Validate market data
            if (data.empty()) {
                ERROR("Empty market data provided");
                return make_error<void>(ErrorCode::MARKET_DATA_ERROR, "Empty market data provided",
                                        "PortfolioManager");
            }

            // Update historical returns for all symbols
            update_historical_returns(data);

            // Store current positions for each strategy to detect changes
            for (const auto& [id, info] : strategies_) {
                prev_positions[id] = info.current_positions;
            }

            // Store current portfolio positions to detect changes
            prev_portfolio_positions = get_positions_internal();

            // Process data through each strategy
            for (auto& [id, info] : strategies_) {
                Logger::register_component(info.strategy->get_metadata().name);
                if (!info.strategy) {
                    ERROR("Null strategy pointer found for ID: " + id);
                    return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                            "Null strategy pointer found for ID: " + id,
                                            "PortfolioManager");
                }

                try {
                    // Store current positions
                    info.current_positions = info.strategy->get_positions();

                    std::ostringstream oss;
                    for (auto& [sym, pos] : info.current_positions) {
                        oss << sym << ": " << pos.quantity << ", ";
                    }
                    DEBUG("Current positions for strategy " + id + ": " + oss.str());

                    // Process market data through strategy
                    auto result = info.strategy->on_data(data);
                    if (result.is_error()) {
                        ERROR("Error processing data for strategy " + id + ": " +
                              result.error()->what());
                        std::cerr << "Error processing data for strategy " << id << ": "
                                  << result.error()->what() << std::endl;
                    }

                    // Get target positions using polymorphic dispatch
                    // Each strategy type implements get_target_positions() appropriately:
                    // - BaseStrategy: returns get_positions() (standard positions map)
                    // - TrendFollowing/Fast/Slow: returns positions from instrument_data_
                    // This automatically handles all strategy types without type-checking
                    info.target_positions = info.strategy->get_target_positions();
                    DEBUG("Retrieved " + std::to_string(info.target_positions.size()) + 
                          " target positions from strategy " + id);

                    processed_strategies.push_back(id);

                } catch (const std::exception& e) {
                    ERROR("Exception processing strategy " + id + ": " + std::string(e.what()));
                    continue;
                }
            }
        }

        //  Iterative dynamic opt + risk management loop
        // Up to 5 iterations for convergence to fully integer positions. The final rounding step
        // can cause minor tracking error/risk profile deviation

        int max_iterations = 5;
        int iteration = 0;
        bool done = false;

        while (!done && iteration++ < max_iterations) {
            INFO("Iteration " + std::to_string(iteration) + " of dynamic optimization + risk loop");

            // Dynamic Optimization step
            if (config_.use_optimization && optimizer_) {
                try {
                    Logger::register_component("DynamicOptimizer");
                    auto opt_result = optimize_positions();
                    if (opt_result.is_error()) {
                        WARN("Portfolio optimization failed in iteration " +
                             std::to_string(iteration) + ": " +
                             std::string(opt_result.error()->what()) +
                             ", continuing without optimization");
                    }
                } catch (const std::exception& e) {
                    WARN("Exception during portfolio optimization in iteration " +
                         std::to_string(iteration) + ": " + std::string(e.what()) +
                         ", continuing without optimization");
                }
            }

            // Risk Management step
            bool has_risk_manager =
                (external_risk_manager_ != nullptr) || (risk_manager_ != nullptr);
            if (config_.use_risk_management && has_risk_manager) {
                try {
                    Logger::register_component("RiskManager");
                    auto risk_result = apply_risk_management(data);
                    if (risk_result.is_error()) {
                        WARN("Portfolio risk management failed in iteration " +
                             std::to_string(iteration) + ": " +
                             std::string(risk_result.error()->what()) +
                             ", continuing without risk management");
                    } else {
                        INFO("Portfolio risk management applied successfully in iteration " +
                             std::to_string(iteration));
                    }
                } catch (const std::exception& e) {
                    WARN("Exception during risk management in iteration " +
                         std::to_string(iteration) + ": " + std::string(e.what()) +
                         ", continuing without risk management");
                }
            } else {
                INFO("Risk management not enabled, skipping risk checks in iteration " +
                     std::to_string(iteration));
            }

            // Check for partial contracts in final positions
            bool partials_found = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (const auto& [id, info] : strategies_) {
                    for (const auto& [symbol, pos] : info.target_positions) {
                        double fractional = std::abs(static_cast<double>(pos.quantity) -
                                                     std::round(static_cast<double>(pos.quantity)));
                        if (fractional > 1e-6) {
                            partials_found = true;
                            INFO("Fractional contract detected in iteration " +
                                 std::to_string(iteration) + ": " + symbol +
                                 ", quantity=" + std::to_string(pos.quantity));
                            break;
                        }
                    }
                    if (partials_found)
                        break;
                }
            }

            if (!partials_found) {
                INFO("No partial contracts after iteration " + std::to_string(iteration) +
                     ". Converged!");
                done = true;
            }
        }

        // This safeguard forcibly rounds all final positions to integers in case of conflicting
        // rounding logic
        if (!done) {
            WARN("Max iterations reached (" + std::to_string(max_iterations) +
                 "). Forcing final rounding to remove any partial contracts.");

            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& [id, info] : strategies_) {
                for (auto& [symbol, pos] : info.target_positions) {
                    double original_quantity = static_cast<double>(pos.quantity);
                    pos.quantity =
                        static_cast<Decimal>(std::round(static_cast<double>(pos.quantity)));
                    if (std::abs(original_quantity - static_cast<double>(pos.quantity)) > 1e-6) {
                        INFO("Final forced rounding for " + symbol + ": " +
                             std::to_string(original_quantity) + " -> " +
                             std::to_string(pos.quantity));
                    }
                }
            }
            INFO("Final rounding completed. No partial contracts remain.");
        } else {
            INFO("Final positions fully integer after " + std::to_string(iteration) +
                 " iterations.");
        }

        // Final verification of all positions for partial contracts
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& [id, info] : strategies_) {
                for (const auto& [symbol, pos] : info.target_positions) {
                    double fractional = std::abs(static_cast<double>(pos.quantity) -
                                                 std::round(static_cast<double>(pos.quantity)));
                    if (fractional > 1e-6) {
                        ERROR("FINAL CHECK: Fractional contract detected for " + symbol +
                              " after all iterations. Quantity=" + std::to_string(pos.quantity));
                    }
                }
            }
            
            // CRITICAL FIX: Update current_positions with optimized/rounded target_positions
            // This ensures get_strategy_positions() returns integer positions, not fractional ones
            for (auto& [id, info] : strategies_) {
                info.current_positions = info.target_positions;
            }
        }

        // Get optimized positions (should be whole numbers after dynamic optimization)
        auto post_opt = get_portfolio_positions();
        {
            std::ostringstream oss3;
            for (const auto& [symbol, pos] : post_opt) {
                oss3 << symbol << ": " << pos.quantity << ", ";
            }
            DEBUG("Post-optimization positions: " + oss3.str());
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);

            // DO NOT clear strategy_executions_ here - they need to accumulate across all periods
            // for saving at the end of the backtest. Each period's executions are appended.
            // Only clear at the very beginning of the backtest (handled elsewhere if needed)

            // Skip execution generation during warmup to prevent warmup executions from being created
            if (!skip_execution_generation) {
                // Check if this is first post-warmup day for portfolio-level executions
                // (check BEFORE generating strategy executions)
                bool is_first_post_warmup_day_portfolio = true;
                for (const auto& [strategy_id, _] : strategies_) {
                    if (strategy_executions_[strategy_id].size() > 0) {
                        is_first_post_warmup_day_portfolio = false;
                        break;
                    }
                }
                bool should_generate_portfolio_establishment_execs = is_first_post_warmup_day_portfolio;

                // Generate execution reports per strategy (before aggregation)
                // This allows accurate per-strategy execution tracking
                for (const auto& [strategy_id, info] : strategies_) {
                auto& strategy_execs = strategy_executions_[strategy_id];
                // Start counter from current size to ensure unique IDs across all periods
                int exec_counter = static_cast<int>(strategy_execs.size());

                INFO("Generating executions for strategy " + strategy_id + 
                      ", target_positions size: " + std::to_string(info.target_positions.size()) +
                      ", existing executions: " + std::to_string(exec_counter));

                // Get previous positions for this strategy
                const auto& prev_strategy_positions = prev_positions[strategy_id];
                INFO("Previous positions for strategy " + strategy_id + 
                      " size: " + std::to_string(prev_strategy_positions.size()));

                // OPTION 3 ENHANCEMENT: Detect first post-warmup day by checking if no executions
                // have been generated yet (exec_counter == 0). On the first post-warmup day, 
                // generate "establishment executions" for all non-zero positions, even if they 
                // match previous positions. This ensures executions show how we got to the positions,
                // not just changes. With Option 3, positions accumulate during warmup but executions
                // are cleared, so exec_counter == 0 indicates first post-warmup day.
                bool is_first_post_warmup_day = (exec_counter == 0);
                bool should_generate_establishment_execs = is_first_post_warmup_day;

                // Generate executions based on individual strategy position changes
                for (const auto& [symbol, new_pos] : info.target_positions) {
                    double current_qty = 0.0;
                    auto prev_pos_it = prev_strategy_positions.find(symbol);
                    if (prev_pos_it != prev_strategy_positions.end()) {
                        current_qty = static_cast<double>(prev_pos_it->second.quantity);
                    }

                    double new_qty = static_cast<double>(new_pos.quantity);

                    // Generate execution if:
                    // 1. Position changed (normal case), OR
                    // 2. This is first post-warmup day and position is non-zero (establishment execution)
                    bool position_changed = (std::abs(new_qty - current_qty) > 1e-6);
                    bool is_establishment_exec = should_generate_establishment_execs && 
                                                 (std::abs(new_qty) > 1e-6);
                    
                    if (position_changed || is_establishment_exec) {
                        // Calculate trade size
                        // For establishment executions, use the full new_qty (we're establishing the position)
                        // For normal changes, use the difference
                        double trade_size = is_establishment_exec ? new_qty : (new_qty - current_qty);
                        Side side = trade_size > 0 ? Side::BUY : Side::SELL;

                        // Find latest price for symbol
                        double latest_price = 0.0;
                        for (const auto& bar : data) {
                            if (bar.symbol == symbol) {
                                latest_price = static_cast<double>(bar.close);
                                break;
                            }
                        }

                        if (latest_price == 0.0) {
                            continue;  // Skip if price not available
                        }

                        // Create execution report for this strategy
                        ExecutionReport exec;
                        exec.order_id = "PM-" + strategy_id + "-" + std::to_string(exec_counter);
                        exec.exec_id = "EX-" + strategy_id + "-" + std::to_string(exec_counter);
                        exec.symbol = symbol;
                        exec.side = side;
                        exec.filled_quantity = std::abs(trade_size);
                        exec.fill_price = latest_price;
                        // CRITICAL FIX: Execution fill_time should use the CURRENT day's timestamp,
                        // not the previous day's bars timestamp. The 'data' parameter contains
                        // previous day's bars (for signal generation), but executions happen on
                        // the current day. Use current_timestamp if provided, otherwise fall back to data timestamp.
                        exec.fill_time = current_timestamp.has_value() ? current_timestamp.value() : 
                                        (data.empty() ? std::chrono::system_clock::now() : data[0].timestamp);
                        // Calculate transaction costs using the same model as backtesting
                        // Base commission: 5 basis points * quantity
                        double commission = std::abs(trade_size) * 0.0005;
                        // Market impact: 5 basis points * quantity * price  
                        double market_impact = std::abs(trade_size) * latest_price * 0.0005;
                        // Fixed cost per trade
                        double fixed_cost = 1.0;
                        exec.commission = commission + market_impact + fixed_cost;
                        exec.is_partial = false;

                        // Add to strategy-specific executions
                        strategy_execs.push_back(exec);
                        exec_counter++;
                        std::string exec_type = is_establishment_exec ? " [ESTABLISHMENT]" : "";
                        INFO("Generated execution for strategy " + strategy_id + 
                             ": " + symbol + " " + (side == Side::BUY ? "BUY" : "SELL") + 
                             " qty=" + std::to_string(exec.filled_quantity) + exec_type);
                    }
                }
                INFO("Total executions generated for strategy " + strategy_id + 
                     ": " + std::to_string(strategy_execs.size()));
            }

            // Also generate portfolio-level executions (aggregated) for backward compatibility
            recent_executions_.clear();
            
            for (const auto& [symbol, new_pos] : post_opt) {
                double current_qty = 0.0;

                // Get previous portfolio position quantity for this symbol
                auto prev_pos_it = prev_portfolio_positions.find(symbol);
                if (prev_pos_it != prev_portfolio_positions.end()) {
                    current_qty = static_cast<double>(prev_pos_it->second.quantity);
                }

                double new_qty = static_cast<double>(new_pos.quantity);
                
                // Generate execution if:
                // 1. Position changed (normal case), OR
                // 2. This is first post-warmup day and position is non-zero (establishment execution)
                bool position_changed = (std::abs(new_qty - current_qty) > 1e-6);
                bool is_establishment_exec = should_generate_portfolio_establishment_execs && 
                                             (std::abs(new_qty) > 1e-6);
                
                if (position_changed || is_establishment_exec) {
                    // Calculate trade size
                    // For establishment executions, use the full new_qty (we're establishing the position)
                    // For normal changes, use the difference
                    double trade_size = is_establishment_exec ? new_qty : (new_qty - current_qty);
                    Side side = trade_size > 0 ? Side::BUY : Side::SELL;

                    // Find latest price for symbol
                    double latest_price = 0.0;
                    for (const auto& bar : data) {
                        if (bar.symbol == symbol) {
                            latest_price = static_cast<double>(bar.close);
                            break;
                        }
                    }

                    if (latest_price == 0.0) {
                        continue;  // Skip if price not available
                    }

                    // Create execution report
                    ExecutionReport exec;
                    exec.order_id = "PM-" + id_ + "-" + std::to_string(recent_executions_.size());
                    exec.exec_id = "EX-" + id_ + "-" + std::to_string(recent_executions_.size());
                    exec.symbol = symbol;
                    exec.side = side;
                    exec.filled_quantity = std::abs(trade_size);
                    exec.fill_price = latest_price;
                    // CRITICAL FIX: Execution fill_time should use the CURRENT day's timestamp,
                    // not the previous day's bars timestamp. The 'data' parameter contains
                    // previous day's bars (for signal generation), but executions happen on
                    // the current day. Use current_timestamp if provided, otherwise fall back to data timestamp.
                    exec.fill_time = current_timestamp.has_value() ? current_timestamp.value() : 
                                    (data.empty() ? std::chrono::system_clock::now() : data[0].timestamp);
                    // Calculate transaction costs using the same model as backtesting
                    // Base commission: 5 basis points * quantity
                    double commission = std::abs(trade_size) * 0.0005;
                    // Market impact: 5 basis points * quantity * price  
                    double market_impact = std::abs(trade_size) * latest_price * 0.0005;
                    // Fixed cost per trade
                    double fixed_cost = 1.0;
                    exec.commission = commission + market_impact + fixed_cost;
                    exec.is_partial = false;

                    // Add to recent executions (portfolio-level)
                    recent_executions_.push_back(exec);
                }
            }
            }  // End of if (!skip_execution_generation) block
        }
        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::UNKNOWN_ERROR,
                                std::string("Error processing market data: ") + e.what(),
                                "PortfolioManager");
    }
}

std::vector<double> PortfolioManager::calculate_weights_per_contract(
    const std::vector<std::string>& symbols, double capital) const {
    std::vector<double> weights_per_contract(symbols.size(), 0.0);

    for (size_t i = 0; i < symbols.size(); ++i) {
        const std::string& symbol = symbols[i];

        // Get contract multiplier/size for this instrument
        double contract_size = 1.0;  // Default
        double price = 1.0;          // Default
        double fx_rate = 1.0;        // Default exchange rate

        // Look up instrument details if available
        if (registry_ && registry_->has_instrument(symbol)) {
            auto instrument = registry_->get_instrument(symbol);
            contract_size = instrument->get_multiplier();

            // Get latest price from positions or market data
            auto pos_it = get_portfolio_positions().find(symbol);
            if (pos_it != get_portfolio_positions().end()) {
                price = static_cast<double>(pos_it->second.average_price);
            }
        }

        // Calculate weight per contract = (notional per contract) / capital
        double notional_per_contract = contract_size * price * fx_rate;
        weights_per_contract[i] = notional_per_contract / capital;

        // Ensure weight is positive and reasonable
        if (weights_per_contract[i] <= 0.0 || std::isnan(weights_per_contract[i])) {
            WARN("Invalid weight per contract for " + symbol + ", using default of 0.01");
            weights_per_contract[i] = 0.01;  // Reasonable default
        }
    }

    return weights_per_contract;
}

std::vector<double> PortfolioManager::calculate_trading_costs(
    const std::vector<std::string>& symbols, double capital) const {
    std::vector<double> costs(symbols.size(), 0.0);

    // Collect all trading data once
    std::unordered_map<std::string, const InstrumentData*> all_trading_data;
    for (const auto& [strategy_id, info] : strategies_) {
        auto trend_strategy = std::dynamic_pointer_cast<TrendFollowingStrategy>(info.strategy);
        if (trend_strategy) {
            const auto& strategy_data = trend_strategy->get_all_instrument_data();
            for (const auto& [symbol, data] : strategy_data) {
                all_trading_data[symbol] = &data;
            }
        }
    }

    for (size_t i = 0; i < symbols.size(); ++i) {
        const std::string& symbol = symbols[i];

        // Default cost as a proportion of capital (e.g., 5 basis points)
        double cost_proportion = 0.0005;

        // Check if any strategy has specific costs for this symbol
        for (const auto& [strategy_id, info] : strategies_) {
            const auto& strategy_config = info.strategy->get_config();
            if (strategy_config.costs.count(symbol) > 0) {
                cost_proportion = strategy_config.costs.at(symbol);
                break;  // Use first match
            }
        }

        // Get contract size and price for this symbol
        auto it = all_trading_data.find(symbol);
        if (it != all_trading_data.end()) {
            const auto& data = *(it->second);
            double contract_size = data.contract_size;
            double price = data.price_history.empty() ? 1.0 : data.price_history.back();
            double fx_rate = 1.0;  // Default exchange rate

            // Calculate notional per contract
            double notional_per_contract = contract_size * price * fx_rate;
            double cost_per_contract = cost_proportion * notional_per_contract;
            costs[i] = cost_per_contract / capital;
        } else {
            WARN("Symbol " + symbol + " not found in trading data, using default cost");
            costs[i] = 0.0005;  // Reasonable default
        }
    }

    return costs;
}

void PortfolioManager::update_historical_returns(const std::vector<Bar>& data) {
    if (data.empty())
        return;

    // Collect symbols from the data for lookup
    std::set<std::string> data_symbols;
    for (const auto& bar : data) {
        data_symbols.insert(bar.symbol);
    }

    // Get price history from strategies
    bool got_history = false;
    for (const auto& [id, info] : strategies_) {
        // Try to get price history from this strategy
        auto price_history = info.strategy->get_price_history();

        if (!price_history.empty()) {
            INFO("Retrieved price history from strategy " + id + " for " +
                 std::to_string(price_history.size()) + " symbols");

            // For each symbol, update our price history
            for (const auto& [symbol, prices] : price_history) {
                // Only process if this symbol is in the current data (for efficiency)
                if (data_symbols.count(symbol) > 0) {
                    // Update our price history with the strategy's data
                    price_history_[symbol] = prices;

                    DEBUG("Updated price history for " + symbol + " with " +
                          std::to_string(prices.size()) + " points");

                    got_history = true;
                }
            }
        }
    }

    // Now calculate returns for each symbol that has price history
    for (const auto& [symbol, prices] : price_history_) {
        // Need at least 2 prices to calculate a return
        if (prices.size() < 2) {
            DEBUG("Symbol " + symbol + " has only " + std::to_string(prices.size()) +
                  " prices, skipping return calculation");
            continue;
        }

        // Clear previous returns for this symbol
        historical_returns_[symbol].clear();

        // Calculate returns - use all available history
        for (size_t i = 1; i < prices.size(); ++i) {
            double prev_price = prices[i - 1];
            double curr_price = prices[i];

            if (prev_price <= 0.0)
                continue;

            double ret = (curr_price - prev_price) / prev_price;

            if (std::isfinite(ret)) {
                historical_returns_[symbol].push_back(ret);
            }
        }

        DEBUG("Calculated " + std::to_string(historical_returns_[symbol].size()) +
              " returns for symbol " + symbol + " from " + std::to_string(prices.size()) +
              " prices");

        // Limit history length if needed
        if (historical_returns_[symbol].size() > max_history_length_) {
            // Keep only the most recent returns
            size_t excess = historical_returns_[symbol].size() - max_history_length_;
            historical_returns_[symbol].erase(historical_returns_[symbol].begin(),
                                              historical_returns_[symbol].begin() + excess);
        }
    }

    // Log aggregate stats
    size_t total_returns = 0;
    for (const auto& [symbol, returns] : historical_returns_) {
        total_returns += returns.size();
    }

    INFO("Total historical data: " + std::to_string(total_returns) + " returns across " +
         std::to_string(historical_returns_.size()) + " symbols");
}

std::vector<std::vector<double>> PortfolioManager::calculate_covariance_matrix(
    const std::unordered_map<std::string, std::vector<double>>& returns_by_symbol) {
    // Get all symbols in a consistent order
    std::vector<std::string> ordered_symbols;
    for (const auto& [symbol, _] : returns_by_symbol) {
        ordered_symbols.push_back(symbol);
    }
    std::sort(ordered_symbols.begin(), ordered_symbols.end());

    size_t num_assets = ordered_symbols.size();

    if (num_assets == 0) {
        ERROR("No assets available for covariance calculation");
    }

    // Find minimum length of return series for all symbols
    size_t min_periods = SIZE_MAX;
    for (const auto& symbol : ordered_symbols) {
        if (returns_by_symbol.at(symbol).empty()) {
            // If any symbol has no returns, we can't calculate covariance
            WARN("Symbol " + symbol + " has no return data");
            continue;
        }
        min_periods = std::min(min_periods, returns_by_symbol.at(symbol).size());
    }

    if (min_periods == SIZE_MAX)
        min_periods = 0;

    if (min_periods < 20) {  // Need sufficient data
        WARN("Insufficient return data for covariance calculation: " + std::to_string(min_periods) +
             " periods");
        // Return diagonal matrix with default variance
        std::vector<std::vector<double>> default_cov(num_assets,
                                                     std::vector<double>(num_assets, 0.0));
        for (size_t i = 0; i < num_assets; ++i) {
            default_cov[i][i] = 0.01;  // Default variance on diagonal
        }
        return default_cov;
    }

    // Create a matrix of aligned returns
    std::vector<std::vector<double>> aligned_returns(min_periods,
                                                     std::vector<double>(num_assets, 0.0));

    for (size_t i = 0; i < num_assets; ++i) {
        const auto& symbol = ordered_symbols[i];
        const auto& returns = returns_by_symbol.at(symbol);

        // Take the most recent min_periods returns
        size_t start_idx = returns.size() - min_periods;
        for (size_t j = 0; j < min_periods; ++j) {
            aligned_returns[j][i] = returns[start_idx + j];
        }
    }

    // Calculate means for each asset using aligned returns
    std::vector<double> means(num_assets, 0.0);
    for (size_t i = 0; i < num_assets; ++i) {
        for (size_t t = 0; t < min_periods; ++t) {
            means[i] += aligned_returns[t][i];
        }
        means[i] /= min_periods;
    }

    // Calculate covariance matrix
    std::vector<std::vector<double>> covariance(num_assets, std::vector<double>(num_assets, 0.0));

    // Avoid division by zero when min_periods == 1
    double divisor = (min_periods > 1) ? (min_periods - 1) : 1.0;

    for (size_t i = 0; i < num_assets; ++i) {
        for (size_t j = 0; j < num_assets; ++j) {
            double cov_sum = 0.0;
            for (size_t t = 0; t < min_periods; ++t) {
                cov_sum += (aligned_returns[t][i] - means[i]) * (aligned_returns[t][j] - means[j]);
            }

            covariance[i][j] = cov_sum / divisor;

            // Annualize the covariance (assuming daily data with 252 trading days)
            covariance[i][j] *= 252.0;
        }
    }

    return covariance;
}

Result<void> PortfolioManager::optimize_positions() {
    try {
        // Get unique symbols across all strategies and collect data under lock
        std::vector<std::string> symbols;
        std::unordered_map<std::string, const InstrumentData*> all_trading_data;
        std::vector<double> current_weights;
        std::vector<double> target_weights;
        std::vector<double> weights_per_contract;
        std::vector<double> costs;
        std::unordered_map<std::string, std::vector<double>> returns_by_symbol;
        
        // Store original contributions per strategy per symbol for proportional distribution
        // Map: symbol -> strategy_id -> contribution (quantity * allocation * weight_per_contract)
        std::unordered_map<std::string, std::unordered_map<std::string, double>> original_contribs;
        std::unordered_map<std::string, double> total_contribs;

        {
            std::lock_guard<std::mutex> lock(mutex_);

            // First collect all potential symbols
            std::vector<std::string> all_symbols;
            for (const auto& [_, info] : strategies_) {
                if (!info.use_optimization)
                    continue;

                for (const auto& [symbol, _] : info.target_positions) {
                    all_symbols.push_back(symbol);
                }
            }

            // Remove duplicates
            std::sort(all_symbols.begin(), all_symbols.end());
            all_symbols.erase(std::unique(all_symbols.begin(), all_symbols.end()),
                              all_symbols.end());

            if (all_symbols.empty()) {
                INFO("No symbols found for optimization, skipping");
                return Result<void>();
            }

            int min_history_length = 20;  // Minimum history length for covariance calculation

            // Filter symbols to only those with sufficient historical data FIRST
            for (const auto& symbol : all_symbols) {
                auto it = historical_returns_.find(symbol);
                if (it != historical_returns_.end() &&
                    it->second.size() >= static_cast<size_t>(min_history_length)) {
                    symbols.push_back(symbol);
                    returns_by_symbol[symbol] = it->second;  // Copy the data under lock
                } else {
                    INFO("Symbol " + symbol +
                         " has insufficient historical data for optimization, skipping symbol");
                }
            }

            if (symbols.empty()) {
                INFO("No symbols have sufficient historical data for optimization, skipping");
                return Result<void>();
            }

            // Collect all instrument data
            for (const auto& [id, info] : strategies_) {
                auto trend_strategy =
                    std::dynamic_pointer_cast<TrendFollowingStrategy>(info.strategy);
                if (trend_strategy) {
                    const auto& strategy_data = trend_strategy->get_all_instrument_data();
                    for (const auto& [symbol, data] : strategy_data) {
                        all_trading_data[symbol] = &data;
                    }
                }
            }

            // Calculate weights per contract (only for valid symbols)
            weights_per_contract.reserve(symbols.size());

            for (auto const& symbol : symbols) {
                // Get contract size and price for this symbol
                auto it = all_trading_data.find(symbol);
                if (it != all_trading_data.end()) {
                    const auto& data = *(it->second);
                    double contract_size = data.contract_size;
                    double price = data.price_history.empty() ? 1.0 : data.price_history.back();
                    double fx_rate = 1.0;  // Default exchange rate

                    // Calculate notional per contract
                    double notional_per_contract = contract_size * price * fx_rate;
                    weights_per_contract.push_back(notional_per_contract /
                                                   static_cast<double>(config_.total_capital));
                } else {
                    WARN("Symbol " + symbol + " not found in trading data, using default weight");
                    weights_per_contract.push_back(0.01);  // Reasonable default
                }
            }

            // Build current and target in weight space (only for valid symbols)
            current_weights.resize(symbols.size(), 0.0);
            target_weights.resize(symbols.size(), 0.0);

            for (size_t i = 0; i < symbols.size(); ++i) {
                const std::string& symbol = symbols[i];

                // Aggregate across strategies
                for (const auto& [strat_id, info] : strategies_) {
                    if (!info.use_optimization)
                        continue;

                    double allocation = info.allocation;
                    if (info.current_positions.count(symbol)) {
                        current_weights[i] +=
                            static_cast<double>(info.current_positions.at(symbol).quantity) *
                            weights_per_contract[i] * allocation;
                    }
                    if (info.target_positions.count(symbol)) {
                        double contrib = static_cast<double>(info.target_positions.at(symbol).quantity) *
                                        weights_per_contract[i] * allocation;
                        target_weights[i] += contrib;
                        
                        // Store original contribution for proportional distribution after optimization
                        original_contribs[symbol][strat_id] = contrib;
                        total_contribs[symbol] += contrib;
                    }
                }
            }

            // Calculate trading costs (inside lock since it accesses strategies_)
            costs = calculate_trading_costs(symbols, static_cast<double>(config_.total_capital));
        }  // End of mutex lock scope

        // Calculate covariance matrix (outside lock, using copied data)
        auto covariance = calculate_covariance_matrix(returns_by_symbol);

        // Call the optimizer
        if (!optimizer_) {
            ERROR("Optimizer not initialized");
            return make_error<void>(ErrorCode::NOT_INITIALIZED, "Optimizer not initialized",
                                    "PortfolioManager");
        }

        auto result = optimizer_->optimize(current_weights, target_weights, costs,
                                           weights_per_contract, covariance);

        if (result.is_error()) {
            return make_error<void>(result.error()->code(),
                                    "Optimization failed: " + std::string(result.error()->what()),
                                    "PortfolioManager");
        }

        // Log optimization result metrics
        INFO("Optimization metrics: tracking error=" +
             std::to_string(result.value().tracking_error) +
             ", cost=" + std::to_string(result.value().cost_penalty) +
             ", iterations=" + std::to_string(result.value().iterations));

        const auto& optimized_positions = result.value().positions;

        // Apply optimized positions back to strategies under lock
        {
            std::lock_guard<std::mutex> lock(mutex_);

            for (size_t i = 0; i < symbols.size(); ++i) {
                const auto& symbol = symbols[i];

                // Compute raw contract count from optimized weight
                double raw_contracts = optimized_positions[i] / weights_per_contract[i];
                // Round to integer contracts
                int rounded_contracts = static_cast<int>(std::round(raw_contracts));

                // Distribute proportionally based on each strategy's original contribution
                double total_original = total_contribs[symbol];
                
                for (auto& [strat_id, info] : strategies_) {
                    if (!info.use_optimization)
                        continue;
                    if (!info.target_positions.count(symbol))
                        continue;
                    
                    // Calculate this strategy's share of the optimized position
                    double share = 0.0;
                    if (total_original > 1e-8 && original_contribs[symbol].count(strat_id) > 0) {
                        share = original_contribs[symbol][strat_id] / total_original;
                    }
                    
                    // Distribute proportionally, then undo allocation scaling for storage
                    // Strategy gets: (optimized_contracts * share) / allocation
                    double strategy_contracts = rounded_contracts * share / info.allocation;
                    info.target_positions[symbol].quantity = 
                        static_cast<Decimal>(std::round(strategy_contracts));
                }
            }

            // Log final positions after optimization
            DEBUG("Final optimized positions:");
            for (size_t i = 0; i < symbols.size(); ++i) {
                const std::string& symbol = symbols[i];
                double original_position = 0.0;
                double optimized_position = 0.0;

                // Sum up positions across all strategies
                for (const auto& [_, info] : strategies_) {
                    if (info.target_positions.count(symbol) > 0) {
                        optimized_position +=
                            static_cast<double>(info.target_positions.at(symbol).quantity) *
                            info.allocation;
                    }
                }

                // Get original position from before optimization (stored in your trading data)
                for (const auto& [_, info] : strategies_) {
                    auto trend_strategy =
                        std::dynamic_pointer_cast<TrendFollowingStrategy>(info.strategy);
                    if (trend_strategy) {
                        const auto& data = trend_strategy->get_all_instrument_data();
                        auto it = data.find(symbol);
                        if (it != data.end()) {
                            original_position = it->second.final_position;
                            break;
                        }
                    }
                }

                INFO("Symbol " + symbol + ": raw=" + std::to_string(original_position) +
                     ", optimized=" + std::to_string(optimized_position) +
                     ", change=" + std::to_string(optimized_position - original_position));
            }
        }  // End of mutex lock scope

        INFO("Position optimization completed successfully");
        return Result<void>();

    } catch (const std::exception& e) {
        ERROR("Error during optimization: " + std::string(e.what()));
        return make_error<void>(ErrorCode::UNKNOWN_ERROR,
                                std::string("Error during optimization: ") + e.what(),
                                "PortfolioManager");
    }
}

Result<void> PortfolioManager::apply_risk_management(const std::vector<Bar>& data) {
    Logger::register_component("RiskManager");
    // Use external risk manager if available, otherwise use internal manager
    RiskManager* active_manager =
        external_risk_manager_ ? external_risk_manager_.get() : risk_manager_.get();

    if (!active_manager) {
        WARN("Risk manager not initialized, skipping risk management");
        return Result<void>();
    } else {
        INFO("Using risk manager");
    }

    try {
        for (auto const& bar : data) {
            risk_history_.push_back(bar);
        }
        size_t lookback = config_.risk_config.lookback_period;
        if (risk_history_.size() > lookback) {
            // keep only the last 'lookback' bars
            risk_history_.erase(risk_history_.begin(),
                                risk_history_.end() - static_cast<long>(lookback));
        }

        MarketData market_data = active_manager->create_market_data(risk_history_);

        // Collect all positions
        auto portfolio_positions = get_portfolio_positions();

        // Check if we have positions to process
        if (portfolio_positions.empty()) {
            INFO("No positions to apply risk management to");
            return Result<void>();
        }

        // Collect volatility from strategies under lock
        std::unordered_map<std::string, double> volatilities;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& [id, info] : strategies_) {
                auto trend_strategy =
                    std::dynamic_pointer_cast<TrendFollowingStrategy>(info.strategy);
                if (trend_strategy) {
                    const auto& trading_data = trend_strategy->get_all_instrument_data();
                    for (const auto& [symbol, data] : trading_data) {
                        volatilities[symbol] = data.current_volatility;
                    }
                }
            }
        }

        // Apply risk management with proper error handling
        try {
            auto result = active_manager->process_positions(portfolio_positions, market_data, {});
            if (result.is_error()) {
                ERROR("Risk management calculation failed: " + std::string(result.error()->what()));
                return Result<void>();  // Don't fail the entire operation
            }

            // Apply risk scaling if necessary
            const auto& risk_result = result.value();
            INFO("Risk management result: risk_exceeded=" +
                 std::to_string(risk_result.risk_exceeded) +
                 ", scale=" + std::to_string(risk_result.recommended_scale) +
                 ", portfolio_mult=" + std::to_string(risk_result.portfolio_multiplier) +
                 ", jump_mult=" + std::to_string(risk_result.jump_multiplier) +
                 ", correlation_mult=" + std::to_string(risk_result.correlation_multiplier) +
                 ", leverage_mult=" + std::to_string(risk_result.leverage_multiplier));

            if (risk_result.risk_exceeded) {
                WARN("Risk limits exceeded, scaling positions by " +
                     std::to_string(risk_result.recommended_scale));

                // DESIGN DECISION: Risk scaling applies to target_positions only (Approach A)
                // Rationale: Risk management reduces the strategy's desired exposure, not actual holdings.
                // When current_positions ≈ target_positions (normal case), this behaves correctly.
                // Edge cases (current ≠ target) result in slightly more aggressive de-risking,
                // which is acceptable for risk management purposes.
                // Alternative Approach B (scale both current and target) would provide immediate
                // proportional de-risking but changes "what we think we hold" which could confuse
                // PnL tracking. We keep Approach A for consistency and simplicity.
                //
                // Example: current=+12, target=+10, scale=0.5
                //   new_target = 10 × 0.5 = +5
                //   trade = 5 - 12 = sell 7 contracts
                //   end position = +5 (50% of desired, not 50% of actual)
                
                // Scale positions in all strategies under lock
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    for (auto& [id, info] : strategies_) {
                        for (auto& [symbol, pos] : info.target_positions) {
                            pos.quantity *= risk_result.recommended_scale;
                        }
                    }
                }
            } else {
                INFO("Risk limits not exceeded, no scaling needed");
            }
        } catch (const std::exception& e) {
            ERROR("Exception during risk management: " + std::string(e.what()));
            return Result<void>();  // Don't fail the entire operation
        }

        INFO("Risk management applied successfully");
        return Result<void>();

    } catch (const std::exception& e) {
        ERROR("Error during risk management: " + std::string(e.what()));
        return make_error<void>(ErrorCode::UNKNOWN_ERROR,
                                std::string("Error during risk management: ") + e.what(),
                                "PortfolioManager");
    }
}

Result<void> PortfolioManager::update_allocations(
    const std::unordered_map<std::string, double>& allocations) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto validation = validate_allocations(allocations);
    if (validation.is_error()) {
        return validation;
    }

    // Update allocations and scale positions accordingly
    for (auto& [id, info] : strategies_) {
        if (allocations.count(id)) {
            double scale = allocations.at(id) / info.allocation;
            info.allocation = allocations.at(id);

            // Scale positions
            for (auto& [symbol, pos] : info.target_positions) {
                pos.quantity *= scale;
            }
        }
    }

    return Result<void>();
}

std::unordered_map<std::string, Position> PortfolioManager::get_portfolio_positions() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::unordered_map<std::string, Position> portfolio_positions;

    for (const auto& [_, info] : strategies_) {
        for (const auto& [symbol, pos] : info.target_positions) {
            if (portfolio_positions.count(symbol) == 0) {
                portfolio_positions[symbol] = pos;
                portfolio_positions[symbol].quantity *= info.allocation;
            } else {
                portfolio_positions[symbol].quantity += pos.quantity * info.allocation;
            }
        }
    }

    return portfolio_positions;
}

std::unordered_map<std::string, double> PortfolioManager::get_required_changes() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::unordered_map<std::string, double> changes;

    for (const auto& [_, info] : strategies_) {
        for (const auto& [symbol, target] : info.target_positions) {
            double current = info.current_positions.count(symbol)
                                 ? static_cast<double>(info.current_positions.at(symbol).quantity)
                                 : 0.0;

            changes[symbol] = (static_cast<double>(target.quantity) - current) * info.allocation;
        }
    }

    return changes;
}

Result<void> PortfolioManager::validate_allocations(
    const std::unordered_map<std::string, double>& allocations) const {
    if (allocations.empty() || strategies_.empty()) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT, "No strategies to allocate to",
                                "PortfolioManager");
    }

    double total = 0.0;

    for (const auto& [id, allocation] : allocations) {
        if (strategies_.find(id) == strategies_.end()) {
            return make_error<void>(ErrorCode::INVALID_ARGUMENT, "Strategy " + id + " not found",
                                    "PortfolioManager");
        }

        if (allocation < config_.min_strategy_allocation ||
            allocation > config_.max_strategy_allocation) {
            return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                    "Allocation for " + id + " out of bounds", "PortfolioManager");
        }

        total += allocation;
    }

    if (std::abs(total - 1.0) > 1e-6) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT, "Allocations must sum to 1.0",
                                "PortfolioManager");
    }

    return Result<void>();
}

std::vector<ExecutionReport> PortfolioManager::get_recent_executions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return recent_executions_;
}

std::unordered_map<std::string, std::vector<ExecutionReport>> PortfolioManager::get_strategy_executions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return strategy_executions_;
}

void PortfolioManager::clear_execution_history() {
    std::lock_guard<std::mutex> lock(mutex_);
    // Only clear portfolio-level executions (recent_executions_)
    // DO NOT clear strategy_executions_ - they need to accumulate across all periods
    // for saving at the end of the backtest
    recent_executions_.clear();
    // strategy_executions_ is NOT cleared here - it accumulates for final save
}

void PortfolioManager::clear_all_executions() {
    std::lock_guard<std::mutex> lock(mutex_);
    // Clear both portfolio-level and strategy-level executions
    // Used during warmup to ensure no executions from warmup period persist
    recent_executions_.clear();
    strategy_executions_.clear();
}

std::vector<std::shared_ptr<StrategyInterface>> PortfolioManager::get_strategies() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::shared_ptr<StrategyInterface>> result;
    result.reserve(strategies_.size());

    for (const auto& [_, info] : strategies_) {
        result.push_back(info.strategy);
    }

    return result;
}

std::unordered_map<std::string, std::unordered_map<std::string, Position>> PortfolioManager::get_strategy_positions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::unordered_map<std::string, std::unordered_map<std::string, Position>> result;
    
    for (const auto& [strategy_id, info] : strategies_) {
        result[strategy_id] = info.current_positions;  // These are the optimized positions
    }
    
    return result;
}

double PortfolioManager::get_portfolio_value(
    const std::unordered_map<std::string, double>& current_prices) const {
    static int call_count = 0;
    call_count++;

    // Start with total capital (reserve is for margin, not excluded from portfolio value)
    double portfolio_value = static_cast<double>(config_.total_capital);

    if (call_count <= 3) {
        INFO("PV_CALL #" + std::to_string(call_count) + ": starting with total capital: $" +
              std::to_string(portfolio_value));
    }

    DEBUG("Portfolio value calculation starting with total capital: $" +
          std::to_string(portfolio_value));

    // Acquire the mutex and get a copy of the portfolio positions
    std::unordered_map<std::string, Position> positions_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        positions_copy = get_positions_internal();
    }

    // Log position PnL values for first call only
    if (call_count == 1) {
        double total_rpnl = 0.0;
        for (const auto& [symbol, pos] : positions_copy) {
            double rpnl = static_cast<double>(pos.realized_pnl);
            total_rpnl += rpnl;
            INFO("FIRST_PV: " + symbol + " realized_pnl=$" + std::to_string(rpnl) +
                  " qty=" + std::to_string(static_cast<double>(pos.quantity)));
        }
        INFO("FIRST_PV TOTAL realized_pnl=$" + std::to_string(total_rpnl));
    }

    DEBUG("Found " + std::to_string(positions_copy.size()) +
          " positions for portfolio value calculation");
    DEBUG("Available current prices for " + std::to_string(current_prices.size()) + " symbols");

    // Process the positions outside of the mutex lock
    int positions_with_prices = 0;
    int positions_without_prices = 0;

    for (const auto& [symbol, pos] : positions_copy) {
        auto it = current_prices.find(symbol);
        if (it != current_prices.end()) {
            // Calculate unrealized P&L using current market price
            double current_price = it->second;
            double avg_price = static_cast<double>(pos.average_price);
            double quantity = static_cast<double>(pos.quantity);

            // Only calculate fresh unrealized if position has non-zero unrealized stored
            // For REALIZED_ONLY accounting (futures), unrealized_pnl is always 0
            // and this fresh calculation would be incorrect (missing point_value multiplier)
            if (std::abs(static_cast<double>(pos.unrealized_pnl)) > 1e-6) {
                // For equities: unrealized_pnl = quantity * (current_price - avg_price)
                double unrealized_pnl = quantity * (current_price - avg_price);
                portfolio_value += unrealized_pnl;
            }
            positions_with_prices++;
        } else {
            // If no current price available, use the stored unrealized P&L
            WARN("No current price available for position " + symbol + ", using stored P&L: $" +
                 std::to_string(static_cast<double>(pos.unrealized_pnl)));
            portfolio_value += static_cast<double>(pos.unrealized_pnl);
            positions_without_prices++;
        }

        // Add realized P&L for this position
        double rpnl = static_cast<double>(pos.realized_pnl);
        portfolio_value += rpnl;
    }

    if (positions_without_prices > 0) {
        DEBUG("Portfolio valuation: " + std::to_string(positions_with_prices) +
              " positions with current prices, " + std::to_string(positions_without_prices) +
              " positions using stored P&L");
    }

    DEBUG("Final portfolio value: $" + std::to_string(portfolio_value));
    return portfolio_value;
}

std::unordered_map<std::string, Position> PortfolioManager::get_positions_internal() const {
    // This method is called with the mutex already locked
    std::unordered_map<std::string, Position> portfolio_positions;

    for (const auto& [_, info] : strategies_) {
        // CRITICAL FIX: Read positions directly from strategy (with P&L) instead of cached
        // target_positions
        const auto& strategy_positions = info.strategy->get_positions();
        for (const auto& [symbol, pos] : strategy_positions) {
            if (portfolio_positions.count(symbol) == 0) {
                portfolio_positions[symbol] = pos;
                portfolio_positions[symbol].quantity *= info.allocation;
                portfolio_positions[symbol].realized_pnl *= Decimal(info.allocation);
                portfolio_positions[symbol].unrealized_pnl *= Decimal(info.allocation);
            } else {
                portfolio_positions[symbol].quantity += pos.quantity * info.allocation;
                portfolio_positions[symbol].realized_pnl +=
                    pos.realized_pnl * Decimal(info.allocation);
                portfolio_positions[symbol].unrealized_pnl +=
                    pos.unrealized_pnl * Decimal(info.allocation);
            }
        }
    }

    return portfolio_positions;
}

}  // namespace trade_ngin