#pragma once

#include "trade_ngin/optimization/dynamic_optimizer.hpp"
#include "trade_ngin/portfolio/component_book.hpp"

#include <string>
#include <vector>

namespace trade_ngin {

struct ComponentOptimizerInstrument {
    InstrumentIdentity instrument;
    Timestamp mark_as_of;
    double mark;
    double price_multiplier;
    std::string quote_currency;
    Quantity calculation_increment;
    double cash_cost_per_increment;
    std::string cost_currency;
    std::string increment_source_id;
    std::string cost_source_id;
};

struct ComponentOptimizerClose {
    InstrumentIdentity instrument;
    Timestamp timestamp;
    double close;
};

struct ComponentOptimizerInputs {
    std::string expected_portfolio_id;
    std::string expected_date;
    std::string expected_portfolio_type;
    std::string expected_revision;
    std::string market_snapshot_id;
    Timestamp valuation_time;
    std::string capital_currency;
    std::vector<ComponentOptimizerInstrument> instruments;
    std::vector<Timestamp> expected_observation_times;
    std::vector<ComponentOptimizerClose> closes;
};

struct ComponentOptimizerBinding {
    InstrumentIdentity instrument;
    std::vector<ComponentPositionKey> members;
    Quantity previous_net_quantity;
    Quantity proposed_net_quantity;
};

struct ComponentOptimizerPreparedInputs {
    // Every vector and both covariance axes use the bindings' exact order.
    std::vector<ComponentOptimizerBinding> bindings;
    std::vector<double> current_weights;
    std::vector<double> target_weights;
    std::vector<double> weights_per_increment;
    std::vector<double> cost_coefficients;
    std::vector<std::vector<double>> annualized_covariance;
};

struct ComponentOptimizerEvaluation {
    ComponentBookOverlay evaluated_book;
    ComponentBookProposal evaluated_proposal;
    ComponentOptimizerInputs evaluated_inputs;
    DynamicOptConfig evaluated_config;
    ComponentOptimizerPreparedInputs prepared;
    OptimizationResult optimization;
    OptimizationTrace trace;
};

class ComponentOptimizerEvaluator final {
public:
    explicit ComponentOptimizerEvaluator(DynamicOptConfig config);
    Result<ComponentOptimizerEvaluation> evaluate(
        const ComponentBookContext& context,
        const ComponentBookProposal& proposal,
        const ComponentOptimizerInputs& inputs) const;
private:
    const DynamicOptimizer optimizer_;
};

}  // namespace trade_ngin
