// src/portfolio/portfolio_manager.cpp
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/state_manager.hpp"
#include "trade_ngin/data/market_data_bus.hpp"
#include <numeric>

namespace trade_ngin {

PortfolioManager::PortfolioManager(PortfolioConfig config)
    : config_(std::move(config)) {
    
    if (config_.use_optimization) {
        optimizer_ = std::make_unique<DynamicOptimizer>(config_.opt_config);
    }
    
    if (config_.use_risk_management) {
        risk_manager_ = std::make_unique<RiskManager>(config_.risk_config);
    }

    // Register with state manager
    ComponentInfo info{
        ComponentType::PORTFOLIO_MANAGER,
        ComponentState::INITIALIZED,
        "PORTFOLIO_MANAGER",
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
    std::lock_guard<std::mutex> lock(mutex_);
    
    try {
        // Process data through each strategy
        for (auto& [id, info] : strategies_) {
            // Store current positions
            info.current_positions = info.strategy->get_positions();
            
            // Process market data through strategy
            auto result = info.strategy->on_data(data);
            if (result.is_error()) {
                ERROR("Error processing data for strategy " + id + ": " + 
                      result.error()->what());
                continue;
            }
            
            // Store target positions
            info.target_positions = info.strategy->get_positions();
        }
        
        // Optimize positions if any strategies use optimization
        if (config_.use_optimization) {
            auto opt_result = optimize_positions();
            if (opt_result.is_error()) {
                return opt_result;
            }
        }
        
        // Apply risk management if enabled
        if (config_.use_risk_management) {
            auto risk_result = apply_risk_management();
            if (risk_result.is_error()) {
                return risk_result;
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

Result<void> PortfolioManager::optimize_positions() {
    // Collect positions and parameters for optimization
    std::vector<std::string> symbols;
    std::unordered_map<std::string, std::vector<double>> strategy_positions;
    std::unordered_map<std::string, double> costs;
    
    // First pass: collect unique symbols and initialize storage
    for (const auto& [id, info] : strategies_) {
        if (!info.use_optimization) continue;
        
        for (const auto& [symbol, pos] : info.target_positions) {
            if (std::find(symbols.begin(), symbols.end(), symbol) == symbols.end()) {
                symbols.push_back(symbol);
                strategy_positions[symbol] = std::vector<double>();
                costs[symbol] = 0.0;  // Will be updated with actual costs
            }
        }
    }
    
    if (symbols.empty()) {
        return Result<void>();  // Nothing to optimize
    }
    
    // Second pass: collect positions and costs for each symbol
    for (const auto& symbol : symbols) {
        std::vector<double> current_pos;
        std::vector<double> target_pos;
        std::vector<double> symbol_costs;
        
        for (const auto& [id, info] : strategies_) {
            if (!info.use_optimization) continue;
            
            double curr = info.current_positions.count(symbol) ? 
                         info.current_positions.at(symbol).quantity : 0.0;
            double targ = info.target_positions.count(symbol) ?
                         info.target_positions.at(symbol).quantity : 0.0;
            
            current_pos.push_back(curr * info.allocation);
            target_pos.push_back(targ * info.allocation);
            
            // Get trading costs from strategy config
            const auto& strategy_config = info.strategy->get_config();
            double cost = strategy_config.costs.count(symbol) ?
                         strategy_config.costs.at(symbol) : 1.0;
            symbol_costs.push_back(cost);
        }
        
        // For each symbol, run optimization
        auto result = optimizer_->optimize_single_period(
            current_pos,
            target_pos,
            symbol_costs,
            std::vector<double>(current_pos.size(), 1.0),  // Equal weights for now
            std::vector<std::vector<double>>(current_pos.size(),  // Identity matrix for now
                std::vector<double>(current_pos.size(), 0.0))
        );
        
        if (result.is_error()) {
            return make_error<void>(
                result.error()->code(),
                "Optimization failed for " + symbol + ": " + result.error()->what(),
                "PortfolioManager"
            );
        }
        
        // Update positions in strategies
        size_t idx = 0;
        for (auto& [id, info] : strategies_) {
            if (!info.use_optimization) continue;
            
            if (info.target_positions.count(symbol)) {
                info.target_positions[symbol].quantity = 
                    result.value().optimized_positions[idx] / info.allocation;
            }
            ++idx;
        }
    }
    
    return Result<void>();
}

Result<void> PortfolioManager::apply_risk_management() {
    if (!risk_manager_) {
        return Result<void>();  // Risk management not enabled
    }
    
    // Collect all positions for portfolio-wide risk calculation
    auto portfolio_positions = get_portfolio_positions();
    
    // Apply risk management
    auto result = risk_manager_->process_positions(portfolio_positions);
    if (result.is_error()) {
        return make_error<void>(
            result.error()->code(),
            "Risk management failed: " + std::string(result.error()->what()),
            "PortfolioManager"
        );
    }
    
    // If risk limits exceeded, scale down positions proportionally
    if (result.value().risk_exceeded) {
        double scale_factor = result.value().recommended_scale;
        
        for (auto& [_, info] : strategies_) {
            if (!info.use_risk_management) continue;
            
            for (auto& [symbol, pos] : info.target_positions) {
                pos.quantity *= scale_factor;
            }
        }
    }
    
    return Result<void>();
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

} // namespace trade_ngin