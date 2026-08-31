#pragma once

#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace trade_ngin {

/**
 * @brief How far back the corporate-action window must reach, and which
 *        holdings need price closes topped up beyond the bulk load.
 */
struct CorpActionWindow {
    std::time_t start{0};                 ///< UTC seconds; window lower bound.
    std::vector<std::string> deep_symbols; ///< Established before the bulk load.
    std::time_t deep_start{0};            ///< Earliest inception among those.
};

/// Parse YYYY-MM-DD as a UTC instant. 0 on malformed input.
inline std::time_t parse_ymd_utc(const std::string& ymd) {
    std::tm tm{};
    std::istringstream ss(ymd);
    ss >> std::get_time(&tm, "%Y-%m-%d");
    if (ss.fail()) return 0;
    return timegm(&tm);
}

/**
 * @brief Derive the corp-action lookback from when positions were ESTABLISHED.
 *
 * NOT from Position::last_update: load_positions_by_date() selects
 * `WHERE DATE(last_update) = DATE($n)`, so every row it returns carries the
 * requested date by construction (the live table has zero rows where
 * last_update differs from date). Any lookback derived from it collapses to
 * "yesterday" and silently leaves the window at its floor -- which is exactly
 * the 14-day window that dropped 8 of the 9 dividends the configured universe
 * saw after live last wrote on 2026-05-03.
 *
 * A holding older than the bulk price load does not truncate the window; its
 * symbol is reported in deep_symbols so the closes its events need can be
 * topped up per symbol. Over-fetching is safe -- trading.corp_action_applied
 * rejects an already-applied event -- so this errs wide by design.
 *
 * @param today_t       Now, UTC seconds.
 * @param min_days      Floor, covering weekend/holiday stacks cheaply.
 * @param bulk_days     How far the bulk price load reaches (live.historical_days).
 * @param inception     symbol -> YYYY-MM-DD first held non-zero.
 */
inline CorpActionWindow derive_corp_action_window(
    std::time_t today_t, long min_days, long bulk_days,
    const std::unordered_map<std::string, std::string>& inception) {
    constexpr long kSecondsPerDay = 24 * 60 * 60;

    CorpActionWindow w;
    w.start = today_t - min_days * kSecondsPerDay;
    w.deep_start = today_t;

    const std::time_t bulk_start = today_t - bulk_days * kSecondsPerDay;

    for (const auto& [sym, ymd] : inception) {
        const std::time_t inc = parse_ymd_utc(ymd);
        if (inc <= 0) continue;
        if (inc < w.start) w.start = inc;
        if (inc < bulk_start) {
            w.deep_symbols.push_back(sym);
            if (inc < w.deep_start) w.deep_start = inc;
        }
    }
    return w;
}

/// Format a UTC instant as YYYY-MM-DD (bar keys are UTC, never localtime).
inline std::string format_ymd_utc(std::time_t t) {
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[11];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return std::string(buf);
}

}  // namespace trade_ngin
