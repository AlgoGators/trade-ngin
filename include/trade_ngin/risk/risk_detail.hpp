#pragma once

// LOOP_SPEC sections 7.2 and 7.3: the loop's record of one rebalance, and the risk_detail jsonb
// object built from it (trading.live_results.risk_detail and backtest.equity_curve.risk_detail,
// migration 020). Nine flat keys, pinned by tests/portfolio/test_one_pass_book.cpp
// (RiskDetailJson.TheNineKeysArePinned).

#include <array>
#include <map>
#include <string>

#include <nlohmann/json.hpp>

namespace trade_ngin {

/**
 * @brief The one pass's record of the last rebalance of a book that names an overlay sleeve
 *        (LOOP_SPEC sections 4 to 6): what the runners store as risk_detail and risk_scale and
 *        the forecast-sign closes they book as their own fills.
 */
struct OnePassDay {
    bool ran{false};      ///< the pass ran on this call (false: no overlay sleeve, or nothing to size)
    bool sized{false};    ///< a rebalance that trades (not a warm-up cycle)
    bool refused{false};  ///< the overlay could not answer: the held book is stored, nothing traded
    double risk_requested{1.0};        ///< m_t, 1.0 with no cut
    std::string binding_term{"none"};  ///< R, R_jump, R_shock, L_g, L_n or none
    std::string over_limit_after_rounding_terms;  ///< ';'-separated, empty when none
    double over_limit_after_rounding_excess{0.0}; ///< in units of the largest non-zero stored u_i
    std::string over_limit_by_hold_terms;         ///< ';'-separated, CAP included, empty when none
    std::string over_limit_by_hold_symbols;       ///< sorted, space-separated, empty when none
    bool overlay_blind{false};
    double sizing_capital{0.0};        ///< E_t
    /// The pass's own book over the capped target's gross (0: flat target). The live runners store
    /// PortfolioManager::delivered_scale_for_book of the book they store, not this figure.
    double risk_scale{0.0};
    double capped_target_gross{0.0};   ///< notional, at the raw signal closes, held rows included
    double stored_gross{0.0};          ///< notional of the stored book at the same closes
    /// The forecast-sign closes, per sleeve and symbol: the signed fill that took the sleeve's held
    /// quantity to flat before the search.
    std::map<std::string, std::map<std::string, double>> sign_closes;

    /// A rebalance whose row carries risk_detail: the pass ran on a cycle that trades and the
    /// overlay answered.
    bool stores_detail() const { return ran && sized && !refused; }
};

/// risk_detail's keys, in the order of section 7.3's table.
inline constexpr std::array<const char*, 9> kRiskDetailKeys = {
    "risk_requested",
    "binding_term",
    "over_limit_after_rounding_terms",
    "over_limit_after_rounding_excess",
    "over_limit_by_hold_terms",
    "over_limit_by_hold_symbols",
    "overlay_blind",
    "sizing_capital",
    "account_value"};

/**
 * @brief The risk_detail object of one sized rebalance. `account_value` is V_t at the instant the
 *        sizing capital was read: the starting capital plus the cumulative settled net P&L the
 *        sizing capital was built from (section 3.1).
 */
inline nlohmann::json risk_detail_json(const OnePassDay& day, double account_value) {
    nlohmann::json j = nlohmann::json::object();
    auto text_or_null = [](const std::string& text) {
        return text.empty() ? nlohmann::json(nullptr) : nlohmann::json(text);
    };
    j["risk_requested"] = day.risk_requested;
    j["binding_term"] = day.binding_term;
    j["over_limit_after_rounding_terms"] = text_or_null(day.over_limit_after_rounding_terms);
    j["over_limit_after_rounding_excess"] =
        day.over_limit_after_rounding_terms.empty()
            ? nlohmann::json(nullptr)
            : nlohmann::json(day.over_limit_after_rounding_excess);
    j["over_limit_by_hold_terms"] = text_or_null(day.over_limit_by_hold_terms);
    j["over_limit_by_hold_symbols"] = text_or_null(day.over_limit_by_hold_symbols);
    j["overlay_blind"] = day.overlay_blind;
    j["sizing_capital"] = day.sizing_capital;
    j["account_value"] = account_value;
    return j;
}

}  // namespace trade_ngin
