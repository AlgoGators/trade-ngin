// src/strategy/base_strategy.cpp

#include "trade_ngin/strategy/base_strategy.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/state_manager.hpp"
#include "trade_ngin/data/market_data_bus.hpp"
#include <sstream>
#include <iostream>

namespace trade_ngin {

BaseStrategy::BaseStrategy(std::string id,
                         StrategyConfig config,
                         std::shared_ptr<DatabaseInterface> db)
    : id_(std::move(id))
    , config_(std::move(config))
    , state_(StrategyState::INITIALIZED)
    , db_(std::move(db)) {
    
    // Initialize metadata
    metadata_.id = id_;
    metadata_.name = "Base Strategy";
    metadata_.description = "Base strategy implementation";
}

Result<void> BaseStrategy::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (state_ != StrategyState::INITIALIZED) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Strategy not initialized",
            "BaseStrategy"
        );
    }

    // Validate configuration
    auto validation = validate_config();
    if (validation.is_error()) {
        return validation;
    }

    try {
        // Initialize database connection if needed
        if (!db_) {
            return make_error<void>(
                ErrorCode::NOT_INITIALIZED,
                "Database interface not initialized",
                "BaseStrategy"
            );
        }

        // Register with state manager with proper error handling
        ComponentInfo info{
            ComponentType::STRATEGY,
            ComponentState::INITIALIZED,
            id_,
            "",
            std::chrono::system_clock::now(),
            {
                {"capital_allocation", config_.capital_allocation},
                {"max_leverage", config_.max_leverage}
            }
        };

        // Before registering, ensure any previous registration is cleared
        StateManager::reset_instance();

        auto register_result = StateManager::instance().register_component(info);
        if (register_result.is_error()) {
            return make_error<void>(
                register_result.error()->code(),
                "Failed to register with StateManager: " + 
                std::string(register_result.error()->what()),
                "BaseStrategy"
            );
        }

        // Subscribe to market data with proper error handling
        MarketDataCallback callback = [this](const MarketDataEvent& event) {
            try {
                if (event.type == MarketDataEventType::BAR) {
                    Bar bar;
                    bar.timestamp = event.timestamp;
                    bar.symbol = event.symbol;
                    bar.open = event.numeric_fields.at("open");
                    bar.high = event.numeric_fields.at("high");
                    bar.low = event.numeric_fields.at("low");
                    bar.close = event.numeric_fields.at("close");
                    bar.volume = event.numeric_fields.at("volume");

                    std::vector<Bar> bars{bar};
                    auto result = this->on_data(bars);
                    if (result.is_error()) {
                        ERROR("Error processing bar data: " + 
                            std::string(result.error()->what()));
                    }
                }
            } catch (const std::exception& e) {
                ERROR("Error in market data callback: " + std::string(e.what()));
            }
        };

        SubscriberInfo sub_info{
            id_,
            {MarketDataEventType::BAR},
            {},  // Empty means subscribe to all symbols
            callback
        };

        auto subscribe_result = MarketDataBus::instance().subscribe(sub_info);
        if (subscribe_result.is_error()) {
            return subscribe_result;
        }

        is_initialized_ = true;
        
        INFO("Initialized strategy " + id_);
        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::NOT_INITIALIZED,
            std::string("Initialization failed: ") + e.what(),
            "BaseStrategy"
        );
    }
}

Result<void> BaseStrategy::start() {
    if (!is_initialized_) {
        std::cerr << "Strategy must be initialized before starting" << std::endl;
        return make_error<void>(
            ErrorCode::NOT_INITIALIZED,
            "Strategy must be initialized before starting",
            "BaseStrategy"
        );
    }

    auto result = transition_state(StrategyState::RUNNING);

    if (result.is_ok()) {
        running_ = true;
    } else {
        std::cerr << "Failed to transition to RUNNING state: "
                    << (result.error() ? result.error()->what() : "Unknown error") << std::endl;
    }

    return result;
}

Result<void> BaseStrategy::stop() {
    if (config_.save_positions) {
        auto save_result = save_positions();
        if (save_result.is_error()) {
            WARN("Error saving positions on stop: " + save_result.error()->to_string());
        }
    }

    running_ = false;

    return transition_state(StrategyState::STOPPED);
}

Result<void> BaseStrategy::pause() {
    return transition_state(StrategyState::PAUSED);
}

Result<void> BaseStrategy::resume() {
    if (state_ != StrategyState::PAUSED) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Can only resume from PAUSED state",
            "BaseStrategy"
        );
    }
    return transition_state(StrategyState::RUNNING);
}

Result<void> BaseStrategy::on_data(const std::vector<Bar>& data) {
    if (data.empty()) {
        return Result<void>();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    
    if (state_ != StrategyState::RUNNING) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Strategy not in RUNNING state",
            "BaseStrategy"
        );
    }

    try {
        // Validate data before processing
        for (const auto& bar : data) {
            if (bar.symbol.empty()) {
                return make_error<void>(
                    ErrorCode::INVALID_DATA,
                    "Bar has empty symbol",
                    "BaseStrategy"
                );
            }
            if (bar.timestamp == Timestamp{}) {
                return make_error<void>(
                    ErrorCode::INVALID_DATA,
                    "Bar has invalid timestamp",
                    "BaseStrategy"
                );
            }
        }

        // Update positions based on latest prices
        for (const auto& bar : data) {
            if (positions_.count(bar.symbol)) {
                auto& pos = positions_[bar.symbol];
                pos.unrealized_pnl = (bar.close - pos.average_price) * pos.quantity;
            }
        }

        // Update metrics
        auto update_result = update_metrics();
        if (update_result.is_error()) {
            return update_result;
        }

        // Check risk limits
        return check_risk_limits();
    }
    catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Error processing data: ") + e.what(),
            "BaseStrategy"
        );
    }
}


Result<void> BaseStrategy::on_execution(const ExecutionReport& report) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    try {
        // First save execution to database if configured
        if (config_.save_executions) {
            auto save_result = save_executions(report);
            if (save_result.is_error()) {
                return save_result; // Return early if save fails
            }
        }

        // Update position
        auto& pos = positions_[report.symbol];
        
        // Calculate realized PnL if closing position
        if ((pos.quantity > 0 && report.side == Side::SELL) ||
            (pos.quantity < 0 && report.side == Side::BUY)) {
            double realized_pnl = 0.0;
            if (report.side == Side::SELL) {
                realized_pnl = (report.fill_price - pos.average_price) * 
                             report.filled_quantity;
            } else {
                realized_pnl = (pos.average_price - report.fill_price) * 
                             report.filled_quantity;
            }
            
            pos.realized_pnl += realized_pnl;
            metrics_.total_pnl += realized_pnl;
        }
        
        // Update position quantity and average price
        if (report.side == Side::BUY) {
            double new_quantity = pos.quantity + report.filled_quantity;
            if (pos.quantity >= 0) {
                // Adding to long position
                pos.average_price = (pos.average_price * pos.quantity + 
                                  report.fill_price * report.filled_quantity) / 
                                  new_quantity;
            } else {
                // Covering short position
                if (new_quantity >= 0) {
                    pos.average_price = report.fill_price;
                }
            }
            pos.quantity = new_quantity;
        } else {
            double new_quantity = pos.quantity - report.filled_quantity;
            if (pos.quantity <= 0) {
                // Adding to short position
                pos.average_price = (pos.average_price * std::abs(pos.quantity) + 
                                  report.fill_price * report.filled_quantity) / 
                                  std::abs(new_quantity);
            } else {
                // Reducing long position
                if (new_quantity <= 0) {
                    pos.average_price = report.fill_price;
                }
            }
            pos.quantity = new_quantity;
        }
        
        pos.last_update = report.fill_time;
        
        // Update metrics
        metrics_.total_trades++;
        if (report.fill_price > pos.average_price) {
            metrics_.win_rate = (metrics_.win_rate * (metrics_.total_trades - 1) + 1.0) / 
                              metrics_.total_trades;
        }
        
        // Save updated position if configured
        if (config_.save_positions) {
            auto pos_save_result = save_positions();
            if (pos_save_result.is_error()) {
                ERROR("Failed to save positions: " + pos_save_result.error()->to_string());
            }
        }
        
        return Result<void>();
        
    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::UNKNOWN_ERROR,
            "Error processing execution: " + std::string(e.what()),
            "BaseStrategy"
        );
    }
}

Result<void> BaseStrategy::on_signal(const std::string& symbol, double signal) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (state_ != StrategyState::RUNNING) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Strategy not in RUNNING state",
            "BaseStrategy"
        );
    }

    // Store signal
    last_signals_[symbol] = signal;
    
    // Only save to database if configured
    if (config_.save_signals) {
        std::unordered_map<std::string, double> signals;
        signals[symbol] = signal;
        return save_signals(signals);
    }
    
    return Result<void>();
}

StrategyState BaseStrategy::get_state() const {
    return state_;
}

const StrategyMetrics& BaseStrategy::get_metrics() const {
    return metrics_;
}

const StrategyConfig& BaseStrategy::get_config() const {
    return config_;
}

const StrategyMetadata& BaseStrategy::get_metadata() const {
    return metadata_;
}

const std::unordered_map<std::string, Position>& BaseStrategy::get_positions() const {
    return positions_;
}

Result<void> BaseStrategy::update_position(const std::string& symbol, 
                                         const Position& position) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Validate position against limits
    if (config_.position_limits.count(symbol) > 0) {
        if (std::abs(position.quantity) > config_.position_limits.at(symbol)) {
            return make_error<void>(
                ErrorCode::POSITION_LIMIT_EXCEEDED,
                "Position exceeds limit for " + symbol,
                "BaseStrategy"
            );
        }
    }
    
    positions_[symbol] = position;
    
    // Save to database if configured
    if (config_.save_positions) {
        return save_positions();
    }
    
    return Result<void>();
}

Result<void> BaseStrategy::update_risk_limits(const RiskLimits& limits) {
    std::lock_guard<std::mutex> lock(mutex_);
    risk_limits_ = limits;
    return check_risk_limits();
}

Result<void> BaseStrategy::check_risk_limits() {
    // Calculate total position value
    double total_value = 0.0;

    // Keep track of any errors during price lookup
    std::string error_symbols;

    // Add detailed debugging
    std::cout << "=== RISK CHECK DETAILS ===" << std::endl;
    std::cout << "Capital allocation: " << config_.capital_allocation << std::endl;
    std::cout << "Max leverage: " << risk_limits_.max_leverage << std::endl;
    std::cout << "Position limits: " << std::endl;
    
    for (const auto& [symbol, limit] : config_.position_limits) {
        std::cout << "  " << symbol << ": " << limit << std::endl;
    }

    for (const auto& [symbol, position] : positions_) {
        // Skip zero positions to avoid unnecessary calculations
        if (position.quantity == 0) {
            continue;
        }
        
        std::cout << "Position for " << symbol << ": " << position.quantity 
                  << " @ " << position.average_price << std::endl;

        double contract_size = config_.trading_params.count(symbol) > 0 ? config_.trading_params.at(symbol) : 1.0;
        
        double position_value = std::abs(position.quantity * position.average_price * contract_size);
        total_value += position_value;
        
        std::cout << "  Value: " << position_value << std::endl;
    }
    
    // Check leverage
    double leverage = total_value / config_.capital_allocation;
    if (total_value < 0.1) { // If positions are essentially 0
        leverage = 0.0;  // No leverage when no positions
    }

    std::cout << "Total position value: " << total_value << std::endl;
    std::cout << "Calculated leverage: " << leverage << std::endl;
    std::cout << "=========================" << std::endl;

    if (leverage > risk_limits_.max_leverage) {
        return make_error<void>(
            ErrorCode::RISK_LIMIT_EXCEEDED,
            "Leverage exceeds limit: " + std::to_string(leverage),
            "BaseStrategy"
        );
    }
    
    // Check drawdown
    double drawdown = (metrics_.total_pnl / config_.capital_allocation);
    if (drawdown < -risk_limits_.max_drawdown) {
        return make_error<void>(
            ErrorCode::RISK_LIMIT_EXCEEDED,
            "Drawdown exceeds limit: " + std::to_string(drawdown),
            "BaseStrategy"
        );
    }
    
    return Result<void>();
}

Result<void> BaseStrategy::validate_config() const {
    if (config_.capital_allocation <= 0) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Invalid capital allocation",
            "BaseStrategy"
        );
    }
    
    if (config_.max_leverage <= 0) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Invalid max leverage",
            "BaseStrategy"
        );
    }
    
    return Result<void>();
}

Result<void> BaseStrategy::save_executions(const ExecutionReport& exec) {
    try {
        if (!db_) {
            return Result<void>();
        }
        auto result = db_->store_executions({exec}, "trading.executions");
        if (result.is_error()) {
            return make_error<void>(
                ErrorCode::DATABASE_ERROR,
                "Failed to save execution: " + std::string(result.error()->what()),
                "BaseStrategy"
            );
        }
        return Result<void>();
    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::DATABASE_ERROR,
            "Failed to save execution: " + std::string(e.what()),
            "BaseStrategy"
        );
    }
}

Result<void> BaseStrategy::save_positions() {
    try {
        if (!db_) {
            return Result<void>();
        }
        
        std::vector<Position> pos_vec;
        pos_vec.reserve(positions_.size());
        for (const auto& [symbol, pos] : positions_) {
            pos_vec.push_back(pos);
        }
        
        if (!pos_vec.empty()) {
            return db_->store_positions(pos_vec, "trading.positions");
        }
        return Result<void>();
        
    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::DATABASE_ERROR,
            "Failed to save positions: " + std::string(e.what()),
            "BaseStrategy"
        );
    }
}

Result<void> BaseStrategy::save_signals(
    const std::unordered_map<std::string, double>& signals) {
    try {
        if (!db_) {
            return Result<void>();
        }
        // Clear any existing signals before saving new ones
        last_signals_.clear();
        for (const auto& [symbol, signal] : signals) {
            last_signals_[symbol] = signal;
        }
        return db_->store_signals(signals, id_, 
                                std::chrono::system_clock::now(), 
                                "trading.signals");
    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::DATABASE_ERROR,
            "Failed to save signals: " + std::string(e.what()),
            "BaseStrategy"
        );
    }
}

Result<void> BaseStrategy::update_metrics() {
    // Calculate total unrealized P&L
    double total_unrealized = 0.0;
    for (const auto& [symbol, position] : positions_) {
        // Note: This is simplified - in reality you'd need current market prices
        total_unrealized += position.unrealized_pnl;
    }
    
    metrics_.total_pnl = total_unrealized;  // Plus realized P&L
    
    // Update other metrics
    if (metrics_.total_trades > 0) {
        metrics_.profit_factor = metrics_.total_pnl > 0 ? 
            (metrics_.total_pnl / std::abs(metrics_.total_trades)) : 0.0;
    }
    
    return Result<void>();
}

Result<void> BaseStrategy::transition_state(StrategyState new_state) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto validation = validate_state_transition(new_state);
    if (validation.is_error()) {
        return validation;
    }
    
    StrategyState old_state = state_;
    state_ = new_state;
    
    INFO("Strategy " + id_ + " state transition: " + 
         std::to_string(static_cast<int>(old_state)) + " -> " +
         std::to_string(static_cast<int>(new_state)));
    
    return Result<void>();
}

Result<void> BaseStrategy::validate_state_transition(StrategyState new_state) const {
    switch (state_) {
        case StrategyState::INITIALIZED:
            if (new_state != StrategyState::RUNNING) {
                return make_error<void>(
                    ErrorCode::INVALID_ARGUMENT,
                    "Can only transition to RUNNING from INITIALIZED",
                    "BaseStrategy"
                );
            }
            break;
            
        case StrategyState::RUNNING:
            if (new_state != StrategyState::PAUSED && 
                new_state != StrategyState::STOPPED) {
                return make_error<void>(
                    ErrorCode::INVALID_ARGUMENT,
                    "Can only transition to PAUSED or STOPPED from RUNNING",
                    "BaseStrategy"
                );
            }
            break;
            
        case StrategyState::PAUSED:
            if (new_state != StrategyState::RUNNING && 
                new_state != StrategyState::STOPPED) {
                return make_error<void>(
                    ErrorCode::INVALID_ARGUMENT,
                    "Can only transition to RUNNING or STOPPED from PAUSED",
                    "BaseStrategy"
                );
            }
            break;
            
        case StrategyState::STOPPED:
            return make_error<void>(
                ErrorCode::INVALID_ARGUMENT,
                "Cannot transition from STOPPED state",
                "BaseStrategy"
            );
            
        case StrategyState::ERROR:
            return make_error<void>(
                ErrorCode::INVALID_ARGUMENT,
                "Cannot transition from ERROR state",
                "BaseStrategy"
            );
    }
    
    return Result<void>();
}

} // namespace trade_ngin