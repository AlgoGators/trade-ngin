#include "trade_ngin/portfolio/qt_evaluation.hpp"
#include "trade_ngin/portfolio/qt_wire.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <set>
#include <string>

namespace trade_ngin {
namespace {

bool has_text(std::string_view value) {
    return std::any_of(value.begin(), value.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    });
}

bool valid_fingerprint(std::string_view value) {
    return value.size() == 64 &&
           std::all_of(value.begin(), value.end(), [](char ch) {
               return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
           });
}

void set_stage_status(QtEvaluation& output, std::string_view stage,
                      QtEvidenceStatus status) {
    if (stage == "optimizer") output.optimizer_status = status;
    else if (stage == "risk") output.risk_status = status;
    else if (stage == "cost") output.cost_status = status;
}

void stage_unavailable(QtEvaluation& output, std::string_view stage,
                       std::string_view reason) {
    set_stage_status(output, stage, QtEvidenceStatus::Unavailable);
    output.diagnostics.push_back(std::string(stage) + ":" + std::string(reason));
}

void reject_quantities(QtEvaluation& output, std::string_view reason) {
    output.risk_status = QtEvidenceStatus::Rejected;
    output.cost_status = QtEvidenceStatus::Rejected;
    if (output.operation == QtEvaluationOperation::DraftDiagnostic &&
        output.optimizer_status != QtEvidenceStatus::Disabled)
        output.optimizer_status = QtEvidenceStatus::Rejected;
    output.diagnostics.push_back("quantity:" + std::string(reason));
}

enum class QuantityValidation { Valid, Missing, Rejected };

QuantityValidation validate_selected_quantities(
    const ComponentBookOverlay& book, const InstrumentQuantityRules& rules,
    std::string& reason) {
    for (const auto& component : book.components) {
        if (!component.editable) continue;
        const auto rule = rules.find(component.instrument);
        if (rule == rules.end()) {
            reason = "missing_quantity_rule";
            return QuantityValidation::Missing;
        }
        if (rule->second.mode != QuantityRoundingMode::reject_off_increment) {
            reason = "quantity_rule_not_reject_only";
            return QuantityValidation::Rejected;
        }
        if (component.instrument.type == AssetType::FUTURE &&
            component.position.quantity.raw_value() % 100000000LL != 0) {
            reason = "future_fractional_quantity";
            return QuantityValidation::Rejected;
        }
        auto normalized = normalize_quantity(component.position.quantity, rule->second);
        if (normalized.is_error()) {
            reason = normalized.error()->what();
            return QuantityValidation::Rejected;
        }
        if (normalized.value() != component.position.quantity) {
            reason = "quantity_rule_changed_selection";
            return QuantityValidation::Rejected;
        }
    }
    return QuantityValidation::Valid;
}

Result<std::string> selected_book_digest(const ComponentBookOverlay& book) {
    nlohmann::json rows = nlohmann::json::array();
    std::set<ComponentPositionKey> target_keys;
    for (const auto& component : book.components) {
        auto target = component.key;
        if (target.portfolio_type != "qt_proposal" && target.portfolio_type != "qt")
            return make_error<std::string>(ErrorCode::INVALID_ARGUMENT,
                                           "unsupported_qt_stream_mapping", "qt_evaluation");
        target.portfolio_type = "qt";
        if (!target_keys.insert(target).second)
            return make_error<std::string>(ErrorCode::INVALID_DATA,
                                           "duplicate_target_component_key", "qt_evaluation");
        rows.push_back({
            {"key", {{"portfolio_id", target.portfolio_id},
                     {"strategy_id", target.strategy_id},
                     {"strategy_name", target.strategy_name},
                     {"date", target.date}, {"symbol", target.symbol},
                     {"portfolio_type", target.portfolio_type}}},
            {"quantity_exact", component.position.quantity.to_string()}});
    }
    return qt_digest_v1({{"selection_rows", std::move(rows)}});
}

bool same_quantities(const ComponentBookOverlay& left,
                     const ComponentBookOverlay& right) {
    if (left.components.size() != right.components.size()) return false;
    for (size_t i = 0; i < left.components.size(); ++i) {
        if (left.components[i].key != right.components[i].key ||
            left.components[i].position.quantity != right.components[i].position.quantity)
            return false;
    }
    return true;
}

QtEvaluationKernels production_kernels() {
    QtEvaluationKernels kernels;
    kernels.risk = [](const RiskConfig& config, const ComponentBookContext& context,
                      const ComponentBookProposal& proposal,
                      const ComponentRiskInputs& inputs) {
        return ComponentRiskEvaluator(config).evaluate(context, proposal, inputs);
    };
    kernels.empty_owner_risk = [](const RiskConfig& config, const ComponentBookContext& context,
                                   const ComponentBookProposal& proposal, const ComponentRiskInputs& inputs) {
        return ComponentRiskEvaluator(config).evaluate_empty_owner(context,proposal,inputs);
    };
    kernels.optimizer = [](const DynamicOptConfig& config,
                           const ComponentBookContext& context,
                           const ComponentBookProposal& proposal,
                           const ComponentOptimizerInputs& inputs) {
        return ComponentOptimizerEvaluator(config).evaluate(context, proposal, inputs);
    };
    return kernels;
}

}  // namespace

Result<QtEvaluation> evaluate_qt_request(const QtEvaluationRequest& request) {
    return evaluate_qt_request(request, production_kernels());
}

Result<QtEvaluation> evaluate_qt_request(const QtEvaluationRequest& request,
                                         const QtEvaluationKernels& kernels) {
    if (!has_text(request.evaluator_build) ||
        !valid_fingerprint(request.context_fingerprint))
        return make_error<QtEvaluation>(ErrorCode::INVALID_ARGUMENT,
                                        "invalid_evaluator_identity", "qt_evaluation");
    const bool empty_owner=request.input_mode==QtEvaluationInputMode::EmptyOwnerV2;
    if (empty_owner && (!request.context.slots.empty() || !request.proposal.quantities.empty() ||
        !request.risk_inputs.valuations.empty() || !request.risk_inputs.closes.empty() ||
        !request.risk_inputs.expected_observation_times.empty() || !request.quantity_rules.empty() ||
        !request.component_cost_inputs.empty() || request.optimizer_enabled ||
        request.optimizer_inputs || request.optimizer_config))
        return make_error<QtEvaluation>(ErrorCode::INVALID_ARGUMENT,"empty_owner_scope_not_empty","qt_evaluation");
    std::set<ComponentPositionKey> editable_keys;
    for (const auto& slot : request.context.slots)
        if (slot.editable) editable_keys.insert(slot.key);
    std::set<ComponentPositionKey> selected_keys;
    for (const auto& entry : request.proposal.quantities) {
        if (!editable_keys.contains(entry.key))
            return make_error<QtEvaluation>(ErrorCode::INVALID_ARGUMENT,
                                            "selection_outside_editable_scope", "qt_evaluation");
        if (!selected_keys.insert(entry.key).second)
            return make_error<QtEvaluation>(ErrorCode::INVALID_ARGUMENT,
                                            "duplicate_selected_component", "qt_evaluation");
    }
    if (selected_keys != editable_keys)
        return make_error<QtEvaluation>(ErrorCode::INVALID_ARGUMENT,
                                        "incomplete_editable_selection", "qt_evaluation");
    auto selected = overlay_component_book(request.context, request.proposal);
    if (selected.is_error()) {
        const auto* error = selected.error();
        return make_error<QtEvaluation>(error->code(), error->what(), error->component());
    }
    auto digest = selected_book_digest(selected.value());
    if (digest.is_error()) {
        const auto* error = digest.error();
        return make_error<QtEvaluation>(error->code(), error->what(), error->component());
    }

    QtEvaluation output;
    output.operation = request.operation;
    output.input_mode = request.input_mode;
    output.evaluator_build = request.evaluator_build;
    output.context_fingerprint = request.context_fingerprint;
    output.evaluated_book_digest = digest.value();
    output.optimizer_config_source_id = request.optimizer_config_source_id;
    output.risk_config_source_id = request.risk_config_source_id;
    output.evaluated_book = selected.value();
    output.optimizer_status = request.operation == QtEvaluationOperation::SelectedBook ||
                                      !request.optimizer_enabled
                                  ? QtEvidenceStatus::Disabled
                                  : QtEvidenceStatus::Unavailable;

    std::string quantity_reason;
    switch (validate_selected_quantities(output.evaluated_book, request.quantity_rules,
                                         quantity_reason)) {
        case QuantityValidation::Missing:
            stage_unavailable(output, "risk", quantity_reason);
            stage_unavailable(output, "cost", quantity_reason);
            if (output.optimizer_status != QtEvidenceStatus::Disabled)
                stage_unavailable(output, "optimizer", quantity_reason);
            return output;
        case QuantityValidation::Rejected:
            reject_quantities(output, quantity_reason);
            return output;
        case QuantityValidation::Valid:
            break;
    }

    if (request.operation == QtEvaluationOperation::DraftDiagnostic &&
        request.optimizer_enabled) {
        if (!has_text(request.optimizer_config_source_id) ||
            !request.optimizer_config || !request.optimizer_inputs || !kernels.optimizer) {
            stage_unavailable(output, "optimizer", "missing_optimizer_policy_or_inputs");
        } else if (!has_text(request.optimizer_inputs->market_snapshot_id) ||
                   !has_text(request.optimizer_inputs->capital_currency) ||
                   !has_text(request.risk_inputs.market_snapshot_id) ||
                   !has_text(request.risk_inputs.capital_currency)) {
            stage_unavailable(output, "optimizer", "missing_shared_market_source_or_currency");
        } else if (request.optimizer_inputs->market_snapshot_id !=
                   request.risk_inputs.market_snapshot_id ||
                   request.optimizer_inputs->valuation_time !=
                   request.risk_inputs.valuation_time ||
                   request.optimizer_inputs->capital_currency !=
                   request.risk_inputs.capital_currency) {
            output.optimizer_status = QtEvidenceStatus::Rejected;
            output.diagnostics.push_back("optimizer:source_scope_mismatch");
        } else {
            auto optimized = kernels.optimizer(*request.optimizer_config,
                                               request.context, request.proposal,
                                               *request.optimizer_inputs);
            if (optimized.is_error())
                record_stage_error(output, "optimizer", *optimized.error());
            else if (!same_quantities(output.evaluated_book,
                                      optimized.value().evaluated_book)) {
                output.optimizer_status = QtEvidenceStatus::Rejected;
                output.diagnostics.push_back("optimizer:candidate_book_mismatch");
            } else {
                output.optimizer_status = QtEvidenceStatus::Evaluated;
                output.optimizer = std::move(optimized.value());
            }
        }
    }

    const auto& risk_kernel=empty_owner ? kernels.empty_owner_risk : kernels.risk;
    if (!has_text(request.risk_config_source_id) || !risk_kernel) {
        stage_unavailable(output, "risk", "missing_risk_policy_source");
    } else if (!has_text(request.risk_inputs.market_snapshot_id)) {
        stage_unavailable(output, "risk", "missing_risk_market_snapshot_id");
    } else if (!has_text(request.risk_inputs.capital_currency)) {
        stage_unavailable(output, "risk", "missing_risk_capital_currency");
    } else {
        auto assessed = risk_kernel(request.risk_config, request.context,
                                     request.proposal, request.risk_inputs);
        if (assessed.is_error())
            record_stage_error(output, "risk", *assessed.error());
        else if (!same_quantities(output.evaluated_book,
                                  assessed.value().evaluated_book)) {
            output.risk_status = QtEvidenceStatus::Rejected;
            output.diagnostics.push_back("risk:selected_book_mismatch");
        } else {
            output.risk_status = QtEvidenceStatus::Evaluated;
            output.risk = std::move(assessed.value());
            if (empty_owner) output.diagnostics.push_back(
                "risk:empty_owner_no_instrument_observations;correlation_history_not_applicable");
        }
    }

    auto costs = evaluate_selected_component_costs(output.evaluated_book,
        request.component_cost_inputs, request.risk_inputs.capital_currency,
        output.evaluated_book_digest);
    if (costs.is_error())
        record_stage_error(output, "cost", *costs.error());
    else if (costs.value().evaluated_book_digest != output.evaluated_book_digest) {
        output.cost_status = QtEvidenceStatus::Rejected;
        output.diagnostics.push_back("cost:selected_book_digest_mismatch");
    } else {
        output.cost_status = QtEvidenceStatus::Evaluated;
        output.selected_costs = std::move(costs.value());
    }
    return output;
}

void record_stage_error(QtEvaluation& output, std::string_view stage,
                        const TradeError& error) {
    const std::string message = error.what();
    const bool dependency_missing =
        error.code() == ErrorCode::DATA_NOT_FOUND ||
        error.code() == ErrorCode::NOT_INITIALIZED ||
        error.code() == ErrorCode::MARKET_DATA_ERROR ||
        message.find("missing") != std::string::npos ||
        message.find("unsupported") != std::string::npos ||
        message.find("coverage: missing") != std::string::npos;
    set_stage_status(output, stage, dependency_missing
        ? QtEvidenceStatus::Unavailable : QtEvidenceStatus::Rejected);
    output.diagnostics.push_back(std::string(stage) + ":" +
        std::to_string(static_cast<int>(error.code())) + ":" +
        error.component() + ":" + message);
}

}  // namespace trade_ngin
