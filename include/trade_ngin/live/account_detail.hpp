// include/trade_ngin/live/account_detail.hpp
//
// The account detail file attached to every report (HD 10-13): one row for the day, taken from the stored
// live_results row (its text as stored: no new computation) and from the over-limit report the email
// already holds. Column names are plain words. A figure the day does not carry is an empty cell.
#pragma once

#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <memory>

#include "trade_ngin/core/report_over_limit.hpp"
#include "trade_ngin/data/database_interface.hpp"

namespace trade_ngin {

/// The stored live_results columns the file reads, by the name the query gives them.
inline const std::vector<std::string>& account_detail_stored_columns() {
    static const std::vector<std::string> kColumns = {
        "current_portfolio_value", "sizing_capital",         "gross_leverage",
        "net_leverage",            "overlay_gross_leverage", "overlay_net_leverage",
        "overlay_risk",            "var_95_1d",              "max_drawdown_sizing",
        "volatility_sizing",       "worst_day_sizing"};
    return kColumns;
}

/// The SELECT of those columns for one stored row (the same key form the report's other reads use).
inline std::string account_detail_query(const std::string& strategy_id,
                                        const std::string& portfolio_id, const std::string& date) {
    return "SELECT current_portfolio_value, risk_detail->>'sizing_capital', gross_leverage, "
           "net_leverage, overlay_gross_leverage, overlay_net_leverage, overlay_risk, var_95_1d, "
           "max_drawdown_sizing, volatility_sizing, worst_day_sizing "
           "FROM trading.live_results WHERE strategy_id = '" +
           strategy_id + "' AND portfolio_id = '" + portfolio_id + "' AND date = '" + date +
           "' ORDER BY date DESC LIMIT 1";
}

/// The stored row's text by column name; empty when the row cannot be read.
inline std::map<std::string, std::string> read_account_detail_row(
    const std::shared_ptr<DatabaseInterface>& db, const std::string& strategy_id,
    const std::string& portfolio_id, const std::string& date) {
    std::map<std::string, std::string> row;
    if (!db) return row;
    auto result = db->execute_query(account_detail_query(strategy_id, portfolio_id, date));
    if (result.is_error() || result.value()->num_rows() == 0) return row;
    const auto table = result.value();
    const auto& names = account_detail_stored_columns();
    for (int i = 0; i < table->num_columns() && i < static_cast<int>(names.size()); ++i) {
        auto array = std::static_pointer_cast<arrow::StringArray>(table->column(i)->chunk(0));
        if (array && !array->IsNull(0)) row[names[static_cast<size_t>(i)]] = array->GetString(0);
    }
    return row;
}

/// The (item, value) pairs of the day, in reading order (grouped by account_detail_rows). `stored` maps the stored column names to
/// their text; `over_limit` is null on a book that has none (the equity book).
inline std::vector<std::pair<std::string, std::string>> account_detail_columns(
    const std::string& date, const std::map<std::string, std::string>& stored,
    const OverLimitReport* over_limit,
    const std::map<std::string, double>& email_figures = {}) {
    // The figures the email's Account block shows are the file's account-value columns, so the two
    // read the same; where the email has none, the stored row's own text.
    auto email_text = [&](const char* key, const char* stored_key, const char* format) {
        const auto it = email_figures.find(key);
        if (it == email_figures.end()) {
            const auto st = stored.find(stored_key);
            return st == stored.end() ? std::string() : st->second;
        }
        char buf[48];
        std::snprintf(buf, sizeof(buf), format, it->second);
        return std::string(buf);
    };
    auto get = [&](const char* key) {
        const auto it = stored.find(key);
        return it == stored.end() ? std::string() : it->second;
    };
    auto limit = [&](double value) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2f", value);
        return std::string(buf);
    };
    std::vector<std::pair<std::string, std::string>> cols;
    cols.emplace_back("date", date);
    cols.emplace_back("current portfolio value",
                      email_text("Current Portfolio Value", "current_portfolio_value", "%.2f"));
    cols.emplace_back("sizing capital", get("sizing_capital"));
    cols.emplace_back("gross leverage on the account value",
                      email_text("Gross Leverage", "gross_leverage", "%.4f"));
    cols.emplace_back("net leverage on the account value",
                      email_text("Net Leverage", "net_leverage", "%.4f"));
    cols.emplace_back("gross leverage on the sizing capital", get("overlay_gross_leverage"));
    cols.emplace_back("net leverage on the sizing capital", get("overlay_net_leverage"));
    cols.emplace_back("gross leverage limit", over_limit && over_limit->gross_limit > 0
                                                  ? limit(over_limit->gross_limit)
                                                  : std::string());
    cols.emplace_back("net leverage limit", over_limit && over_limit->net_limit > 0
                                                ? limit(over_limit->net_limit)
                                                : std::string());
    cols.emplace_back("per-name cap", over_limit && over_limit->per_name_cap > 0
                                          ? limit(over_limit->per_name_cap)
                                          : std::string());
    const bool reading = over_limit && over_limit->has_reading;
    cols.emplace_back("over limit after rounding",
                      reading ? over_limit_terms_words(over_limit->over_limit_after_rounding_terms)
                              : std::string());
    cols.emplace_back("over limit by hold",
                      reading ? over_limit_terms_words(over_limit->over_limit_by_hold_terms)
                              : std::string());
    cols.emplace_back("held beyond the per-name cap",
                      reading && over_limit_terms_name_cap(over_limit->over_limit_by_hold_terms)
                          ? over_limit->over_limit_by_hold_symbols
                          : std::string());
    cols.emplace_back("expected risk", get("overlay_risk"));
    cols.emplace_back("1-day 95% VaR", get("var_95_1d"));
    cols.emplace_back("max drawdown on the sizing capital", get("max_drawdown_sizing"));
    cols.emplace_back("volatility on the sizing capital", get("volatility_sizing"));
    cols.emplace_back("worst day on the sizing capital", get("worst_day_sizing"));
    return cols;
}


/// The file as rows: group, item, value (HD 10-14): one line per figure, grouped so it reads top to
/// bottom: the account, leverage on the account value, leverage on the sizing capital, the limits,
/// the over-limit marks, risk, and the statistics on the sizing capital.
inline std::vector<std::vector<std::string>> account_detail_rows(
    const std::vector<std::pair<std::string, std::string>>& cols) {
    static const std::vector<std::pair<std::string, std::vector<std::string>>> kGroups = {
        {"Day", {"date"}},
        {"Account", {"current portfolio value", "sizing capital"}},
        {"Leverage on the account value",
         {"gross leverage on the account value", "net leverage on the account value"}},
        {"Leverage on the sizing capital",
         {"gross leverage on the sizing capital", "net leverage on the sizing capital"}},
        {"Limits", {"gross leverage limit", "net leverage limit", "per-name cap"}},
        {"Over-limit marks",
         {"over limit after rounding", "over limit by hold", "held beyond the per-name cap"}},
        {"Risk", {"expected risk", "1-day 95% VaR"}},
        {"Statistics on the sizing capital",
         {"max drawdown on the sizing capital", "volatility on the sizing capital",
          "worst day on the sizing capital"}}};
    std::map<std::string, std::string> by_name(cols.begin(), cols.end());
    std::vector<std::vector<std::string>> rows;
    rows.push_back({"group", "item", "value"});
    for (const auto& [group, items] : kGroups)
        for (const auto& item : items) {
            const auto it = by_name.find(item);
            rows.push_back({group, item, it == by_name.end() ? std::string() : it->second});
        }
    return rows;
}

}  // namespace trade_ngin
