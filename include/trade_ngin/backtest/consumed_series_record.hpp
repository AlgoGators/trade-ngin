// include/trade_ngin/backtest/consumed_series_record.hpp
#pragma once

#include <cstdio>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "trade_ngin/core/record_file.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/roll_series.hpp"
#include "trade_ngin/data/session_classifier.hpp"

namespace trade_ngin {
namespace backtest {

/**
 * @brief The futures backtest's own record of what it consumed, for the oracle's acceptance
 *        (LOOP_SPEC v6.1 section 11: the oracle is fed the engine's consumed-bar set, its hold set
 *        and its rebalance calendar, and the engine's series is compared on every symbol-day).
 *
 * Enabled only when the environment names a directory (TRADE_NGIN_SERIES_DUMP_DIR); a production
 * run never sets it, and a disabled record does nothing. Every cycle with a signal group adds:
 *   - its signal date to the calendar (calendar.csv);
 *   - every non-SESSION verdict of the group that is not withheld to the hold set (hold.csv; a
 *     withheld date is a hold without a hold-set row, L-07), and with it every symbol the record
 *     has seen a bar of that printed no bar in the group (section 6.1: no verdict is a hold);
 *   - every bar it WITHHELD under K-01 to the withheld set (withheld.csv);
 *   - every bar it FED to the strategies and the PortfolioManager to that symbol's consumed
 *     sequence.
 * write() builds each symbol's series once over its whole consumed sequence with the engine's own
 * roll_series::build_series and writes series.csv: one row per consumed bar with the raw close, the
 * vendor id, the flags, the held id, the adjusted level A (anchored on the symbol's last consumed
 * bar) and the adjusted return r (0 on the symbol's first consumed bar).
 * T-ROLLX-FIX commit 2: the last MARKED group (the window's last bar group is marked but never a
 * signal group) is written to final_marks.csv with the marks' own reading of each bar: withheld,
 * change, the contract held after it.
 * Every file is written the way record_file.hpp states: enabling the record begins the run in the
 * directory (every record file an earlier run left there is removed), and write() returns false
 * when a file could not be opened or a row did not reach it.
 */
class ConsumedSeriesRecord {
public:
    void enable(std::string dir) {
        dir_ = std::move(dir);
        (void)record_file::begin_run(dir_);
    }
    bool enabled() const { return !dir_.empty(); }
    const std::string& dir() const { return dir_; }

    void add_cycle(const std::string& signal_date, const std::vector<SymbolDayVerdict>& verdicts,
                   const std::vector<Bar>& withheld, const std::vector<Bar>& fed) {
        if (!enabled()) return;
        calendar_.push_back(signal_date);
        std::set<std::string> printed;
        for (const auto& v : verdicts) {
            printed.insert(v.symbol);
            // L-07 (section 2.1): a withheld date is a hold without a hold-set row.
            if (!v.is_session() && !v.k01_withheld()) hold_.emplace_back(v.date, v.symbol);
        }
        // Section 6.1: a symbol with no verdict is held too. Every symbol the record has seen a
        // bar of (consumed or withheld, the history included) that printed none in this group.
        for (const auto& symbol : seen_) {
            if (printed.count(symbol) == 0) hold_.emplace_back(signal_date, symbol);
        }
        for (const auto& b : withheld) seen_.insert(b.symbol);
        for (const auto& b : fed) seen_.insert(b.symbol);
        for (const auto& b : withheld) {
            withheld_.emplace_back(SessionClassifier::ymd(SessionClassifier::day_of(b.timestamp)),
                                   b.symbol);
        }
        for (const auto& b : fed) consumed_[b.symbol].push_back(b);
    }

    /// The bars before the run's window that seed the estimators' history: the consumed ones join
    /// the symbol's consumed sequence (the series is built over all of it; series.csv writes the
    /// window's rows only) and the withheld ones join the withheld set. No calendar date, no hold.
    void add_history(const std::vector<SymbolDayVerdict>& withheld, const std::vector<Bar>& fed) {
        if (!enabled()) return;
        for (const auto& v : withheld) {
            withheld_.emplace_back(v.date, v.symbol);
            seen_.insert(v.symbol);
        }
        for (const auto& b : fed) {
            seen_.insert(b.symbol);
            consumed_[b.symbol].push_back(b);
            ++history_bars_[b.symbol];
        }
    }

    struct FinalMark {
        std::string symbol, date, instrument_id, held_id;
        double close{0.0};
        bool withheld{false}, change{false};
    };
    /// The marks' reading of the cycle's own bar group; the last call before write() is kept.
    void set_final_marks(std::vector<FinalMark> marks) {
        if (enabled()) final_marks_ = std::move(marks);
    }

    /// Writes the files into the directory; returns false when one cannot be opened or a row did
    /// not reach it.
    bool write() const {
        if (!enabled()) return true;
        std::ofstream cal(dir_ + "/calendar.csv"), hold(dir_ + "/hold.csv"),
            wh(dir_ + "/withheld.csv"), ser(dir_ + "/series.csv");
        if (!cal || !hold || !wh || !ser) return false;
        cal << "date\n";
        for (const auto& d : calendar_) cal << d << "\n";
        hold << "date,symbol\n";
        for (const auto& [d, s] : hold_) hold << d << "," << s << "\n";
        wh << "date,symbol\n";
        for (const auto& [d, s] : withheld_) wh << d << "," << s << "\n";
        ser << "symbol,date,close,instrument_id,change,confirm,flip,pending,held_id,A,r\n";
        for (const auto& [symbol, bars] : consumed_) {
            std::vector<double> raw;
            std::vector<std::string> ids;
            raw.reserve(bars.size());
            ids.reserve(bars.size());
            for (const auto& b : bars) {
                raw.push_back(static_cast<double>(b.close));
                ids.push_back(b.instrument_id);
            }
            const roll_series::Series s = roll_series::build_series(raw, ids);
            const auto seeded = history_bars_.find(symbol);
            const size_t first_window_bar = seeded == history_bars_.end() ? 0 : seeded->second;
            for (size_t t = first_window_bar; t < bars.size(); ++t) {
                ser << symbol << ","
                    << SessionClassifier::ymd(SessionClassifier::day_of(bars[t].timestamp)) << ","
                    << num(raw[t]) << "," << ids[t] << "," << int(s.flags.change[t]) << ","
                    << int(s.flags.confirm[t]) << "," << int(s.flags.flip[t]) << ","
                    << int(s.flags.pending[t]) << "," << s.flags.held_id[t] << ","
                    << num(s.adjusted[t]) << "," << num(t == 0 ? 0.0 : s.returns[t - 1]) << "\n";
            }
        }
        // The history before the window, as it was seeded: one row per consumed bar.
        std::ofstream hist(dir_ + "/history.csv");
        if (!hist) return false;
        hist << "symbol,date,close,instrument_id\n";
        for (const auto& [symbol, bars] : consumed_) {
            const auto seeded = history_bars_.find(symbol);
            const size_t count = seeded == history_bars_.end() ? 0 : seeded->second;
            for (size_t t = 0; t < count; ++t) {
                hist << symbol << ","
                     << SessionClassifier::ymd(SessionClassifier::day_of(bars[t].timestamp)) << ","
                     << num(static_cast<double>(bars[t].close)) << "," << bars[t].instrument_id << "\n";
            }
        }
        std::ofstream fm(dir_ + "/final_marks.csv");
        if (!fm) return false;
        fm << "symbol,date,close,instrument_id,withheld,change,held_id\n";
        for (const auto& m : final_marks_) {
            fm << m.symbol << "," << m.date << "," << num(m.close) << "," << m.instrument_id << ","
               << int(m.withheld) << "," << int(m.change) << "," << m.held_id << "\n";
        }
        for (std::ofstream* file : {&cal, &hold, &wh, &ser, &hist, &fm}) {
            file->close();
            if (file->fail()) return false;
        }
        return true;
    }

private:
    static std::string num(double v) {
        std::ostringstream o;
        o << std::setprecision(17) << v;
        return o.str();
    }

    std::string dir_;
    std::vector<std::string> calendar_;
    std::vector<std::pair<std::string, std::string>> hold_;
    std::vector<std::pair<std::string, std::string>> withheld_;
    std::set<std::string> seen_;  // every symbol a bar of which was consumed or withheld so far
    std::map<std::string, std::vector<Bar>> consumed_;
    std::map<std::string, size_t> history_bars_;  // per symbol: the consumed bars before the window
    std::vector<FinalMark> final_marks_;
};

}  // namespace backtest
}  // namespace trade_ngin
