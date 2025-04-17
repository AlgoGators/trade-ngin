// src/portfolio/portfolio_manager.cpp
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include <set>

namespace trade_ngin {

PortfolioManager::PortfolioManager(PortfolioConfig config, std::string id, InstrumentRegistry* registry)
: config_(std::move(config)), id_(std::move(id)), registry_(std::move(registry)) {

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
            risk_manager_ = nullptr; // Explicitly set to nullptr
        }
    } else {
        INFO("Risk management is disabled in the configuration");
    }
    
    // Initialize with the provided ID
    ComponentInfo info{
        ComponentType::PORTFOLIO_MANAGER,
        ComponentState::INITIALIZED,
        id_,  // Use the provided ID
        "",
        std::chrono::system_clock::now(),
        {
            {"total_capital", config_.total_capital},
            {"reserve_capital", config_.reserve_capital}
        }
    };

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
        }
        else if (event.type == MarketDataEventType::BAR) {
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

    SubscriberInfo sub_info{
        "PORTFOLIO_MANAGER",
        {MarketDataEventType::BAR, MarketDataEventType::POSITION_UPDATE},
        {},  // Subscribe to all symbols
        callback
    };

    auto subscribe_result = MarketDataBus::instance().subscribe(sub_info);
    if (subscribe_result.is_error()) {
        throw std::runtime_error(subscribe_result.error()->what());
    }

    StateManager::instance().update_state("PORTFOLIO_MANAGER", ComponentState::RUNNING);
}

Result<void> PortfolioManager::add_strategy(
    std::shared_ptr<StrategyInterface> strategy,
    double initial_allocation,
    bool use_optimization,
    bool use_risk_management) {
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!strategy) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Strategy cannot be null",
            "PortfolioManager"
        );
    }
    
    const auto& metadata = strategy->get_metadata();
    
    if (strategies_.find(metadata.id) != strategies_.end()) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Strategy with ID " + metadata.id + " already exists",
            "PortfolioManager"
        );
    }
    
    if (initial_allocation < config_.min_strategy_allocation ||
        initial_allocation > config_.max_strategy_allocation) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Initial allocation out of bounds",
            "PortfolioManager"
        );
    }
    
    // Validate total allocation doesn't exceed 1
    double total_allocation = initial_allocation;
    for (const auto& [_, info] : strategies_) {
        total_allocation += info.allocation;
    }
    
    if (total_allocation > 1.0) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Total allocation would exceed 1.0",
            "PortfolioManager"
        );
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

Result<void> PortfolioManager::process_market_data(const std::vector<Bar>& data) {
    std::vector<std::string> processed_strategies;
    
    try {
        std::unordered_map<std::string, std::unordered_map<std::string, Position>> prev_positions;
        {
            std::lock_guard<std::mutex> lock(mutex_);

            // Validate market data
            if (data.empty()) {
                ERROR("Empty market data provided");
                return make_error<void>(
                    ErrorCode::MARKET_DATA_ERROR,
                    "Empty market data provided",
                    "PortfolioManager"
                );
            }

            // Update historical returns for all symbols
            update_historical_returns(data);

            // Store current positions for each strategy to detect changes
            for (const auto& [id, info] : strategies_) {
                prev_positions[id] = info.current_positions;
            }

            // Process data through each strategy
            for (auto& [id, info] : strategies_) {
                Logger::register_component(info.strategy->get_metadata().name);
                if (!info.strategy) {
                    ERROR("Null strategy pointer found for ID: " + id);
                    return make_error<void>(
                        ErrorCode::INVALID_ARGUMENT,
                        "Null strategy pointer found for ID: " + id,
                        "PortfolioManager"
                    );
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
                    
                    // Check if it's a TrendFollowingStrategy and use instrument data if available
                    auto trend_strategy = std::dynamic_pointer_cast<TrendFollowingStrategy>(info.strategy);
                    if (trend_strategy) {
                        // Access the instrument data directly
                        const auto& trading_data = trend_strategy->get_all_instrument_data();
                        
                        // Use final positions from instrument data
                        for (const auto& [symbol, symbol_data] : trading_data) {
                            Position pos;
                            pos.symbol = symbol;
                            pos.quantity = symbol_data.final_position;
                            
                            // Use the latest price
                            pos.average_price = symbol_data.price_history.empty() ? 
                                1.0 : symbol_data.price_history.back();
                            
                            // Use latest timestamp if available
                            pos.last_update = symbol_data.last_update;
                            info.target_positions[symbol] = pos;
                        }
                    } else {
                        // For non-trend strategies, use the regular positions
                        info.target_positions = info.strategy->get_positions();
                    }
                    
                    processed_strategies.push_back(id);
                    
                } catch (const std::exception& e) {
                    ERROR("Exception processing strategy " + id + ": " + std::string(e.what()));
                    continue;
                }
            }
        }

        // Log before potential optimization
        if (config_.use_optimization && optimizer_) {
            try {
                Logger::register_component("DynamicOptimizer");
                auto opt_result = optimize_positions();
                if (opt_result.is_error()) {
                    WARN("Portfolio optimization failed: " + std::string(opt_result.error()->what()) + 
                         ", continuing without optimization");
                }
            } catch (const std::exception& e) {
                WARN("Exception during portfolio optimization: " + std::string(e.what()) + 
                     ", continuing without optimization");
            }
        }
        
        // Log before risk management
        bool has_risk_manager = (external_risk_manager_ != nullptr) || (risk_manager_ != nullptr);
        if (config_.use_risk_management && has_risk_manager) {
            try {
                Logger::register_component("RiskManager");
                auto risk_result = apply_risk_management(data);
                if (risk_result.is_error()) {
                    WARN("Portfolio risk management failed: " + std::string(risk_result.error()->what()) + 
                         ", continuing without risk management");
                } else {
                    INFO("Portfolio risk management applied successfully");
                }
            } catch (const std::exception& e) {
                WARN("Exception during risk management: " + std::string(e.what()) + 
                     ", continuing without risk management");
            }
        } else {
            INFO("Risk management not enabled, skipping risk checks");
            INFO("Use risk manager check: " + std::to_string(config_.use_risk_management));
            INFO("Risk manager check: " + std::to_string(risk_manager_ != nullptr));
        }

        {
            auto post_opt = get_portfolio_positions();
            std::ostringstream oss3;
            for (const auto& [symbol, pos] : post_opt) {
                oss3 << symbol << ": " << pos.quantity << ", ";
            }
            DEBUG("Post-optimization positions: " + oss3.str());
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);

            // Generate execution reports for position changes
            for (auto& [id, info] : strategies_) {
                Logger::register_component(info.strategy->get_metadata().name);
                for (const auto& [symbol, new_pos] : info.target_positions) {
                    double current_qty = 0.0;
                    
                    // Get previous position quantity
                    auto prev_pos_it = prev_positions[id].find(symbol);
                    if (prev_pos_it != prev_positions[id].end()) {
                        current_qty = prev_pos_it->second.quantity;
                    }
                    
                    // If position changed, create execution report
                    if (std::abs(new_pos.quantity - current_qty) > 1e-6) {
                        // Calculate trade size
                        double trade_size = new_pos.quantity - current_qty;
                        Side side = trade_size > 0 ? Side::BUY : Side::SELL;
                        
                        // Find latest price for symbol
                        double latest_price = 0.0;
                        for (const auto& bar : data) {
                            if (bar.symbol == symbol) {
                                latest_price = bar.close;
                                break;
                            }
                        }
                        
                        if (latest_price == 0.0) {
                            continue; // Skip if price not available
                        }
                        
                        // Create execution report
                        ExecutionReport exec;
                        exec.order_id = "PM-" + id + "-" + std::to_string(recent_executions_.size());
                        exec.exec_id = "EX-" + id + "-" + std::to_string(recent_executions_.size());
                        exec.symbol = symbol;
                        exec.side = side;
                        exec.filled_quantity = std::abs(trade_size);
                        exec.fill_price = latest_price;
                        exec.fill_time = data.empty() ? std::chrono::system_clock::now() : data[0].timestamp;
                        exec.commission = 0.0; // Commission calculation would be done by backtest engine
                        exec.is_partial = false;
                        
                        // Add to recent executions
                        recent_executions_.push_back(exec);
                    }
                }
            }   
        }
        
        
        return Result<void>();
        
    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Error processing market data: ") + e.what(),
            "PortfolioManager"
        );
    }
}

std::vector<double> PortfolioManager::calculate_weights_per_contract(
    const std::vector<std::string>& symbols,
    double capital) const {
    
    std::vector<double> weights_per_contract(symbols.size(), 0.0);
    
    for (size_t i = 0; i < symbols.size(); ++i) {
        const std::string& symbol = symbols[i];
        
        // Get contract multiplier/size for this instrument
        double contract_size = 1.0;  // Default
        double price = 1.0;  // Default
        double fx_rate = 1.0;  // Default exchange rate
        
        // Look up instrument details if available
        if (registry_ && registry_->has_instrument(symbol)) {
            auto instrument = registry_->get_instrument(symbol);
            contract_size = instrument->get_multiplier();
            
            // Get latest price from positions or market data
            auto pos_it = get_portfolio_positions().find(symbol);
            if (pos_it != get_portfolio_positions().end()) {
                price = pos_it->second.average_price;
            }
        }
        
        // Calculate weight per contract = (notional per contract) / capital
        double notional_per_contract = contract_size * price * fx_rate;
        weights_per_contract[i] = notional_per_contract / capital;
        
        // Ensure weight is positive and reasonable
        if (weights_per_contract[i] <= 0.0 || std::isnan(weights_per_contract[i])) {
            WARN("Invalid weight per contract for " + symbol + 
                 ", using default of 0.01");
            weights_per_contract[i] = 0.01;  // Reasonable default
        }
    }
    
    return weights_per_contract;
}

std::vector<double> PortfolioManager::calculate_trading_costs(
    const std::vector<std::string>& symbols,
    double capital) const {
    
    std::vector<double> costs(symbols.size(), 0.0);
    
    for (size_t i = 0; i < symbols.size(); ++i) {
        const std::string& symbol = symbols[i];
        
        // Default cost as a proportion of capital (e.g., 5 basis points)
        double cost_proportion = 0.0005;
        
        // Look up specific costs if available in strategy config
        for (const auto& [strategy_id, info] : strategies_) {
            const auto& strategy_config = info.strategy->get_config();
            if (strategy_config.costs.count(symbol) > 0) {
                cost_proportion = strategy_config.costs.at(symbol);
                break;
            }
        }
        
        // Get contract details
        double contract_size = 1.0;
        double price = 1.0;
        double fx_rate = 1.0;
        
        // Look up instrument details if available
        if (registry_ && registry_->has_instrument(symbol)) {
            auto instrument = registry_->get_instrument(symbol);
            contract_size = instrument->get_multiplier();
            
            // Get latest price
            auto pos_it = get_portfolio_positions().find(symbol);
            if (pos_it != get_portfolio_positions().end()) {
                price = pos_it->second.average_price;
            }
        }
        
        // Convert cost to weight terms
        double cost_per_contract = cost_proportion * contract_size * price * fx_rate;
        costs[i] = cost_per_contract / capital;
    }
    
    return costs;
}

void PortfolioManager::update_historical_returns(const std::vector<Bar>& data) {
    if (data.empty()) return;
        
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
            INFO("Retrieved price history from strategy " + id + " for " 
                 + std::to_string(price_history.size()) + " symbols");
            
            // For each symbol, update our price history
            for (const auto& [symbol, prices] : price_history) {
                // Only process if this symbol is in the current data (for efficiency)
                if (data_symbols.count(symbol) > 0) {
                    // Update our price history with the strategy's data
                    price_history_[symbol] = prices;
                    
                    DEBUG("Updated price history for " + symbol + " with " 
                          + std::to_string(prices.size()) + " points");
                    
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
            double prev_price = prices[i-1];
            double curr_price = prices[i];
            
            if (prev_price <= 0.0) continue;
            
            double ret = (curr_price - prev_price) / prev_price;
            
            if (std::isfinite(ret)) {
                historical_returns_[symbol].push_back(ret);
            }
        }
        
        DEBUG("Calculated " + std::to_string(historical_returns_[symbol].size()) + 
              " returns for symbol " + symbol + " from " + 
              std::to_string(prices.size()) + " prices");
        
        // Limit history length if needed
        if (historical_returns_[symbol].size() > max_history_length_) {
            // Keep only the most recent returns
            size_t excess = historical_returns_[symbol].size() - max_history_length_;
            historical_returns_[symbol].erase(
                historical_returns_[symbol].begin(), 
                historical_returns_[symbol].begin() + excess
            );
        }
    }
    
    // Log aggregate stats
    size_t total_returns = 0;
    for (const auto& [symbol, returns] : historical_returns_) {
        total_returns += returns.size();
    }
    
    INFO("Total historical data: " + std::to_string(total_returns) + 
         " returns across " + std::to_string(historical_returns_.size()) + " symbols");
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
    
    if (min_periods == SIZE_MAX) min_periods = 0;
    
    if (min_periods < 20) {  // Need sufficient data
        WARN("Insufficient return data for covariance calculation: " + std::to_string(min_periods) + " periods");
        // Return diagonal matrix with default variance
        std::vector<std::vector<double>> default_cov(num_assets, std::vector<double>(num_assets, 0.0));
        for (size_t i = 0; i < num_assets; ++i) {
            default_cov[i][i] = 0.01;  // Default variance on diagonal
        }
        return default_cov;
    }

    // Create a matrix of aligned returns
    std::vector<std::vector<double>> aligned_returns(min_periods, std::vector<double>(num_assets, 0.0));
    
    for (size_t i = 0; i < num_assets; ++i) {
        const auto& symbol = ordered_symbols[i];
        const auto& returns = returns_by_symbol.at(symbol);
        
        // Take the most recent min_periods returns
        size_t start_idx = returns.size() - min_periods;
        for (size_t j = 0; j < min_periods; ++j) {
            aligned_returns[j][i] = returns[start_idx + j];
        }
    }
    
    // Calculate means for each asset
    std::vector<double> means(num_assets, 0.0);
    for (size_t i = 0; i < num_assets; ++i) {
        const auto& returns = returns_by_symbol.at(ordered_symbols[i]);
        for (size_t t = 0; t < min_periods; ++t) {
            means[i] += returns[t];
        }
        means[i] /= min_periods;
    }
    
    // Calculate covariance matrix
    std::vector<std::vector<double>> covariance(num_assets, std::vector<double>(num_assets, 0.0));
    for (size_t i = 0; i < num_assets; ++i) {
        for (size_t j = 0; j < num_assets; ++j) {
            double cov_sum = 0.0;
            for (size_t t = 0; t < min_periods; ++t) {
                cov_sum += (aligned_returns[t][i] - means[i]) * (aligned_returns[t][j] - means[j]);
            }
            
            covariance[i][j] = cov_sum / (min_periods - 1);
            
            // Annualize the covariance (assuming daily data with 252 trading days)
            covariance[i][j] *= 252.0;
        }
    }
    
    return covariance;
}

Result<void> PortfolioManager::optimize_positions() {
    try {
        // Get unique symbols across all strategies
        std::set<std::string> unique_symbols;
        for (const auto& [_, info] : strategies_) {
            if (!info.use_optimization) continue;
            
            for (const auto& [symbol, _] : info.target_positions) {
                unique_symbols.insert(symbol);
            }
        }
        
        // Convert to ordered vector
        std::vector<std::string> symbols(unique_symbols.begin(), unique_symbols.end());
        
        if (symbols.empty()) {
            return Result<void>();
        }

        int min_history_length = 20; // Minimum history length for covariance calculation
        bool have_sufficient_data = true;

        for (const auto& symbol : symbols) {
            if (historical_returns_.find(symbol) == historical_returns_.end() ||
                historical_returns_[symbol].size() < min_history_length) {
                have_sufficient_data = false;
                WARN("Insufficient historical returns for " + symbol + 
                     ": " + std::to_string(historical_returns_[symbol].size()) + 
                     " (need " + std::to_string(min_history_length) + ")");
            }
        }

        if (!have_sufficient_data) {
            INFO("Not enough historical data yet for optimization. Using target positions directly.");
            // Just apply target positions without optimization
            return Result<void>();
        }

        // Collect all instrument data
        std::unordered_map<std::string, const InstrumentData*> all_trading_data;
        for (const auto& [id, info] : strategies_) {
            auto trend_strategy = std::dynamic_pointer_cast<TrendFollowingStrategy>(info.strategy);
            if (trend_strategy) {
                const auto& strategy_data = trend_strategy->get_all_instrument_data();
                for (const auto& [symbol, data] : strategy_data) {
                    all_trading_data[symbol] = &data;
                }
            }
        }
        
        // Calculate weights per contract
        std::vector<double> weights_per_contract(symbols.size(), 0.0);

        for (size_t i = 0; i < symbols.size(); ++i) {
            const std::string& symbol = symbols[i];

            auto it = all_trading_data.find(symbol);
            if (it != all_trading_data.end()) {
                const auto& data = *(it->second);

                double contract_size = data.contract_size;
                double price = data.price_history.empty() ? 1.0 : data.price_history.back();
                double fx_rate = 1.0;

                double notional_per_contract = contract_size * price * fx_rate;
                weights_per_contract[i] = notional_per_contract / config_.total_capital;
            } else {
                weights_per_contract = calculate_weights_per_contract(symbols, config_.total_capital);
            }

            // Ensure weight is positive and reasonable
            if (weights_per_contract[i] <= 0.0 || std::isnan(weights_per_contract[i])) {
                WARN("Invalid weight per contract for " + symbol + 
                     ", using default of 0.01");
                weights_per_contract[i] = 0.01;  // Reasonable default
            }
        }
            
        // Calculate trading costs
        std::vector<double> costs = calculate_trading_costs(
            symbols, config_.total_capital);
            
        // Build current and target positions in weight terms
        std::vector<double> current_positions(symbols.size(), 0.0);
        std::vector<double> target_positions(symbols.size(), 0.0);
        
        for (size_t i = 0; i < symbols.size(); ++i) {
            const std::string& symbol = symbols[i];
            
            // Aggregate across strategies
            for (const auto& [_, info] : strategies_) {
                if (!info.use_optimization) continue;
                
                // Get current position
                if (info.current_positions.count(symbol) > 0) {
                    current_positions[i] += info.current_positions.at(symbol).quantity * 
                                           weights_per_contract[i] * info.allocation;
                }
                
                // Get target position
                if (info.target_positions.count(symbol) > 0) {
                    target_positions[i] += info.target_positions.at(symbol).quantity * 
                                          weights_per_contract[i] * info.allocation;
                }
            }
        }
        
        // Calculate covariance matrix
        std::vector<std::vector<double>> covariance = calculate_covariance_matrix(historical_returns_);
        
        // Call the optimizer
        if (!optimizer_) {
            ERROR("Optimizer not initialized");
            return make_error<void>(
                ErrorCode::NOT_INITIALIZED,
                "Optimizer not initialized",
                "PortfolioManager"
            );
        }
        
        auto result = optimizer_->optimize(
            current_positions,
            target_positions,
            costs,
            weights_per_contract,
            covariance
        );
        
        if (result.is_error()) {
            return make_error<void>(
                result.error()->code(),
                "Optimization failed: " + std::string(result.error()->what()),
                "PortfolioManager"
            );
        }

        // Log optimization result metrics
        INFO("Optimization metrics: tracking error=" + std::to_string(result.value().tracking_error) + 
        ", cost=" + std::to_string(result.value().cost_penalty) + 
        ", iterations=" + std::to_string(result.value().iterations));
        
        // Apply optimized positions back to strategies
        const auto& optimized_positions = result.value().positions;
        
        for (size_t i = 0; i < symbols.size(); ++i) {
            const std::string& symbol = symbols[i];
            double optimized_weight = optimized_positions[i];
            
            // Convert weight back to contracts
            double optimized_contracts = std::round(optimized_weight / weights_per_contract[i]);
            
            // Distribute across strategies proportionally
            double total_target = 0.0;
            for (const auto& [_, info] : strategies_) {
                if (!info.use_optimization) continue;
                
                if (info.target_positions.count(symbol) > 0) {
                    total_target += std::abs(info.target_positions.at(symbol).quantity * info.allocation);
                }
            }
            
            if (total_target > 0.0) {
                for (auto& [_, info] : strategies_) {
                    if (!info.use_optimization) continue;
                    
                    if (info.target_positions.count(symbol) > 0) {
                        double proportion = (info.target_positions.at(symbol).quantity * info.allocation) / total_target;
                        double new_contracts = optimized_contracts * proportion;
                        
                        // Update target position
                        info.target_positions[symbol].quantity = new_contracts / info.allocation;
                    }
                }
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
                    optimized_position += info.target_positions.at(symbol).quantity * info.allocation;
                }
            }
            
            // Get original position from before optimization (stored in your trading data)
            for (const auto& [_, info] : strategies_) {
                auto trend_strategy = std::dynamic_pointer_cast<TrendFollowingStrategy>(info.strategy);
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
        
        INFO("Position optimization completed successfully");
        return Result<void>();
        
    } catch (const std::exception& e) {
        ERROR("Error during optimization: " + std::string(e.what()));
        return make_error<void>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Error during optimization: ") + e.what(),
            "PortfolioManager"
        );
    }
}

Result<void> PortfolioManager::apply_risk_management(const std::vector<Bar>& data) {
    Logger::register_component("RiskManager");
    // Use external risk manager if available, otherwise use internal manager
    RiskManager* active_manager = external_risk_manager_ ? external_risk_manager_ : risk_manager_.get();

    if (!active_manager) {
        WARN("Risk manager not initialized, skipping risk management");
        return Result<void>();
    } else {
        INFO("Using risk manager");
    }
    
    try {
        for (auto const& bar: data) {
            risk_history_.push_back(bar);
        }
        size_t lookback = config_.risk_config.lookback_period;
        if (risk_history_.size() > lookback) {
            // keep only the last 'lookback' bars
            risk_history_.erase(
                risk_history_.begin(),
                risk_history_.end() - static_cast<long>(lookback)
            );
        }

        MarketData market_data = active_manager->create_market_data(risk_history_);

        // Collect all positions
        auto portfolio_positions = get_portfolio_positions();
        
        // Check if we have positions to process
        if (portfolio_positions.empty()) {
            INFO("No positions to apply risk management to");
            return Result<void>();
        }

        // Collect volatility from strategies
        std::unordered_map<std::string, double> volatilities;
        for (const auto& [id, info] : strategies_) {
            auto trend_strategy = std::dynamic_pointer_cast<TrendFollowingStrategy>(info.strategy);
            if (trend_strategy) {
                const auto& trading_data = trend_strategy->get_all_instrument_data();
                for (const auto& [symbol, data] : trading_data) {
                    volatilities[symbol] = data.current_volatility;
                }
            }
        }
        
        // Apply risk management with proper error handling
        try {
            auto result = active_manager->process_positions(portfolio_positions, market_data);
            if (result.is_error()) {
                ERROR("Risk management calculation failed: " + 
                    std::string(result.error()->what()));
                return Result<void>(); // Don't fail the entire operation
            }
                    
            // Apply risk scaling if necessary
            const auto& risk_result = result.value();
            INFO("Risk management result: risk_exceeded=" 
                + std::to_string(risk_result.risk_exceeded) + 
                ", scale=" + std::to_string(risk_result.recommended_scale) +
                ", portfolio_mult=" + std::to_string(risk_result.portfolio_multiplier) +
                ", jump_mult=" + std::to_string(risk_result.jump_multiplier) +
                ", correlation_mult=" + std::to_string(risk_result.correlation_multiplier) +
                ", leverage_mult=" + std::to_string(risk_result.leverage_multiplier));

            if (risk_result.risk_exceeded) {
                WARN("Risk limits exceeded, scaling positions by " + 
                    std::to_string(risk_result.recommended_scale));
                
                // Scale positions in all strategies
                for (auto& [id, info] : strategies_) {
                    for (auto& [symbol, pos] : info.target_positions) {
                        pos.quantity *= risk_result.recommended_scale;
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
        return make_error<void>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Error during risk management: ") + e.what(),
            "PortfolioManager"
        );
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
            double current = info.current_positions.count(symbol) ?
                           info.current_positions.at(symbol).quantity : 0.0;
            
            changes[symbol] = (target.quantity - current) * info.allocation;
        }
    }
    
    return changes;
}

Result<void> PortfolioManager::validate_allocations(
    const std::unordered_map<std::string, double>& allocations) const {

    if (allocations.empty() || strategies_.empty()) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "No strategies to allocate to",
            "PortfolioManager"
        );
    }
    
    double total = 0.0;
    
    for (const auto& [id, allocation] : allocations) {
        if (strategies_.find(id) == strategies_.end()) {
            return make_error<void>(
                ErrorCode::INVALID_ARGUMENT,
                "Strategy " + id + " not found",
                "PortfolioManager"
            );
        }
        
        if (allocation < config_.min_strategy_allocation ||
            allocation > config_.max_strategy_allocation) {
            return make_error<void>(
                ErrorCode::INVALID_ARGUMENT,
                "Allocation for " + id + " out of bounds",
                "PortfolioManager"
            );
        }
        
        total += allocation;
    }
    
    if (std::abs(total - 1.0) > 1e-6) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Allocations must sum to 1.0",
            "PortfolioManager"
        );
    }
    
    return Result<void>();
}

std::vector<ExecutionReport> PortfolioManager::get_recent_executions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return recent_executions_;
}

void PortfolioManager::clear_execution_history() {
    std::lock_guard<std::mutex> lock(mutex_);
    recent_executions_.clear();
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

double PortfolioManager::get_portfolio_value(const std::unordered_map<std::string, double>& current_prices) const {    
    // Start with available capital
    double portfolio_value = config_.total_capital - config_.reserve_capital;

    // Acquire the mutex and get a copy of the portfolio positions
    std::unordered_map<std::string, Position> positions_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        positions_copy = get_positions_internal();
    }
    
    // Process the positions outside of the mutex lock
    for (const auto& [symbol, pos] : positions_copy) {
        auto it = current_prices.find(symbol);
        if (it != current_prices.end()) {
            // Use the provided price
            portfolio_value += pos.quantity * it->second;
        } else {
            // Fall back to average price if current price not available
            portfolio_value += pos.quantity * pos.average_price;
        }
    }
    
    return portfolio_value;
}

std::unordered_map<std::string, Position> PortfolioManager::get_positions_internal() const {
    // This method is called with the mutex already locked
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

} // namespace trade_ngin