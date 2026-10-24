#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/strategy/base_strategy.hpp"

namespace trade_ngin {

struct PyStrategy : public BaseStrategy {
    using BaseStrategy::BaseStrategy;

    void initialize_from_context(const std::string& id, const StrategyConfig& config,
                                 std::shared_ptr<PostgresDatabase> db,
                                 std::shared_ptr<InstrumentRegistry> registry) {
        std::lock_guard<std::mutex> lock(mutex_);
        id_ = id;
        config_ = config;
        db_ = db;
        registry_ = registry;
        state_ = StrategyState::INITIALIZED;
    }

    std::shared_ptr<InstrumentRegistry> registry_;

    virtual std::vector<Position> generate_positions_from_data(const std::vector<Bar>& data) const {
        // Return empty vector for void state
        (void)data;  // Suppress unused parameter warning
        return {};
    }
};

}  // namespace trade_ngin
