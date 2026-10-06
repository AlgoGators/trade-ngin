// include/trade_ngin/live/trading_days_anchor.hpp
#pragma once

#include <string>

#include "trade_ngin/live/data_freshness.hpp"

namespace trade_ngin {

/**
 * @brief Is the annualization anchor consistent with the book it annualizes?
 *
 * E2-F32. `trading.get_trading_days(strategy, target, portfolio)` takes its
 * start date from `trading.strategy_trading_days_metadata.live_start_date` and
 * falls back to `MIN(date)` over that portfolio's `live_results` only when no
 * row exists. A row that is LATER than the book's own first day is therefore
 * strictly worse than no row at all, and nothing anywhere notices:
 *
 *   EQUITY_MR_PORTFOLIO carries live_start_date 2026-07-24 (seeded by hand for a
 *   10-day window) while the replayed book starts 2026-04-01. Every date before
 *   07-24 gets `GREATEST(1, target - start + 1)` = 1 trading day, so
 *   total_annualized_return collapses onto total_cumulative_return for 115 rows;
 *   at 07-27 the count is 4 and a -3.5 % cumulative return annualizes to
 *   -1674 %. The run exits 0 and the number is stored.
 *
 * The check is one comparison the DB function cannot make, because the function
 * stops looking the moment it finds a metadata row. `effective_anchor` is the
 * anchor that should have been used: the earlier of the two, which is the metadata
 * row when it is sound and the book's own first day when it is not.
 *
 * This is NOT equity-specific. Futures carries the same shape -- as of 2026-09-03
 * LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST / BASE_PORTFOLIO is anchored at
 * 2025-11-11 against live_results from 2025-01-29 -- so the check belongs beside
 * the function, not inside one runner. Wiring the futures runners is a separate
 * change with its own regression, deliberately not made here.
 */
struct TradingDaysAnchor {
    /// True when strategy_trading_days_metadata has a row for this (strategy, portfolio).
    bool has_metadata{false};
    /// live_start_date from that row, YYYY-MM-DD. Empty when there is no row.
    std::string metadata_anchor;
    /// MIN(date) over this portfolio's live_results, YYYY-MM-DD. Empty when the book is new.
    std::string earliest_result;
    /// The anchor annualization should use.
    std::string effective_anchor;
    /// True when the metadata row is LATER than the book's own first day -- the defect.
    bool anchor_is_late{false};
};

/**
 * @param metadata_anchor live_start_date, or "" when no metadata row exists.
 * @param earliest_result MIN(date) over live_results for this (strategy, portfolio),
 *        or "" when the book has no rows yet (a genuine first run).
 *
 * Both are ISO YYYY-MM-DD and compare lexicographically.
 *
 * A metadata row EARLIER than the first result is not flagged: that is the
 * normal shape for a book seeded before its first run, and it annualizes
 * conservatively (more days, smaller figure) rather than explosively.
 */
inline TradingDaysAnchor assess_trading_days_anchor(const std::string& metadata_anchor,
                                                    const std::string& earliest_result) {
    TradingDaysAnchor a;
    a.has_metadata = !metadata_anchor.empty();
    a.metadata_anchor = metadata_anchor;
    a.earliest_result = earliest_result;

    if (!a.has_metadata) {
        // No row: the DB function already falls back to MIN(date), which is right.
        a.effective_anchor = earliest_result;
        return a;
    }
    if (earliest_result.empty()) {
        // Nothing to contradict the row with. A first run has no history, and a
        // seeded anchor is exactly what it is for.
        a.effective_anchor = metadata_anchor;
        return a;
    }

    a.anchor_is_late = metadata_anchor > earliest_result;
    a.effective_anchor = a.anchor_is_late ? earliest_result : metadata_anchor;
    return a;
}

/**
 * @brief Trading days from an anchor to a target date, matching the SQL exactly.
 *
 * `trading.get_trading_days` computes `GREATEST(1, (target - start) + 1)` in
 * whole days. This reproduces it so an override computed here is the same
 * quantity the function would have returned from the same anchor -- not a
 * differently-defined number that happens to be near it.
 *
 * Returns 1 for an empty or malformed anchor, which is the function's own
 * "I do not know" answer.
 */
inline int trading_days_from_anchor(const std::string& anchor_ymd,
                                    const std::string& target_ymd) {
    if (anchor_ymd.empty() || target_ymd.empty()) return 1;
    const long days = calendar_days_between_utc(anchor_ymd, target_ymd);
    const long count = days + 1;
    return count < 1 ? 1 : static_cast<int>(count);
}

}  // namespace trade_ngin
