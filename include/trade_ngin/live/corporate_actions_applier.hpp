#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/types.hpp"

namespace trade_ngin {

/**
 * @brief Type of equity corporate action this applier knows how to adjust.
 *
 * Phase 4 covers ordinary splits, ADR-ratio splits, and ordinary cash
 * dividends. Spinoffs / mergers / ticker changes are out of scope per the
 * Phase 4 plan -- those need basis-cost reallocation logic beyond per-symbol
 * scalar adjustment.
 */
enum class CorpActionType { SPLIT, ADR_SPLIT, DIVIDEND, UNKNOWN };

/**
 * @brief One corporate-action event to apply.
 *
 * `value` carries the event's primary parameter (split factor for SPLIT /
 * ADR_SPLIT, cash $/share for DIVIDEND). `close_t_minus_1` is required for
 * dividends to compute the closeadj ratio change; ignored for splits.
 */
struct CorpActionEvent {
    std::string symbol;
    std::string ex_date;          // YYYY-MM-DD
    CorpActionType type{CorpActionType::UNKNOWN};
    double value{0.0};            // split factor OR dividend $/share
    double close_t_minus_1{0.0};  // required for DIVIDEND only; close at ex_date-1
    // Quantity held at end of business on ex_date - 1 (the eligibility cutoff
    // for cash dividends). Optional: when > 0, the applier records this on
    // PositionAdjustment.quantity_before/after for DIVIDEND events instead of
    // today's live qty, so the audit log's cash-flow figure reflects the
    // ex-date holding even when running a catch-up apply days later
    // (ultrareview bug_021). Has NO effect on the actual position quantity
    // (dividends never change qty), nor on splits.
    double qty_at_ex_date{0.0};
};

/**
 * @brief Auditable record of a position adjustment.
 *
 * Returned from CorporateActionsApplier::apply for logging, state-file
 * persistence (idempotency dedup), and operator audit trails.
 */
struct PositionAdjustment {
    std::string symbol;
    std::string event_date;
    CorpActionType type;
    double quantity_before{0.0};
    double quantity_after{0.0};
    double avg_price_before{0.0};
    double avg_price_after{0.0};
    double event_value{0.0};       // split factor or $/share
    double ratio_change{1.0};      // for dividends: 1 + d/close_t_minus_1
};

/**
 * @brief Pure-logic applier that mutates a position map in place.
 *
 * No DB dependency, no file I/O -- the caller (live equity app) is
 * responsible for sourcing events (PostgresDatabase::get_corporate_actions),
 * passing close[T-1] prices on dividends, and persisting the adjusted
 * positions afterward. Designed for direct unit testing.
 *
 * Adjustment math (audit §1.12, §1.15):
 *   - SPLIT (factor F):      qty *= F; avg_price /= F
 *   - ADR_SPLIT (factor F):  same as SPLIT
 *   - DIVIDEND (d, close_t_minus_1):
 *         ratio = 1 + d / close_t_minus_1
 *         avg_price /= ratio  (keeps avg_price in the post-rescale closeadj frame)
 *
 * Positions with zero quantity are skipped (no adjustment, no record).
 * Events targeting symbols absent from the positions map are skipped.
 * Stacked events on the same symbol apply in the input order.
 */
class CorporateActionsApplier {
public:
    /**
     * @brief Apply events to positions in place; return the audit log.
     *
     * @param positions Map to mutate. Keys are symbols; values are Position.
     * @param events    Events to apply (in order).
     * @return Vector of PositionAdjustment records, one per applied event.
     */
    static std::vector<PositionAdjustment> apply(
        std::unordered_map<std::string, Position>& positions,
        const std::vector<CorpActionEvent>& events);

    // Stringify the type for logs / audit files.
    static const char* type_to_string(CorpActionType t);

    // Parse the action text from equities_data.corporate_action into the enum.
    static CorpActionType type_from_action_string(const std::string& action);
};

}  // namespace trade_ngin
