// include/trade_ngin/live/live_roll_legs.hpp
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/roll_series.hpp"

namespace trade_ngin {

/**
 * @brief The live runners' ROLL legs, booked by STATE.
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
 *                       and legs it, with ids dated the confirming bar.
 *   a re-run            the first run re-wrote the T-1 row with the contract held after the T-1 bar,
 *                       so the caller passes the contract its stored legs rolled out of
 *                       (PostgresDatabase::get_stored_roll_contracts) as the recorded one, unless an
 *                       earlier date already booked that roll (contract_held_coming_into).
 *   a replay after a    the runner stores a run's legs BEFORE it re-writes the T-1 row, so a
 *   stopped run         stopped run leaves either the row as it was (the roll is still owed) or the
 *                       legs (the re-run case). A row re-written with no leg stored (a run of an older
 *                       binary): a roll the T-1 bar itself confirms is always owed to the run whose
 *                       T-1 it is, so it is legged whatever the row says.
 *   no recorded         a symbol with no stored row, or a row written before migration 016: the roll
 *   contract            its T-1 bar confirms.
 *
 * The closing leg's price is the close of the last consumed bar before the first change bar and the
 * opening leg's the change bar's close. With the per-contract source both are the
 * roll bar's own: the old contract's close on it and the new contract's; a roll known on its bar is
 * confirmed by that bar, a vendor switch by the next one (roll_series.hpp).
 */
struct ConfirmedRoll {
    std::string symbol;
    std::string confirm_date;  ///< YYYY-MM-DD: the confirming bar's date (the legs' id date)
    std::string outgoing_id;
    std::string incoming_id;
    double closing_price{0.0};
    double opening_price{0.0};
    int change_bars{0};
    /// A roll known on its bar (per_contract_rolls: a crossover, a
    /// deadline, a confirmed vendor switch dated earlier); false for a switch the next consumed bar
    /// confirmed (every roll of the vendor's series; a vendor switch bar with the source on).
    bool known_on_bar{false};
    /// Migration 040: the held contract's move booked on the roll bar, in
    /// price points (RollTracker::Status::roll_bar_points); nullopt on a roll of the vendor's series.
    std::optional<double> roll_bar_points;
    /// A micro's change of price source (RollTracker::Status::price_source_change):
    /// the runner books no legs and no cost for it, only a PRICE_SOURCE_CHANGE line with its gap.
    bool price_source_change{false};
};

/**
 * @brief The move a run books for a symbol whose roll it legs LATE (a confirming bar dated before
 *        T-1): the moves of the consumed bars after the stored state that no earlier run booked.
 *        The T-1 row books them all: `settle_to` - `settle_from` in price points per contract.
 *
 *        live_rolls_by_state gives the WHOLE sum: every consumed bar after the stored state, each
 *        against its previous consumed close, 0 on a change bar; `booked_after` is the
 *        date of the bar the stored book stands at. What earlier runs already booked of it is not in
 *        the bars: a bar with no instrument id is not judged, so a run can consume it,
 *        book its move and leave the roll pending, and the same bar back-filled later was consumed
 *        by no run. It is in the STORED rows: a run that consumed a bar booked its move on the
 *        positions row dated that bar, a run
 *        that saw a hole booked 0. The caller reads the rows dated after `booked_after` and before
 *        T-1 and adds late_booked_points to `settle_from`, so the T-1 row books the whole sum less
 *        what is already stored, to the cent, whatever each earlier run saw.
 */
struct LateRollSettlement {
    double settle_to{0.0};    ///< the symbol's last consumed close (dated T-1 or earlier)
    double settle_from{0.0};  ///< settle_to less the moves to book
    std::string booked_after; ///< YYYY-MM-DD: the bar the stored book stands at
};

/// What earlier runs booked of a late roll's sum, in price points per contract.
struct LateBooked {
    double points{0.0};
    std::vector<std::string> dates;  ///< the row dates that carry a booked move
    /// Non-empty: the stored rows cannot decide (the caller refuses the run with this text).
    std::string undecided;
};

/**
 * @brief The moves the stored rows of `symbol` dated strictly after `after_date` and strictly
 *        before `t1_date` already carry, per contract: for each such date, the row's
 *        daily realised P&L over (its quantity x the point value). A row that books 0 adds
 *        nothing: its run saw a hole, a change bar or a bar that did not move, and a consumed bar
 *        whose move was exactly 0 therefore reads as unconsumed and adds 0 to the late sum, which
 *        is the same number. A date with no row booked nothing.
 *
 *        The rows cannot decide, and `undecided` says why, when a row books a P&L on a zero
 *        quantity, when the point value is not positive, or when two sleeves' rows of one date
 *        book different moves per contract (one settlement is booked for every sleeve of the
 *        symbol, so the sleeves must have been booked the same bars).
 */
inline LateBooked late_booked_points(const std::string& symbol, const std::string& after_date,
                                     const std::string& t1_date,
                                     const std::vector<StoredRealisedRow>& rows, double point_value) {
    LateBooked out;
    std::map<std::string, std::vector<const StoredRealisedRow*>> by_date;
    for (const auto& row : rows) {
        if (row.symbol != symbol || !(row.date > after_date) || !(row.date < t1_date)) continue;
        by_date[row.date].push_back(&row);
    }
    for (const auto& [date, day_rows] : by_date) {
        bool any = false, booked = false, unbooked_holder = false;
        double points = 0.0;
        for (const StoredRealisedRow* row : day_rows) {
            if (row->realised == 0.0) {
                if (row->quantity != 0.0) unbooked_holder = true;
                continue;
            }
            if (row->quantity == 0.0 || !(point_value > 0.0)) {
                out.undecided = "the " + row->sleeve + " row dated " + date + " books " +
                                std::to_string(row->realised) + " on a quantity of " +
                                std::to_string(row->quantity) + " (point value " +
                                std::to_string(point_value) + ")";
                return out;
            }
            const double p = row->realised / (row->quantity * point_value);
            // The stored P&L carries six decimals: a tenth of a cent a contract tells two moves apart.
            if (any && std::abs(p - points) * point_value > 1e-3) {
                out.undecided = "the sleeves' rows dated " + date + " book different moves per contract";
                return out;
            }
            points = p;
            any = booked = true;
        }
        if (booked && unbooked_holder) {
            out.undecided = "of the sleeves holding it on " + date + " one booked a move and another booked 0";
            return out;
        }
        if (booked) {
            out.points += points;
            out.dates.push_back(date);
        }
    }
    return out;
}

/**
 * @brief The contract each held symbol's book held coming into the run date (issue #188): what
 *        live_rolls_by_state takes as recorded.
 *
 * `t1_row`: per held symbol, the contract on its stored T-1 positions row. `todays_legs`: per
 * symbol, the contract the ROLL legs already stored for the run date rolled out of (a re-run;
 * PostgresDatabase::get_stored_roll_contracts). `earlier_rolls`: per symbol, the rolls stored for
 * EARLIER dates of the run's bar window, oldest first, each the contract its closing leg rolled
 * out of and the one its opening leg rolled into (get_previous_rolls). Every id is read as the
 * bars are (through the declared relabels).
 *
 * A re-run of a roll day: the first run re-wrote the T-1 row with the contract held after the T-1
 * bar, so the contract held coming in is on the day's own legs, and it is taken from them.
 *
 * A re-run in a repair after late bars: the first pass booked the roll late, on this date, and the
 * re-runs of earlier dates have since booked it where it belongs (one roll, or several: A to B and
 * B to C re-booked where the first pass legged one A to C here). The day's own legs are then
 * stale: the earlier rolls rolled out of the same contract AFTER they last rolled into it, and a
 * held contract is rolled out of once each time it is entered. The T-1 row, written by the
 * earlier date's re-run, carries the state before this date, and it is kept (taking the stale
 * legs booked the rolls a second time). A contract that recurs (A to B, B to A, then A to C) is
 * told apart: the roll back into A comes after the roll out of it, so A is held again.
 *
 * `stale`: the symbols whose day's own legs were set aside, in name order.
 */
struct HeldComingIn {
    std::unordered_map<std::string, std::string> contract;
    std::vector<std::string> stale;
};

/// True when `rolls` (oldest first) roll out of `contract` after they last roll into it.
inline bool rolled_out_of_since_entered(const std::vector<StoredRoll>& rolls,
                                        const std::string& contract) {
    bool out_since = false;
    for (const auto& roll : rolls) {
        if (roll.incoming == contract) out_since = false;
        if (roll.outgoing == contract) out_since = true;
    }
    return out_since;
}

inline HeldComingIn contract_held_coming_into(
    const std::unordered_map<std::string, std::string>& t1_row,
    const std::unordered_map<std::string, std::string>& todays_legs,
    const std::unordered_map<std::string, std::vector<StoredRoll>>& earlier_rolls) {
    HeldComingIn out{t1_row, {}};
    for (const auto& [symbol, rolled_out_of] : todays_legs) {
        const auto held = out.contract.find(symbol);
        if (held == out.contract.end() || rolled_out_of.empty()) continue;
        const auto earlier = earlier_rolls.find(symbol);
        if (earlier != earlier_rolls.end() &&
            rolled_out_of_since_entered(earlier->second, rolled_out_of)) {
            out.stale.push_back(symbol);
            continue;
        }
        held->second = rolled_out_of;
    }
    std::sort(out.stale.begin(), out.stale.end());
    return out;
}

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
            roll_series::RollTracker::Status status;
        };
        std::vector<Step> steps;
        roll_series::RollTracker tracker;
        for (const auto& [ts, bar] : series) {
            const std::string d = core::format_utc_date(ts);
            if (d > t1_date) break;
            steps.push_back({d, static_cast<double>(bar->close),
                             tracker.add(bar->instrument_id, static_cast<double>(bar->close),
                                         bar->roll_from)});
        }
        const auto rec = recorded_contract.find(symbol);
        const bool recorded = rec != recorded_contract.end() && !rec->second.empty();
        // Where the stored book stands. The stored row is dated T-1 and was written by the run
        // before this one, which had not consumed the T-1 bar: its contract is the one held after a
        // bar dated BEFORE the row's date. The book stands at the last such bar held in the recorded
        // contract, so a contract that recurs (A, B, A, the second A confirmed by the T-1 bar) is
        // placed in the segment the row was written in and both rolls are owed (the last bar held
        // in the contract, the T-1 bar included, would read that as nothing owed
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
                                 st.last_close_before_change, st.change_bar_close, st.bars_pending,
                                 st.mark_close.has_value(), st.roll_bar_points, st.price_source_change});
        }
        if (late) {
            // The whole sum after the stored state; the caller takes off what the stored rows
            // dated after it already booked (see LateRollSettlement).
            double moves = 0.0;
            for (int i = stands + 1; i < static_cast<int>(steps.size()); ++i) {
                // 0 on a change bar; a per-contract roll bar moves to the old contract's close
                // (RollTracker::Status::move)
                moves += steps[i].status.move(steps[i].close, steps[i - 1].close);
            }
            out.late[symbol] = {steps.back().close, steps.back().close - moves, steps[stands].date};
        }
    }
    return out;
}

/**
 * @brief A bar's session-classifier verdict reads the classifier's trailing
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
