#pragma once

#include <string>
#include <vector>

namespace trade_ngin {

/**
 * @brief Mechanical effect a corporate action has on a held position.
 *
 * The vendor emits 19 distinct action labels, but only four things can
 * actually happen to a position, and each class has a different data source
 * and a different handler. Classifying by effect -- rather than switching on
 * 19 labels -- keeps the handlers honest about what data they need and makes
 * the coverage gaps explicit (see docs/CORP_ACTIONS_DATA_BOUNDARY.md).
 *
 *   PRICE_RESTATING   The price series is rescaled; share count and/or cost
 *                     basis move. split, adrratiosplit, spinoff,
 *                     spinoffdividend, dividend.
 *                     SOURCE: per-bar split_factor / div_cash on
 *                     equities_data.ohlcv_1d -- alive and current.
 *
 *   SERIES_CONTINUITY Same company, new symbol; history must stitch across
 *                     the rename. tickerchangefrom, tickerchangeto.
 *                     SOURCE: equities_data.ticker_aliases -- partial
 *                     (curated subset, not the full rename history).
 *
 *   TERMINATION       The series ends or the holding transforms into
 *                     something else. mergerfrom/to, acquisitionby/of,
 *                     delisted, voluntarydelisting, regulatorydelisting,
 *                     bankruptcyliquidation, spunofffrom.
 *                     SOURCE: exit TIMING from ohlcv_1d.delisting_date
 *                     (current); deal TERMS (contraticker + ratio) only from
 *                     equities_data.corporate_action, which is frozen.
 *
 *   INFORMATIONAL     No effect on a position. listed, relation, initiated.
 *                     No handler.
 */
enum class CorpActionClass {
    PRICE_RESTATING,
    SERIES_CONTINUITY,
    TERMINATION,
    INFORMATIONAL,
    UNRECOGNIZED
};

/**
 * @brief Map a vendor action label to its mechanical effect class.
 *
 * Comparison is case-sensitive against the vendor's lowercase labels.
 * Unknown labels return UNRECOGNIZED so a new vendor label surfaces as a
 * warning rather than being silently treated as harmless.
 */
CorpActionClass classify_action(const std::string& vendor_label);

/** Stringify for logs and audit records. */
const char* corp_action_class_to_string(CorpActionClass c);

/**
 * @brief Vendor labels belonging to a class.
 *
 * Used to build the SQL action-filter for the class-specific queries, so the
 * label list lives in exactly one place.
 */
const std::vector<std::string>& vendor_labels_for_class(CorpActionClass c);

/**
 * @brief Last date on which equities_data.corporate_action received events.
 *
 * The feed stopped here; anything after this date is not in that table. The
 * TERMINATION deal-terms path queries it anyway (finding nothing today) so a
 * revived feed activates the handler with no code change.
 */
inline constexpr const char* kCorpActionTableFrozenAfter = "2025-08-29";

}  // namespace trade_ngin
