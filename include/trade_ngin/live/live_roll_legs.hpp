// include/trade_ngin/live/live_roll_legs.hpp
#pragma once

#include <algorithm>
#include <chrono>
#include <ctime>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/roll_series.hpp"

namespace trade_ngin {

/**
 * @brief The live runners' ROLL legs, booked by STATE (LOOP_SPEC v6.2 sections 2.1, 2.2, 6.5;
 *        T-ROLLX-FIX commit 4, B8).
 *
 * A run legs every roll confirmed since the contract recorded on the symbol's stored T-1 positions
 * row, never the rolls of a calendar span. The symbol's CONSUMED sequence is walked bar by bar from
 * its first bar of the window; the last bar at which the held contract is the recorded one is where
 * the stored book stands, and every roll a later bar confirms (dated up to the run's T-1) is owed:
 *
 *   a normal day        the stored row carries the contract held after the bars before T-1; the T-1
 *                       bar confirms; the roll is legged on this run.
 *   a late confirming   a feed hole on the confirming bar's own T-1 run, the bar back-filled before
 *   bar                 the next run: that run finds the roll confirmed after the recorded contract
 *                       and legs it, with ids dated the confirming bar. A calendar span never did.
 *   a re-run            the first run re-wrote the T-1 row with the contract held after the T-1 bar,
 *                       so the caller passes the contract its stored legs rolled out of
 *                       (PostgresDatabase::get_stored_roll_contracts) as the recorded one.
 *   a replay after a    the runner stores a run's legs BEFORE it re-writes the T-1 row (commit 5), so a
 *   stopped run         stopped run leaves either the row as it was (the roll is still owed) or the
 *                       legs (the re-run case). A row re-written with no leg stored (a run of an older
 *                       binary): a roll the T-1 bar itself confirms is always owed to the run whose
 *                       T-1 it is, so it is legged whatever the row says.
 *   no recorded         a symbol with no stored row, or a row written before migration 016: the roll
 *   contract            its T-1 bar confirms, as before.
 *
 * The closing leg's price is the close of the last consumed bar before the first change bar and the
 * opening leg's the change bar's close.
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

/**
 * @brief The move a run books for a symbol whose roll it legs LATE (a confirming bar dated before
 *        T-1): the consumed bars after the stored state that no earlier run consumed. The T-1 row
 *        books them all: `settle_to` - `settle_from` in price points per contract, the sum of each
 *        such bar's move against its previous consumed close, 0 on a change bar (section 6.6).
 *
 *        Which bars an earlier run consumed: an id-carrying bar after the pending change would have
 *        confirmed, reverted or extended it on its own run, so none of those was consumed. A bar
 *        with NO id is not judged (section 2.2): an earlier run can consume it, book its move and
 *        leave the roll pending. Every id-less bar after the stored state and dated before T-1 is
 *        therefore taken as consumed on its own run, its move booked against that run's previous
 *        consumed close, and is left out of the late sum (commit 5, finding 4: with the confirming
 *        bar back-filled BEHIND such a bar the sum booked its move twice).
 */
struct LateRollSettlement {
    double settle_to{0.0};    ///< the symbol's last consumed close (dated T-1 or earlier)
    double settle_from{0.0};  ///< settle_to less the unbooked moves
};

struct LiveRollState {
    std::vector<ConfirmedRoll> rolls;  ///< every roll this run legs, by symbol then date
    std::unordered_map<std::string, LateRollSettlement> late;  ///< symbols with a late roll
    /// Held symbols whose recorded contract is at no bar of the consumed sequence: the stored state
    /// cannot be placed, so the rolls owed cannot be told (the caller refuses the run).
    std::vector<std::string> unplaced;
};

/// `recorded_contract`: per held symbol, the contract on its stored T-1 row ("" when none is
/// recorded). `consumed`: the window without the withheld bars. `t1_date`: the run's T-1.
inline LiveRollState live_rolls_by_state(
    const std::vector<Bar>& consumed,
    const std::unordered_map<std::string, std::string>& recorded_contract,
    const std::string& t1_date) {
    std::map<std::string, std::map<Timestamp, const Bar*>> by_symbol;
    for (const auto& bar : consumed) by_symbol[bar.symbol][bar.timestamp] = &bar;
    LiveRollState out;
    for (const auto& [symbol, series] : by_symbol) {
        struct Step {
            std::string date;
            double close;
            bool idless;
            roll_series::RollTracker::Status status;
        };
        std::vector<Step> steps;
        roll_series::RollTracker tracker;
        for (const auto& [ts, bar] : series) {
            const std::string d = core::format_utc_date(ts);
            if (d > t1_date) break;
            steps.push_back({d, static_cast<double>(bar->close), bar->instrument_id.empty(),
                             tracker.add(bar->instrument_id, static_cast<double>(bar->close))});
        }
        const auto rec = recorded_contract.find(symbol);
        const bool recorded = rec != recorded_contract.end() && !rec->second.empty();
        // Where the stored book stands. The stored row is dated T-1 and was written by the run
        // before this one, which had not consumed the T-1 bar: its contract is the one held after a
        // bar dated BEFORE the row's date. The book stands at the last such bar held in the recorded
        // contract, so a contract that recurs (A, B, A, the second A confirmed by the T-1 bar) is
        // placed in the segment the row was written in and both rolls are owed (commit 5, finding
        // 5; the last bar held in the contract, the T-1 bar included, read that as nothing owed
        // before T-1). Only when no earlier bar is held in it does the T-1 bar place it: a row
        // already re-written after the T-1 bar.
        int stands = -1;
        if (recorded) {
            int at_t1 = -1;
            for (int i = static_cast<int>(steps.size()) - 1; i >= 0; --i) {
                if (steps[i].status.held_id != rec->second) continue;
                if (steps[i].date < t1_date) {
                    stands = i;
                    break;
                }
                at_t1 = i;
            }
            if (stands < 0) stands = at_t1;
            if (stands < 0) {
                out.unplaced.push_back(symbol);
                continue;
            }
        }
        bool late = false;
        for (int i = 0; i < static_cast<int>(steps.size()); ++i) {
            const auto& st = steps[i].status;
            if (!st.confirm) continue;
            const bool since_state = recorded && i > stands;
            if (!since_state && steps[i].date != t1_date) continue;
            if (since_state && steps[i].date != t1_date) late = true;
            out.rolls.push_back({symbol, steps[i].date, st.previous_held_id, st.held_id,
                                 st.last_close_before_change, st.change_bar_close, st.bars_pending});
        }
        if (late) {
            double unbooked = 0.0;
            // The last bar after the stored state an earlier run consumed (see LateRollSettlement).
            int booked_to = stands;
            for (int i = stands + 1; i < static_cast<int>(steps.size()); ++i) {
                if (!steps[i].status.change) unbooked += steps[i].close - steps[i - 1].close;
                if (steps[i].idless && steps[i].date < t1_date) booked_to = i;
            }
            unbooked -= steps[booked_to].close - steps[stands].close;
            out.late[symbol] = {steps.back().close, steps.back().close - unbooked};
        }
    }
    return out;
}

/**
 * @brief LOOP_SPEC v6.2 section 2.1 (N-2): a bar's K-01 verdict reads the classifier's trailing
 *        history, the same in every engine. The live runner loads this many calendar days of bars
 *        before its window into the SessionClassifier ONLY (the strategies' window is unchanged, and
 *        the consumed set is still taken over the window), so a print near the window's start is
 *        judged as the backtest judges it and a bar withheld on its own run is never fed later.
 *
 *        The classifier's norm is the median of the 20 trailing weekday SESSION bars
 *        (SessionClassifierConfig::norm_window_bars), about 28 calendar days. Each of those bars'
 *        own SESSION flag was judged on the 20 before it, so one norm window is not enough: 120
 *        calendar days is about 85 weekday bars, four norm windows, enough for a contract that
 *        prints on fewer days and for the flags behind the window's first norm to have been judged
 *        on full norms themselves. Measured on the 24-run CONSERVATIVE and BASE replays: with 120
 *        days every live verdict equals the 16-year backtest record's.
 */
inline constexpr int kK01ClassifierHistoryDays = 120;

/// The first instant of the classifier's history prefix for a window starting at `window_start`.
inline Timestamp k01_classifier_history_start(const Timestamp& window_start) {
    return window_start - std::chrono::hours(24 * kK01ClassifierHistoryDays);
}

/// The bars of `loaded` that belong to the prefix: dated from the history start and strictly
/// before the window (a bar at or after `window_start` is the window's own).
inline std::vector<Bar> k01_classifier_history(const std::vector<Bar>& loaded,
                                               const Timestamp& window_start) {
    const Timestamp from = k01_classifier_history_start(window_start);
    std::vector<Bar> out;
    for (const auto& bar : loaded) {
        if (bar.timestamp >= from && bar.timestamp < window_start) out.push_back(bar);
    }
    return out;
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
