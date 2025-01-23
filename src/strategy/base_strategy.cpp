// src/strategy/base_strategy.cpp

#include "trade_ngin/strategy/base_strategy.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/state_manager.hpp"
#include "trade_ngin/data/market_data_bus.hpp"
#include <sstream>


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
            "Strategy already initialized",
            "BaseStrategy"
        );
    }

    // Validate configuration
    auto validation = validate_config();
    if (validation.is_error()) {
        return validation;
    }

    // Initialize database connection if needed
    if (!db_) {
        return make_error<void>(
            ErrorCode::NOT_INITIALIZED,
            "Database interface not initialized",
            "BaseStrategy"
        );
    }

    // Register with state manager
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

    auto register_result = StateManager::instance().register_component(info);
    if (register_result.is_error()) {
        return register_result;
    }

    // Subscribe to market data
    MarketDataCallback callback = [this](const MarketDataEvent& event) {
        if (event.type == MarketDataEventType::BAR) {
            try {
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
                    ERROR("Error processing bar data: " + std::string(result.error()->what()));
                }
            } catch (const std::exception& e) {
                ERROR("Error in market data callback: " + std::string(e.what()));
            }
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

    INFO("Initialized strategy " + id_);
    return Result<void>({});
}

Result<void> BaseStrategy::start() {
    auto result = StateManager::instance().update_state(
        id_,
        ComponentState::RUNNING
    );

    if (result.is_ok()) {
        state_ = StrategyState::RUNNING;
    }

    return result;
}

Result<void> BaseStrategy::start() {
    return transition_state(StrategyState::RUNNING);
}

Result<void> BaseStrategy::stop() {
    auto result = transition_state(StrategyState::STOPPED);
    if (result.is_ok()) {
        // Save final positions and signals
        if (config_.save_positions) {
            save_positions();
        }
    }
    return result;
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
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (state_ != StrategyState::RUNNING) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Strategy not in RUNNING state",
            "BaseStrategy"
        );
    }

    // Update metrics based on new data
    update_metrics();

    // Check risk limits
    auto risk_check = check_risk_limits();
    if (risk_check.is_error()) {
        return risk_check;
    }

    return Result<void>({});
}

Result<void> BaseStrategy::on_execution(const ExecutionReport& report) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    try {
        // First save execution to database if configured
        if (config_.save_executions) {
            auto save_result = save_executions(report);
            if (save_result.is_error()) {
                ERROR("Failed to save execution: " + save_result.error()->to_string());
                // Continue with position update even if save fails
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
        
        return Result<void>({});
        
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
    
    // Save to database if configured
    if (config_.save_signals) {
        return save_signals({{symbol, signal}});
    }
    
    return Result<void>({});
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
    
    return Result<void>({});
}

Result<void> BaseStrategy::update_risk_limits(const RiskLimits& limits) {
    std::lock_guard<std::mutex> lock(mutex_);
    risk_limits_ = limits;
    return check_risk_limits();
}

Result<void> BaseStrategy::check_risk_limits() {
    // Calculate total position value
    double total_value = 0.0;
    for (const auto& [symbol, position] : positions_) {
        total_value += std::abs(position.quantity * position.average_price);
    }
    
    // Check leverage
    double leverage = total_value / config_.capital_allocation;
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
    
    return Result<void>({});
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
    
    return Result<void>({});
}

Result<void> BaseStrategy::save_executions(const ExecutionReport& exec) {
    try {
        if (db_) {
            return db_->store_executions({exec}, "trading.executions");
        }
        return Result<void>({});
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
        if (db_) {
            std::vector<Position> pos_vec;
            pos_vec.reserve(positions_.size());
            for (const auto& [symbol, pos] : positions_) {
                pos_vec.push_back(pos);
            }
            return db_->store_positions(pos_vec, "trading.positions");
        }
        return Result<void>({});
    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::DATABASE_ERROR,
            "Failed to save positions: " + std::string(e.what()),
            "BaseStrategy"
        );
    }
}

Result<void> BaseStrategy::save_signals(const std::unordered_map<std::string, double>& signals) {
    try {
        if (db_) {
            return db_->store_signals(signals, id_, std::chrono::system_clock::now(), "trading.signals");
        }
        return Result<void>({});
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
    
    return Result<void>({});
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
    
    return Result<void>({});
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
    
    return Result<void>({});
}

} // namespace trade_ngin