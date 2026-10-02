// include/trade_ngin/live/live_roll_legs.hpp
#pragma once

#include <algorithm>
#include <chrono>
#include <ctime>
#include <map>
#include <string>
#include <vector>

#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/roll_series.hpp"

namespace trade_ngin {

/**
 * @brief The live runners' ROLL legs (LOOP_SPEC v6.1 sections 2.1, 2.2, 6.5; T-ROLLX-FIX commit 3).
 *
 * A live run consumes every bar dated after the PREVIOUS run's T-1 up to its own T-1 (one bar a
 * symbol on a daily chain; several after missed runs, a catch-up). Every roll a consumed bar of
 * that span CONFIRMS books its two legs on this run (L-09), whether or not the confirming bar is the
 * run's last consumed bar; a confirming bar dated on or before the previous run's T-1 was booked by
 * that run, so each roll is booked exactly once. The sequence is walked bar by bar from each
 * symbol's first consumed bar of the window, so the closing leg's price is the close of the last
 * consumed bar before the first change bar and the opening leg's the change bar's close.
 */
struct ConfirmedRoll {
    std::string symbol;
    std::string confirm_date;  ///< YYYY-MM-DD: the confirming bar's date (the legs' id date)
    std::string outgoing_id;
    std::string incoming_id;
    double closing_price{0.0};
    double opening_price{0.0};
    int change_bars{0};
};

/// Every roll confirmed by a CONSUMED bar dated in (since_exclusive, t1_date], by symbol then date.
inline std::vector<ConfirmedRoll> rolls_confirmed_in(const std::vector<Bar>& consumed,
                                                     const std::string& since_exclusive,
                                                     const std::string& t1_date) {
    std::map<std::string, std::map<Timestamp, const Bar*>> by_symbol;
    for (const auto& bar : consumed) by_symbol[bar.symbol][bar.timestamp] = &bar;
    std::vector<ConfirmedRoll> out;
    for (const auto& [symbol, series] : by_symbol) {
        roll_series::RollTracker tracker;
        for (const auto& [ts, bar] : series) {
            const auto st = tracker.add(bar->instrument_id, static_cast<double>(bar->close));
            if (!st.confirm) continue;
            const std::string d = core::format_utc_date(ts);
            if (d <= since_exclusive || d > t1_date) continue;
            out.push_back({symbol, d, st.previous_held_id, st.held_id, st.last_close_before_change,
                           st.change_bar_close, st.bars_pending});
        }
    }
    return out;
}

/// "YYYY-MM-DD" one calendar day earlier (UTC).
inline std::string day_before(const std::string& ymd) {
    std::tm tm{};
    tm.tm_year = std::stoi(ymd.substr(0, 4)) - 1900;
    tm.tm_mon = std::stoi(ymd.substr(5, 2)) - 1;
    tm.tm_mday = std::stoi(ymd.substr(8, 2));
    const auto t = std::chrono::system_clock::from_time_t(timegm(&tm)) - std::chrono::hours(24);
    return core::format_utc_date(t);
}

/**
 * @brief The exclusive lower bound of this run's span: the bars a run dated after the previous run
 *        newly consumes are those dated after the previous run's date less one day (that run
 *        consumed every bar dated before its own date). With no previous run, the T-1 bar alone.
 *        The previous run is the latest date with the book's positions stored
 *        (PostgresDatabase::get_previous_book_date): a run whose live_results write failed stored its
 *        legs and its book, and is a run here.
 */
inline std::string live_legs_since(const std::string& previous_run_date, const std::string& t1_date) {
    return day_before(previous_run_date.empty() ? t1_date : previous_run_date);
}

/// "YYYYMMDD" from "YYYY-MM-DD" (the id date of a live leg).
inline std::string compact_date(const std::string& ymd) {
    std::string out;
    for (char c : ymd) {
        if (c != '-') out += c;
    }
    return out;
}

}  // namespace trade_ngin
