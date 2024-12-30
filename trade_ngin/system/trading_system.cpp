#include "trading_system.hpp"
#include <stdexcept>

void TradingSystem::addInstrument(std::unique_ptr<Instrument> instrument) {
    instruments_.push_back(std::move(instrument));
}

void TradingSystem::addStrategy(std::unique_ptr<Strategy> strategy, double weight) {
    if (weight < 0.0 || weight > 1.0) {
        throw std::invalid_argument("Strategy weight must be between 0 and 1");
    }
    weighted_strategies_.emplace_back(weight, std::move(strategy));
}

void TradingSystem::setRiskMeasure(std::unique_ptr<RiskMeasure> risk_measure) {
    risk_measure_ = std::move(risk_measure);
}

void TradingSystem::initialize() {
    // Convert instruments to raw pointers for portfolio
    std::vector<Instrument*> instrument_ptrs;
    for (const auto& inst : instruments_) {
        instrument_ptrs.push_back(inst.get());
    }
    
    // Initialize portfolio
    portfolio_.set_capital(capital_);
    portfolio_.set_instruments(&instrument_ptrs);
    
    // Convert strategies to raw pointers with weights
    std::vector<std::pair<double, Strategy*>> weighted_strategy_ptrs;
    for (const auto& ws : weighted_strategies_) {
        weighted_strategy_ptrs.emplace_back(ws.first, ws.second.get());
    }
    portfolio_.set_weighted_strategies(weighted_strategy_ptrs);
    
    if (risk_measure_) {
        portfolio_.set_risk_object(risk_measure_.get());
    }
}

void TradingSystem::update() {
    // Update instrument data
    for (auto& instrument : instruments_) {
        if (auto* future = dynamic_cast<Future*>(instrument.get())) {
            future->add_data(*data_client_, Agg::DAILY, RollType::CALENDAR, ContractType::FRONT);
        }
    }
    
    // Update strategy positions
    for (auto& [weight, strategy] : weighted_strategies_) {
        // Strategy updates would go here
        // Each strategy should update its positions based on new data
    }
}

void TradingSystem::execute() {
    // Get current positions and target positions
    DataFrame current_positions = portfolio_.positions();
    
    // Calculate exposure
    DataFrame exposure = portfolio_.exposure();
    
    // Execute trades to reach target positions
    // This would interface with your order execution system
    // For now, we'll just print the positions
    std::cout << "Current positions:\n";
    // Add position printing logic here
} 