// src/risk/basic_risk_modules.cpp
#include "trade_ngin/risk/basic_risk_modules.hpp"
#include <cmath>
#include <stdexcept>

namespace trade_ngin {

const char* risk_condition_kind_name(RiskCondition::Kind k) {
    switch (k) {
        case RiskCondition::Kind::ALWAYS:
            return "always";
        case RiskCondition::Kind::NEVER:
            return "never";
        case RiskCondition::Kind::LAP_AT_LEAST:
            return "lap_at_least";
        case RiskCondition::Kind::NONZERO_POSITIONS_ABOVE:
            return "nonzero_positions_above";
        case RiskCondition::Kind::MAX_ABS_QUANTITY_ABOVE:
            return "max_abs_quantity_above";
    }
    return "never";
}

bool RiskCondition::holds(const std::unordered_map<std::string, Position>& book,
                          const RiskContext& ctx) const {
    switch (kind) {
        case Kind::ALWAYS:
            return true;
        case Kind::NEVER:
            return false;
        case Kind::LAP_AT_LEAST:
            return ctx.phase == RiskPhase::LAP && static_cast<double>(ctx.lap) >= threshold;
        case Kind::NONZERO_POSITIONS_ABOVE: {
            size_t nonzero = 0;
            for (const auto& [symbol, pos] : book) {
                (void)symbol;
                if (pos.quantity != Decimal(0.0)) ++nonzero;
            }
            return static_cast<double>(nonzero) > threshold;
        }
        case Kind::MAX_ABS_QUANTITY_ABOVE:
            for (const auto& [symbol, pos] : book) {
                (void)symbol;
                if (std::abs(static_cast<double>(pos.quantity)) > threshold) return true;
            }
            return false;
    }
    return false;
}

nlohmann::json RiskCondition::to_json() const {
    return nlohmann::json{{"kind", risk_condition_kind_name(kind)}, {"threshold", threshold}};
}

namespace {

nlohmann::json terms_json(const std::set<RiskTerm>& terms) {
    nlohmann::json j = nlohmann::json::array();
    for (auto t : terms) j.push_back(risk_term_name(t));
    return j;
}

}  // namespace

ConstantScaleRiskModule::ConstantScaleRiskModule(std::string id, double scale, bool every_lap)
    : id_(std::move(id)), scale_(scale), every_lap_(every_lap) {
    if (!(scale >= 0.0 && scale <= 1.0)) {  // also false for NaN
        throw std::invalid_argument("ConstantScaleRiskModule " + id_ +
                                    ": scale must be in [0, 1], got " + std::to_string(scale));
    }
}

Result<RiskDecision> ConstantScaleRiskModule::evaluate(
    const std::unordered_map<std::string, Position>& book, const RiskContext& ctx) {
    (void)book;
    RiskDecision d;
    d.module_id = id_;
    const bool later_lap = ctx.phase == RiskPhase::LAP && ctx.lap > 1;
    if (scale_ < 1.0 && (every_lap_ || !later_lap)) {
        d.action = RiskAction::SCALE;
        d.scale = scale_;
    }
    return Result<RiskDecision>(std::move(d));
}

nlohmann::json ConstantScaleRiskModule::describe() const {
    return nlohmann::json{{"id", id_},
                          {"type", type_},
                          {"terms", terms_json(terms())},
                          {"params", {{"scale", scale_}, {"every_lap", every_lap_}}}};
}

WarnRiskModule::WarnRiskModule(std::string id, RiskCondition condition, std::string reason)
    : id_(std::move(id)), condition_(condition), reason_(std::move(reason)) {}

Result<RiskDecision> WarnRiskModule::evaluate(const std::unordered_map<std::string, Position>& book,
                                              const RiskContext& ctx) {
    RiskDecision d;
    d.module_id = id_;
    if (condition_.holds(book, ctx)) {
        d.action = RiskAction::WARN;
        d.reason = reason_;
    }
    return Result<RiskDecision>(std::move(d));
}

nlohmann::json WarnRiskModule::describe() const {
    return nlohmann::json{{"id", id_},
                          {"type", type_},
                          {"terms", terms_json(terms())},
                          {"params", {{"condition", condition_.to_json()}, {"reason", reason_}}}};
}

RefuseOnConditionRiskModule::RefuseOnConditionRiskModule(std::string id, RiskCondition condition,
                                                         std::string reason)
    : id_(std::move(id)), condition_(condition), reason_(std::move(reason)) {}

Result<RiskDecision> RefuseOnConditionRiskModule::evaluate(
    const std::unordered_map<std::string, Position>& book, const RiskContext& ctx) {
    RiskDecision d;
    d.module_id = id_;
    if (condition_.holds(book, ctx)) {
        d.action = RiskAction::REFUSE;
        d.reason = reason_;
    }
    return Result<RiskDecision>(std::move(d));
}

nlohmann::json RefuseOnConditionRiskModule::describe() const {
    return nlohmann::json{{"id", id_},
                          {"type", type_},
                          {"terms", terms_json(terms())},
                          {"params", {{"condition", condition_.to_json()}, {"reason", reason_}}}};
}

}  // namespace trade_ngin
