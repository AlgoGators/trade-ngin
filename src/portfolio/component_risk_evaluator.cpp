#include "trade_ngin/portfolio/component_risk_evaluator.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <map>
#include <unordered_map>
#include <utility>

namespace trade_ngin {

ComponentRiskEvaluator::ComponentRiskEvaluator(RiskConfig config)
    : risk_manager_(std::move(config)) {}

namespace {

bool has_text(const std::string& token) {
    return std::any_of(token.begin(), token.end(), [](unsigned char character) {
        return !std::isspace(character);
    });
}

std::string identity_text(const InstrumentIdentity& identity) {
    return std::to_string(static_cast<int>(identity.type)) + ":" + identity.symbol;
}

}  // namespace

Result<ComponentRiskEvaluation> ComponentRiskEvaluator::evaluate(
    const ComponentBookContext& context, const ComponentBookProposal& proposal,
    const ComponentRiskInputs& inputs) {
    using Evaluation = ComponentRiskEvaluation;
    const auto invalid = [](const std::string& reason) -> Result<Evaluation> {
        return make_error<Evaluation>(ErrorCode::INVALID_ARGUMENT, reason,
                                      "ComponentRiskEvaluator");
    };
    const auto forwarded = [](const TradeError& error) -> Result<Evaluation> {
        return make_error<Evaluation>(error.code(), error.what(), error.component());
    };
    try {
        if (inputs.expected_portfolio_id != context.portfolio_id ||
            inputs.expected_date != context.date ||
            inputs.expected_portfolio_type != context.portfolio_type ||
            inputs.expected_revision != context.revision) {
            return invalid("risk_context_mismatch: supplied=" +
                           inputs.expected_portfolio_id + ":" + inputs.expected_date + ":" +
                           inputs.expected_portfolio_type + ":" + inputs.expected_revision +
                           " context=" + context.portfolio_id + ":" + context.date + ":" +
                           context.portfolio_type + ":" + context.revision);
        }
        if (!has_text(inputs.market_snapshot_id)) {
            return invalid("risk_market_snapshot_id: empty provenance token");
        }
        if (!has_text(inputs.capital_currency)) {
            return invalid("risk_capital_currency: empty currency token");
        }

        auto overlay = overlay_component_book(context, proposal);
        if (overlay.is_error()) return forwarded(*overlay.error());
        const auto& book = overlay.value();
        if (book.instruments.empty()) return invalid("empty_component_risk_book");

        std::map<InstrumentIdentity, const InstrumentQuantityAggregate*> aggregate_by_type;
        for (const auto& aggregate : book.instruments) {
            aggregate_by_type.emplace(aggregate.instrument, &aggregate);
        }

        std::map<InstrumentIdentity, const ComponentRiskValuation*> valuation_by_type;
        for (const auto& valuation : inputs.valuations) {
            if (valuation.mark_as_of != inputs.valuation_time) {
                return invalid("risk_mark_asof_mismatch: " + identity_text(valuation.instrument));
            }
            if (!has_text(valuation.quote_currency) ||
                valuation.quote_currency != inputs.capital_currency) {
                return invalid("risk_currency_mismatch: " + identity_text(valuation.instrument) +
                               ":" + valuation.quote_currency + " vs " + inputs.capital_currency);
            }
            if (!valuation_by_type.emplace(valuation.instrument, &valuation).second) {
                return invalid("duplicate_risk_valuation: " + identity_text(valuation.instrument));
            }
            if (!aggregate_by_type.contains(valuation.instrument)) {
                return invalid("risk_valuation_coverage: extra " +
                               identity_text(valuation.instrument));
            }
        }
        if (valuation_by_type.size() != aggregate_by_type.size()) {
            for (const auto& [identity, aggregate] : aggregate_by_type) {
                (void)aggregate;
                if (!valuation_by_type.contains(identity)) {
                    return invalid("risk_valuation_coverage: missing " + identity_text(identity));
                }
            }
        }
        for (const auto& close : inputs.closes) {
            if (!aggregate_by_type.contains(close.instrument)) {
                return invalid("risk_history_identity: " + identity_text(close.instrument));
            }
        }

        std::map<InstrumentIdentity, RiskCalculationId> id_by_type;
        std::map<RiskCalculationId, InstrumentIdentity> type_by_id;
        std::unordered_map<RiskCalculationId, Quantity> quantities;
        std::vector<RiskValuationInput> valuations;
        std::vector<RiskCloseInput> closes;
        std::vector<ComponentRiskBinding> bindings;
        quantities.reserve(book.instruments.size());
        valuations.reserve(book.instruments.size());
        closes.reserve(inputs.closes.size());
        bindings.reserve(book.instruments.size());
        for (size_t index = 0; index < book.instruments.size(); ++index) {
            const auto& aggregate = book.instruments[index];
            RiskCalculationId id = "risk:" + std::to_string(index);
            if (!id_by_type.emplace(aggregate.instrument, id).second ||
                !type_by_id.emplace(id, aggregate.instrument).second) {
                return invalid("risk_binding_bijection: " + identity_text(aggregate.instrument));
            }
            quantities.emplace(id, aggregate.net_quantity);
            const auto& typed_value = *valuation_by_type.at(aggregate.instrument);
            valuations.push_back({id, typed_value.mark, typed_value.price_multiplier});
            bindings.push_back({aggregate.instrument, std::move(id)});
        }
        if (id_by_type.size() != book.instruments.size() ||
            type_by_id.size() != book.instruments.size()) {
            return invalid("risk_binding_bijection: cardinality");
        }
        for (const auto& close : inputs.closes) {
            closes.push_back({id_by_type.at(close.instrument), close.timestamp, close.close});
        }

        auto snapshot = risk_manager_.make_frozen_snapshot(
            inputs.valuation_time, valuations, inputs.expected_observation_times, closes);
        if (snapshot.is_error()) return forwarded(*snapshot.error());
        auto risk = risk_manager_.process_positions_frozen(quantities, *snapshot.value());
        if (risk.is_error()) return forwarded(*risk.error());

        Evaluation output;
        output.evaluated_book = book;
        output.evaluated_proposal = proposal;
        output.evaluated_inputs = inputs;
        output.evaluated_config = risk_manager_.get_config();
        output.bindings = std::move(bindings);
        output.risk = risk.value();
        std::sort(output.evaluated_proposal.quantities.begin(),
                  output.evaluated_proposal.quantities.end(),
                  [](const auto& left, const auto& right) { return left.key < right.key; });
        std::sort(output.evaluated_inputs.valuations.begin(),
                  output.evaluated_inputs.valuations.end(),
                  [](const auto& left, const auto& right) {
                      return left.instrument < right.instrument;
                  });
        std::sort(output.evaluated_inputs.closes.begin(),
                  output.evaluated_inputs.closes.end(),
                  [](const auto& left, const auto& right) {
                      if (left.instrument == right.instrument) {
                          return left.timestamp < right.timestamp;
                      }
                      return left.instrument < right.instrument;
                  });
        return Result<Evaluation>(std::move(output));
    } catch (const std::exception& error) {
        return make_error<Evaluation>(ErrorCode::INVALID_RISK_CALCULATION,
                                      std::string("risk_adapter_failure: ") + error.what(),
                                      "ComponentRiskEvaluator");
    }
}


Result<ComponentRiskEvaluation> ComponentRiskEvaluator::evaluate_empty_owner(
    const ComponentBookContext& context, const ComponentBookProposal& proposal,
    const ComponentRiskInputs& inputs) {
    const auto invalid=[](const char* reason) {
        return make_error<ComponentRiskEvaluation>(ErrorCode::INVALID_ARGUMENT,reason,"ComponentRiskEvaluator");
    };
    if(!context.slots.empty() || !proposal.quantities.empty() || !inputs.valuations.empty() ||
       !inputs.closes.empty() || !inputs.expected_observation_times.empty())return invalid("empty_owner_scope_not_empty");
    if(inputs.expected_portfolio_id!=context.portfolio_id || inputs.expected_date!=context.date ||
       inputs.expected_portfolio_type!=context.portfolio_type || inputs.expected_revision!=context.revision)
        return invalid("risk_context_mismatch");
    if(!has_text(inputs.market_snapshot_id) || !has_text(inputs.capital_currency))return invalid("missing_risk_source_or_currency");
    auto overlay=overlay_component_book(context,proposal);
    if(overlay.is_error())return make_error<ComponentRiskEvaluation>(overlay.error()->code(),overlay.error()->what(),overlay.error()->component());
    if(!overlay.value().components.empty() || !overlay.value().instruments.empty())return invalid("empty_owner_overlay_not_empty");
    auto computed=risk_manager_.process_empty_owner_book();
    if(computed.is_error())return make_error<ComponentRiskEvaluation>(computed.error()->code(),computed.error()->what(),computed.error()->component());
    ComponentRiskEvaluation output;
    output.evaluated_book=overlay.value();output.evaluated_proposal=proposal;output.evaluated_inputs=inputs;
    output.evaluated_config=risk_manager_.get_config();output.risk=computed.value();
    return Result<ComponentRiskEvaluation>(std::move(output));
}

}  // namespace trade_ngin
