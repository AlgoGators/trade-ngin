// include/trade_ngin/live/late_bar_warning.hpp
#pragma once

#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/session_classifier.hpp"

namespace trade_ngin {

/**
 * @brief A late bar: a bar of a held symbol that arrived after the run that should have consumed
 *        it. The warning a live run prints for it, and nothing else.
 *
 * The run dated D+1 settles the row dated D on the D bar. When that bar is missing at that run, the
 * row books 0. When it arrives afterwards, the next run settles its own Day T-1 against the D close
 * (the previous CONSUMED bar), so the move from the bar before D to D is on no stored row. The
 * ruling (HD 2026-10-07): no catch-up and no refusal; the run that first consumes the late bar
 * prints ONE line at WARNING naming the symbol, the bar's date, the unbooked amount (the quantity
 * of the stored row of that date x (the bar's close - the close of the consumed bar before it) x
 * the point value) and the remedy: re-run the date that settles the bar and every later date in
 * order. No stored value changes.
 *
 * A bar is late when all of these hold, walking back from the consumed bar before the symbol's
 * T-1 bar (so each late bar is named once, by the first run whose settlement steps over it):
 *   - the symbol is held in the stored T-1 book and has a consumed T-1 bar on this run;
 *   - the bar's close differs from the close of the consumed bar before it;
 *   - the two bars are of one contract (a change bar and a flip pair book 0 by design, section 6.6);
 *   - the stored rows of the bar's date hold the symbol at a non-zero quantity and every one of
 *     them books a realized P&L of exactly 0.
 * The walk stops at the first bar that fails one of them. A symbol whose roll this run settles
 * late (LiveRollState::late) is left out: that settlement books its unbooked bars itself.
 */
struct LateBar {
    std::string symbol;
    std::string date;            ///< the late bar's date
    Timestamp timestamp{};       ///< the late bar's timestamp
    double quantity{0.0};        ///< the stored rows' quantity on that date, summed over the sleeves
    double close{0.0};           ///< the late bar's close
    double previous_close{0.0};  ///< the close of the consumed bar before it

    double unbooked(double point_value) const { return quantity * (close - previous_close) * point_value; }
};

/// Every sleeve's stored rows of the date of `bar_time` (all symbols; empty when none or unreadable).
using StoredRowsOfDate = std::function<std::vector<Position>(const Timestamp& bar_time)>;

inline std::vector<LateBar> find_late_bars(const std::vector<Bar>& consumed, const std::string& t1_date,
                                           const std::unordered_set<std::string>& held_symbols,
                                           const std::unordered_set<std::string>& late_roll_symbols,
                                           const StoredRowsOfDate& stored_rows_of) {
    std::map<std::string, std::vector<const Bar*>> by_symbol;
    for (const auto& bar : consumed) {
        if (held_symbols.count(bar.symbol) && !late_roll_symbols.count(bar.symbol)) {
            by_symbol[bar.symbol].push_back(&bar);
        }
    }
    const auto date_of = [](const Bar* bar) {
        return SessionClassifier::ymd(SessionClassifier::day_of(bar->timestamp));
    };
    std::vector<LateBar> late;
    for (auto& [symbol, seq] : by_symbol) {
        std::stable_sort(seq.begin(), seq.end(),
                         [](const Bar* a, const Bar* b) { return a->timestamp < b->timestamp; });
        if (seq.size() < 3 || date_of(seq.back()) != t1_date) continue;
        for (size_t t = seq.size() - 2; t >= 1; --t) {
            const Bar* bar = seq[t];
            const Bar* before = seq[t - 1];
            if (bar->close == before->close) break;
            if (!bar->instrument_id.empty() && !before->instrument_id.empty() &&
                bar->instrument_id != before->instrument_id) {
                break;
            }
            double quantity = 0.0;
            bool booked = false;
            for (const auto& row : stored_rows_of(bar->timestamp)) {
                if (row.symbol != symbol) continue;
                if (static_cast<double>(row.realized_pnl) != 0.0) booked = true;
                quantity += static_cast<double>(row.quantity);
            }
            if (booked || quantity == 0.0) break;
            LateBar found;
            found.symbol = symbol;
            found.date = date_of(bar);
            found.timestamp = bar->timestamp;
            found.quantity = quantity;
            found.close = static_cast<double>(bar->close);
            found.previous_close = static_cast<double>(before->close);
            late.push_back(found);
        }
    }
    return late;
}

/// The one line of a late bar.
inline std::string late_bar_warning_line(const LateBar& bar, double point_value) {
    const std::string settling_run =
        SessionClassifier::ymd(SessionClassifier::day_of(bar.timestamp) + std::chrono::days(1));
    char text[640];
    std::snprintf(text, sizeof(text),
                  "LATE_BAR %s %s: this run is the first to consume that bar and the stored row of "
                  "%s books 0 at quantity %g, so %.2f is on no stored row (%g x (%.10g - %.10g) x "
                  "%g: the quantity, the bar's close less the close of the bar before it, the point "
                  "value). Nothing is changed by this run. Remedy: re-run %s (the date that settles "
                  "the %s bar) and every later date in order",
                  bar.symbol.c_str(), bar.date.c_str(), bar.date.c_str(), bar.quantity,
                  bar.unbooked(point_value), bar.quantity, bar.close, bar.previous_close, point_value,
                  settling_run.c_str(), bar.date.c_str());
    return text;
}

}  // namespace trade_ngin
