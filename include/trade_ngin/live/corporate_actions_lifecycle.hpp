#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/live/corporate_actions_classification.hpp"

namespace trade_ngin {

/**
 * @brief One historical-ticker -> current-symbol mapping (class 2).
 *
 * Sourced from equities_data.ticker_aliases. `effective_until` is the last
 * date on which the historical ticker was in use; bars on or before it belong
 * to the old symbol, bars after it to the new one.
 */
struct TickerAlias {
    std::string historical_ticker;
    std::string current_symbol;
    std::string effective_until;  // YYYY-MM-DD; empty when open-ended
    std::string note;
};

/**
 * @brief Class-3 event: the series ends, or the holding becomes something else.
 *
 * `contra_ticker` / `ratio` carry the deal terms when they are available. They
 * come only from equities_data.corporate_action, which stopped receiving
 * events on 2025-08-29 -- so for anything after that date `has_terms` is false
 * and the handler takes the documented final-close exit instead. When the feed
 * revives, the same query starts returning rows and the terms path activates
 * with no code change.
 */
struct TerminationEvent {
    std::string symbol;
    std::string event_date;    // YYYY-MM-DD (ex/effective date)
    std::string vendor_label;  // e.g. "mergerto", "delisted"
    std::string contra_ticker; // acquirer / successor symbol; empty when unknown
    double ratio{0.0};         // shares of contra per share held; 0 when unknown
    bool has_terms{false};     // true only when contra_ticker and ratio are usable
};

/**
 * @brief What the lifecycle handler did to one position.
 */
enum class LifecycleOutcome {
    EXITED_AT_FINAL_CLOSE,  ///< terms unavailable -> closed at the last traded price
    CONVERTED_TO_CONTRA,    ///< terms available -> rolled into the successor symbol
    RENAMED,                ///< class 2: position re-keyed to the current symbol
    SKIPPED_NO_POSITION,    ///< event for a symbol we do not hold
    SKIPPED_NO_PRICE        ///< exit required but no final close available
};

/**
 * @brief Auditable record of a lifecycle adjustment.
 */
struct LifecycleAdjustment {
    std::string symbol;
    std::string event_date;
    std::string vendor_label;
    CorpActionClass action_class{CorpActionClass::TERMINATION};
    LifecycleOutcome outcome{LifecycleOutcome::SKIPPED_NO_POSITION};
    double quantity_before{0.0};
    double quantity_after{0.0};   ///< 0 on exit; converted share count on rollover
    double exit_price{0.0};       ///< final close used for the exit (0 if none)
    double realized_delta{0.0};   ///< cash P&L booked by an exit
    std::string contra_ticker;    ///< populated on CONVERTED_TO_CONTRA
};

/**
 * @brief Handlers for the two classes that change WHAT you hold, not its price.
 *
 * Pure logic, no DB or file I/O: the caller sources aliases, termination
 * events and final closes, then hands them here. Mirrors the design of
 * CorporateActionsApplier (class 1) so both are directly unit-testable.
 */
class CorporateActionsLifecycle {
public:
    /**
     * @brief Class 2 -- re-key positions held under a superseded ticker.
     *
     * A position still keyed by a historical ticker is moved to the current
     * symbol so its history stitches across the rename. Quantity and cost
     * basis are untouched (a rename is not an economic event). If both the
     * old and the new key are present, the quantities are summed and the old
     * key removed -- the two rows are the same holding.
     *
     * Aliases are a curated subset, not the full rename history: an unmapped
     * historical ticker simply stays as-is, which is the pre-existing
     * behaviour and never loses a position.
     */
    static std::vector<LifecycleAdjustment> apply_renames(
        std::unordered_map<std::string, Position>& positions,
        const std::vector<TickerAlias>& aliases,
        const std::string& as_of_date);

    /**
     * @brief Class 3 -- terminate or transform holdings.
     *
     * With terms (`has_terms`): the position converts into `ratio` shares of
     * `contra_ticker`, carrying cost basis across so the roll is P&L-neutral
     * at the moment of conversion.
     *
     * Without terms: the position is closed at `final_closes[symbol]`, the
     * realized P&L booked, and a WARN emitted naming the event and the data
     * gap. This is the documented fallback and is exactly right for cash
     * deals; for stock-for-stock it approximates the post-event path.
     *
     * @param final_closes Last traded price per symbol; required for exits.
     */
    static std::vector<LifecycleAdjustment> apply_terminations(
        std::unordered_map<std::string, Position>& positions,
        const std::vector<TerminationEvent>& events,
        const std::unordered_map<std::string, double>& final_closes);

    static const char* outcome_to_string(LifecycleOutcome o);
};

}  // namespace trade_ngin
