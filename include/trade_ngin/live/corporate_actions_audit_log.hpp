#pragma once

#include <set>
#include <string>
#include <tuple>
#include <vector>

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
     * @brief Load existing dedup records from disk.
     * @return true if the file existed and parsed; false if first-run.
     *         Either way, the in-memory state is initialized.
     */
    bool load();

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
     * Informational ONLY: closeadj already captures dividend total-return
     * via price continuity (Phase 4 avg_price frame-alignment fix). Do NOT
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

    std::string file_path() const;
};

}  // namespace trade_ngin
