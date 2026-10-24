// include/trade_ngin/data/roll_series.hpp
#pragma once

#include <functional>
#include <optional>
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
 * The adjusted level is the raw close PLUS the sum of the LATER steps (new minus old on every
 * change bar), re-anchored on the latest bar (Panama, additive per segment). It can be at or below
 * zero and is used only for differences (the EMAs). The adjusted return is the adjusted change over
 * the RAW previous close, r_t = (A_t - A_t-1) / P_t-1, exactly 0 on a change bar; on every other
 * bar it equals the raw simple return, so a series with no change bar is untouched.
 *
 * Rolls step 3 (portfolio.json per_contract_rolls on; per_contract_source.hpp): the bars are the
 * held contract's own and a roll bar carries the contract left (Bar::roll_from, passed alongside
 * the ids). On such a bar the step is the new close minus the OLD contract's close on the same bar,
 * and the return is the old contract's own over the bar, (old close - previous close) / previous
 * close; the date is a change bar but its return is not 0. A roll known on its bar (KNOWN) is
 * confirmed on that bar: no hold, the legs booked on it, priced at its two closes. On a vendor
 * switch only the next bar confirms (AWAITS_NEXT) the bar is the HELD contract's own and the field
 * names the target: the bar is a pending (held) bar, not a change bar, and every level reads the
 * held contract's close. The next consumed bar decides: held going into it in the target (its id,
 * or a known roll's old id) confirms the roll there, both legs at the switch bar's two closes, and
 * that bar's move is measured from the target's close on the switch bar (`base_close`); otherwise
 * nothing rolled and nothing is resolved. An AFTER_ROLL field acts only when the last consumed
 * contract is the one it names: the consumer withheld the roll bar (and the bars since), so this
 * bar is the roll, KNOWN, at this bar's two closes. With no field passed (the switch
 * off, the micros) every rule above is today's, bit for bit.
 *
 * Rolls step 4 (R3 audit A, the 6L 2013-09-11 case): an AFTER_ROLL field confirms an awaited vendor
 * switch only when it follows that switch, its date being the switch bar's. A field dated later
 * names a later roll into the same target that the consumer withheld: the awaited switch was not
 * confirmed, and the later roll is booked on this bar as a withheld roll is.
 *
 * Rolls step 4, ruling 19 (per_contract_switch_bar "vendor"): an AWAITS_NEXT_VENDOR switch bar is
 * the TARGET's own bar and the field names the held contract. The bar is pending (held); its move
 * ends at the held contract's close (old_close, mark_close). Confirmed by the next consumed bar,
 * the legs are the switch bar's two closes (closing the field's, opening its own) and the next move
 * starts from its own close; not confirmed, the next move starts from the held contract's close
 * (base_close, previous_close). The booked moves and the legs are AWAITS_NEXT's; the raw levels on
 * the switch bar are the target's.
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
    /// Rolls step 3: on a roll known on its bar, the old contract's close (the bar's move ends at it,
    /// its step is the close minus it); nullopt elsewhere and on every bar when no field is passed.
    /// The legs of a known roll: closing old_close[leg_to], opening the close of leg_to.
    std::vector<std::optional<double>> old_close;
    /// Rolls step 3: on a vendor switch bar the next consumed bar confirms, the target's close: the
    /// close the next bar's move starts from. Its legs: closing the close of leg_from (the switch
    /// bar's, the old contract's own), opening leg_opening_price.
    std::vector<std::optional<double>> base_close;
    std::vector<std::optional<double>> leg_opening_price;  ///< confirm bars of a vendor switch

    size_t size() const { return change.size(); }
    /// True when the last bar leaves a change pending (the symbol is held on the next cycle).
    bool pending_at_end() const { return !pending.empty() && pending.back(); }
};

/// Each consumed bar's roll field (Bar::roll_from), aligned with the ids; empty = none passed.
using RollFields = std::vector<std::optional<RollFrom>>;

/// The flags of one symbol's consumed bars, oldest first. An empty id is "unknown". `roll_from`,
/// when not empty, is aligned with the ids (rolls step 3).
ChangeFlags classify_instrument_changes(const std::vector<std::string>& instrument_ids,
                                        const RollFields& roll_from = {});

/// The adjusted levels: raw[t] + the sum over change bars j > t of (raw[j] - raw[j - 1]).
std::vector<double> adjusted_levels(const std::vector<double>& raw_closes,
                                    const std::vector<bool>& change);

/// The adjusted returns, one per bar from the second: r[t - 1] = (A_t - A_t-1) / raw[t - 1], 0 on a
/// change bar (and 0 when the raw previous close is not positive, as the raw return would be).
std::vector<double> adjusted_returns(const std::vector<double>& raw_closes,
                                     const std::vector<bool>& change);

/// The closes the next bar's return is measured from: raw, with a confirmed vendor switch bar read
/// as the target's close (ChangeFlags::base_close). `raw` itself when no field was passed. For a
/// divisor only: the switch bar's own level is the held contract's close.
std::vector<double> return_bases(const std::vector<double>& raw_closes, const ChangeFlags& flags);

/// The same two on the flags, rolls step 3: a bar's move runs from base (or the previous close) to
/// old_close (or its close); the step is the rest of the close difference. With no field passed
/// they equal the two above exactly.
std::vector<double> adjusted_levels(const std::vector<double>& raw_closes, const ChangeFlags& flags);
std::vector<double> adjusted_returns(const std::vector<double>& raw_closes,
                                     const ChangeFlags& flags);

/// The series a signal consumer reads: raw levels, adjusted levels, adjusted returns, the flags.
struct Series {
    std::vector<double> raw;  ///< the closes as consumed (the held contract's on every bar)
    std::vector<double> adjusted;
    std::vector<double> returns;  ///< raw.size() - 1 entries (empty for fewer than two bars)
    ChangeFlags flags;
};

/// Builds the series of one symbol's consumed bars (closes, ids and roll fields aligned, oldest
/// first).
Series build_series(const std::vector<double>& raw_closes,
                    const std::vector<std::string>& instrument_ids,
                    const RollFields& roll_from = {});

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
        /// Rolls step 3: on a roll bar, the old contract's close: the bar's move (P&L, return) is
        /// to it, not to `close`.
        std::optional<double> mark_close;
        /// Rolls step 3: the bar that confirms a vendor switch: its move is from this close (the
        /// target's on the switch bar), not from the previous bar's close.
        std::optional<double> previous_close;
        /// LOOP_SPEC v6.1 sections 2.1, 2.2 (D37): the symbol is HELD while its last consumed bar is
        /// a change bar (a pending roll, a third id, either bar of a flip) or an id-less bar inside a
        /// pending roll (code review D2: `change` alone missed that bar).
        bool holds() const { return (change && !confirm) || pending; }
        /// The bar's move in price points: (mark_close or close) - (previous_close or `prev`), 0
        /// on a change bar with neither (today's rule). `prev` is the previous consumed close.
        double move(double close, double prev) const {
            if (change && !mark_close && !previous_close) return 0.0;
            return (mark_close ? *mark_close : close) - (previous_close ? *previous_close : prev);
        }
    };
    /// `roll_from` is the bar's roll field (rolls step 3). Two rolls resolved on one bar (a switch
    /// confirmed by a bar that is itself a roll) throw std::runtime_error.
    Status add(const std::string& instrument_id, double close,
               const std::optional<RollFrom>& roll_from = std::nullopt);
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
    // Rolls step 3: a vendor switch (AWAITS_NEXT) the next bar decides.
    bool awaiting_{false};
    std::string awaiting_id_;
    double awaiting_close_{0.0};
    double awaiting_old_close_{0.0};
    std::string awaiting_old_id_;
    std::string awaiting_date_;  ///< rolls step 4: the switch bar's date (RollFrom::date)
    bool awaiting_on_target_{false};  ///< rolls step 4, ruling 19: the switch bar was the target's
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
