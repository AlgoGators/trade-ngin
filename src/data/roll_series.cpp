// src/data/roll_series.cpp
#include "trade_ngin/data/roll_series.hpp"

#include <map>
#include <stdexcept>

namespace trade_ngin::roll_series {

ChangeFlags classify_instrument_changes(const std::vector<std::string>& ids) {
    const size_t n = ids.size();
    ChangeFlags f;
    f.change.assign(n, false);
    f.confirm.assign(n, false);
    f.flip.assign(n, false);
    f.rolled.assign(n, false);
    f.pending.assign(n, false);
    f.leg_from.assign(n, -1);
    f.leg_to.assign(n, -1);
    f.held_id.assign(n, std::string());
    if (n == 0) return f;

    std::string held;            // the confirmed id the position is held in
    std::string last_known;      // the last known id seen (the previous consumed bar's, skipping unknowns)
    std::vector<size_t> pending;  // the change bars of the unresolved sequence, oldest first
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

Series build_series(const std::vector<double>& raw, const std::vector<std::string>& ids) {
    if (ids.size() != raw.size()) {
        throw std::invalid_argument("roll_series::build_series: closes and ids differ in length");
    }
    Series s;
    s.raw = raw;
    s.flags = classify_instrument_changes(ids);
    s.adjusted = adjusted_levels(raw, s.flags.change);
    s.returns = adjusted_returns(raw, s.flags.change);
    return s;
}

RollTracker::Status RollTracker::add(const std::string& id, double close) {
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

std::unordered_map<std::string, RollTracker::Status> roll_status_of(const std::vector<Bar>& bars) {
    std::map<std::string, std::map<Timestamp, const Bar*>> by_symbol;
    for (const auto& bar : bars) by_symbol[bar.symbol][bar.timestamp] = &bar;
    std::unordered_map<std::string, RollTracker::Status> out;
    for (const auto& [symbol, series] : by_symbol) {
        RollTracker tracker;
        RollTracker::Status last;
        for (const auto& [ts, bar] : series) {
            last = tracker.add(bar->instrument_id, static_cast<double>(bar->close));
        }
        out[symbol] = last;
    }
    return out;
}

}  // namespace trade_ngin::roll_series
