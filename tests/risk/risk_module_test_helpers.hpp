// tests/risk/risk_module_test_helpers.hpp
//
// The two module assignments a test needs when it builds a PortfolioConfig by hand.
//
// Header-only and TEST-ONLY on purpose. A production "build a carver module from a
// RiskConfig struct" helper would be a second way of assembling the gate's numbers -- a
// defaults path by another name -- and schema 2 exists to leave exactly one: the book's
// own risk.json, parsed by parse_risk_schema.
#pragma once

#include <string>

#include "trade_ngin/risk/risk_manager.hpp"
#include "trade_ngin/risk/risk_module_config.hpp"

namespace trade_ngin {
namespace testing {

/// "This test's portfolio runs no risk layer", spelled the way a config has to spell it.
inline RiskModuleConfig test_none_module(std::string id = "none") {
    RiskModuleConfig m;
    m.id = std::move(id);
    m.type = "none";
    m.params = NoneModuleConfig{"unit test: this portfolio runs no risk layer", "tests",
                                "2026-09-19"};
    return m;
}

/// A carver module carrying the seven values the test already set on its RiskConfig, so a
/// test that used to say `use_risk_management = true` keeps gating with the same numbers.
inline RiskModuleConfig test_carver_module(const RiskConfig& config, std::string id = "carver",
                                           int min_gate_dates = 21) {
    CarverModuleConfig c;
    c.var_limit = config.var_limit;
    c.jump_risk_limit = config.jump_risk_limit;
    c.max_correlation = config.max_correlation;
    c.max_gross_leverage = config.max_gross_leverage;
    c.max_net_leverage = config.max_net_leverage;
    c.confidence_level = config.confidence_level;
    c.lookback_period = config.lookback_period;
    c.lookback_unit = "bars";
    c.min_gate_dates = min_gate_dates;
    c.missing_symbol_policy = "ignore";
    c.missing_symbol_policy_reason = "unit test";
    RiskModuleConfig m;
    m.id = std::move(id);
    m.type = "carver";
    m.params = c;
    return m;
}

}  // namespace testing
}  // namespace trade_ngin
