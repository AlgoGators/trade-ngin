// include/trade_ngin/live/corp_action_feed_status.hpp
#pragma once

#include <string>

#include "trade_ngin/live/corporate_actions_classification.hpp"

namespace trade_ngin {

/**
 * @brief Whether the equities_data.corporate_action deal-terms feed is still frozen.
 *
 * E4 item 3. The termination handler's WARN text named a COMPILED-IN date
 * (`kCorpActionTableFrozenAfter`, "2025-08-29") as the point the feed stopped.
 * That is a fact about the database, not about the code, and the code cannot
 * know it: the day the vendor subscription is restarted and the ACTIONS ingest
 * backfills, every one of those WARNs starts asserting something false, and it
 * takes a rebuild to stop it. Worse, the operator reading the log has no signal
 * that the revival happened at all -- the message is identical before and after.
 *
 * So the date is MEASURED at startup (`SELECT max(date) FROM
 * equities_data.corporate_action`) and reported. The constant survives only as
 * the fallback for the case where the measurement itself failed, and as the
 * reference point that tells us the feed moved.
 *
 * Note this is the DEAL-TERMS feed (class-3 acquisitions, mergers, delistings,
 * and the class-2 renames), not the price feed. Bar freshness is a different
 * question with a different answer -- see live/data_freshness.hpp.
 */
struct CorpActionFeedStatus {
    /// True when the last-row date came from the database rather than the constant.
    bool measured{false};
    /// Last date present in equities_data.corporate_action, YYYY-MM-DD.
    std::string last_row_date;
    /// True when `last_row_date` is strictly before the run's as-of date.
    bool frozen{false};
    /// True when the measured date is later than the compiled-in constant: the
    /// feed has been revived since this binary was built, and the paths that
    /// were dormant (deal-terms rollover) are now live.
    bool revived_since_build{false};
};

/**
 * @brief Build the status from a measured max date.
 *
 * @param measured_max_date `max(date)` from equities_data.corporate_action, or
 *        "" when the query failed or the table is empty. An empty value falls
 *        back to the compiled-in constant with `measured = false`, so the log
 *        line and the WARN text degrade to the old behaviour rather than to a
 *        blank.
 * @param as_of_ymd the run's as-of date, YYYY-MM-DD.
 *
 * Future-dated placeholder rows are the reason `frozen` compares against the
 * as-of date rather than "is there anything recent": equities_data.corporate_action
 * carries tickerchange rows dated 2027-07-18, so a naive max() reports a feed
 * that is ahead of the run. `frozen` answers "did the feed stop before this run",
 * which is what the WARN needs to say, and a max beyond the as-of date makes it
 * false -- correctly, because such a feed has not stopped.
 */
inline CorpActionFeedStatus assess_corp_action_feed(const std::string& measured_max_date,
                                                    const std::string& as_of_ymd) {
    CorpActionFeedStatus s;
    if (measured_max_date.empty()) {
        s.measured = false;
        s.last_row_date = kCorpActionTableFrozenAfter;
    } else {
        s.measured = true;
        s.last_row_date = measured_max_date;
        s.revived_since_build = measured_max_date > std::string(kCorpActionTableFrozenAfter);
    }
    s.frozen = !as_of_ymd.empty() && s.last_row_date < as_of_ymd;
    return s;
}

/// One log line, the same shape whether measured or fallen back, so a grep over
/// two runs shows the feed moving.
inline std::string describe_corp_action_feed(const CorpActionFeedStatus& s) {
    std::string out = "deal-terms feed last row " + s.last_row_date + ", frozen: " +
                      (s.frozen ? "yes" : "no");
    if (!s.measured) {
        out += " (NOT MEASURED -- could not read equities_data.corporate_action, "
               "falling back to the compiled-in date " +
               std::string(kCorpActionTableFrozenAfter) + ")";
    } else if (s.revived_since_build) {
        out += " (REVIVED since this binary was built, which recorded " +
               std::string(kCorpActionTableFrozenAfter) +
               " -- the deal-terms and rename paths that were dormant are now live)";
    }
    return out;
}

}  // namespace trade_ngin
