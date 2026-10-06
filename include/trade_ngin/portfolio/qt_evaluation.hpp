#pragma once

#include "trade_ngin/portfolio/component_optimizer_evaluator.hpp"
#include "trade_ngin/portfolio/component_quantity_normalization.hpp"
#include "trade_ngin/portfolio/component_risk_evaluator.hpp"
#include "trade_ngin/portfolio/qt_component_cost.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace trade_ngin {

enum class QtEvaluationOperation { DraftDiagnostic, SelectedBook };
enum class QtEvaluationInputMode { LegacyV1, EmptyOwnerV2 };
enum class QtEvidenceStatus { Evaluated, Disabled, Rejected, Unavailable };

struct QtEvaluationRequest {
    QtEvaluationOperation operation;
    std::string evaluator_build;
    std::string context_fingerprint;
    // A draft diagnostic carries an explicit, versioned optimizer policy.
    bool optimizer_enabled{false};
    std::string optimizer_config_source_id;
    std::string risk_config_source_id;
    ComponentBookContext context;
    ComponentBookProposal proposal;
    std::optional<ComponentOptimizerInputs> optimizer_inputs;
    std::optional<DynamicOptConfig> optimizer_config;
    ComponentRiskInputs risk_inputs;
    RiskConfig risk_config;
    InstrumentQuantityRules quantity_rules;
    std::vector<QtComponentCostInput> component_cost_inputs;
    QtEvaluationInputMode input_mode{QtEvaluationInputMode::LegacyV1};
};

struct QtEvaluation {
    QtEvaluationOperation operation;
    std::string evaluator_build;
    std::string context_fingerprint;
    std::string evaluated_book_digest;
    std::string optimizer_config_source_id;
    std::string risk_config_source_id;
    ComponentBookOverlay evaluated_book;
    QtEvidenceStatus optimizer_status{QtEvidenceStatus::Unavailable};
    QtEvidenceStatus risk_status{QtEvidenceStatus::Unavailable};
    QtEvidenceStatus cost_status{QtEvidenceStatus::Unavailable};
    std::optional<ComponentOptimizerEvaluation> optimizer;
    std::optional<ComponentRiskEvaluation> risk;
    std::optional<QtSelectedCostEvaluation> selected_costs;
    std::vector<std::string> diagnostics;
    QtEvaluationInputMode input_mode{QtEvaluationInputMode::LegacyV1};
};

Result<QtEvaluation> evaluate_qt_request(const QtEvaluationRequest& request);

// Bounded internal seam for proving call counts. The single-argument entrypoint
// always supplies callbacks that call the existing production kernels.
struct QtEvaluationKernels {
    std::function<Result<ComponentRiskEvaluation>(
        const RiskConfig&, const ComponentBookContext&, const ComponentBookProposal&,
        const ComponentRiskInputs&)> risk;
    std::function<Result<ComponentOptimizerEvaluation>(
        const DynamicOptConfig&, const ComponentBookContext&,
        const ComponentBookProposal&, const ComponentOptimizerInputs&)> optimizer;
    std::function<Result<ComponentRiskEvaluation>(
        const RiskConfig&, const ComponentBookContext&, const ComponentBookProposal&,
        const ComponentRiskInputs&)> empty_owner_risk;
};
Result<QtEvaluation> evaluate_qt_request(const QtEvaluationRequest& request,
                                         const QtEvaluationKernels& kernels);
void record_stage_error(QtEvaluation& evaluation, std::string_view stage, const TradeError& error);

}  // namespace trade_ngin
