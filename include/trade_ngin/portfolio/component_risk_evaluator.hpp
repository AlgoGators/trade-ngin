#pragma once

#include "trade_ngin/portfolio/component_book.hpp"
#include "trade_ngin/risk/risk_manager.hpp"

#include <string>
#include <vector>

namespace trade_ngin {

struct ComponentRiskValuation {
    InstrumentIdentity instrument;
    Timestamp mark_as_of;
    double mark;
    double price_multiplier;
    std::string quote_currency;
};

struct ComponentRiskClose {
    InstrumentIdentity instrument;
    Timestamp timestamp;
    double close;
};

struct ComponentRiskInputs {
    std::string expected_portfolio_id;
    std::string expected_date;
    std::string expected_portfolio_type;
    std::string expected_revision;
    std::string market_snapshot_id;
    Timestamp valuation_time;
    std::string capital_currency;
    std::vector<ComponentRiskValuation> valuations;
    std::vector<Timestamp> expected_observation_times;
    std::vector<ComponentRiskClose> closes;
};

struct ComponentRiskBinding {
    InstrumentIdentity instrument;
    RiskCalculationId calculation_id;
};

struct ComponentRiskEvaluation {
    ComponentBookOverlay evaluated_book;
    ComponentBookProposal evaluated_proposal;
    ComponentRiskInputs evaluated_inputs;
    RiskConfig evaluated_config;
    std::vector<ComponentRiskBinding> bindings;
    RiskResult risk;
};

class ComponentRiskEvaluator final {
public:
    explicit ComponentRiskEvaluator(RiskConfig config);

    Result<ComponentRiskEvaluation> evaluate(
        const ComponentBookContext& context,
        const ComponentBookProposal& proposal,
        const ComponentRiskInputs& inputs);

    // Explicit operation; the general nonempty evaluator remains unchanged.
    Result<ComponentRiskEvaluation> evaluate_empty_owner(
        const ComponentBookContext&, const ComponentBookProposal&, const ComponentRiskInputs&);

private:
    RiskManager risk_manager_;
};

}  // namespace trade_ngin
