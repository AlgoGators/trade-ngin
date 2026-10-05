// include/trade_ngin/portfolio/sizing_capital.hpp
//
// T-7b-2 9c (HD 2026-09-25): compounding. The futures book is sized on the account's current
// equity, not on the constant initial capital (Carver sizes on current trading capital). The
// quantity is the SAME on both paths: the account's equity marked at the close of the newest bar
// the sizing reads (the signal bar, T-1), after every fill before it. It never reads the future:
// the day being sized has no mark yet on either path.
//
//   backtest  the equity curve's last row when the cycle for day T sizes: the row of the previous
//             cycle, marked at T-1's close (this cycle's row is appended after its fills).
//   live      the run for date T finalises Day T-1 only AFTER it sizes (STEP 4 writes Day T-1's row as the row
//             before it plus Day T-1's settlement move less Day T-1's costs; STEP 5 reads it back as the day's
//             previous portfolio value). So it is rebuilt before sizing from STEP 4's own three parts: the stored
//             portfolio value of the latest row BEFORE Day T-1, plus the Day T-1 settlement move of each sleeve's
//             stored T-1 book, q x (close(T-1) - close(T-2)) x point value, LivePnLManager's SETTLED rule (a
//             position missing either close settles nothing, as PHASE 5 books it), less the costs stored on Day
//             T-1's row. It never reads Day T-1's own stored value, which is finalised already when a date is run
//             again (a replay, a chain's first day) and not yet otherwise, so the figure is the same either way.
//             With no Day T-1 row stored, STEP 4 updates nothing and STEP 5 reads the latest stored row before
//             the run date: that value is the figure.
//
// Header-only and pure: the runners and the coordinator call these, the tests pin them.
#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "trade_ngin/core/types.hpp"

namespace trade_ngin {

/// The backtest's sizing equity for the cycle about to size: the equity curve's last row (the
/// previous cycle's close), or `initial_capital` when the curve is empty. Warm-up rows are flat at
/// the initial capital, so warm-up sizes exactly as before.
inline double backtest_sizing_equity(const std::vector<std::pair<Timestamp, double>>& equity_curve,
                                     double initial_capital) {
    return equity_curve.empty() ? initial_capital : equity_curve.back().second;
}

/// The live runner's sizing equity and its parts, for the log line.
struct LiveSizingEquity {
    double equity{0.0};         ///< day_before + t1_settlement - t1_costs (day_before alone without a T-1 row)
    bool t1_row{false};         ///< a Day T-1 row is stored (STEP 4 will finalise it)
    double day_before{0.0};     ///< the latest stored value before Day T-1 (before the run date without a T-1 row)
    double t1_settlement{0.0};  ///< Day T-1's settlement move of the stored T-1 books
    double t1_costs{0.0};       ///< the transaction costs stored on Day T-1's row
    int priced{0};              ///< held positions with both closes (they settle)
    int unpriced{0};            ///< held positions missing a T-1 or a T-2 close (settle 0)
};

/// The live runner's sizing equity (see the file comment). With `t1_row_stored`, `day_before` is the stored
/// portfolio value of the latest row before Day T-1 and `t1_costs` Day T-1's stored daily transaction costs;
/// without it, `day_before` is the latest stored value before the run date and is the figure. `t1_books` are the
/// sleeves' stored books of the previous day, one map per sleeve; `t1_closes` and `t2_closes` are the price
/// manager's Day T-1 and Day T-2 closes (the maps PHASE 5 finalises with); `point_value` is
/// LivePnLManager::get_point_value. A zero quantity contributes nothing and is not counted.
/// T-ROLLX-FIX commit 4 (D-B; LOOP_SPEC v6.2 sections 2.1, 3.1, 6.6): the maps and
/// `zero_settlement_symbols` are STEP 4's own, the T-1 settlement on the CONSUMED bars
/// (live/session_book_gate.hpp, consumed_t1_settlement): a change bar and a withheld bar settle 0 and
/// every other bar settles against its previous consumed close, so the book is sized on the equity its
/// stored rows will show.
inline LiveSizingEquity live_sizing_equity(
    bool t1_row_stored, double day_before, double t1_costs,
    const std::vector<std::unordered_map<std::string, Position>>& t1_books,
    const std::unordered_map<std::string, double>& t1_closes,
    const std::unordered_map<std::string, double>& t2_closes,
    const std::function<double(const std::string&)>& point_value,
    const std::unordered_set<std::string>& zero_settlement_symbols = {}) {
    LiveSizingEquity out;
    out.t1_row = t1_row_stored;
    out.day_before = day_before;
    if (!t1_row_stored) {
        out.equity = day_before;
        return out;
    }
    out.t1_costs = t1_costs;
    for (const auto& book : t1_books) {
        for (const auto& [symbol, position] : book) {
            const double quantity = position.quantity.as_double();
            if (quantity == 0.0) continue;
            const auto t1 = t1_closes.find(symbol);
            const auto t2 = t2_closes.find(symbol);
            if (t1 == t1_closes.end() || t2 == t2_closes.end()) {
                ++out.unpriced;
                continue;
            }
            // A change bar or a withheld bar settles nothing (LivePnLManager's zero_pnl_symbols).
            if (!zero_settlement_symbols.count(symbol)) {
                out.t1_settlement += quantity * (t1->second - t2->second) * point_value(symbol);
            }
            ++out.priced;
        }
    }
    out.equity = out.day_before + out.t1_settlement - out.t1_costs;
    return out;
}

}  // namespace trade_ngin
