#include "portfolio.hpp"

Portfolio::Portfolio(PortfolioConfig config) : config_(std::move(config)) {}

void Portfolio::addInstrument(std::shared_ptr<Instrument> instrument) {
    instruments_.push_back(std::move(instrument));
    updateCache(); // Invalidate cache when instruments change
}

void Portfolio::addStrategy(std::shared_ptr<Strategy> strategy, double weight) {
    if (weight < 0.0 || weight > 1.0) {
        throw std::invalid_argument("Strategy weight must be between 0 and 1");
    }
    weighted_strategies_.emplace_back(weight, std::move(strategy));
    validateWeights();
    updateCache();
}

void Portfolio::update() {
    // Update all instruments
    for (auto& instrument : instruments_) {
        instrument->update(*data_client_);
    }

    // Update all strategies
    for (auto& [weight, strategy] : weighted_strategies_) {
        strategy->update(getMarketData());
    }

    updateCache();
    checkRiskLimits();
}

void Portfolio::rebalance() {
    validateWeights();
    
    // Calculate target positions
    DataFrame target_positions;
    for (const auto& [weight, strategy] : weighted_strategies_) {
        auto strat_pos = strategy->positions();
        if (target_positions.empty()) {
            target_positions = strat_pos.mul_scalar(weight);
        } else {
            target_positions = target_positions.add(strat_pos.mul_scalar(weight));
        }
    }

    // Apply position and risk limits
    applyPositionLimits();
    applyRiskLimits();
    
    // Generate orders for the difference
    auto current = getPositions();
    auto trades_needed = target_positions.subtract(current);
    executeOrders(trades_needed);
}

void Portfolio::checkRiskLimits() {
    if (!risk_engine_) return;
    
    auto metrics = risk_engine_->calculateRisk(*this);
    
    if (metrics.leverage > config_.max_leverage ||
        metrics.var > config_.risk_limits["VAR"] ||
        metrics.max_drawdown > config_.risk_limits["MaxDrawdown"]) {
        adjustPositions();
    }
}

void Portfolio::updateCache() {
    multipliers_.reset();
    prices_.reset();
    positions_.reset();
    exposure_.reset();
}
