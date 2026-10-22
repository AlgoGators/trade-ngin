// include/trade_ngin/live/settled_stamp.hpp
//
// trading.live_results.settled_at (migration 029; T-8D-2 R43, LOOP_SPEC sections 3.1 and 7.2):
// the stamp a live run writes on the rows of its book it has settled, and the test of whether
// the run's finalize of Day T-1 earned it.
//
// A run writes its own row with settled_at NULL. After STEP 4 (the level rewrite and the
// equity-curve point of Day T-1) and the statistics refresh finished without error, the run
// stamps now() on every earlier row of its key still NULL, in ONE UPDATE: the Day T-1 row it
// finalized, a row whose levels were already final (a flat book, a day with no move) and a row
// an earlier run left. The stamp is withheld, and every row stays as it is, on:
//
//   the no-prices skip   no Day T-1 closes with a book held, or no Day T-2 closes: the day's move
//                        cannot be measured (the sizing read's own two tests, live_sizing_read.hpp)
//   a finalize warning   Day T-1's row could not be read or its level UPDATE matched no row, the
//                        equity-curve point was not rewritten, or the statistics were not refreshed
//
// A failed level UPDATE stops the run (exit 1) before the stamp on all three runners.
//
// Header-only: the three runners call it, the tests read it without a database.
#pragma once

#include <string>

namespace trade_ngin {

/// The one statement that settles a book's earlier rows: every row of the key dated before the
/// run date that carries no stamp. The run's own row is not matched (it is inserted after, NULL).
inline std::string settled_stamp_sql(const std::string& strategy_id, const std::string& portfolio_id,
                                     const std::string& run_date) {
    return "UPDATE trading.live_results SET settled_at = now() "
           "WHERE strategy_id = '" + strategy_id + "' AND portfolio_id = '" + portfolio_id +
           "' AND DATE(date) < '" + run_date + "' AND settled_at IS NULL";
}

/// Whether the run's finalize of Day T-1 finished without error. Open until a path withholds
/// the stamp; the first reason is the one logged.
class SettledStampGate {
public:
    void withhold(const std::string& reason) {
        if (reason_.empty()) reason_ = reason;
    }
    bool open() const { return reason_.empty(); }
    const std::string& reason() const { return reason_; }

private:
    std::string reason_;
};

/// The SETTLED_AT line of a run that stamped.
inline std::string settled_stamp_log_line(const std::string& run_date, size_t rows) {
    return "SETTLED_AT date=" + run_date + " stamped=" + std::to_string(rows) +
           " row(s) dated before the run date";
}

/// The SETTLED_AT line of a run that did not: no row is stamped and the next clean run stamps
/// every row this one left.
inline std::string settled_stamp_withheld_log_line(const std::string& run_date,
                                                   const SettledStampGate& gate) {
    return "SETTLED_AT date=" + run_date + " stamped=none reason=\"" + gate.reason() +
           "\": no row is settled by this run";
}

}  // namespace trade_ngin
