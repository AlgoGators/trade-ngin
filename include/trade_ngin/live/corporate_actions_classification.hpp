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
 * @brief FALLBACK ONLY: the date equities_data.corporate_action was last seen
 *        receiving events, as of the commit that wrote this line.
 *
 * NOT the answer to "is the feed frozen" -- that is a fact about the database,
 * and the runner MEASURES it (`PostgresDatabase::get_corp_action_feed_last_date`,
 * `assess_corp_action_feed` in live/corp_action_feed_status.hpp) and logs it at
 * startup. A compiled-in date cannot notice a restarted subscription: after a
 * backfill every WARN quoting it asserts something false until somebody
 * rebuilds, and nothing tells the operator the dormant deal-terms path just went
 * live.
 *
 * This constant survives for two jobs only: the fallback when the measurement
 * itself failed, and the reference point that lets the startup line say the feed
 * has MOVED since the build. Do not read it as current fact.
 *
 * The TERMINATION deal-terms path queries the table regardless (finding nothing
 * today) so a revived feed activates the handler with no code change.
 */
inline constexpr const char* kCorpActionTableFrozenAfter = "2025-08-29";

}  // namespace trade_ngin
