// include/trade_ngin/live/data_freshness.hpp
#pragma once

#include <ctime>
#include <string>
#include <vector>
#include <unordered_map>

namespace trade_ngin {

/**
 * @brief How current the price feed is for the symbols a run actually loaded.
 *
 * BA-12 / C-1 C8. The freshness guard used to take the GLOBAL maximum bar
 * timestamp across every loaded bar. At universe scale that is not a freshness
 * measure at all: one symbol printing today puts the maximum at today, so 851
 * symbols could be months stale and the guard would report the feed current.
 * The question "is the feed fresh" is answered by the STALEST symbol, not the
 * freshest one.
 *
 * Two further properties the old form got wrong:
 *
 *  * No data at all was treated as "nothing to check" -- the guard sat behind
 *    `if (!all_bars.empty())`. An empty load is the MOST stale state reachable,
 *    not a neutral one, so it is reported as such here and the caller decides.
 *
 *  * Staleness was `duration_cast<hours>(end - latest) / 24` on instants. Bars
 *    are midnight-UTC keys, so an instant subtraction is off by one whenever the
 *    two ends sit either side of a fractional day. Calendar days between two
 *    UTC date keys is the quantity the tolerance is expressed in.
 */
struct FeedFreshness {
    /// False when no symbol had a bar. Maximally stale, never neutral.
    bool any_data{false};
    /// The oldest "last bar" across all symbols, YYYY-MM-DD. Empty when !any_data.
    std::string stalest_date;
    /// Which symbol is furthest behind. Empty when !any_data.
    std::string stalest_symbol;
    /// Calendar days from `stalest_date` to the as-of date. 0 when !any_data.
    long days_behind{0};
    /// How many symbols were assessed. B-ii: this is the size of the REQUESTED
    /// universe when one is supplied, not the number that answered.
    std::size_t symbols{0};
    /// B-ii: requested symbols with no bar at all. A symbol that never printed
    /// cannot set `stalest_date` -- it is not "a few days behind", it is absent,
    /// which is a different and worse condition. Counted separately so the
    /// threshold policy on `days_behind` is unchanged.
    std::size_t absent{0};
    /// First absent symbol in sort order, so the report is deterministic. Empty
    /// when `absent == 0`.
    std::string absent_symbol;
};

/// Parse YYYY-MM-DD as a UTC instant; 0 on malformed input. Kept local so this
/// header stays free of the corp-action window's includes.
inline std::time_t freshness_parse_ymd_utc(const std::string& ymd) {
    if (ymd.size() < 10) return 0;
    std::tm tm{};
    tm.tm_year = 0;
    // Hand-parsed rather than via get_time: this is called per symbol and
    // std::istringstream is needlessly expensive at 852 names.
    auto digits = [&](std::size_t at, std::size_t n, int& out) -> bool {
        int v = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const char c = ymd[at + i];
            if (c < '0' || c > '9') return false;
            v = v * 10 + (c - '0');
        }
        out = v;
        return true;
    };
    int y = 0, m = 0, d = 0;
    if (ymd[4] != '-' || ymd[7] != '-') return 0;
    if (!digits(0, 4, y) || !digits(5, 2, m) || !digits(8, 2, d)) return 0;
    if (m < 1 || m > 12 || d < 1 || d > 31) return 0;
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
    return timegm(&tm);
}

/// Whole calendar days from `from_ymd` to `to_ymd`. Negative if `from` is later.
/// 0 if either date is malformed -- the caller cannot act on an unparseable key,
/// and reporting a huge staleness for a typo would be its own false alarm.
inline long calendar_days_between_utc(const std::string& from_ymd, const std::string& to_ymd) {
    const std::time_t a = freshness_parse_ymd_utc(from_ymd);
    const std::time_t b = freshness_parse_ymd_utc(to_ymd);
    if (a <= 0 || b <= 0) return 0;
    return static_cast<long>((b - a) / (24 * 60 * 60));
}

/**
 * @brief Assess feed freshness from the per-symbol newest-bar map.
 *
 * @param last_bar_date symbol -> newest loaded bar, YYYY-MM-DD (the map the
 *        runner already builds for the corp-action horizon gate).
 * @param as_of_ymd     the run's end date, YYYY-MM-DD.
 * @param requested     B-ii: the universe the run ASKED for. `last_bar_date` only
 *        contains symbols that returned rows, so a name with zero bars is invisible
 *        to a check driven by that map -- it cannot be the minimum and cannot set
 *        the bound, and the guard reports "stalest of 9" on a ten-name book while
 *        the tenth reaches day T unpriced. Pass the universe and absence is seen.
 *        Empty preserves the original behaviour for callers that have no universe.
 *
 * Symbols whose date will not parse are counted but cannot set the bound; a map
 * consisting only of such entries reports `any_data = false`, because nothing in
 * it establishes that any symbol is current.
 */
inline FeedFreshness assess_feed_freshness(
    const std::unordered_map<std::string, std::string>& last_bar_date,
    const std::string& as_of_ymd,
    const std::vector<std::string>& requested = {}) {
    FeedFreshness f;
    f.symbols = requested.empty() ? last_bar_date.size() : requested.size();

    // B-ii: absence first, and named deterministically.
    for (const auto& symbol : requested) {
        auto it = last_bar_date.find(symbol);
        if (it != last_bar_date.end() && freshness_parse_ymd_utc(it->second) > 0) continue;
        ++f.absent;
        if (f.absent_symbol.empty() || symbol < f.absent_symbol) f.absent_symbol = symbol;
    }

    for (const auto& [symbol, ymd] : last_bar_date) {
        if (freshness_parse_ymd_utc(ymd) <= 0) continue;
        // MIN, not max: the stalest symbol is what decides whether the feed is
        // usable. Ties broken by symbol name so the report is deterministic.
        if (!f.any_data || ymd < f.stalest_date ||
            (ymd == f.stalest_date && symbol < f.stalest_symbol)) {
            f.any_data = true;
            f.stalest_date = ymd;
            f.stalest_symbol = symbol;
        }
    }

    if (f.any_data) {
        f.days_behind = calendar_days_between_utc(f.stalest_date, as_of_ymd);
    }
    return f;
}

}  // namespace trade_ngin
