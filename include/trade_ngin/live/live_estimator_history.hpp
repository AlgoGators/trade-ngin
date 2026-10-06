// include/trade_ngin/live/live_estimator_history.hpp
#pragma once

#include <chrono>
#include <vector>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/session_classifier.hpp"
#include "trade_ngin/strategy/trend_estimator.hpp"

namespace trade_ngin {

/**
 * @brief The live runners' history before their bar window, for the trend sleeves' estimators.
 *
 * A live run feeds its strategies the consumed bars of its window (the configured historical days
 * before the run date). The estimators' window is W consumed bars, which reaches back further. The
 * runner therefore also loads the bars from `estimator_history_start` up to the window, and hands
 * the sleeves the CONSUMED ones (K-01: a withheld bar is never consumed, in any engine) before it
 * feeds the window. Those bars reach the sleeves' own histories only: the PortfolioManager's
 * price history, the cost models, the risk reader and the T-1 classification stay on the window.
 *
 * The verdicts of the history's bars come from a classifier of their own, fed the history's bars
 * and their vendor ids in date order: each bar is judged against the bars before it, as every
 * engine judges a bar. The window's classifier is not this one and is not changed by it.
 */

/// The first instant of the history loaded before a window starting at `window_start`.
inline Timestamp estimator_history_start(const Timestamp& window_start) {
    return window_start - std::chrono::hours(24 * trend_estimator::kHistoryCalendarDays);
}

/// The consumed bars of the history: `loaded` restricted to the bars stored from the history start
/// and strictly before the window, judged by their own classifier (with `ids` when the read
/// succeeded; without them the instrument-id limb judges no bar), the withheld ones left out.
///
/// The window's own query selects on the STORED time of a bar (midnight UTC of its date), and a
/// loaded bar's timestamp is not that instant (the loader's conversion moves it by some hours
/// inside the same UTC date). The split therefore reads the bar's date at midnight UTC, the
/// instant the window's query compared: a bar is history exactly when the window did not load it.
inline std::vector<Bar> estimator_history_consumed(
    const std::vector<Bar>& loaded, const Timestamp& window_start,
    const Result<std::vector<market_data_utils::FuturesInstrumentId>>& ids,
    std::vector<SymbolDayVerdict>* withheld = nullptr) {
    const Timestamp from = estimator_history_start(window_start);
    std::vector<Bar> history;
    for (const auto& bar : loaded) {
        const Timestamp stored = SessionClassifier::day_of(bar.timestamp);
        if (stored >= from && stored < window_start) history.push_back(bar);
    }
    SessionClassifier classifier;
    classifier.add_bars(history);
    (void)feed_instrument_ids(classifier, ids);
    return k01_consumed_bars(classifier, history, withheld);
}

}  // namespace trade_ngin
