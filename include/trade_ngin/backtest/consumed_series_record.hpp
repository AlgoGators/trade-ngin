// include/trade_ngin/backtest/consumed_series_record.hpp
#pragma once

#include <cstdio>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>

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
 *   - every non-SESSION verdict of the group to the hold set (hold.csv);
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
 */
class ConsumedSeriesRecord {
public:
    void enable(std::string dir) { dir_ = std::move(dir); }
    bool enabled() const { return !dir_.empty(); }

    void add_cycle(const std::string& signal_date, const std::vector<SymbolDayVerdict>& verdicts,
                   const std::vector<Bar>& withheld, const std::vector<Bar>& fed) {
        if (!enabled()) return;
        calendar_.push_back(signal_date);
        for (const auto& v : verdicts) {
            if (!v.is_session()) hold_.emplace_back(v.date, v.symbol);
        }
        for (const auto& b : withheld) {
            withheld_.emplace_back(SessionClassifier::ymd(SessionClassifier::day_of(b.timestamp)),
                                   b.symbol);
        }
        for (const auto& b : fed) consumed_[b.symbol].push_back(b);
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

    /// Writes the files into the directory; returns false when one cannot be opened.
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
            for (size_t t = 0; t < bars.size(); ++t) {
                ser << symbol << ","
                    << SessionClassifier::ymd(SessionClassifier::day_of(bars[t].timestamp)) << ","
                    << num(raw[t]) << "," << ids[t] << "," << int(s.flags.change[t]) << ","
                    << int(s.flags.confirm[t]) << "," << int(s.flags.flip[t]) << ","
                    << int(s.flags.pending[t]) << "," << s.flags.held_id[t] << ","
                    << num(s.adjusted[t]) << "," << num(t == 0 ? 0.0 : s.returns[t - 1]) << "\n";
            }
        }
        std::ofstream fm(dir_ + "/final_marks.csv");
        if (!fm) return false;
        fm << "symbol,date,close,instrument_id,withheld,change,held_id\n";
        for (const auto& m : final_marks_) {
            fm << m.symbol << "," << m.date << "," << num(m.close) << "," << m.instrument_id << ","
               << int(m.withheld) << "," << int(m.change) << "," << m.held_id << "\n";
        }
        return static_cast<bool>(ser) && static_cast<bool>(fm);
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
    std::map<std::string, std::vector<Bar>> consumed_;
    std::vector<FinalMark> final_marks_;
};

}  // namespace backtest
}  // namespace trade_ngin
