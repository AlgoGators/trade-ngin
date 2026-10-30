#pragma once

// The loop's record of one rebalance, and the risk_detail jsonb
// object built from it (trading.live_results.risk_detail and backtest.equity_curve.risk_detail,
// migration 020). Nine flat keys, pinned by tests/portfolio/test_one_pass_book.cpp
// (RiskDetailJson.TheNineKeysArePinned).

#include <array>
#include <map>
#include <string>

#include <nlohmann/json.hpp>

namespace trade_ngin {

/**
 * @brief The one pass's record of the last rebalance of a book that names an overlay sleeve:
 *        what the runners store as risk_detail and risk_scale and
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
    /// The overlay's readings of the pass's stored book (one_pass::DayResult::stored_readings)
    /// and the window's annualisation factor, copied on a rebalance the overlay answered: the
    /// source of the overlay columns (risk/overlay_columns.hpp). On a blind window
    /// `covariance_readings` is false and the three risk readings are 0 and not computed. The
    /// live runners store PortfolioManager::overlay_readings_for_book of the book they store.
    bool covariance_readings{false};
    double overlay_risk{0.0};            ///< R of the stored book
    double overlay_risk_jump{0.0};       ///< R_jump
    double overlay_risk_shock{0.0};      ///< R_shock
    double overlay_gross_leverage{0.0};  ///< L
    double overlay_net_leverage{0.0};    ///< L_net, signed
    double window_bars_per_year{0.0};    ///< the gate window's bars a year, 0 while blind
    /// The forecast-sign closes, per sleeve and symbol: the signed fill that took the sleeve's held
    /// quantity to flat before the search.
    std::map<std::string, std::map<std::string, double>> sign_closes;

    /// Contract C9, the desk's risk report: the overlay's readings of the book the pass stored and
    /// the limits in force, in reading units (the three risk limits are their ratio x tau). Set on
    /// a pass that answered (`checks` true); written only into the desk record of a qt row
    /// (desk_checks_json), never into risk_detail's nine keys.
    bool checks{false};
    bool covariance_measured{false};  ///< R, R_jump and R_shock were read (false: a blind window)
    double tau{0.0};
    double stored_r{0.0};
    double stored_r_jump{0.0};
    double stored_r_shock{0.0};
    double stored_l{0.0};
    double stored_l_net{0.0};  ///< signed; the limit is on its absolute value
    double limit_r{0.0};
    double limit_r_jump{0.0};
    double limit_r_shock{0.0};
    double limit_l{0.0};
    double limit_l_net{0.0};
    double r_max_ratio{0.0};        ///< the configured R_max, R_jump_max and R_shock_max
    double r_jump_max_ratio{0.0};
    double r_shock_max_ratio{0.0};
    double per_name_cap{0.0};       ///< L: the most |contracts x weight per contract| per symbol
    double largest_name{0.0};       ///< the stored book's largest |N_i u_i|
    std::string largest_name_symbol;
    int trim_max{0};
    double trimmed_contracts{0.0};  ///< contracts the trim removed, summed over the symbols

    /// A rebalance whose row carries risk_detail: the pass ran on a cycle that trades and the
    /// overlay answered.
    bool stores_detail() const { return ran && sized && !refused; }
};

/// risk_detail's keys, in their fixed order.
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
 *        sizing capital was built from.
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

/**
 * @brief Contract C9: every risk limit of the stored book with its reading and its max, for the
 *        desk record (`risk_detail.desk.checks`). Null when the pass did not answer (no pass, a
 *        refusal). A covariance reading on a blind window is null (not measured).
 */
inline nlohmann::json desk_checks_json(const OnePassDay& day) {
    if (!day.checks) return nlohmann::json(nullptr);
    auto reading = [&](double value) {
        return day.covariance_measured ? nlohmann::json(value) : nlohmann::json(nullptr);
    };
    nlohmann::json j = nlohmann::json::object();
    j["tau"] = day.tau;
    j["covariance_measured"] = day.covariance_measured;
    j["R"] = {{"value", reading(day.stored_r)}, {"max", day.limit_r},
              {"max_ratio", day.r_max_ratio}};
    j["R_jump"] = {{"value", reading(day.stored_r_jump)}, {"max", day.limit_r_jump},
                   {"max_ratio", day.r_jump_max_ratio}};
    j["R_shock"] = {{"value", reading(day.stored_r_shock)}, {"max", day.limit_r_shock},
                    {"max_ratio", day.r_shock_max_ratio}};
    j["L_g"] = {{"value", day.stored_l}, {"max", day.limit_l}};
    j["L_n"] = {{"value", day.stored_l_net}, {"max", day.limit_l_net}};
    j["CAP"] = {{"value", day.largest_name}, {"max", day.per_name_cap},
                {"symbol", day.largest_name_symbol}};
    j["TRIM"] = {{"value", day.trimmed_contracts}, {"max", day.trim_max}};
    return j;
}

}  // namespace trade_ngin
