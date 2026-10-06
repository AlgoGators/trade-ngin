// include/trade_ngin/risk/overlay_record.hpp
#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>

#include "trade_ngin/risk/overlay.hpp"

namespace trade_ngin {
namespace overlay {

/// A date as YYYY-MM-DD from a whole day number counted from 1970-01-01.
inline std::string ordinal_date(double ordinal) {
    long z = static_cast<long>(ordinal) + 719468;
    const long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned long doe = static_cast<unsigned long>(z - era * 146097);
    const unsigned long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long y = static_cast<long>(yoe) + era * 400;
    const unsigned long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned long mp = (5 * doy + 2) / 153;
    const unsigned long d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned long m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) ++y;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04ld-%02lu-%02lu", y, m, d);
    return buf;
}

/**
 * @brief The overlay's own record of one evaluation, for the acceptance comparison against the
 *        reference implementation: one row a participant in <dir>/overlay_book_<id>.csv and one row
 *        an evaluation in <dir>/overlay_days_<id>.csv.
 *
 * Written only when the environment names a directory (TRADE_NGIN_SERIES_DUMP_DIR, the consumed
 * series record's variable); a production run never sets it, and without it nothing is opened,
 * written or logged. Every value is printed with seventeen significant digits. `signal_date` is the
 * window's last date; `quantities[i]` is the book's quantity of participant i.
 */
inline void append_overlay_record(const std::string& id, int lap, bool warmup, double capital,
                                  const Inputs& inputs, const Evaluation& e,
                                  const std::vector<double>& quantities) {
    const char* dir = std::getenv("TRADE_NGIN_SERIES_DUMP_DIR");
    if (dir == nullptr || *dir == '\0') return;
    const std::string signal_date =
        inputs.ordinals.empty() ? std::string("none") : ordinal_date(inputs.ordinals.back());
    auto open = [&](const std::string& name, const char* header) -> std::FILE* {
        const std::string path = std::string(dir) + "/" + name + "_" + id + ".csv";
        bool fresh = true;
        if (std::FILE* probe = std::fopen(path.c_str(), "r")) {
            fresh = false;
            std::fclose(probe);
        }
        std::FILE* out = std::fopen(path.c_str(), "a");
        if (out != nullptr && fresh) std::fprintf(out, "%s\n", header);
        return out;
    };
    if (std::FILE* days = open("overlay_days",
                               "date,lap,warmup,capital,tau,mode,window_dates,complete_dates,"
                               "first_date,last_date,bars_per_year,R,R_jump,R_shock,L,L_net,"
                               "R_max,R_jump_max,R_shock_max,L_max,L_net_max,m,binding,m_R,m_R_jump,"
                               "m_R_shock,m_L,m_L_net,outside")) {
        std::string outside;
        for (const auto& s : e.outside) outside += (outside.empty() ? "" : " ") + s;
        std::fprintf(days,
                     "%s,%d,%d,%.17g,%.17g,%s,%zu,%zu,%s,%s,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,"
                     "%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%s,%.17g,%.17g,%.17g,%.17g,%.17g,%s\n",
                     signal_date.c_str(), lap, warmup ? 1 : 0, capital, inputs.tau,
                     GateWindow::name(e.window.mode), e.window.window_dates,
                     e.window.complete_dates,
                     e.window.window_dates ? ordinal_date(e.window.first_ordinal).c_str() : "none",
                     e.window.window_dates ? ordinal_date(e.window.last_ordinal).c_str() : "none",
                     e.window.bars_per_year, e.readings.risk, e.readings.jump, e.readings.shock,
                     e.readings.gross, e.readings.net, e.limits.risk, e.limits.jump,
                     e.limits.shock, e.limits.gross, e.limits.net, e.multiplier.m,
                     e.multiplier.binding.c_str(), e.multiplier.risk, e.multiplier.jump,
                     e.multiplier.shock, e.multiplier.gross, e.multiplier.net, outside.c_str());
        std::fclose(days);
    }
    if (std::FILE* book = open("overlay_book",
                               "date,lap,symbol,quantity,weight,close,multiplier,jump_sigma_daily,"
                               "sigma_jump,has_series,in_r,in_shock,no_return,short_history,"
                               "shock_sigma")) {
        for (std::size_t i = 0; i < inputs.symbols.size(); ++i) {
            std::fprintf(book, "%s,%d,%s,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%d,%d,%d,%d,%d,%.17g\n",
                         signal_date.c_str(), lap, inputs.symbols[i].c_str(),
                         i < quantities.size() ? quantities[i] : 0.0, e.weights[i], inputs.close[i],
                         inputs.multiplier[i], inputs.jump_sigma_daily[i], e.sigma_jump[i],
                         static_cast<int>(inputs.has_series[i]),
                         static_cast<int>(e.window.in_r[i]), static_cast<int>(e.window.in_shock[i]),
                         static_cast<int>(e.window.no_return[i]),
                         static_cast<int>(e.window.short_history[i]), e.window.shock_sigma[i]);
        }
        std::fclose(book);
    }
}

}  // namespace overlay
}  // namespace trade_ngin
