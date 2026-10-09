#pragma once

// LOOP_SPEC section 7.7: the log lines of one rebalance of the one pass, as text. One OVERLAY and
// one OPTIMISER line a rebalance (a refused rebalance: the OVERLAY line only), one BOOK line, and
// the RISK_TRIM and RISK_OVER_LIMIT_BY_HOLD warnings of section 6.4. Pure functions of the pass's
// inputs and result, so a test can pin every field.

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "trade_ngin/optimization/one_pass.hpp"
#include "trade_ngin/risk/overlay_record.hpp"

namespace trade_ngin {
namespace one_pass {

namespace log_detail {

inline std::string num(double v) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.10g", v);
    return buf;
}

inline std::string names(const std::vector<std::string>& symbols, const Mask& mask) {
    std::string out;
    for (std::size_t i = 0; i < mask.size() && i < symbols.size(); ++i) {
        if (mask[i]) out += (out.empty() ? "" : " ") + symbols[i];
    }
    return out.empty() ? std::string("-") : out;
}

inline long count(const Mask& mask) {
    long n = 0;
    for (char c : mask) n += c ? 1 : 0;
    return n;
}

}  // namespace log_detail

/// The OVERLAY line: m, the binding term, the five readings of the capped target (the three risk
/// readings print "blind" on a blind window), the limits, the five multipliers, the capital, tau,
/// the window (mode, dates, first, last, complete dates, dropped dates, whether the complete-date
/// covariance engaged), the participants, and the symbols left out of each reading. A refused
/// rebalance prints "OVERLAY refused" with the reason.
inline std::string overlay_line(const std::vector<std::string>& symbols, const DayInputs& in,
                                const DayResult& r) {
    using log_detail::num;
    if (!r.refusal.empty() && !r.refusal_on_reread) {
        return "OVERLAY refused capital=" + num(in.capital) + " tau=" + num(in.tau) +
               " participants=" + std::to_string(r.participants.size()) + " reason=\"" + r.refusal +
               "\"";
    }
    const auto& w = r.window;
    const bool blind = w.blind();
    auto part_names = [&](const std::vector<char>& mask) {
        std::string out;
        for (std::size_t a = 0; a < mask.size() && a < r.participants.size(); ++a) {
            if (mask[a]) out += (out.empty() ? "" : " ") + symbols[r.participants[a]];
        }
        return out.empty() ? std::string("-") : out;
    };
    return "OVERLAY m=" + num(r.multiplier.m) + " binding=" + r.multiplier.binding +
           " R=" + (blind ? "blind" : num(r.readings.risk)) +
           " R_jump=" + (blind ? "blind" : num(r.readings.jump)) +
           " R_shock=" + (blind ? "blind" : num(r.readings.shock)) +
           " L_g=" + num(r.readings.gross) + " L_n=" + num(r.readings.net) +
           " limits R_max=" + num(in.limits.risk) + " R_jump_max=" + num(in.limits.jump) +
           " R_shock_max=" + num(in.limits.shock) + " L_max=" + num(in.limits.gross) +
           " L_net_max=" + num(in.limits.net) + " multipliers R=" + num(r.multiplier.risk) +
           " R_jump=" + num(r.multiplier.jump) + " R_shock=" + num(r.multiplier.shock) +
           " L_g=" + num(r.multiplier.gross) + " L_n=" + num(r.multiplier.net) +
           " capital=" + num(in.capital) + " tau=" + num(in.tau) +
           " window=" + overlay::GateWindow::name(w.mode) +
           " dates=" + std::to_string(w.window_dates) +
           " first=" + (w.window_dates ? overlay::ordinal_date(w.first_ordinal) : "none") +
           " last=" + (w.window_dates ? overlay::ordinal_date(w.last_ordinal) : "none") +
           " complete_dates=" + std::to_string(w.complete_dates) +
           " dropped_dates=" + std::to_string(w.window_dates - w.complete_dates) +
           " f5_engaged=" + (w.mode == overlay::GateWindow::Mode::kComplete ? "1" : "0") +
           " participants=" + std::to_string(r.participants.size()) +
           " free=" + std::to_string(log_detail::count(r.free)) +
           " held=" + std::to_string(log_detail::count(r.fixed)) +
           " no_return=[" + part_names(w.no_return) + "] short_history=[" +
           part_names(w.short_history) + "]";
}

/// The OPTIMISER line: the search's tracking error with its cost term, its passes and whether the
/// pass cap ended it, TE_h, B_sigma and the symbol that sets it ("floor" when the floor governs),
/// a, whether the buffer traded and whether the rounding returned the book to the held one, the
/// forecast-sign closes, the cap clips, the close-outs and the free rows on the 0.01 diagonal.
inline std::string optimiser_line(const std::vector<std::string>& symbols, const DayResult& r) {
    using log_detail::num;
    Mask stale(symbols.size(), 0);
    for (std::size_t a = 0; a < r.free_rows.size() && a < r.stale.size(); ++a) {
        if (r.stale[a]) stale[r.free_rows[a]] = 1;
    }
    return std::string("OPTIMISER searched=") + (r.searched ? "1" : "0") + " te=" + num(r.search_te) +
           " passes=" + std::to_string(r.passes) + " pass_capped=" + (r.pass_capped ? "1" : "0") +
           " te_h=" + num(r.te_held) + " b_sigma=" + num(r.b_sigma) + " b_symbol=" +
           (r.b_symbol < 0 ? std::string("floor") : symbols[static_cast<std::size_t>(r.b_symbol)]) +
           " a=" + num(r.a) + " traded=" + (r.traded ? "1" : "0") +
           " returned_to_held=" + (r.returned_to_held ? "1" : "0") +
           " free=" + std::to_string(r.free_rows.size()) +
           " sign_closes=[" + log_detail::names(symbols, r.sign_closed) + "] cap_clips=[" +
           log_detail::names(symbols, r.clipped) + "] close_outs=[" +
           log_detail::names(symbols, r.closeout) + "] stale=[" + log_detail::names(symbols, stale) +
           "]" + (r.returned_to_held ? " buffer traded, rounding returned to held" : "");
}

/// The BOOK line: the gross weights on the sizing capital of the raw target N*, the capped target,
/// the scaled target, the held book and the stored book, the stored book's net weight, the count
/// of held rows, the deferral-band holds, the equity slow rule's zeroed symbols, and every
/// participant's row as symbol:N*:scaled:held:stored.
inline std::string book_line(const std::vector<std::string>& symbols, const DayInputs& in,
                             const DayResult& r, const Mask& slow_zeroed) {
    using log_detail::num;
    double raw = 0.0, capped = 0.0, scaled = 0.0, held = 0.0, stored = 0.0, net = 0.0;
    std::string rows;
    for (std::size_t i = 0; i < symbols.size(); ++i) {
        const double u = r.u[i];
        if (r.free[i]) raw += std::abs(in.target[i]) * u;
        capped += std::abs(r.capped_target[i]) * u;
        scaled += std::abs(r.scaled_target[i]) * u;
        held += std::abs(in.held[i]) * u;
        stored += std::abs(r.book[i]) * u;
        net += r.book[i] * u;
        if (!r.participant[i]) continue;
        rows += (rows.empty() ? "" : " ") + symbols[i] + ":" + num(r.free[i] ? in.target[i] : 0.0) +
                ":" + num(r.scaled_target[i]) + ":" + num(in.held[i]) + ":" + num(r.book[i]);
    }
    return "BOOK raw_target_gross=" + num(raw) + " target_gross=" + num(capped) +
           " scaled_target_gross=" + num(scaled) + " held_gross=" + num(held) +
           " stored_gross=" + num(stored) + " stored_net=" + num(net) +
           " held_count=" + std::to_string(log_detail::count(r.fixed)) +
           " band_holds=[" + log_detail::names(symbols, r.band) + "] slow_rule_zeroed=[" +
           log_detail::names(symbols, slow_zeroed) + "] rows=[" + (rows.empty() ? "-" : rows) + "]";
}

/// The RISK_TRIM warning of a trading day whose stored book is still above a limit after the trim:
/// the terms in section 6.4's order with each excess, the largest excess in units of the largest
/// non-zero stored u_i, the contracts the trim removed and whether it ran to its cap.
inline std::string risk_trim_line(const std::vector<std::string>& symbols, const DayResult& r) {
    using log_detail::num;
    std::string terms, removed;
    for (const auto& [term, excess] : r.over_limit) {
        terms += (terms.empty() ? "" : ";") + term + ":" + num(excess);
    }
    for (std::size_t i = 0; i < symbols.size(); ++i) {
        if (r.trimmed[i] != 0.0) {
            removed += (removed.empty() ? "" : " ") + symbols[i] + ":" + num(r.trimmed[i]);
        }
    }
    return "RISK_TRIM over_limit_after_rounding terms=[" + terms + "] excess_units=" +
           num(r.over_limit_excess_units) + " trimmed=[" + (removed.empty() ? "-" : removed) +
           "] trim_capped=" + (r.trim_capped ? "1" : "0") +
           ": the stored book is above the limit after the trim and is stored as it is";
}

/// The RISK_OVER_LIMIT_BY_HOLD warning: the terms the held rows alone keep over (CAP for a held
/// row beyond the per-name cap) and the held rows named.
inline std::string over_limit_by_hold_line(const std::vector<std::string>& symbols,
                                           const DayResult& r) {
    std::string terms;
    for (const auto& term : r.by_hold_terms) terms += (terms.empty() ? "" : ";") + term;
    return "RISK_OVER_LIMIT_BY_HOLD terms=[" + terms + "] symbols=[" +
           log_detail::names(symbols, r.by_hold) +
           "]: the held rows keep the book over; they are held, not cut";
}

/// The families the lap loop's optimiser and gate wrote and section 7.7 keeps, from the pass's own
/// window and covariance: T4_RISK_WINDOW (the overlay's window), OPTIMIZER_NOT_SIGNALLING (the
/// listed symbols no sleeve signals yet), COVARIANCE_DATE_ALIGNED (the free rows the optimiser's
/// covariance estimates, of all the free rows), COVARIANCE_MAX_RHO (the largest |rho| among the
/// free rows the stored book holds), and the two warnings COVARIANCE_STALE_PARTICIPANT (a free row
/// whose last close trails the newest by more than five union dates) and COVARIANCE_FLOOR_DROP
/// (a free row the drop rule left out): each on the guarded 0.01 diagonal.
struct KeptLines {
    std::vector<std::string> info;
    std::vector<std::string> warn;
};

inline KeptLines kept_lines(const std::vector<std::string>& symbols, const DayInputs& in,
                            const DayResult& r) {
    using log_detail::num;
    KeptLines out;
    const auto& w = r.window;
    out.info.push_back("T4_RISK_WINDOW dates=" + std::to_string(w.window_dates) +
                       " symbols=" + std::to_string(r.participants.size()) +
                       " complete_dates=" + std::to_string(w.complete_dates) +
                       " dates_dropped=" + std::to_string(w.window_dates - w.complete_dates) +
                       " f5_engaged=" +
                       (w.mode == overlay::GateWindow::Mode::kComplete ? "1" : "0") +
                       " window=" + overlay::GateWindow::name(w.mode));
    {
        std::string list;
        long count = 0, nonzero = 0;
        for (std::size_t i = 0; i < symbols.size(); ++i) {
            if (in.signalling[i]) continue;
            ++count;
            nonzero += in.held[i] != 0.0 ? 1 : 0;
            list += (list.empty() ? "" : ",") + symbols[i];
        }
        if (count > 0) {
            out.info.push_back("OPTIMIZER_NOT_SIGNALLING count=" + std::to_string(count) +
                               " symbols=" + list + " held=" + std::to_string(nonzero) +
                               ": no sleeve signals them (warm-up); outside the search and the "
                               "optimiser's covariance");
        }
    }
    if (!r.searched) return out;
    const std::size_t f = r.free_rows.size();
    long estimated = 0;
    for (std::size_t a = 0; a < f; ++a) {
        const bool filled = a < r.covariance.filled.size() && r.covariance.filled[a];
        estimated += filled ? 0 : 1;
        if (!filled) continue;
        const std::string& symbol = symbols[r.free_rows[a]];
        if (a < r.stale.size() && r.stale[a]) {
            out.warn.push_back("COVARIANCE_STALE_PARTICIPANT symbol=" + symbol +
                               ": its last close trails the newest by more than five union dates; "
                               "left out of the optimiser's date intersection; its column is the "
                               "guarded 0.01 variance with zero covariances");
        } else {
            out.warn.push_back("COVARIANCE_FLOOR_DROP symbol=" + symbol +
                               ": left out of the optimiser's date intersection (under 20 returns "
                               "with it); its column is the guarded 0.01 variance with zero "
                               "covariances");
        }
    }
    out.info.push_back("COVARIANCE_DATE_ALIGNED symbols=" + std::to_string(estimated) + "/" +
                       std::to_string(f) + " bars_per_year=" + num(r.covariance.bars_per_year) +
                       " left_out=" + std::to_string(static_cast<long>(f) - estimated));
    {
        double best = -1.0;
        std::string pair = "-";
        long held = 0;
        for (std::size_t a = 0; a < f; ++a) held += r.book[r.free_rows[a]] != 0.0 ? 1 : 0;
        for (std::size_t a = 0; a < f; ++a) {
            if (r.book[r.free_rows[a]] == 0.0) continue;
            for (std::size_t b = a + 1; b < f; ++b) {
                if (r.book[r.free_rows[b]] == 0.0) continue;
                const double va = r.covariance.matrix[a][a], vb = r.covariance.matrix[b][b];
                if (!(va > 0.0) || !(vb > 0.0)) continue;
                const double rho = std::abs(r.covariance.matrix[a][b] / std::sqrt(va * vb));
                if (rho > best) {
                    best = rho;
                    pair = symbols[r.free_rows[a]] + "/" + symbols[r.free_rows[b]];
                }
            }
        }
        out.info.push_back("COVARIANCE_MAX_RHO held=" + std::to_string(held) + " optimizer=" +
                           (best < 0.0 ? std::string("-") : num(best)) + " pair=" + pair);
    }
    return out;
}

}  // namespace one_pass
}  // namespace trade_ngin
