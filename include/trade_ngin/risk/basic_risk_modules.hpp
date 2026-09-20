// include/trade_ngin/risk/basic_risk_modules.hpp
//
// The plug-and-play risk modules: a constant cut, a warning and a refusal on a
// condition. None of them logs anything; the PortfolioManager logs the actions it
// takes on their decisions.
#pragma once

#include <string>
#include <unordered_map>
#include "trade_ngin/risk/risk_module.hpp"

namespace trade_ngin {

/// A JSON-describable condition over the book and the call, so a factory can build these
/// modules from config.
struct RiskCondition {
    enum class Kind { ALWAYS, NEVER, LAP_AT_LEAST, NONZERO_POSITIONS_ABOVE, MAX_ABS_QUANTITY_ABOVE };
    Kind kind{Kind::NEVER};
    double threshold{0.0};  ///< LAP_AT_LEAST: ctx.lap >= threshold (LAP phase only);
                            ///< NONZERO_POSITIONS_ABOVE: count(q != 0) > threshold;
                            ///< MAX_ABS_QUANTITY_ABOVE: any |q| > threshold

    bool holds(const std::unordered_map<std::string, Position>& book, const RiskContext& ctx) const;
    nlohmann::json to_json() const;  ///< {"kind":"lap_at_least","threshold":3}
};

const char* risk_condition_kind_name(RiskCondition::Kind k);  ///< "always" "never" "lap_at_least" ...

/// A fixed cut. Throws std::invalid_argument unless 0 <= scale <= 1 (NaN rejected).
/// scale == 1.0 returns NONE (never SCALE{1.0}). Once per rebalance by default: NONE when
/// phase == LAP and lap > 1; every_lap = true reproduces the loop's per-lap compounding.
class ConstantScaleRiskModule final : public RiskModule {
public:
    ConstantScaleRiskModule(std::string id, double scale, bool every_lap = false);

    const std::string& id() const override { return id_; }
    const std::string& type() const override { return type_; }
    std::set<RiskTerm> terms() const override { return {RiskTerm::MAGNITUDE}; }
    std::set<RiskAction> capabilities() const override { return {RiskAction::SCALE}; }
    Result<RiskDecision> evaluate(const std::unordered_map<std::string, Position>& book,
                                  const RiskContext& ctx) override;
    nlohmann::json describe() const override;

    double scale() const { return scale_; }
    bool every_lap() const { return every_lap_; }

private:
    std::string id_;
    std::string type_{"constant_scale"};
    double scale_;
    bool every_lap_;
};

/// WARN(reason) when the condition holds, else NONE.
class WarnRiskModule final : public RiskModule {
public:
    WarnRiskModule(std::string id, RiskCondition condition, std::string reason);

    const std::string& id() const override { return id_; }
    const std::string& type() const override { return type_; }
    std::set<RiskTerm> terms() const override { return {RiskTerm::CUSTOM}; }
    std::set<RiskAction> capabilities() const override { return {RiskAction::WARN}; }
    Result<RiskDecision> evaluate(const std::unordered_map<std::string, Position>& book,
                                  const RiskContext& ctx) override;
    nlohmann::json describe() const override;

private:
    std::string id_;
    std::string type_{"warn"};
    RiskCondition condition_;
    std::string reason_;
};

/// REFUSE(reason) when the condition holds, else NONE.
class RefuseOnConditionRiskModule final : public RiskModule {
public:
    RefuseOnConditionRiskModule(std::string id, RiskCondition condition, std::string reason);

    const std::string& id() const override { return id_; }
    const std::string& type() const override { return type_; }
    std::set<RiskTerm> terms() const override { return {RiskTerm::CUSTOM}; }
    std::set<RiskAction> capabilities() const override { return {RiskAction::REFUSE}; }
    Result<RiskDecision> evaluate(const std::unordered_map<std::string, Position>& book,
                                  const RiskContext& ctx) override;
    nlohmann::json describe() const override;

private:
    std::string id_;
    std::string type_{"refuse"};
    RiskCondition condition_;
    std::string reason_;
};

}  // namespace trade_ngin
