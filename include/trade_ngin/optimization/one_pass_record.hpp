// include/trade_ngin/optimization/one_pass_record.hpp
#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "trade_ngin/core/record_file.hpp"
#include "trade_ngin/optimization/one_pass.hpp"
#include "trade_ngin/risk/overlay_record.hpp"

namespace trade_ngin {
namespace one_pass {

/**
 * @brief The rebalance's own record of one day, for the acceptance comparison against the
 *        reference implementation: one row a day in <dir>/onepass_days_<id>.csv and one row a
 *        symbol in <dir>/onepass_book_<id>.csv.
 *
 * Written only when the environment names a directory (TRADE_NGIN_SERIES_DUMP_DIR, the consumed
 * series record's variable); a production run never sets it, and without it nothing is opened,
 * written or logged. Every value is printed with seventeen significant digits. `date` is the
 * rebalance's own signal date: the date of the newest bar the call was fed (a backtest feeds one
 * signal group, a live run its window up to Day T-1), which is not always the last date of the
 * overlay's window. A cycle that runs no pass (every bar of its signal group withheld) has a day
 * row too, with mode `no_pass` and no other value, and no symbol row. The files are written the way
 * record_file.hpp states.
 *
 * A book on buffer_step_rule "edge" adds four day columns at the end (a fraction book's record is
 * unchanged): edge_steps (the buffer's one-contract steps), edge_order (each step as
 * SYMBOL+1 or SYMBOL-1, in the order taken, ";" between), edge_te (the TE of the buffer's book
 * against the search's answer) and edge_stop ("inside": at or below B_sigma; "no_lower": no step
 * lowered the error; "-": the buffer did not trade).
 */
namespace record_detail {

inline std::string days_header(bool edge) {
    return std::string(
               "date,cycle_date,warmup,capital,mode,window_dates,complete_dates,R,R_jump,"
               "R_shock,L,L_net,m,binding,searched,te,passes,pass_capped,te_h,B_sigma,"
               "B_symbol,a,traded,returned_to_held,stored_R,stored_R_jump,stored_R_shock,"
               "stored_L,stored_L_net,over_limit,over_limit_excess_units,by_hold_terms,"
               "trim_capped,target_gross,stored_gross,risk_scale,opt_bars_per_year,carver_R,"
               "carver_stored_R") +
           (edge ? ",edge_steps,edge_order,edge_te,edge_stop" : "");
}

/// The edge columns' order string: SYMBOL+1 or SYMBOL-1 per step, ";" between ("-": none).
inline std::string edge_order(const std::vector<std::string>& symbols, const DayResult& r) {
    std::string out;
    for (const auto& [symbol, step] : r.edge_steps) {
        out += (out.empty() ? "" : ";") + symbols[symbol] + (step > 0.0 ? "+1" : "-1");
    }
    return out.empty() ? std::string("-") : out;
}

}  // namespace record_detail

/// The day row of a cycle that ran no pass: its two dates, the warm-up flag and mode `no_pass`
/// (`edge`: the book is on buffer_step_rule edge, so the row has the edge columns, empty).
inline void append_no_pass_record(const std::string& id, const std::string& signal_date,
                                  const std::string& cycle_date, bool warmup, bool edge = false) {
    const char* dir = std::getenv("TRADE_NGIN_SERIES_DUMP_DIR");
    if (dir == nullptr || *dir == '\0') return;
    const std::string name = "onepass_days_" + id + ".csv";
    if (std::FILE* days =
            record_file::open_rows(dir, name, record_detail::days_header(edge).c_str())) {
        std::fprintf(days, "%s,%s,%d,,no_pass,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,%s\n",
                     signal_date.c_str(), cycle_date.c_str(), warmup ? 1 : 0,
                     edge ? ",,,," : "");
        record_file::close_rows(days, dir, name);
    }
}

inline void append_one_pass_record(const std::string& id, const std::string& signal_date,
                                   const std::string& cycle_date, bool warmup,
                                   const std::vector<std::string>& symbols, const DayInputs& in,
                                   const DayResult& r) {
    const char* dir = std::getenv("TRADE_NGIN_SERIES_DUMP_DIR");
    if (dir == nullptr || *dir == '\0') return;
    const std::string days_name = "onepass_days_" + id + ".csv";
    const std::string book_name = "onepass_book_" + id + ".csv";
    const bool edge = in.buffer_step_rule == BufferStepRule::kEdge;
    if (std::FILE* days =
            record_file::open_rows(dir, days_name, record_detail::days_header(edge).c_str())) {
        std::string over, by_hold;
        for (const auto& [term, excess] : r.over_limit) {
            (void)excess;
            over += (over.empty() ? "" : ";") + term;
        }
        for (const auto& term : r.by_hold_terms) by_hold += (by_hold.empty() ? "" : ";") + term;
        std::fprintf(days,
                     "%s,%s,%d,%.17g,%s,%zu,%zu,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%s,%d,%.17g,%ld,%d,"
                     "%.17g,%.17g,%s,%.17g,%d,%d,%.17g,%.17g,%.17g,%.17g,%.17g,%s,%.17g,%s,%d,%.17g,%.17g,"
                     "%.17g,%.17g,%.17g,%.17g",
                     signal_date.c_str(), cycle_date.c_str(), warmup ? 1 : 0, in.capital,
                     overlay::GateWindow::name(r.window.mode), r.window.window_dates,
                     r.window.complete_dates, r.readings.risk, r.readings.jump, r.readings.shock,
                     r.readings.gross, r.readings.net, r.multiplier.m, r.multiplier.binding.c_str(),
                     r.searched ? 1 : 0, r.search_te, r.passes, r.pass_capped ? 1 : 0, r.te_held,
                     r.b_sigma, r.b_symbol < 0 ? "floor" : symbols[static_cast<std::size_t>(r.b_symbol)].c_str(),
                     r.a, r.traded ? 1 : 0, r.returned_to_held ? 1 : 0, r.stored_readings.risk,
                     r.stored_readings.jump, r.stored_readings.shock, r.stored_readings.gross,
                     r.stored_readings.net, over.empty() ? "-" : over.c_str(), r.over_limit_excess_units,
                     by_hold.empty() ? "-" : by_hold.c_str(), r.trim_capped ? 1 : 0, r.target_gross,
                     r.stored_gross, r.risk_scale, r.covariance.bars_per_year,
                     r.carver_readings.risk, r.carver_stored_readings.risk);
        if (edge) {
            std::fprintf(days, ",%zu,%s,%.17g,%s", r.edge_steps.size(),
                         record_detail::edge_order(symbols, r).c_str(), r.edge_te,
                         !r.traded ? "-" : (r.edge_inside ? "inside" : "no_lower"));
        }
        std::fputc('\n', days);
        record_file::close_rows(days, dir, days_name);
    }
    if (std::FILE* book = record_file::open_rows(
            dir, book_name,
            "date,symbol,multiplier,close,u,held,target,first_forecast,cost,signalling,"
            "first_signalling,hold,has_bar,ever_signalled,band,free,fixed,closeout,"
            "participant,capped_target,cap_bound,scaled_target,sign_closed,search_book,"
            "pre_trim,trimmed,book,clipped,by_hold,sign_fill,rest_fill,cost_adv,cost_vol_mult")) {
        for (std::size_t i = 0; i < symbols.size(); ++i) {
            std::fprintf(book,
                         "%s,%s,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                         "%.17g,%d,%.17g,%d,%.17g,%.17g,%.17g,%.17g,%d,%d,%.17g,%.17g,%.17g,%.17g\n",
                         signal_date.c_str(), symbols[i].c_str(), in.multiplier[i], in.close[i], r.u[i],
                         in.held[i], in.target[i], in.first_forecast[i], in.cost[i],
                         static_cast<int>(in.signalling[i]), static_cast<int>(in.first_signalling[i]),
                         static_cast<int>(in.hold[i]), static_cast<int>(in.has_bar[i]),
                         static_cast<int>(in.ever_signalled[i]), static_cast<int>(r.band[i]),
                         static_cast<int>(r.free[i]), static_cast<int>(r.fixed[i]),
                         static_cast<int>(r.closeout[i]), static_cast<int>(r.participant[i]),
                         r.capped_target[i], static_cast<int>(r.cap_bound[i]), r.scaled_target[i],
                         static_cast<int>(r.sign_closed[i]), r.search_book[i], r.pre_trim[i],
                         r.trimmed[i], r.book[i], static_cast<int>(r.clipped[i]),
                         static_cast<int>(r.by_hold[i]), r.sign_fill[i], r.rest_fill[i],
                         i < in.cost_adv.size() ? in.cost_adv[i] : 0.0,
                         i < in.cost_vol_mult.size() ? in.cost_vol_mult[i] : 0.0);
        }
        record_file::close_rows(book, dir, book_name);
    }
}

}  // namespace one_pass
}  // namespace trade_ngin
