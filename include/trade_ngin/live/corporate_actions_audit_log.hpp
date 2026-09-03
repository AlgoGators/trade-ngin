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
 * Each entry pins one (symbol, ex_date, action) tuple as "already applied" so
 * the daily live app can re-run safely without double-adjusting positions.
 *
 * TWO BACKINGS, AND ONLY ONE IS OPERATIONAL.
 *
 * The DB-backed constructor is what the live app uses, and the only one safe
 * for deciding whether an event has been applied. trading.corp_action_applied
 * (migration 002) survives the redeploys that used to wipe the state file, and
 * it is the only backing that can bridge ticker renames: that bridge needs
 * equities_data.ticker_aliases, which requires a database handle. A file-backed
 * log therefore cannot tell that an event applied under AA is the same event now
 * resurfacing under HWM, and would apply it a second time.
 *
 * The file backing survives for exactly two uses: importing a pre-existing
 * applied_corp_actions.json once (migrate_state_file_to_db), and tests that
 * exercise file semantics directly. bridges_renames() reports which backing is
 * in force, and the live runner refuses to adjust positions without it.
 *
 * (Superseded note, kept for context: this class was originally file-only
 * because the Phase 4 scope constraint forbade new schema. Migration 002 lifted
 * that, and the DB table the audit originally asked for now exists.)
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
    /**
     * @brief The latest ex-date recorded as applied, "" when none.
     *
     * E2-F23: a row whose ex-date is on or after the run date cannot have been
     * written by an earlier session of a forward-only book -- it came from a later
     * pass (a replay, or a same-day re-run). A run that sees one must refuse rather
     * than skip the event and finalize the T-1 book in the wrong frame.
     */
    std::string latest_applied_ex_date() const {
        std::string latest;
        for (const auto& key : applied_) {
            const std::string& ex = std::get<1>(key);
            if (ex > latest) latest = ex;
        }
        return latest;
    }

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
     * @brief Persist the dedup record inside a caller-owned unit of work.
     *
     * The DB-backed counterpart to save(), for the case where the adjusted
     * positions and the dedup rows that protect them must commit together.
     * Does not commit -- the caller owns that. Errors if this log is
     * file-backed, since a filesystem write cannot join a DB transaction.
     */
    Result<void> save_in(DbTransaction& txn) const;

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

    /**
     * @brief Dividend detail as the legacy state file carried it.
     *
     * Public because `dividend_detail_for` below takes and returns it, and that
     * rule has to be testable without a database (E2-F39 / BA-15). Note there is
     * no action_type here: every entry in this list IS a dividend, which is
     * precisely why the import needs the type passed in alongside.
     */
    struct DividendEvent {
        std::string symbol;
        std::string ex_date;
        double qty_held{0.0};
        double dividend_per_share{0.0};
        double total_cash{0.0};
    };

    /**
     * @brief Which legacy dividend detail belongs to an applied event, if any.
     *
     * E2-F39 / BA-15. The legacy state file records applied events keyed on
     * (symbol, ex_date, action_type) but carries dividend detail keyed only on
     * (symbol, ex_date) -- DividendEvent has no action_type, because every entry
     * in it IS a dividend. The import matched on (symbol, ex_date) alone, so when
     * a symbol had a SPLIT and a DIVIDEND on the SAME ex-date, the SPLIT row also
     * inherited the dividend's qty_held, dividend_per_share and total_cash. The
     * cash then appears twice in cumulative dividend income: once on the dividend
     * row, once on the split row that never paid anything.
     *
     * Returns nullptr when the event is not a dividend, or when no detail matches.
     *
     * Static and public so the rule is testable without a database: the import it
     * serves runs inside migrate_state_file_to_db(), which requires a live
     * connection and a file on disk.
     */
    static const DividendEvent* dividend_detail_for(
        const std::string& symbol, const std::string& ex_date, CorpActionType type,
        const std::vector<DividendEvent>& events) {
        // A split, an ADR split or a termination pays no dividend. Only a
        // DIVIDEND event may carry dividend detail.
        if (type != CorpActionType::DIVIDEND) return nullptr;
        for (const auto& de : events) {
            if (de.symbol == symbol && de.ex_date == ex_date) return &de;
        }
        return nullptr;
    }

private:
    using AppliedKey = std::tuple<std::string, std::string, CorpActionType>;

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

public:
    /**
     * @brief Whether this log can recognise an event across a ticker rename.
     *
     * Only the DB backing can: the bridge reads equities_data.ticker_aliases.
     * A file-backed log answers is_applied() on the pre-rename symbol alone, so
     * an event already applied under the old ticker is applied again under the
     * new one -- quantity re-multiplied, cost basis re-rescaled, permanently.
     * The live runner checks this before adjusting any position.
     */
    bool bridges_renames() const { return db_backed(); }

private:

    /// Import a legacy state file into the DB when the DB has no rows yet.
    bool migrate_state_file_to_db();
};

}  // namespace trade_ngin
