#pragma once

#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <memory>

#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/live/corporate_actions_applier.hpp"

namespace trade_ngin {

/**
 * @brief On-disk dedup record for corp-action events applied to a strategy.
 *
 * Persists to `<state_dir>/applied_corp_actions.json` per strategy. Each
 * entry pins one (symbol, ex_date, action) tuple as "already applied" so
 * the daily live app can re-run safely without double-adjusting positions.
 *
 * No DB dependency by design -- the Phase 4 scope constraint forbids new
 * schema. The audit's original dividend_ledger DB table is the natural
 * future home; this state file is a stand-in until that constraint lifts.
 *
 * File format:
 * {
 *   "applied": [
 *     {"symbol": "AAPL", "ex_date": "2024-08-12", "action": "DIVIDEND"},
 *     {"symbol": "NVDA", "ex_date": "2024-06-10", "action": "SPLIT"}
 *   ],
 *   "dividend_events": [
 *     {"symbol": "AAPL", "ex_date": "2024-08-12",
 *      "qty_held": 100.0, "dividend_per_share": 0.25, "total_cash": 25.0}
 *   ]
 * }
 *
 * Concurrency: single-process semantics. The live equity app runs as a
 * cron-triggered batch; concurrent invocations on the same state_dir are
 * not supported (would race on save).
 */
class CorporateActionsAuditLog {
public:
    explicit CorporateActionsAuditLog(std::string state_dir);

    /**
     * @brief DB-backed dedup, with one-time import of any legacy state file.
     *
     * Preferred construction for the live app. The JSON state file lived under
     * a container path with no volume declared, so losing it on redeploy was
     * the default -- and a dedup record that evaporates makes a lookback window
     * wide enough to cover a real outage unsafe. Backing the record with
     * trading.corp_action_applied is what allows the window to be derived from
     * actual state (see migration 002).
     *
     * state_dir is still supplied so an existing applied_corp_actions.json can
     * be imported once; the file is left in place afterwards, not deleted.
     */
    CorporateActionsAuditLog(std::string state_dir,
                             std::shared_ptr<PostgresDatabase> db,
                             std::string portfolio_id,
                             std::string strategy_id,
                             std::string strategy_name);

    /**
     * @brief Load existing dedup records.
     *
     * Three outcomes, deliberately distinguishable -- collapsing them into one
     * bool is what made a transient read failure look identical to a genuine
     * first run, and an empty applied-set re-applies every event in the window
     * (splits re-multiply quantity, dividends re-rescale basis). Since the
     * window now reaches back to position inception rather than 14 days, that
     * blast radius can span years.
     *
     * @return error  -- the dedup record could NOT be read. The caller MUST NOT
     *                   apply corporate actions; skipping a day is recoverable
     *                   on the next run (the window covers it), double-applying
     *                   is not.
     * @return true   -- records loaded; in-memory state populated.
     * @return false  -- read succeeded and found nothing: genuine first run.
     *                   Safe to proceed.
     */
    Result<bool> load();

    /**
     * @brief Has this (symbol, ex_date, action) tuple already been applied?
     */
    bool is_applied(const std::string& symbol,
                    const std::string& ex_date,
                    CorpActionType action) const;

    /**
     * @brief Record an adjustment as applied (idempotency dedup).
     *        Also captures dividend cash-flow detail when type is DIVIDEND.
     */
    void record(const PositionAdjustment& adjustment);

    /**
     * @brief Write current state to disk. Creates parent directory if needed.
     * @return true on successful write, false on filesystem error.
     */
    bool save() const;

    /**
     * @brief Sum of total_cash across all recorded dividend events.
     *
     * Cumulative across the lifetime of the state file (no date filter --
     * the file is per-strategy and grows monotonically, so "all events" IS
     * "cumulative for this strategy"). Returns 0 if no dividends recorded.
     *
     * Used by the live equity app to populate
     * trading.live_results.total_dividend_income at daily finalization
     * (Phase 4.5). Splits do not contribute (only DIVIDEND events are
     * captured in dividend_events_).
     *
     * Informational ONLY: the equity price series is total-return adjusted,
     * so dividend value is ALREADY in mark-to-market P&L via price
     * continuity. Post Phase 4.2 that adjustment is computed in-engine from
     * per-bar div_cash/split_factor rather than read from the vendor's
     * closeadj column, but the accounting consequence is unchanged: do NOT
     * add this value to P&L totals -- it would double-count.
     */
    double total_cumulative_dividend_income() const;

    /** Test helper: clear in-memory state (does not touch disk). */
    void clear_in_memory() {
        applied_.clear();
        dividend_events_.clear();
    }

    const std::string& state_dir() const { return state_dir_; }

private:
    using AppliedKey = std::tuple<std::string, std::string, CorpActionType>;
    struct DividendEvent {
        std::string symbol;
        std::string ex_date;
        double qty_held{0.0};
        double dividend_per_share{0.0};
        double total_cash{0.0};
    };

    std::string state_dir_;
    std::set<AppliedKey> applied_;
    std::vector<DividendEvent> dividend_events_;

    // Set only for the DB-backed construction. When null the class behaves
    // exactly as before (file-only), which is what the unit tests exercise.
    std::shared_ptr<PostgresDatabase> db_;
    std::string portfolio_id_;
    std::string strategy_id_;
    std::string strategy_name_;
    // Rows recorded since the last save(), so save() writes a delta rather
    // than re-inserting the whole lifetime set on every run.
    mutable std::vector<PostgresDatabase::AppliedCorpActionRow> pending_;

    std::string file_path() const;

    /// True when constructed with a database handle.
    bool db_backed() const { return db_ != nullptr; }

    /// Import a legacy state file into the DB when the DB has no rows yet.
    bool migrate_state_file_to_db();
};

}  // namespace trade_ngin
