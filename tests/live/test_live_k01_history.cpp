// tests/live/test_live_k01_history.cpp
//
// N-2 (T-ROLLX-FIX commit 4; LOOP_SPEC v6.2 section 2.1): a bar's K-01 verdict reads the classifier's
// trailing history, the same on every run. The live runner loads the bars of a fixed prefix before
// its window (kK01ClassifierHistoryDays) into the SessionClassifier only; the consumed set is still
// taken over the window. Composed here as the runner composes it: a run dated R loads the window
// [R - 730 days, R) and the prefix before it, feeds the classifier the prefix (k01_classifier_history)
// and the window, and takes k01_consumed_bars over the window. The shapes are the second
// adversary's N2a and N2b (lead2/evidence/T-ROLLX-FIX_ADV2/fixtures).

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/data/session_classifier.hpp"
#include "trade_ngin/live/live_roll_legs.hpp"

using namespace trade_ngin;

namespace {

const std::string kSym = "XA.v.0";
constexpr int kWindowDays = 730;

Timestamp at(const std::string& ymd) {
    std::tm tm{};
    tm.tm_year = std::stoi(ymd.substr(0, 4)) - 1900;
    tm.tm_mon = std::stoi(ymd.substr(5, 2)) - 1;
    tm.tm_mday = std::stoi(ymd.substr(8, 2));
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}
std::string plus(const std::string& ymd, int n) {
    return core::format_utc_date(at(ymd) + std::chrono::hours(24 * n));
}
bool weekday(const std::string& ymd) {
    const std::time_t t = std::chrono::system_clock::to_time_t(at(ymd));
    std::tm tm{};
    gmtime_r(&t, &tm);
    return tm.tm_wday != 0 && tm.tm_wday != 6;
}

// XA on every weekday from `first` to `last` at `volume` lots, `odd_date` printing `odd_volume`.
std::vector<Bar> series(const std::string& first, const std::string& last, double volume,
                        const std::string& odd_date, double odd_volume) {
    std::vector<Bar> out;
    int i = 0;
    for (std::string d = first; d <= last; d = plus(d, 1), ++i) {
        if (!weekday(d)) continue;
        const double close = 100.0 + 0.1 * i + 0.7 * std::sin(0.9 * i);
        Bar b;
        b.symbol = kSym;
        b.timestamp = at(d);
        b.open = Decimal(close);
        b.high = Decimal(close * 1.01);
        b.low = Decimal(close * 0.99);
        b.close = Decimal(close);
        b.volume = d == odd_date ? odd_volume : volume;
        b.instrument_id = "A1";
        out.push_back(b);
    }
    return out;
}

struct K01Run {
    std::set<std::string> consumed;
    std::set<std::string> withheld;
    size_t history_bars{0};
};

// The runner's K-01 feed for a run dated `run_date`. The database read is modelled by the date
// filters: the window query returns the bars in [start, run), the history query the bars from the
// history start up to the window start; k01_classifier_history keeps the prefix.
K01Run live_run(const std::vector<Bar>& stored, const std::string& run_date) {
    const Timestamp start = at(plus(run_date, -kWindowDays));
    const Timestamp history_start = k01_classifier_history_start(start);
    std::vector<Bar> window, history_query;
    for (const auto& b : stored) {
        if (b.timestamp >= start && b.timestamp < at(run_date)) window.push_back(b);
        if (b.timestamp >= history_start && b.timestamp <= start) history_query.push_back(b);
    }
    SessionClassifier classifier;
    classifier.add_bars(window);
    const auto history = k01_classifier_history(history_query, start);
    classifier.add_bars(history);
    for (const std::vector<Bar>* feed : {&history, static_cast<const std::vector<Bar>*>(&window)}) {
        for (const auto& b : *feed) {
            classifier.add_instrument_id(b.symbol, SessionClassifier::day_of(b.timestamp), b.instrument_id);
        }
    }
    K01Run out;
    out.history_bars = history.size();
    std::vector<SymbolDayVerdict> withheld;
    for (const auto& b : k01_consumed_bars(classifier, window, &withheld)) {
        out.consumed.insert(core::format_utc_date(b.timestamp));
    }
    for (const auto& v : withheld) out.withheld.insert(v.date);
    return out;
}

}  // namespace

// The prefix is the classifier's alone: it is strictly before the window and at least four norm
// windows long.
TEST(LiveK01History, ThePrefixIsBeforeTheWindowAndLongerThanTheNorm) {
    EXPECT_GE(kK01ClassifierHistoryDays, 4 * 28) << "four 20-weekday norm windows";
    const auto stored = series("2023-06-05", "2026-04-30", 100000.0, "", 0.0);
    const K01Run r = live_run(stored, "2026-04-07");
    EXPECT_GT(r.history_bars, 80u) << "about 85 weekday bars in 120 calendar days";
    EXPECT_EQ(r.consumed.count(plus("2026-04-07", -kWindowDays - 1)), 0u) << "a prefix bar is never consumed";
}

// N2a: a 240-lot corrupt print against a 100,000-lot norm on 2024-04-08 is withheld on its own run.
// On the run whose window STARTS on it (2024-04-08 + 730 days) the window alone gives it no norm
// (240 lots is above the 50-lot floor: SESSION), and the runner fed it. With the history it is
// judged as on its own run: withheld, never fed later.
TEST(LiveK01History, ACorruptPrintWithheldOnItsOwnRunIsNeverFedLater) {
    const std::string bad = "2024-04-08";
    const auto stored = series("2023-06-05", "2026-04-30", 100000.0, bad, 240.0);
    EXPECT_EQ(live_run(stored, plus(bad, 1)).withheld.count(bad), 1u) << "withheld on its own run";
    const K01Run late = live_run(stored, plus(bad, kWindowDays));
    EXPECT_EQ(late.withheld.count(bad), 1u) << "the window starts on it: still withheld";
    EXPECT_EQ(late.consumed.count(bad), 0u) << "never fed later";
}

// N2b, the converse: a 40-lot print against a 100-lot norm is a SESSION with its history
// (40 >= 0.25 x 100) and is consumed on its own run; at the window's start, with no norm, the
// 50-lot floor withheld it. With the history it stays consumed.
TEST(LiveK01History, ABarConsumedOnItsOwnRunIsNeverWithheldLater) {
    const std::string thin = "2024-04-08";
    const auto stored = series("2023-06-05", "2026-04-30", 100.0, thin, 40.0);
    EXPECT_EQ(live_run(stored, plus(thin, 1)).consumed.count(thin), 1u) << "consumed on its own run";
    const K01Run late = live_run(stored, plus(thin, kWindowDays));
    EXPECT_EQ(late.consumed.count(thin), 1u) << "the window starts on it: still consumed";
    EXPECT_EQ(late.withheld.count(thin), 0u);
}

// Every run of two years gives each bar the verdict its own run gave it.
TEST(LiveK01History, EveryRunGivesABarTheVerdictOfItsOwnRun) {
    const std::string bad = "2024-04-08";
    const auto stored = series("2023-06-05", "2026-04-30", 100000.0, bad, 240.0);
    for (int offset : {2, 30, 365, kWindowDays - 1, kWindowDays}) {
        const K01Run r = live_run(stored, plus(bad, offset));
        EXPECT_EQ(r.withheld, (std::set<std::string>{bad})) << "run " << plus(bad, offset);
    }
}
