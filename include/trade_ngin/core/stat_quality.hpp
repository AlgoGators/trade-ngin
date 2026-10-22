// include/trade_ngin/core/stat_quality.hpp
// The STAT_QUALITY log lines (T-8D adversary F10, T-8D-2 R82): the held symbols that printed no
// bar on the date their row is settled on. Such a row is held and marked at its last close, so
// the day's figure carries no move of the symbol and its next bar carries the whole gap's. The
// lines say on which days a statistic rests on such rows; they are INFO lines and nothing reads
// them back: no stored value depends on this header.
//
// Live: one line a finalize (LivePnLManager::finalize_previous_day), the count and the list.
// Backtest: the coordinator's summary carries one run line and one line for every stored date
// with such a symbol (RunRecord).

#pragma once

#include <cstddef>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace trade_ngin {
namespace stat_quality {

/// ": A, B, C" in the set's order, or nothing for an empty set.
inline std::string symbol_list(const std::set<std::string>& symbols) {
    std::string out;
    for (const auto& symbol : symbols) out += (out.empty() ? ": " : ", ") + symbol;
    return out;
}

/// The line of one live finalize: `held` symbols of the settled book (non-zero quantity) and, of
/// them, the ones with no close on the settled date.
inline std::string finalize_line(size_t held, const std::set<std::string>& no_bar) {
    return "STAT_QUALITY finalize held=" + std::to_string(held) +
           " no_bar=" + std::to_string(no_bar.size()) + symbol_list(no_bar);
}

/// One stored date of a backtest that holds a symbol with no bar.
struct Day {
    std::string date;              ///< the UTC date of the stored rows
    size_t held{0};                ///< distinct symbols held that date (non-zero quantity)
    std::set<std::string> no_bar;  ///< of them, the symbols that printed no bar that date
    size_t no_bar_rows{0};         ///< their position rows (one a sleeve holding the symbol)
};

/// The line of one such date; `on_grid` says whether the date is an observation of the run's
/// statistics (a date of its statistics grid).
inline std::string day_line(const Day& day, bool on_grid) {
    return "STAT_QUALITY date=" + day.date + " held=" + std::to_string(day.held) +
           " no_bar=" + std::to_string(day.no_bar.size()) + " grid=" + (on_grid ? "yes" : "no") +
           symbol_list(day.no_bar);
}

/// A backtest's record over its stored dates, and its summary lines.
class RunRecord {
public:
    /// One stored date: the distinct symbols held, the ones of them with no bar that date and
    /// their position rows. A date holding nothing is not counted.
    void note_day(const std::string& date, size_t held, std::set<std::string> no_bar,
                  size_t no_bar_rows) {
        if (held == 0) return;
        ++held_days_;
        if (no_bar.empty()) return;
        days_.push_back({date, held, std::move(no_bar), no_bar_rows});
    }

    void clear() {
        held_days_ = 0;
        days_.clear();
    }

    const std::vector<Day>& days() const { return days_; }
    size_t held_days() const { return held_days_; }

    /// The run line, then one line a recorded date in the order noted. The run line: the dates
    /// with a held symbol that printed no bar, of the dates with a held book; how many of them
    /// are dates of the statistics grid (`on_grid`); their position rows; their distinct symbols.
    std::vector<std::string> lines(const std::function<bool(const std::string&)>& on_grid) const {
        size_t grid_days = 0, rows = 0;
        std::set<std::string> symbols;
        std::vector<std::string> day_lines;
        day_lines.reserve(days_.size());
        for (const auto& day : days_) {
            const bool observed = on_grid(day.date);
            if (observed) ++grid_days;
            rows += day.no_bar_rows;
            symbols.insert(day.no_bar.begin(), day.no_bar.end());
            day_lines.push_back(day_line(day, observed));
        }
        std::vector<std::string> out;
        out.reserve(days_.size() + 1);
        out.push_back("STAT_QUALITY run no_bar_days=" + std::to_string(days_.size()) + " of " +
                      std::to_string(held_days_) + " held days; grid_days=" +
                      std::to_string(grid_days) + " rows=" + std::to_string(rows) +
                      " symbols=" + std::to_string(symbols.size()) +
                      ": the stored dates on which a held symbol printed no bar (held, marked "
                      "at last close), and how many of them are dates of the statistics grid");
        out.insert(out.end(), day_lines.begin(), day_lines.end());
        return out;
    }

private:
    size_t held_days_{0};
    std::vector<Day> days_;
};

}  // namespace stat_quality
}  // namespace trade_ngin
