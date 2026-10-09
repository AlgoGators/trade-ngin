// include/trade_ngin/data/roll_series.hpp
#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/types.hpp"

namespace trade_ngin::roll_series {

/**
 * @brief The contract switches of one symbol's CONSUMED bar sequence, and the series built on it
 *        (LOOP_SPEC v6.1 sections 2.1 to 2.3; FUTURES_ROLL_PNL_DESIGN.md sections 16 to 19 are the
 *        rationale).
 *
 * The continuous `.v.0` series splices the vendor's volume-ranked contracts: on the bar where the
 * vendor switches contract (its instrument_id differs from the previous consumed bar's) the close
 * jumps by the price gap between two contracts, which is not a return. The functions here mark
 * those bars on the sequence a consumer has actually consumed (its own history, in date order) and
 * build the back-adjusted series every RETURN consumer reads:
 *
 *   change[t]   the bar's id differs from the last KNOWN id before it (a change bar). Its return
 *               is excluded from every return series (set to 0; the date stays in every window),
 *               its daily P&L is 0 and the symbol is held through it (D37). Bar 0 is never a
 *               change. A bar with no id, or with no known id before it, is not judged: it is
 *               neither a change nor a confirmation and leaves a pending sequence pending.
 *   confirm[t]  the bar confirms a pending change as a ROLL: its id equals the last pending change
 *               bar's. The two ROLL fills are booked on this bar (section 6.5): the closing leg at
 *               close[leg_from[t]] (the last consumed bar before the first pending change), the
 *               opening leg at close[leg_to[t]] (the change bar into the kept id).
 *   flip[t]     the bar belongs to a pending sequence that reverted to the held contract's id, the
 *               reverting bar included: a FLIP, no fills, every bar of it excluded and held.
 *   rolled[t]   a change bar a later confirmation resolved as a roll.
 *   pending[t]  the bar is a change bar of a sequence still unresolved at this bar (the symbol's
 *               last consumed bar being pending is the hold of section 2.1; a third id while a
 *               change is pending extends the sequence, and a later return to an older id after a
 *               confirmation is a new roll: the series did switch twice).
 *
 * The adjusted level is the raw close PLUS the sum of the LATER steps (new minus old on every change
 * bar), re-anchored on the latest bar (Panama, additive per segment). It can be at or below zero and
 * is used only for differences (the EMAs). The adjusted return is the adjusted change over the RAW
 * previous close, r_t = (A_t - A_t-1) / P_t-1, exactly 0 on a change bar; on every other bar it equals
 * the raw simple return, so a series with no change bar is untouched.
 */
struct ChangeFlags {
    std::vector<bool> change;
    std::vector<bool> confirm;
    std::vector<bool> flip;
    std::vector<bool> rolled;
    std::vector<bool> pending;
    std::vector<int> leg_from;  ///< confirm bars only; -1 elsewhere
    std::vector<int> leg_to;    ///< confirm bars only; -1 elsewhere
    /// The id the position is held in at each bar (the confirmed id; the first known id before
    /// any change). Empty while no id is known.
    std::vector<std::string> held_id;

    size_t size() const { return change.size(); }
    /// True when the last bar leaves a change pending (the symbol is held on the next cycle).
    bool pending_at_end() const { return !pending.empty() && pending.back(); }
};

/// The flags of one symbol's consumed bars, oldest first. An empty id is "unknown".
ChangeFlags classify_instrument_changes(const std::vector<std::string>& instrument_ids);

/// The adjusted levels: raw[t] + the sum over change bars j > t of (raw[j] - raw[j - 1]).
std::vector<double> adjusted_levels(const std::vector<double>& raw_closes,
                                    const std::vector<bool>& change);

/// The adjusted returns, one per bar from the second: r[t - 1] = (A_t - A_t-1) / raw[t - 1], 0 on a
/// change bar (and 0 when the raw previous close is not positive, as the raw return would be).
std::vector<double> adjusted_returns(const std::vector<double>& raw_closes,
                                     const std::vector<bool>& change);

/// The series a signal consumer reads: raw levels, adjusted levels, adjusted returns, the flags.
struct Series {
    std::vector<double> raw;
    std::vector<double> adjusted;
    std::vector<double> returns;  ///< raw.size() - 1 entries (empty for fewer than two bars)
    ChangeFlags flags;
};

/// Builds the series of one symbol's consumed bars (closes and ids aligned, oldest first).
Series build_series(const std::vector<double>& raw_closes,
                    const std::vector<std::string>& instrument_ids);

/**
 * @brief The same rule, one bar at a time (the engines mark and hold as the bars arrive): the
 *        status of the bar just added, with the closes the legs read on a confirming bar.
 *
 * Equivalent to classify_instrument_changes on the whole sequence (a test pins the equivalence).
 * `add` takes the bar's id and close; `last_close_before_change` is the close of the consumed
 * bar before the first pending change bar (the closing leg's price, section 6.5) and
 * `change_bar_close` the close of the change bar into the kept id (the opening leg's price).
 */
class RollTracker {
public:
    struct Status {
        bool change{false};
        bool confirm{false};
        bool flip{false};
        bool pending{false};
        std::string held_id;             ///< the contract the position is held in after this bar
        std::string previous_held_id;    ///< on a confirm bar: the contract rolled out of
        double last_close_before_change{0.0};  ///< confirm bars: the closing leg's price
        double change_bar_close{0.0};          ///< confirm bars: the opening leg's price
        int bars_pending{0};             ///< confirm or flip bars: the change bars resolved
        /// LOOP_SPEC v6.1 sections 2.1, 2.2 (D37): the symbol is HELD while its last consumed bar is
        /// a change bar (a pending roll, a third id, either bar of a flip) or an id-less bar inside a
        /// pending roll (code review D2: `change` alone missed that bar).
        bool holds() const { return change || pending; }
    };
    Status add(const std::string& instrument_id, double close);
    const std::string& held_id() const { return held_; }
    bool pending() const { return !pending_.empty(); }
    size_t bars() const { return n_; }

private:
    std::string held_;
    std::string last_known_;
    double last_close_{0.0};
    size_t n_{0};
    struct Pending {
        std::string id;
        double close;
    };
    std::vector<Pending> pending_;
    double close_before_pending_{0.0};
};

/**
 * @brief The two ROLL fills of one sleeve's held position on the CONFIRMING bar (section 6.5):
 *        the closing leg at the last pre-change consumed close (the outgoing contract), the opening
 *        leg at the change bar's close (the incoming contract), both |q_held| contracts, the
 *        closing leg on the side opposite to the position, realised 0 on both, each costed by
 *        `cost_of(symbol, signed quantity, price)` (an upper bound: rolls trade as spreads),
 *        execution_type ROLL, netting_adjustment 0, in the stored order (closing, opening).
 *        `exec_id_close` / `exec_id_open` and the order ids are the caller's (live:
 *        EXEC_<symbol>_<YYYYMMDD>_RC / _RO; backtest: RL-<sid>-<n>). An empty vector when
 *        q_held is 0. A non-positive leg price is refused by the caller (STRICT: a leg without
 *        a usable close is a STOP, not a default), so this function requires both positive.
 */
struct RollLegCost {
    double commissions_fees{0.0};
    double implicit_price_impact{0.0};
    double slippage_market_impact{0.0};
    double total_transaction_costs{0.0};
};
std::vector<ExecutionReport> make_roll_legs(
    const std::string& symbol, double q_held, double closing_price, double opening_price,
    const std::string& outgoing_id, const std::string& incoming_id, const Timestamp& fill_time,
    const std::string& exec_id_close, const std::string& order_id_close,
    const std::string& exec_id_open, const std::string& order_id_open,
    const std::function<RollLegCost(const std::string&, double, double)>& cost_of);

/// The status of each symbol's LAST bar in `bars`, every bar of the symbol taken in time order
/// through a RollTracker (a catch-up is evaluated bar by bar), keyed by symbol. `bars` must be the
/// CONSUMED bars (LOOP_SPEC v6.1 section 2.1, K-01): the live runners pass their window without the
/// withheld bars (session_classifier.hpp, k01_consumed_bars), never the loaded window.
std::unordered_map<std::string, RollTracker::Status> roll_status_of(const std::vector<Bar>& bars);

}  // namespace trade_ngin::roll_series
