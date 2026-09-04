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
 * @brief Which side of a class-3 event the row's own `ticker` sits on.
 *
 * The nine TERMINATION labels do NOT all describe the death of the ticker the
 * row is keyed on. Three of them are the counterparty's view of somebody
 * else's termination, keyed on the name that SURVIVES:
 *
 *   COF  acquisitionof  DFS   -- COF is the acquirer; DFS is what died.
 *   XYZ  mergerfrom     ABC   -- XYZ is the surviving merger party.
 *   RAL  spunofffrom    FTV   -- RAL is the spinoff CHILD, newly listed.
 *
 * Feeding those to a query keyed on `ticker` and turning each row into a
 * TerminationEvent closes out a holding in a company that is alive and still
 * printing bars (E2-F26): 1,611 acquisitionof, 78 spunofffrom and 13
 * mergerfrom rows in equities_data.corporate_action sit on tickers that kept
 * printing 30+ days after the row. Unreachable for the ten configured symbols,
 * reachable on any universe-scale replay, and ARMED by the deal-terms feed
 * revival that E4 asks for -- so the split lands before the ask is sent.
 *
 * This is deliberately a SECOND axis, not a narrowing of CorpActionClass.
 * `spunofffrom` is still a TERMINATION: it describes a termination event, its
 * handler is still the lifecycle, and `classify_action` must keep saying so.
 * What changes is only which ticker the row's terms apply TO.
 */
enum class TerminationKeying {
    ROW_TICKER_TERMINATES,  ///< the row's `ticker` is the name that dies
    COUNTERPARTY_ROW        ///< the row's `ticker` SURVIVES; `contraticker` died
};

/**
 * @brief Keying of one class-3 vendor label.
 *
 * Only meaningful for labels that `classify_action` maps to TERMINATION; any
 * other label reports ROW_TICKER_TERMINATES, which is why every caller must
 * check the class first (`terms_row_terminates_its_ticker` does).
 */
TerminationKeying termination_keying(const std::string& vendor_label);

/**
 * @brief Class-3 labels with a given keying, sorted, for the SQL IN-lists.
 *
 * The two lists PARTITION `vendor_labels_for_class(TERMINATION)`; a query that
 * wants only the rows describing the death of their own ticker asks for
 * ROW_TICKER_TERMINATES.
 */
const std::vector<std::string>& vendor_labels_for_termination_keying(TerminationKeying k);

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
