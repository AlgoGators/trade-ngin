// include/trade_ngin/live/live_sizing_read.hpp
//
// T-7b-3 R-3: the futures live runners' sizing reads. The runner sizes the book on the account's
// equity at the close of T-1 (portfolio/sizing_capital.hpp), rebuilt from three stored reads: Day
// T-1's live_results row (its costs, and whether it exists), the latest live_results row before it
// (or before the run date without a Day T-1 row) and each sleeve's stored Day T-1 book. Each read
// has an ordinary "nothing stored" answer, which sizes as before, and a database error.
//
// A database error is not "nothing stored": treated as one it sized the whole book on the initial
// capital (a failed Day T-1 row and a failed previous-row read) or on a book with a sleeve missing,
// and the run traded on it. The only "nothing stored" answers are, per loader:
//
//   LiveDataLoader::load_live_results                INVALID_ARGUMENT "No live results found for date ..."
//                                                    (a query that returned no row)
//   PostgresDatabase::get_previous_live_aggregates   DATABASE_ERROR "No previous aggregates found for ..."
//                                                    (a query that returned no row; the loader reports the
//                                                    no-row case with the database error code, so the
//                                                    message is what tells it apart)
//   DatabaseInterface::load_positions_by_date        none: a sleeve with no stored book is OK and empty
//
// Every other error stops the sizing, split by what was read (HD 2026-09-27, T-7b-3 ruling 5):
//
//   kHoldBook   every sleeve's Day T-1 book loaded, and the Day T-1 row or the previous-row read
//               failed: there is a book but no equity to size it on. The run holds every strategy
//               at its seeded T-1 book (no rebalance, no order), stores the day with the metadata
//               row marked (risk_refusal, scope "sizing"), flags the email and exits
//               kRiskModuleFailureExitCode, as a RISK_MODULE_FAILURE day does.
//   kRefuseRun  a sleeve's Day T-1 book failed to load: there is nothing to hold. The run refuses
//               to start (exit 1, above the live_run_metadata upsert, so no row), as
//               set_sizing_capital's refusal does.
//
// Header-only: the runners call it, the tests feed it a mock database.
#pragma once

#include <chrono>
#include <cmath>
#include <functional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/database_interface.hpp"
#include "trade_ngin/live/live_data_loader.hpp"
#include "trade_ngin/live/risk_module_failure.hpp"
#include "trade_ngin/portfolio/sizing_capital.hpp"

namespace trade_ngin {

/// load_live_results found no row for the date (the only answer that means "no Day T-1 row").
inline bool is_no_live_results_row(const TradeError& error) {
    return error.code() == ErrorCode::INVALID_ARGUMENT &&
           std::string(error.what()).rfind("No live results found", 0) == 0;
}

/// get_previous_live_aggregates found no row before the date (the only answer that means "none
/// stored").
inline bool is_no_previous_live_aggregates(const TradeError& error) {
    return error.code() == ErrorCode::DATABASE_ERROR &&
           std::string(error.what()).rfind("No previous aggregates found", 0) == 0;
}

/// What the runner does with the sizing reads (see the file comment).
enum class LiveSizingOutcome {
    kSized,      ///< every read answered: size on `equity`
    kHoldBook,   ///< the books loaded, the equity or previous-row read failed: hold, mark, exit 3
    kRefuseRun,  ///< a sleeve's book failed to load: refuse to start, exit 1, no row
};

/**
 * @brief What the run knows of the bars behind its P&L history, for the sizing capital's settled
 *        test (LOOP_SPEC section 3.1).
 *
 * `bar_dates` are the dates (YYYY-MM-DD) on which the run loaded a bar of any symbol, over the
 * whole span it loaded; `first_bar_date` is the earliest of them. `no_t1_closes` and
 * `no_t2_closes` are the finalize's own two tests on the price manager's raw maps: the Day T-1
 * close map is empty, and the Day T-2 close map is empty.
 */
struct LiveSizingCalendar {
    std::set<std::string> bar_dates;
    std::string first_bar_date;
    bool no_t1_closes{false};
    bool no_t2_closes{false};
};

/// The sizing reads' outcome and, when sized, the figure and where its day_before came from.
struct LiveSizingRead {
    LiveSizingOutcome outcome{LiveSizingOutcome::kSized};
    LiveSizingEquity equity;        ///< kSized only: the account's value rebuilt at the close of T-1
    std::string day_before_source;  ///< kSized only, for the SIZING_CAPITAL log line
    std::string failure;            ///< kHoldBook / kRefuseRun: the read(s) that failed, with the error
    /// kSized only: the half-compounded sizing capital over the settled history (section 3.1).
    HalfCompounding capital;
    std::string settled_through;    ///< the last settled date the capital read ("none": no settled day)
    int settled_rows{0};            ///< the settled days behind it, Day T-1 included when it settled
    /// Day T-1 is on a failure path: the capital is the last settled one and nothing is added for it.
    bool t1_unsettled{false};
    std::string t1_date;            ///< Day T-1, YYYY-MM-DD
    std::string t1_unsettled_reason;
    std::vector<std::string> earlier_unsettled;  ///< earlier dates still unsettled, in date order
    /// With a Day T-1 row and a stored row before it: the starting capital plus the sum of every
    /// stored daily_pnl before Day T-1, less the stored value of the row before Day T-1. Zero on a
    /// chain whose rows are complete and in order; anything else is P&L the stored value carries
    /// and no row does (or the reverse), which the sizing capital, built from rows, cannot see.
    bool history_compared{false};
    double history_gap{0.0};
};

/**
 * @brief Whether a stored day of the history before Day T-1 is still unsettled.
 *
 * A run settles row D on the run of D + 1, unless that run was on the no-prices path: it had no
 * close dated D while the book held positions. This is the test read from what is stored, the
 * row's own book (`active_positions`) and the dates the run loaded a bar on; it is the calendar
 * limb of sizing_history_row_settled below, beside the row's settled_at (migration 029).
 *
 * The no-bar-day rule: a held day on which no symbol prints (a Saturday) is never finalized, so
 * its stored daily_pnl, the costs of that day's fills, is final. It counts at that stored value,
 * in its own date's place, as soon as ANY bar dated after it is loaded. trading.equity_curve and
 * live_results.current_portfolio_value already carry those costs; this makes the sizing capital
 * and risk_detail.account_value agree with them. A no-bar day with no later bar loaded is still
 * unsettled, and only Day T-1 itself is withheld by its failure paths, for its one run. A date
 * before the loaded span counts as settled, which agrees with the rule (every loaded bar is dated
 * after it), so the capital does not step when a day leaves the loaded window.
 */
inline bool sizing_history_day_unsettled(const LiveDataLoader::PnlHistoryRow& row,
                                         const LiveSizingCalendar& calendar) {
    if (row.active_positions <= 0) return false;
    if (calendar.first_bar_date.empty() || row.date < calendar.first_bar_date) return false;
    if (calendar.bar_dates.count(row.date) != 0) return false;
    return calendar.bar_dates.upper_bound(row.date) == calendar.bar_dates.end();
}

/**
 * @brief Whether a stored day of the history before Day T-1 counts in the sizing capital.
 *
 * A row counts when its settled_at is set (migration 029: the stamp of the run that settled it,
 * or the migration's backfill, which LOOP_SPEC section 15 erratum 1 counts as settled whatever
 * the calendar says), OR when the no-bar-day rule above calls it settled.
 */
inline bool sizing_history_row_settled(const LiveDataLoader::PnlHistoryRow& row,
                                       const LiveSizingCalendar& calendar) {
    if (row.settled_at_set) return true;
    // THE CALENDAR LIMB. Both limbs exist because the run after a no-prices day reads its sizing
    // history before its own stamp: the held Saturday row is still NULL when Monday's run sizes.
    // PENDING the lead's ruling on whether this limb stays (T-8a commit (12)); it is this one line.
    return !sizing_history_day_unsettled(row, calendar);
}

/// The runner's sizing reads for the run date `now` and what they decide. The calls, their order
/// and their arguments are the runner's before T-7b-3: Day T-1's row, the previous row (before
/// Day T-1 with a Day T-1 row, before the run date without one), then each sleeve's Day T-1 book.
/// `t1_closes`, `t2_closes` and `zero_settlement_symbols` are the T-1 settlement on the consumed
/// bars (consumed_t1_settlement), the inputs STEP 4 finalises Day T-1 with.
inline LiveSizingRead read_live_sizing_equity(
    LiveDataLoader& data_loader, DatabaseInterface& db, const std::string& strategy_id,
    const std::string& portfolio_id, const std::vector<std::string>& sleeves, const Timestamp& now,
    double initial_capital, const std::unordered_map<std::string, double>& t1_closes,
    const std::unordered_map<std::string, double>& t2_closes,
    const std::function<double(const std::string&)>& point_value,
    const std::unordered_set<std::string>& zero_settlement_symbols,
    const LiveSizingCalendar& calendar) {
    LiveSizingRead out;
    std::string equity_failure;
    const auto sizing_t1 = now - std::chrono::hours(24);
    out.t1_date = core::format_utc_date(sizing_t1);
    auto t1_row = data_loader.load_live_results(strategy_id, portfolio_id, sizing_t1);
    if (t1_row.is_error() && !is_no_live_results_row(*t1_row.error())) {
        equity_failure = "Day T-1's live_results row could not be read (" +
                         std::string(t1_row.error()->what()) + ")";
    }
    const bool t1_row_stored = t1_row.is_ok();
    // With a Day T-1 row: the latest row before it (STEP 4's day_before). Without one: the latest
    // row before the run date (what STEP 5 reads when STEP 4 updates nothing).
    double day_before = initial_capital;
    out.day_before_source = "none stored, the initial capital";
    auto stored = db.get_previous_live_aggregates(strategy_id, portfolio_id,
                                                  t1_row_stored ? sizing_t1 : now,
                                                  "trading.live_results");
    if (stored.is_ok()) {
        day_before = std::get<0>(stored.value());
        out.day_before_source = t1_row_stored
                                    ? "the latest stored row before Day T-1"
                                    : "no Day T-1 row: the latest stored row before the run date";
    } else if (!is_no_previous_live_aggregates(*stored.error())) {
        equity_failure += std::string(equity_failure.empty() ? "" : "; ") +
                          "the latest live_results row before " +
                          (t1_row_stored ? "Day T-1" : "the run date") + " could not be read (" +
                          std::string(stored.error()->what()) + ")";
    }
    // The book's stored daily net P&L before Day T-1, the history the sizing capital is recomputed
    // from on every run. A failed read is an equity read that failed: there is a book to hold.
    auto history = data_loader.load_sizing_pnl_history(strategy_id, portfolio_id, sizing_t1);
    if (history.is_error()) {
        equity_failure += std::string(equity_failure.empty() ? "" : "; ") +
                          "the stored P&L history before Day T-1 could not be read (" +
                          std::string(history.error()->what()) + ")";
    }
    std::vector<std::unordered_map<std::string, Position>> t1_books;
    for (const auto& sleeve : sleeves) {
        auto t1_book =
            db.load_positions_by_date(strategy_id, sleeve, portfolio_id, sizing_t1, "trading.positions");
        if (t1_book.is_error()) {
            out.outcome = LiveSizingOutcome::kRefuseRun;
            out.failure = "sleeve " + sleeve + "'s Day T-1 book could not be read (" +
                          std::string(t1_book.error()->what()) + ")";
            if (!equity_failure.empty()) out.failure += "; " + equity_failure;
            return out;
        }
        t1_books.push_back(t1_book.value());
    }
    if (!equity_failure.empty()) {
        out.outcome = LiveSizingOutcome::kHoldBook;
        out.failure = equity_failure;
        return out;
    }
    out.equity = live_sizing_equity(t1_row_stored, day_before,
                                    t1_row_stored ? t1_row.value().daily_transaction_costs : 0.0,
                                    t1_books, t1_closes, t2_closes, point_value,
                                    zero_settlement_symbols);

    // The sizing capital (LOOP_SPEC section 3.1): the half compounding of the settled history in
    // date order, then Day T-1's net rebuilt from STEP 4's own parts, unless Day T-1 is on one of
    // the finalize's failure paths.
    std::vector<double> settled_nets;
    out.settled_through = "none";
    for (const auto& row : history.value()) {
        if (!sizing_history_row_settled(row, calendar)) {
            out.earlier_unsettled.push_back(row.date);
            continue;
        }
        settled_nets.push_back(row.daily_pnl);
        out.settled_through = row.date;
    }
    bool held = false;
    for (const auto& book : t1_books) {
        for (const auto& [symbol, position] : book) {
            if (position.quantity.as_double() != 0.0) held = true;
        }
    }
    if (!t1_row_stored) {
        // A book's first run has no row at all and nothing to settle; a book with rows and no Day
        // T-1 row has a day the finalize cannot update.
        if (!history.value().empty()) {
            out.t1_unsettled = true;
            out.t1_unsettled_reason = "no Day T-1 row";
        }
    } else if (calendar.no_t1_closes && held) {
        out.t1_unsettled = true;
        out.t1_unsettled_reason = "no T-1 closes";
    } else if (calendar.no_t2_closes) {
        out.t1_unsettled = true;
        out.t1_unsettled_reason = "no T-2 closes";
    } else {
        settled_nets.push_back(out.equity.t1_settlement - out.equity.t1_costs);
        out.settled_through = out.t1_date;
    }
    out.settled_rows = static_cast<int>(settled_nets.size());
    out.capital = half_compounded_capital(initial_capital, settled_nets);
    if (t1_row_stored && stored.is_ok() && !history.value().empty()) {
        double history_sum = 0.0;
        for (const auto& row : history.value()) history_sum += row.daily_pnl;
        out.history_compared = true;
        out.history_gap = initial_capital + history_sum - day_before;
    }
    return out;
}

/// The stored rows and the stored value part by more than a cent.
inline bool sizing_history_mismatch(const LiveSizingRead& read) {
    return read.history_compared && std::abs(read.history_gap) > 0.01;
}

/// The SIZING_CAPITAL_HISTORY line (WARN) printed beside SIZING_CAPITAL when they do.
inline std::string sizing_capital_history_log_line(const std::string& run_date,
                                                   const LiveSizingRead& read) {
    return "SIZING_CAPITAL_HISTORY date=" + run_date + " gap=" + std::to_string(read.history_gap) +
           " day_before=" + std::to_string(read.equity.day_before) +
           ": the starting capital plus the stored daily_pnl of every row before Day T-1 is not "
           "the stored value of the row before Day T-1; the sizing capital and "
           "risk_detail.account_value are built from the rows";
}

/// The SIZING_CAPITAL line of a sized run (LOOP_SPEC section 7.7): the capital, the account the
/// settled history gives, the running peak of the cumulative settled P&L and the last settled date
/// read, then the parts Day T-1 was rebuilt from.
inline std::string sizing_capital_log_line(const std::string& run_date, const LiveSizingRead& read) {
    return "SIZING_CAPITAL date=" + run_date + " capital=" + std::to_string(read.capital.capital) +
           " account=" + std::to_string(read.capital.account) +
           " peak=" + std::to_string(read.capital.peak) +
           " settled_through=" + read.settled_through +
           " settled_rows=" + std::to_string(read.settled_rows) +
           " earlier_unsettled=" + std::to_string(read.earlier_unsettled.size()) +
           " day_before=" + std::to_string(read.equity.day_before) + " (" +
           read.day_before_source + ") t1_settlement=" + std::to_string(read.equity.t1_settlement) +
           " t1_costs=" + std::to_string(read.equity.t1_costs) +
           " priced=" + std::to_string(read.equity.priced) +
           " unpriced=" + std::to_string(read.equity.unpriced);
}

/// The SIZING_CAPITAL_UNSETTLED line (WARN) of a run whose Day T-1 is on a failure path.
inline std::string sizing_capital_unsettled_log_line(const std::string& run_date,
                                                     const LiveSizingRead& read) {
    return "SIZING_CAPITAL_UNSETTLED date=" + run_date + " unsettled=" + read.t1_date +
           " reason=\"" + read.t1_unsettled_reason + "\" capital=" +
           std::to_string(read.capital.capital) +
           ": the sizing capital of the last settled day (" + read.settled_through +
           ") is kept and nothing is added for the unsettled day";
}

/// The mark and flag of a sizing hold: today's live_run_metadata row's risk_refusal (through
/// mark_risk_refusal), the email flag (flag_email_body_for_risk_module_failure names it by its
/// scope) and the exit code (live_run_exit_code: kRiskModuleFailureExitCode). The keys are
/// sleeve_risk_module_failure's, so the watchdog and the email read it as they read a held
/// sleeve; scope "sizing" and module "SIZING_CAPITAL" say what refused.
inline nlohmann::json sizing_hold_refusal(const std::string& failure) {
    nlohmann::json j;
    j["scope"] = "sizing";
    j["action"] = "REFUSE";
    j["module"] = "SIZING_CAPITAL";
    j["error"] = failure;
    j["reason"] = "the account's equity could not be read to size the book (" + failure +
                  "); every strategy was held at its seeded T-1 book";
    j["scope_id"] = "book";
    j["phase"] = "SIZING";
    j["lap"] = 0;
    j["applied"] = "REFUSE";
    return j;
}

}  // namespace trade_ngin
