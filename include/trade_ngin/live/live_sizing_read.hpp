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
#include <functional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "trade_ngin/core/error.hpp"
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

/// The sizing reads' outcome and, when sized, the figure and where its day_before came from.
struct LiveSizingRead {
    LiveSizingOutcome outcome{LiveSizingOutcome::kSized};
    LiveSizingEquity equity;        ///< kSized only
    std::string day_before_source;  ///< kSized only, for the SIZING_CAPITAL log line
    std::string failure;            ///< kHoldBook / kRefuseRun: the read(s) that failed, with the error
};

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
    const std::unordered_set<std::string>& zero_settlement_symbols) {
    LiveSizingRead out;
    std::string equity_failure;
    const auto sizing_t1 = now - std::chrono::hours(24);
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
    return out;
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
