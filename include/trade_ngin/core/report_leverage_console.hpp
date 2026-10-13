// include/trade_ngin/core/report_leverage_console.hpp
#pragma once

#include <cstdio>
#include <optional>
#include <string>

namespace trade_ngin {

/**
 * @brief A notional over a capital, or no value when there is no capital to divide by (none was
 *        read, or it is not positive). The rule live_results.config's gross_leverage follows
 *        (core/resolved_sleeves.hpp, futures_live_results_config_json).
 */
inline std::optional<double> leverage_on(double notional, std::optional<double> capital) {
    if (!capital || !(*capital > 0.0))
        return std::nullopt;
    return notional / *capital;
}

/**
 * @brief One console leverage line (ends with a newline): `<name> (<denominator>): 1.60x`, or
 *        `<name> (<denominator>): n/a (<why>)` when the denominator has no value. T-8D R24 and
 *        T-8D-2 R68: every console leverage line names what it divides by.
 */
inline std::string leverage_console_line(const std::string& name, const std::string& denominator,
                                         double notional, std::optional<double> capital,
                                         const std::string& why_no_value) {
    const std::string head = name + " (" + denominator + "): ";
    const std::optional<double> leverage = leverage_on(notional, capital);
    if (!leverage)
        return head + "n/a (" + why_no_value + ")\n";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2fx", *leverage);
    return head + buf + "\n";
}

/// The futures runners' first line: the day's gross notional over the day's sizing capital E_t,
/// the figure the day's live_results.config states as gross_leverage. No figure on a run that
/// read no sizing capital (a sizing hold), where the config carries no gross_leverage either.
inline std::string gross_leverage_console_on_sizing_capital(double gross_notional,
                                                            std::optional<double> sizing_capital) {
    return leverage_console_line("Gross Leverage", "on the sizing capital", gross_notional,
                                 sizing_capital, "no sizing capital was read this run");
}

/// The futures runners' day-T line: the day's gross notional over the day's account value V_t
/// (the row's current_portfolio_value).
inline std::string gross_leverage_console_on_account_value(double gross_notional,
                                                           double account_value) {
    return leverage_console_line("Gross Leverage", "on the account value", gross_notional,
                                 account_value, "the account value is not positive");
}

/// The equity runner's one line: the day's gross notional over the day's account value (the
/// row's current_portfolio_value), the row's stored portfolio_leverage.
inline std::string portfolio_leverage_console_on_account_value(double gross_notional,
                                                               double account_value) {
    return leverage_console_line("Portfolio Leverage", "gross / the day's account value",
                                 gross_notional, account_value,
                                 "the account value is not positive");
}

}  // namespace trade_ngin
