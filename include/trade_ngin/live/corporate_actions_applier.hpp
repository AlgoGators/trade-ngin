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
// E2-F13: TERMINATION is a class-3 lifecycle event (delisting / acquisition), not a
// price-restating class-1 event like SPLIT or DIVIDEND. It is in this enum solely so a
// termination can be written to trading.corp_action_applied and deduped like any other
// applied event -- before this, terminations were recorded NOWHERE and left no audit trail.
// The applier itself never produces one; CorporateActionsLifecycle does.
enum class CorpActionType { SPLIT, ADR_SPLIT, DIVIDEND, TERMINATION, UNKNOWN };

/**
 * @brief One corporate-action event to apply.
 *
 * `value` carries the event's primary parameter (split factor for SPLIT /
 * ADR_SPLIT, cash $/share for DIVIDEND). `close_at_ex_date` is required for
 * dividends to compute the price-adjustment ratio change; ignored for splits.
 */
struct CorpActionEvent {
    std::string symbol;
    std::string ex_date;          // YYYY-MM-DD
    CorpActionType type{CorpActionType::UNKNOWN};
    double value{0.0};            // split factor OR dividend $/share
    // Required for DIVIDEND only. The close ON the ex-date, which is the
    // denominator build_equity_adjusted_query uses (it scales pre-dividend bars
    // by close_D / (close_D + div_D)). Using close[ex_date - 1] here instead
    // puts cost basis in a slightly different frame than the marks, drifting on
    // every dividend. See test_corp_actions_frame_consistency.
    //
    // It must be a RAW close, not an adjusted one: the applier's per-event
    // rescale has to equal compute_backward_adjustment_factors' per-event step,
    // and that works in raw closes. An adjusted close carries every LATER event
    // in the window, so under stacked events the two frames diverge.
    double close_at_ex_date{0.0};
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
    double ratio_change{1.0};      // for dividends: 1 + d/close_at_ex_date
};

/**
 * @brief Pure-logic applier that mutates a position map in place.
 *
 * No DB dependency, no file I/O -- the caller (live equity app) is
 * responsible for sourcing events (PostgresDatabase::get_corporate_actions),
 * passing the raw ex-date close on dividends, and persisting the adjusted
 * positions afterward. Designed for direct unit testing.
 *
 * Adjustment math (audit §1.12, §1.15):
 *   - SPLIT (factor F):      qty *= F; avg_price /= F
 *   - ADR_SPLIT (factor F):  same as SPLIT
 *   - DIVIDEND (d, close_at_ex_date):
 *         ratio = 1 + d / close_at_ex_date
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

    /**
     * @brief Inverse of type_to_string() -- parses the ENUM-NAME form.
     *
     * Distinct from type_from_action_string(), which parses the VENDOR form
     * ("dividend", "split"). The two are not interchangeable: round-tripping a
     * stored type through the vendor parser silently yields UNKNOWN, which
     * would make a persisted dedup record fail to match and re-apply the event.
     */
    static CorpActionType type_from_type_string(const std::string& type_name);
};

}  // namespace trade_ngin
