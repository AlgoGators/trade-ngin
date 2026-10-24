// include/trade_ngin/core/report_rolls.hpp
//
// Rolls step 4 (R4b; HD 2026-10-23, rulings 10 and 18): what the daily report says about the futures
// book's rolls with portfolio.json's per_contract_rolls on.
//
//   * The roll line, one per roll in the roll legs block: the symbol's name as the Symbols Reference
//     has it, the two contract months by name, the market move booked on the roll bar, the price
//     difference between the two contracts (points and dollars, never profit or loss) and the roll's
//     cost (both legs). The figures are the closing leg's split (migration 040).
//   * The Rollover Warning box, the design and format of the box removed on 2026-10-13 (b3fbb849 on
//     main-nonQT; c00dd1d6 on main): a held contract is listed once its roll window opens (five CME
//     trading days before its deadline), with the contract the rule rolls into (the cycle's front
//     month), the run date by which the roll happens at the latest (the last actionable bar's run) and
//     the deadline it guards. Built from the per-contract rule's outlook, never a hand-coded calendar.
//   * The Symbols Reference's Front Month column (the contract held) and its Contract Months column
//     read from the root's cycle.
//
// With the switch off the runner hands nothing here and the report is today's, byte for byte. Pure
// and header-only: the runners fill the structs (PostgresDatabase::report_roll_view), the report
// renders them, the tests pin both.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "trade_ngin/core/types.hpp"

namespace trade_ngin {

/// One held futures root's roll outlook on the settled day (contract_rolls::ContractRollSeries::outlook).
/// The contracts print as the removed box printed them (git show c00dd1d6^:src/core/email_sender.cpp,
/// get_front_month_symbol): the IB symbol, the month code and the year's last digit ("JPYZ5").
struct ReportRollOutlook {
    std::string root;            ///< "6J": the Databento symbol (the Symbols Reference's key)
    std::string label;           ///< "JPY": the IB symbol the box prints (the root when not known)
    /// "JPYZ5": the contract the book holds after the settled day's bar (the roll tracker's, so the
    /// old contract until its roll legs are booked), in the IB format
    std::string held;
    std::string held_symbol;     ///< "6JZ5": the same contract's own code (contract_ohlcv_1d.raw_symbol)
    std::string cycle;           ///< "HMUZ": the root's cycle (contract_rolls::roll_cycle_table)
    /// False for a root the rule does not roll (the micros stay on the vendor's series until their
    /// full-size contracts' bars are loaded): its warning line names the last trading day only.
    bool rule_rolls{false};
    /// The settled day is inside the held contract's window (from the fifth CME trading day before its
    /// deadline). False while the rule has rolled and the book still holds the old contract.
    bool in_window{false};
    std::string roll_by;         ///< YYYY-MM-DD: the run date of the last actionable bar (empty: micros)
    std::string deadline;        ///< YYYY-MM-DD: the deadline (first notice or last trading day)
    bool first_notice{false};    ///< the deadline is the first notice day
    std::string target;          ///< "JPYH6": the cycle's front month after the held contract (IB format)
};

/// "JPYZ5" from the IB symbol "JPY", the root "6J" and the contract code "6JZ5" (or "NGV26"): the IB
/// symbol, the month code, the year's last digit. The code itself when it is not the root's.
inline std::string report_ib_contract(const std::string& ib, const std::string& root, const std::string& code) {
    if (code.size() < root.size() + 2 || code.compare(0, root.size(), root) != 0) return code;
    return (ib.empty() ? root : ib) + code[root.size()] + code.back();
}

/// The words of one roll's line (keyed by its closing leg's exec id).
struct ReportRollNames {
    std::string name;        ///< "Silver": the contract metadata's Name
    std::string from_month;  ///< "December"
    std::string to_month;    ///< "March"
    int gap_places{4};       ///< the decimals of the contract's tick (0.005 -> 3)
};

/// Everything the report is handed about rolls; empty (the switch off) prints nothing new.
struct ReportRolls {
    bool per_contract_rolls{false};
    std::map<std::string, ReportRollOutlook> outlook;  ///< by root
    std::map<std::string, ReportRollNames> names;       ///< by closing leg exec id
};

namespace report_rolls_detail {
inline std::string with_commas(const std::string& digits) {
    std::string text = digits;
    const size_t dot = text.find('.');
    for (int at = static_cast<int>(dot == std::string::npos ? text.size() : dot) - 3; at > 0; at -= 3)
        text.insert(static_cast<size_t>(at), ",");
    return text;
}
}  // namespace report_rolls_detail

/// "January" for 1 ... "December" for 12; empty otherwise.
inline std::string report_month_name(unsigned month) {
    static const char* const kNames[] = {"January", "February", "March",     "April",   "May",      "June",
                                         "July",    "August",   "September", "October", "November", "December"};
    return month >= 1 && month <= 12 ? kNames[month - 1] : std::string();
}

/// The month of a contract code ("SIZ5" with root "SI" -> 12), 0 when the code does not have one.
inline unsigned report_contract_month(const std::string& root, const std::string& raw_symbol) {
    static const std::string kCodes = "FGHJKMNQUVXZ";
    if (raw_symbol.size() <= root.size() || raw_symbol.compare(0, root.size(), root) != 0) return 0;
    const size_t at = kCodes.find(raw_symbol[root.size()]);
    return at == std::string::npos ? 0 : static_cast<unsigned>(at + 1);
}

/// A cycle's months as the Contract Months column prints them: "HKNUZ" -> "Mar May Jul Sep Dec".
inline std::string report_cycle_months(const std::string& cycle) {
    static const std::string kCodes = "FGHJKMNQUVXZ";
    std::string out;
    for (char c : cycle) {
        const size_t at = kCodes.find(c);
        if (at == std::string::npos) continue;
        if (!out.empty()) out += " ";
        out += report_month_name(static_cast<unsigned>(at + 1)).substr(0, 3);
    }
    return out;
}

/// The decimals a tick needs (0.005 -> 3, 0.015625 -> 6, 0.25 -> 2, 1 -> 0); 4 when the tick is not
/// positive, and at most 8.
inline int report_tick_places(double tick) {
    if (!(tick > 0.0)) return 4;
    for (int places = 0; places <= 8; ++places) {
        const double scaled = tick * std::pow(10.0, places);
        if (std::abs(scaled - std::round(scaled)) < 1e-9 * std::max(1.0, scaled)) return places;
    }
    return 8;
}

/// Whole dollars with thousands separators, the sign in front: -1075.4 -> "-$1,075".
inline std::string report_whole_dollars(double value) {
    char buf[64];
    const double rounded = std::round(value);
    std::snprintf(buf, sizeof(buf), "%.0f", std::abs(rounded));
    return std::string(rounded < 0 ? "-$" : "$") + report_rolls_detail::with_commas(buf);
}

/// Dollars to the cent with thousands separators, the sign in front: 45.2916 -> "$45.29".
inline std::string report_cents(double value) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f", std::abs(value));
    const std::string text = buf;
    return std::string(value < 0 && text != "0.00" ? "-$" : "$") + report_rolls_detail::with_commas(text);
}

/**
 * @brief The roll line: "Silver, December to March: market move booked -$1,075; contract price
 *        difference 0.670 ($3,350, not profit or loss); roll cost $45.29."
 *
 * `closing` is the roll's closing leg (its split, migration 040), `roll_cost` both legs' costs. A
 * roll whose names were not found reads its symbol and no months. The difference prints unsigned. A
 * leg with no split (a roll of the vendor's series) gets the roll cost only, with the reason.
 */
inline std::string report_roll_line(const ReportRollNames& names, const std::string& symbol,
                                    const ExecutionReport& closing, double roll_cost) {
    std::string text = names.name.empty() ? symbol : names.name;
    if (!names.from_month.empty() && !names.to_month.empty())
        text += ", " + names.from_month + " to " + names.to_month;
    if (!closing.roll_bar_move || !closing.roll_gap_points || !closing.roll_gap_value) {
        // A roll of the vendor's series (the micros until their full-size contracts are loaded): no two
        // closes on one bar, so no move or difference to show (lead, R4b fix round 1).
        return text + ": roll cost " + report_cents(roll_cost) +
               " (market move and contract price difference shown once the full-size contracts are loaded).";
    }
    // The difference is not profit or loss: printed without a sign (lead, R4b fix round 1); stored signed.
    char points[64];
    std::snprintf(points, sizeof(points), "%.*f", names.gap_places, std::abs(*closing.roll_gap_points));
    text += ": market move booked " + report_whole_dollars(*closing.roll_bar_move) +
            "; contract price difference " + points + " (" +
            report_whole_dollars(std::abs(*closing.roll_gap_value)) + ", not profit or loss); roll cost " +
            report_cents(roll_cost) + ".";
    return text;
}

/// "07-29" from "2026-07-29".
inline std::string report_month_day(const std::string& ymd) {
    return ymd.size() == 10 ? ymd.substr(5) : ymd;
}

/**
 * @brief The Rollover Warning box (the 2026-10-13 design: the red box, "Currently holding X -> Switch
 *        to Y"), one line per held root whose window is open, in root order, with the latest run date
 *        of the roll and the deadline: "by 07-30, first notice 07-31". A micro (the vendor's series)
 *        names its last trading day only. Empty when no held contract is inside its window.
 */
inline std::string report_rollover_warning_html(const ReportRolls& rolls) {
    std::vector<const ReportRollOutlook*> due;
    for (const auto& [root, o] : rolls.outlook) {
        (void)root;
        if (o.in_window && !o.held.empty()) due.push_back(&o);
    }
    if (due.empty()) return std::string();
    std::string html =
        "<div style=\"background-color: #fff5f5; border-left: 4px solid #dc2626; padding: 15px; margin: 20px 0; "
        "font-size: 13px; font-family: Arial, sans-serif;\">\n"
        "<p style=\"color: #991b1b; margin: 0 0 10px 0;\"><strong>Rollover Notice:</strong> These contracts are "
        "approaching their roll. Each position moves to the next contract of its cycle on or before the date "
        "shown.</p>\n"
        "<ul style=\"color: #991b1b; margin: 5px 0 0 20px; padding: 0;\">\n";
    for (const auto* o : due) {
        const std::string label = o->label.empty() ? o->root : o->label;
        if (!o->rule_rolls) {
            // The micros: they roll when the vendor's series does; the line names the last trading day.
            html += "<li><strong>" + label + ":</strong> Currently holding <strong>" + o->held +
                    "</strong>, rolls with the vendor's series, last trading day " + report_month_day(o->deadline) +
                    "</li>\n";
            continue;
        }
        html += "<li><strong>" + label + ":</strong> Currently holding <strong>" + o->held +
                "</strong> \xE2\x86\x92 Switch to <strong>" + (o->target.empty() ? std::string("the next contract") : o->target) +
                "</strong> by " + report_month_day(o->roll_by) + ", " +
                (o->first_notice ? "first notice " : "last trading day ") + report_month_day(o->deadline) +
                "</li>\n";
    }
    html += "</ul>\n</div>\n";
    return html;
}

/// The Front Month cell of a Symbols Reference row (the contract held), empty when not known.
inline std::string report_front_month(const ReportRolls& rolls, const std::string& root) {
    const auto it = rolls.outlook.find(root);
    return it == rolls.outlook.end() ? std::string() : it->second.held;
}

/// The Contract Months cell with the switch on: the root's cycle; `metadata_months` when the root
/// has no outlook.
inline std::string report_contract_months(const ReportRolls& rolls, const std::string& root,
                                          const std::string& metadata_months) {
    const auto it = rolls.outlook.find(root);
    return it == rolls.outlook.end() || it->second.cycle.empty() ? metadata_months
                                                                 : report_cycle_months(it->second.cycle);
}

/// The footer's futures sentence with the switch on (it replaces "We currently only provide data for
/// the front-month contract and are working on integrating roll data.").
inline constexpr const char* kReportRollsFooter =
    "Each position is held in the front contract of its cycle and rolled to the next one before the first "
    "notice day or the last trading day.";

/// The roll footnote with the switch on, for a settled row whose bar was a per-contract roll bar.
inline constexpr const char* kReportRollBarNote =
    "Contract rolled that day. The old contract's move that day is booked; the price difference between "
    "the two contracts is not profit or loss.";

}  // namespace trade_ngin
