#pragma once

// What a run records about its own configuration (T-8D R24 and R25, T-8D-2 R53 and R73, LOOP_SPEC
// sections 7.5 and 7.5.1): each sleeve's resolved values, the keys of trading.live_results.config
// that are taken on the day's sizing capital, and the keys the run-metadata JSON objects gain.
// The runners are `main()`s; the JSON they store is built here so it can be tested.

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace trade_ngin {

/// The nested key of live_results.config and of backtest.run_metadata.portfolio_config that holds
/// one object per sleeve, keyed by the sleeve's id.
inline constexpr const char* kSleevesKey = "sleeves";
/// The sessions a year the run's stored statistics are annualised with: the factor the figures
/// USE. A live run's is its own grid's (defaults.json statistics.sessions_per_year). A backtest's
/// is the factor its calculator applies today, 252 on every book.
inline constexpr const char* kStatisticsKKey = "statistics_K";
/// A backtest only, and only while the two differ: the ruled sessions a year of the run's grid
/// (311.0574 on a futures book) that its stored statistics do NOT use yet. T-8b's commit (1)
/// aligns the calculator; statistics_K then takes this value and this key goes.
inline constexpr const char* kStatisticsKRuledNotAppliedKey = "statistics_K_ruled_not_applied";

/**
 * @brief One sleeve's values as the runner resolved them from the loaded configuration and handed
 *        them to the strategy it built. `allocation` is the sleeve's share of the sizing capital
 *        (strategies.<id>.default_allocation after the runner's normalisation), a sizing input.
 *        `idm` is absent on a sleeve whose strategy has none (the equity mean reversion sleeve).
 */
struct ResolvedSleeve {
    std::string id;
    std::optional<double> idm;
    double risk_target{0.0};
    double allocation{0.0};
};

/// {"<sleeve id>": {"idm", "risk_target", "allocation"}}: only the keys the sleeve resolves.
/// `with_allocation` false leaves the allocation out, for a JSON object that already carries it
/// (backtest.run_metadata.portfolio_config's strategy_allocations).
inline nlohmann::json resolved_sleeves_json(const std::vector<ResolvedSleeve>& sleeves,
                                            bool with_allocation = true) {
    nlohmann::json out = nlohmann::json::object();
    for (const auto& sleeve : sleeves) {
        nlohmann::json j = nlohmann::json::object();
        if (sleeve.idm) j["idm"] = *sleeve.idm;
        j["risk_target"] = sleeve.risk_target;
        if (with_allocation) j["allocation"] = sleeve.allocation;
        out[sleeve.id] = j;
    }
    return out;
}

/// The run's one INFO line: every sleeve's resolved idm, risk_target and allocation.
inline std::string resolved_sleeves_log_line(const std::string& book,
                                             const std::vector<ResolvedSleeve>& sleeves) {
    std::string line = "SLEEVE_CONFIG book=" + book;
    for (const auto& sleeve : sleeves) {
        line += " sleeve=" + sleeve.id +
                " idm=" + (sleeve.idm ? std::to_string(*sleeve.idm) : std::string("none")) +
                " risk_target=" + std::to_string(sleeve.risk_target) +
                " allocation=" + std::to_string(sleeve.allocation);
    }
    return line;
}

/**
 * @brief The day's sizing capital E_t a futures row's config states. A row that stores risk_detail
 *        states risk_detail's sizing_capital. A row with no risk_detail (the overlay refused, or
 *        nothing was sized) states the capital the run read and set before the rebalance. A sizing
 *        hold read no capital: there is none to state.
 */
inline std::optional<double> stored_config_sizing_capital(bool row_stores_risk_detail,
                                                          double risk_detail_sizing_capital,
                                                          bool run_read_sizing_capital,
                                                          double run_sizing_capital) {
    if (row_stores_risk_detail) return risk_detail_sizing_capital;
    if (run_read_sizing_capital) return run_sizing_capital;
    return std::nullopt;
}

/**
 * @brief trading.live_results.config of a futures book's row. `strategy_type` is the row's
 *        strategy_id, top level, a JSON string (AlgoLens keys on it). capital_allocation is the
 *        day's sizing capital and gross_leverage the row's gross notional over it; both are left
 *        out on a row with no sizing capital, or one that is not positive. The top-level weight,
 *        risk_target and idm are the literals the string has always carried, kept for readers
 *        outside this repository: the sleeves' own values are under "sleeves".
 */
inline nlohmann::json futures_live_results_config_json(const std::string& strategy_id,
                                                       const std::vector<ResolvedSleeve>& sleeves,
                                                       std::optional<double> sizing_capital,
                                                       int active_positions, double gross_notional,
                                                       double net_notional) {
    nlohmann::json j;
    j["strategy_type"] = strategy_id;
    j["weight"] = 0.03;
    j["risk_target"] = 0.2;
    j["idm"] = 2.5;
    j[kSleevesKey] = resolved_sleeves_json(sleeves);
    if (sizing_capital && *sizing_capital > 0.0) {
        j["capital_allocation"] = *sizing_capital;
        j["gross_leverage"] = gross_notional / *sizing_capital;
    }
    j["active_positions"] = active_positions;
    j["gross_notional"] = gross_notional;
    j["net_notional"] = net_notional;
    return j;
}

/// The window rule and warm-up of a backtest run and its statistics convention, added to
/// backtest.run_metadata.portfolio_config. An unset frozen_end_date is stored as null.
/// statistics_K is the factor the run's stored statistics were annualised with (`applied_k`, the
/// calculator's own), never a factor they do not use; the ruled factor of the run's grid
/// (`ruled_k`) is recorded beside it, under its own key, only while it is not the one applied.
inline void add_backtest_run_keys(nlohmann::json& portfolio_config, int lookback_years,
                                  const std::string& frozen_end_date, int warmup_days,
                                  double applied_k, double ruled_k) {
    portfolio_config["lookback_years"] = lookback_years;
    portfolio_config["frozen_end_date"] =
        frozen_end_date.empty() ? nlohmann::json(nullptr) : nlohmann::json(frozen_end_date);
    portfolio_config["warmup_days"] = warmup_days;
    portfolio_config[kStatisticsKKey] = applied_k;
    if (ruled_k != applied_k) portfolio_config[kStatisticsKRuledNotAppliedKey] = ruled_k;
}

}  // namespace trade_ngin
