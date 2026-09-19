// src/risk/carver_risk_module.cpp
#include "trade_ngin/risk/carver_risk_module.hpp"
#include "trade_ngin/core/logger.hpp"

namespace trade_ngin {

CarverRiskModule::CarverRiskModule(std::string id, RiskConfig config,
                                   const std::vector<Bar>* pm_window)
    : id_(std::move(id)), rm_(std::move(config)), pm_window_(pm_window) {}

std::set<RiskTerm> CarverRiskModule::terms() const {
    return {RiskTerm::COMPOSITION, RiskTerm::MAGNITUDE};
}

std::set<RiskAction> CarverRiskModule::capabilities() const {
    return {RiskAction::SCALE};
}

void CarverRiskModule::on_bars(const std::vector<Bar>& bars, const RiskContext& ctx) {
    (void)bars;
    (void)ctx;
    market_data_ = rm_.create_market_data(*pm_window_);
}

RiskDecision CarverRiskModule::to_decision(const RiskResult& r, const std::string& module_id) {
    RiskDecision d;
    d.module_id = module_id;
    if (r.risk_exceeded) {
        d.action = RiskAction::SCALE;
        d.scale = r.recommended_scale;
    } else {
        d.action = RiskAction::NONE;
        d.scale = 1.0;
    }
    d.metrics = r;
    return d;
}

Result<RiskDecision> CarverRiskModule::evaluate(
    const std::unordered_map<std::string, Position>& book, const RiskContext& ctx) {
    (void)ctx;
    auto result = rm_.process_positions(book, market_data_, {});
    if (result.is_error()) {
        return make_error<RiskDecision>(result.error()->code(), result.error()->what(),
                                        "RiskManager");
    }

    const auto& risk_result = result.value();
    INFO("Risk management result: risk_exceeded=" + std::to_string(risk_result.risk_exceeded) +
         ", scale=" + std::to_string(risk_result.recommended_scale) +
         ", portfolio_mult=" + std::to_string(risk_result.portfolio_multiplier) +
         ", jump_mult=" + std::to_string(risk_result.jump_multiplier) +
         ", correlation_mult=" + std::to_string(risk_result.correlation_multiplier) +
         ", leverage_mult=" + std::to_string(risk_result.leverage_multiplier));

    RiskDecision decision = to_decision(risk_result, id_);
    // Blind: the same tests RiskManager's own early returns make, recorded as data only.
    bool mapped = false;
    for (const auto& [symbol, pos] : book) {
        (void)pos;
        if (market_data_.symbol_indices.count(symbol)) {
            mapped = true;
            break;
        }
    }
    decision.blind = market_data_.returns.empty() || market_data_.covariance.empty() ||
                     market_data_.symbol_indices.empty() || market_data_.ordered_symbols.empty() ||
                     !mapped;
    return Result<RiskDecision>(std::move(decision));
}

nlohmann::json CarverRiskModule::describe() const {
    nlohmann::json terms_json = nlohmann::json::array();
    for (auto t : terms()) terms_json.push_back(risk_term_name(t));
    return nlohmann::json{
        {"id", id_}, {"type", type_}, {"terms", terms_json}, {"config", rm_.get_config().to_json()}};
}

}  // namespace trade_ngin
