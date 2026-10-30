// src/data/roll_series.cpp
#include "trade_ngin/data/roll_series.hpp"

#include <cmath>
#include <map>
#include <stdexcept>

namespace trade_ngin::roll_series {

ChangeFlags classify_instrument_changes(const std::vector<std::string>& ids,
                                        const RollFields& roll_from) {
    const size_t n = ids.size();
    if (!roll_from.empty() && roll_from.size() != n) {
        throw std::invalid_argument(
            "roll_series::classify_instrument_changes: ids and roll fields differ in length");
    }
    ChangeFlags f;
    f.change.assign(n, false);
    f.confirm.assign(n, false);
    f.flip.assign(n, false);
    f.rolled.assign(n, false);
    f.pending.assign(n, false);
    f.leg_from.assign(n, -1);
    f.leg_to.assign(n, -1);
    f.held_id.assign(n, std::string());
    f.old_close.assign(n, std::nullopt);
    f.base_close.assign(n, std::nullopt);
    f.leg_opening_price.assign(n, std::nullopt);
    if (n == 0) return f;
    auto field = [&](size_t t) -> const std::optional<RollFrom>& {
        static const std::optional<RollFrom> none;
        return roll_from.empty() ? none : roll_from[t];
    };

    std::string held;            // the confirmed id the position is held in
    std::string last_known;      // the last known id seen (the previous consumed bar's, skipping unknowns)
    std::vector<size_t> pending;  // the change bars of the unresolved sequence, oldest first
    // Rolls step 3: a vendor switch (AWAITS_NEXT) the next bar decides: its bar, the target's id.
    int awaiting = -1;
    std::string awaiting_target;
    std::string awaiting_date;  // rolls step 4: the switch bar's date (RollFrom::date)
    bool awaiting_on_target = false;  // rolls step 4, ruling 19: the switch bar is the target's
    for (size_t t = 0; t < n; ++t) {
        const std::string& id = ids[t];
        if (id.empty()) {
            // Unknown: not judged. A pending sequence stays pending (the bar is held with it).
            f.held_id[t] = held;
            if (!pending.empty()) f.pending[t] = true;
            continue;
        }
        if (last_known.empty()) {
            held = id;
            last_known = id;
            f.held_id[t] = held;
            continue;
        }
        const std::optional<RollFrom>& rf = field(t);
        if (awaiting >= 0) {
            // The contract held going into this bar decides the switch: the bar's own id, or the
            // contract a roll known on this bar leaves.
            const std::string& before = rf && rf->kind == RollFromKind::KNOWN ? rf->instrument_id : id;
            const size_t a = static_cast<size_t>(awaiting);
            pending.clear();
            awaiting = -1;
            const bool on_target = awaiting_on_target;
            awaiting_on_target = false;
            // Rolls step 4: a field following a later roll into the target does not confirm it.
            const bool later_roll = rf && rf->kind == RollFromKind::AFTER_ROLL &&
                                    !rf->date.empty() && !awaiting_date.empty() &&
                                    rf->date != awaiting_date;
            if (before == awaiting_target && !later_roll) {
                // The roll happened on the switch bar: both legs at its two closes (the old
                // contract's own close, the target's in the field), this bar's move from the target.
                f.confirm[t] = true;
                f.leg_to[t] = static_cast<int>(a);
                if (!on_target) {
                    f.leg_from[t] = static_cast<int>(a);
                    f.leg_opening_price[t] = roll_from[a]->close;
                    f.base_close[a] = roll_from[a]->close;
                }
                // (ruling 19 "vendor": the switch bar is the target's, so the closing leg is its
                // old_close, the opening leg and the start of the next move its own close)
                f.rolled[a] = true;
                // ("vendor": the switch bar already carries the target's id, so this is no change)
                f.change[t] = on_target ? id != awaiting_target : id != last_known;
                held = awaiting_target;
                last_known = id;
                if (!rf || rf->kind == RollFromKind::AFTER_ROLL) {
                    f.held_id[t] = held;
                    continue;
                }
            } else if (on_target) {
                // Ruling 19 "vendor": nothing rolled; this bar moves from the held contract's close
                // on the switch bar, not from the target's the bar carried.
                f.base_close[a] = roll_from[a]->close;
            }
        }
        const bool after_roll_acts = rf && rf->kind == RollFromKind::AFTER_ROLL &&
                                     rf->instrument_id == last_known && id != last_known;
        if (rf && rf->kind == RollFromKind::AWAITS_NEXT) {
            // The bar is the held contract's own; the field names the target.
            f.pending[t] = true;
            pending.assign(1, t);
            awaiting = static_cast<int>(t);
            awaiting_target = rf->instrument_id;
            awaiting_date = rf->date;
            last_known = id;
            f.held_id[t] = held;
            continue;
        }
        if (rf && rf->kind == RollFromKind::AWAITS_NEXT_VENDOR) {
            // Ruling 19 "vendor": the bar is the target's own; the field names the held contract,
            // whose close the bar's move ends at.
            f.pending[t] = true;
            f.old_close[t] = rf->close;
            pending.assign(1, t);
            awaiting = static_cast<int>(t);
            awaiting_target = id;
            awaiting_date = rf->date;
            awaiting_on_target = true;
            last_known = rf->instrument_id;
            f.held_id[t] = held;
            continue;
        }
        if (rf && (rf->kind == RollFromKind::KNOWN || after_roll_acts)) {
            // Known on its bar: confirmed here, both legs at this bar's two closes.
            f.change[t] = true;
            f.old_close[t] = rf->close;
            f.confirm[t] = true;
            f.leg_to[t] = static_cast<int>(t);
            f.rolled[t] = true;
            held = id;
            last_known = id;
            pending.clear();
            f.held_id[t] = held;
            continue;
        }
        const bool change = id != last_known;
        f.change[t] = change;
        last_known = id;
        if (pending.empty()) {
            if (change) {
                pending.push_back(t);
                f.pending[t] = true;
            }
            f.held_id[t] = held;
            continue;
        }
        const size_t last = pending.back();
        if (id == ids[last]) {
            // The new id is kept: the change was a ROLL; the legs are booked on this bar.
            f.confirm[t] = true;
            f.leg_from[t] = static_cast<int>(pending.front()) - 1;
            f.leg_to[t] = static_cast<int>(last);
            f.rolled[last] = true;
            held = ids[last];
            pending.clear();
        } else if (id == held) {
            // Reverted to the held contract: a FLIP, no legs; the reverting bar is part of it.
            for (size_t p : pending) f.flip[p] = true;
            f.flip[t] = true;
            pending.clear();
        } else {
            // A third id while a change is pending: another change bar of the same sequence.
            pending.push_back(t);
            f.pending[t] = true;
        }
        f.held_id[t] = held;
    }
    return f;
}

std::vector<double> adjusted_levels(const std::vector<double>& raw, const std::vector<bool>& change) {
    const size_t n = raw.size();
    if (change.size() != n) {
        throw std::invalid_argument("roll_series::adjusted_levels: closes and flags differ in length");
    }
    std::vector<double> adjusted(n, 0.0);
    double later_steps = 0.0;  // the sum of the steps on change bars after t
    for (size_t k = n; k-- > 0;) {
        adjusted[k] = raw[k] + later_steps;
        if (k > 0 && change[k]) later_steps += raw[k] - raw[k - 1];
    }
    return adjusted;
}

std::vector<double> adjusted_returns(const std::vector<double>& raw, const std::vector<bool>& change) {
    const size_t n = raw.size();
    if (change.size() != n) {
        throw std::invalid_argument("roll_series::adjusted_returns: closes and flags differ in length");
    }
    std::vector<double> r;
    if (n < 2) return r;
    r.assign(n - 1, 0.0);
    for (size_t t = 1; t < n; ++t) {
        if (change[t] || !(raw[t - 1] > 0.0)) {
            r[t - 1] = 0.0;
        } else {
            // On a non-change bar A_t - A_t-1 == raw[t] - raw[t-1] exactly (the later-step sum is
            // the same on both), so the adjusted return is the raw simple return.
            r[t - 1] = (raw[t] - raw[t - 1]) / raw[t - 1];
        }
    }
    return r;
}

std::vector<double> return_bases(const std::vector<double>& raw, const ChangeFlags& flags) {
    if (flags.base_close.size() != raw.size()) {
        throw std::invalid_argument("roll_series::return_bases: closes and flags differ in length");
    }
    std::vector<double> out = raw;
    for (size_t t = 0; t < out.size(); ++t) {
        if (flags.base_close[t]) out[t] = *flags.base_close[t];
    }
    return out;
}

std::vector<double> adjusted_levels(const std::vector<double>& raw, const ChangeFlags& flags) {
    const size_t n = raw.size();
    if (flags.change.size() != n || flags.old_close.size() != n || flags.base_close.size() != n) {
        throw std::invalid_argument("roll_series::adjusted_levels: closes and flags differ in length");
    }
    std::vector<double> adjusted(n, 0.0);
    double later_steps = 0.0;  // the sum of the steps on change bars after t
    for (size_t k = n; k-- > 0;) {
        adjusted[k] = raw[k] + later_steps;
        if (k == 0) continue;
        if (flags.old_close[k] || flags.base_close[k - 1]) {
            // a per-contract roll: the bar's close less the close its move ends at (the old
            // contract's on a roll bar), plus the close its move starts from (the target's after a
            // confirmed vendor switch) less the previous close
            const double to = flags.old_close[k] ? *flags.old_close[k] : raw[k];
            const double from = flags.base_close[k - 1] ? *flags.base_close[k - 1] : raw[k - 1];
            later_steps += (raw[k] - to) + (from - raw[k - 1]);
        } else if (flags.change[k]) {
            later_steps += raw[k] - raw[k - 1];
        }
    }
    return adjusted;
}

std::vector<double> adjusted_returns(const std::vector<double>& raw, const ChangeFlags& flags) {
    const size_t n = raw.size();
    if (flags.change.size() != n || flags.old_close.size() != n || flags.base_close.size() != n) {
        throw std::invalid_argument("roll_series::adjusted_returns: closes and flags differ in length");
    }
    std::vector<double> r;
    if (n < 2) return r;
    r.assign(n - 1, 0.0);
    for (size_t t = 1; t < n; ++t) {
        // a per-contract roll bar moves to the old contract's close; the bar after a confirmed
        // vendor switch moves from the target's close
        const double from = flags.base_close[t - 1] ? *flags.base_close[t - 1] : raw[t - 1];
        if ((flags.change[t] && !flags.old_close[t] && !flags.base_close[t - 1]) || !(from > 0.0)) {
            r[t - 1] = 0.0;
        } else {
            const double to = flags.old_close[t] ? *flags.old_close[t] : raw[t];
            r[t - 1] = (to - from) / from;
        }
    }
    return r;
}

Series build_series(const std::vector<double>& raw, const std::vector<std::string>& ids,
                    const RollFields& roll_from) {
    if (ids.size() != raw.size()) {
        throw std::invalid_argument("roll_series::build_series: closes and ids differ in length");
    }
    Series s;
    s.flags = classify_instrument_changes(ids, roll_from);
    s.raw = raw;
    s.adjusted = adjusted_levels(raw, s.flags);
    s.returns = adjusted_returns(raw, s.flags);
    return s;
}

RollTracker::Status RollTracker::add(const std::string& id, double close,
                                     const std::optional<RollFrom>& rf) {
    Status st;
    ++n_;
    if (id.empty()) {
        st.held_id = held_;
        st.pending = !pending_.empty();
        last_close_ = close;
        return st;
    }
    if (last_known_.empty()) {
        held_ = id;
        last_known_ = id;
        st.held_id = held_;
        last_close_ = close;
        return st;
    }
    if (awaiting_) {
        // Rolls step 3: the contract held going into this bar decides the vendor switch (its own
        // id, or the contract a roll known on this bar leaves).
        const std::string& before = rf && rf->kind == RollFromKind::KNOWN ? rf->instrument_id : id;
        pending_.clear();
        awaiting_ = false;
        const bool on_target = awaiting_on_target_;
        awaiting_on_target_ = false;
        // Rolls step 4: a field following a later roll into the target does not confirm it.
        const bool later_roll = rf && rf->kind == RollFromKind::AFTER_ROLL && !rf->date.empty() &&
                                !awaiting_date_.empty() && rf->date != awaiting_date_;
        const bool confirmed = before == awaiting_id_ && !later_roll;
        if (!confirmed && on_target) {
            // Ruling 19 "vendor": nothing rolled; this bar moves from the held contract's close on
            // the switch bar, not from the target's the bar carried.
            st.previous_close = awaiting_old_close_;
        }
        if (confirmed) {
            // The roll happened on the switch bar: both legs at its two closes; this bar moves from
            // the target's close (ruling 19 "vendor": the switch bar's own, the previous close).
            st.confirm = true;
            st.previous_held_id = awaiting_old_id_;
            st.last_close_before_change = awaiting_old_close_;
            st.change_bar_close = awaiting_close_;
            st.bars_pending = 1;
            if (!on_target)
                st.previous_close = awaiting_close_;
            // ("vendor": the switch bar already carries the target's id, so this is no change)
            st.change = on_target ? id != awaiting_id_ : id != last_known_;
            held_ = awaiting_id_;
            last_known_ = id;
            if (rf && rf->kind == RollFromKind::KNOWN) {
                throw std::runtime_error(
                    "roll_series::RollTracker: a vendor switch confirmed by a bar that is itself a "
                    "roll (" + awaiting_old_id_ + " to " + held_ + " to " + id + "): two rolls on one bar");
            }
            if (!rf || rf->kind == RollFromKind::AFTER_ROLL) {
                st.held_id = held_;
                last_close_ = close;
                return st;
            }
        }
    }
    if (rf && rf->kind == RollFromKind::AWAITS_NEXT_VENDOR) {
        // Ruling 19 "vendor": the bar is the target's own; the field names the held contract, whose
        // close the bar's move ends at.
        st.pending = true;
        st.mark_close = rf->close;
        pending_.assign(1, Pending{id, close});
        awaiting_ = true;
        awaiting_on_target_ = true;
        awaiting_id_ = id;
        awaiting_close_ = close;
        awaiting_old_close_ = rf->close;
        awaiting_old_id_ = rf->instrument_id;
        awaiting_date_ = rf->date;
        last_known_ = rf->instrument_id;
        st.held_id = held_;
        last_close_ = close;
        return st;
    }
    if (rf && rf->kind == RollFromKind::AWAITS_NEXT) {
        // The bar is the held contract's own; the field names the target.
        st.pending = true;
        pending_.assign(1, Pending{id, close});
        awaiting_ = true;
        awaiting_id_ = rf->instrument_id;
        awaiting_close_ = rf->close;
        awaiting_old_close_ = close;
        awaiting_old_id_ = id;
        awaiting_date_ = rf->date;
        last_known_ = id;
        st.held_id = held_;
        last_close_ = close;
        return st;
    }
    const bool after_roll_acts = rf && rf->kind == RollFromKind::AFTER_ROLL &&
                                 rf->instrument_id == last_known_ && id != last_known_;
    if (rf && (rf->kind == RollFromKind::KNOWN || after_roll_acts)) {
        // (a switch confirmed on this bar never reaches here: a KNOWN field threw above, an
        // AFTER_ROLL field or none returned)
        st.change = true;
        st.mark_close = rf->close;
        st.confirm = true;
        st.previous_held_id = rf->instrument_id;
        st.last_close_before_change = rf->close;
        st.change_bar_close = close;
        st.bars_pending = 1;
        held_ = id;
        last_known_ = id;
        pending_.clear();
        st.held_id = held_;
        last_close_ = close;
        return st;
    }
    st.change = id != last_known_;
    last_known_ = id;
    if (pending_.empty()) {
        if (st.change) {
            close_before_pending_ = last_close_;
            pending_.push_back({id, close});
            st.pending = true;
        }
        st.held_id = held_;
        last_close_ = close;
        return st;
    }
    const Pending& last = pending_.back();
    if (id == last.id) {
        st.confirm = true;
        st.previous_held_id = held_;
        st.last_close_before_change = close_before_pending_;
        st.change_bar_close = last.close;
        st.bars_pending = static_cast<int>(pending_.size());
        held_ = last.id;
        pending_.clear();
    } else if (id == held_) {
        st.flip = true;
        st.bars_pending = static_cast<int>(pending_.size());
        pending_.clear();
    } else {
        pending_.push_back({id, close});
        st.pending = true;
    }
    st.held_id = held_;
    last_close_ = close;
    return st;
}

std::vector<ExecutionReport> make_roll_legs(
    const std::string& symbol, double q_held, double closing_price, double opening_price,
    const std::string& outgoing_id, const std::string& incoming_id, const Timestamp& fill_time,
    const std::string& exec_id_close, const std::string& order_id_close,
    const std::string& exec_id_open, const std::string& order_id_open,
    const std::function<RollLegCost(const std::string&, double, double)>& cost_of) {
    std::vector<ExecutionReport> legs;
    if (q_held == 0.0) return legs;
    if (!(closing_price > 0.0) || !(opening_price > 0.0)) {
        throw std::invalid_argument("roll_series::make_roll_legs: a leg without a usable close (" +
                                    symbol + ")");
    }
    const double qty = std::abs(q_held);
    auto leg = [&](bool closing) {
        ExecutionReport e;
        e.symbol = symbol;
        e.execution_type = ExecutionType::ROLL;
        // The closing leg trades against the position, the opening leg with it.
        const double signed_qty = closing ? -q_held : q_held;
        e.side = signed_qty > 0 ? Side::BUY : Side::SELL;
        e.filled_quantity = Decimal(qty);
        e.fill_price = Decimal(closing ? closing_price : opening_price);
        e.fill_time = fill_time;
        e.exec_id = closing ? exec_id_close : exec_id_open;
        e.order_id = closing ? order_id_close : order_id_open;
        e.instrument_id = closing ? outgoing_id : incoming_id;
        const RollLegCost c = cost_of(symbol, signed_qty, closing ? closing_price : opening_price);
        e.commissions_fees = Decimal(c.commissions_fees);
        e.implicit_price_impact = Decimal(c.implicit_price_impact);
        e.slippage_market_impact = Decimal(c.slippage_market_impact);
        e.total_transaction_costs = Decimal(c.total_transaction_costs);
        e.netting_adjustment = Decimal();
        e.is_partial = false;
        return e;
    };
    legs.push_back(leg(true));
    legs.push_back(leg(false));
    return legs;
}

std::unordered_map<std::string, RollTracker::Status> roll_status_of(const std::vector<Bar>& bars) {
    std::map<std::string, std::map<Timestamp, const Bar*>> by_symbol;
    for (const auto& bar : bars) by_symbol[bar.symbol][bar.timestamp] = &bar;
    std::unordered_map<std::string, RollTracker::Status> out;
    for (const auto& [symbol, series] : by_symbol) {
        RollTracker tracker;
        RollTracker::Status last;
        for (const auto& [ts, bar] : series) {
            last = tracker.add(bar->instrument_id, static_cast<double>(bar->close), bar->roll_from);
        }
        out[symbol] = last;
    }
    return out;
}

}  // namespace trade_ngin::roll_series
