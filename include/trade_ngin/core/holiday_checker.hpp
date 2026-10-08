#pragma once

#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <set>
#include <unordered_set>
#include <string>
#include <unordered_map>
#include <optional>
#include <fstream>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <nlohmann/json.hpp>
#include "logger.hpp"
#include "time_utils.hpp"

namespace trade_ngin {

/**
 * @brief Holiday information structure
 */
struct HolidayInfo {
    std::string date;
    std::string name;
    std::string day_of_week;
    std::string type;
    std::string note;
};

/**
 * @brief US equity-market holiday checker (NYSE/NASDAQ full closures).
 *
 * This is the MARKET calendar, not the federal one: it includes Good Friday
 * (not a federal holiday) and excludes Columbus Day and Veterans Day (federal
 * holidays on which the equity markets trade).
 *
 * Coverage is finite -- the JSON spans a fixed range of years. A query outside
 * that range cannot be answered and is NOT the same as "not a holiday", so
 * `is_holiday` warns once per out-of-range year and callers whose correctness
 * depends on the answer should consult `covers_date` and fail closed. See
 * `scripts/generate_market_holidays.py` for how the file is produced.
 *
 * One calendar is shared by equities and futures. The scheduled closures
 * coincide, but ad-hoc closures (funerals, disasters) can differ between NYSE
 * and CME; the file follows NYSE.
 */
class HolidayChecker {
public:
    /**
     * @brief Resolve the holidays.json path with a fallback chain.
     *
     * Phase 6 §6a -- cron-triggered apps used to fail silently when their
     * CWD didn't match the dev-time `include/trade_ngin/core/` layout,
     * leaving HolidayChecker with an empty map and every `is_holiday`
     * returning false. This helper tries (in order):
     *
     *   1. `TRADE_NGIN_HOLIDAYS_JSON` env var (returned as-is even if it
     *      doesn't exist, so a misconfigured path surfaces via the
     *      load-error log instead of silently falling through).
     *   2. `./include/trade_ngin/core/holidays.json` (dev / source layout).
     *   3. `./holidays.json` (deploy bundle next to the binary).
     *   4. `/etc/trade_ngin/holidays.json` (system-wide fallback).
     *
     * If none of the file paths exist, returns option (2) so the
     * `HolidayChecker::load_holidays` ERROR log names the most likely
     * expected location.
     */
    static std::string resolve_holidays_path() {
        namespace fs = std::filesystem;
        if (const char* env = std::getenv("TRADE_NGIN_HOLIDAYS_JSON")) {
            // Env var wins. We return it verbatim so a misconfigured env
            // value surfaces as a clear load-error, not a silent fallback.
            return std::string(env);
        }
        const std::string candidates[] = {
            "include/trade_ngin/core/holidays.json",
            "holidays.json",
            "/etc/trade_ngin/holidays.json",
        };
        for (const auto& path : candidates) {
            std::error_code ec;
            if (fs::exists(path, ec)) {
                return path;
            }
        }
        return candidates[0];  // dev-layout default for the ERROR log
    }

    /**
     * @brief Constructor - loads holidays from JSON file
     * @param json_path Path to holidays.json file
     */
    explicit HolidayChecker(const std::string& json_path = "holidays.json")
        : json_path_(json_path) {
        loaded_ = load_holidays();
        if (!loaded_) {
            ERROR("Failed to load holidays from: " + json_path_);
        }
    }

    /**
     * @brief Whether the calendar loaded completely.
     *
     * False means the file was missing, unreadable, or threw part way through.
     * A caller whose correctness depends on the calendar must refuse to run on
     * false rather than proceed: `is_holiday` cannot distinguish "open" from
     * "never loaded", and a half-loaded file used to advertise coverage it did
     * not have (BA-1).
     */
    bool loaded() const {
        return loaded_;
    }

    /**
     * @brief Check if a date is a federal holiday
     * @param date Date string in format "YYYY-MM-DD"
     * @return true if holiday, false otherwise
     */
    bool is_holiday(const std::string& date) const {
        // A date outside the calendar's range is unanswerable. Returning false
        // silently -- the previous behaviour -- reads as "trading day" and has
        // no symptom, so warn once per year rather than degrade quietly.
        if (!covers_date(date)) {
            warn_out_of_range_once(date);
        }
        return holidays_.find(date) != holidays_.end();
    }

    /**
     * @brief Whether the calendar covers `year`.
     */
    bool covers_year(int year) const {
        return covered_years_.find(year) != covered_years_.end();
    }

    /**
     * @brief Whether a "YYYY-MM-DD" date falls inside the calendar's coverage.
     *
     * Callers whose correctness depends on holiday awareness (previous-trading-
     * day walks, non-trading-day skips) should check this and fail closed; a
     * false answer from `is_holiday` outside coverage means "unknown", not "no".
     */
    bool covers_date(const std::string& date) const {
        if (date.size() < 4) return false;
        try {
            return covers_year(std::stoi(date.substr(0, 4)));
        } catch (const std::exception&) {
            return false;
        }
    }

    /**
     * @brief How many years the loaded calendar actually covers.
     *
     * `coverage_description()` prints the FIRST and LAST year, which reads as a range and is
     * not one: a file that lost its middle years advertises "2020-2030" while covering two.
     * The count is the only figure that distinguishes them, and callers that refuse to run on
     * a partial load (the three live runners, BA-1) need it in their own log line rather than
     * in a string a human has to read.
     */
    size_t coverage_years() const { return covered_years_.size(); }

    /**
     * @brief Human-readable coverage range for error messages.
     */
    std::string coverage_description() const {
        if (covered_years_.empty()) return "no years loaded";
        return std::to_string(*covered_years_.begin()) + "-" +
               std::to_string(*covered_years_.rbegin());
    }

    /**
     * @brief Get holiday information for a date
     * @param date Date string in format "YYYY-MM-DD"
     * @return HolidayInfo if holiday exists, std::nullopt otherwise
     */
    std::optional<HolidayInfo> get_holiday_info(const std::string& date) const {
        auto it = holidays_.find(date);
        if (it != holidays_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    /**
     * @brief Get holiday name for a date
     * @param date Date string in format "YYYY-MM-DD"
     * @return Holiday name, or empty string if not a holiday
     */
    std::string get_holiday_name(const std::string& date) const {
        auto it = holidays_.find(date);
        if (it != holidays_.end()) {
            return it->second.name;
        }
        return "";
    }

    /**
     * @brief Reload holidays from JSON file
     * @return true if successful, false otherwise
     */
    bool reload() {
        // On failure the staged-swap leaves the calendar already in force
        // untouched, so `loaded_` keeps its previous value rather than
        // reporting a working calendar as unloaded.
        if (load_holidays()) {
            loaded_ = true;
            return true;
        }
        return false;
    }

    /**
     * @brief Find the most recent trading day strictly before `start`.
     *
     * Walks back day-by-day skipping weekends and holidays. Bound of 14
     * covers worst-case US closure stacks (Christmas + week-of-holidays +
     * weekends, or 9/11-style multi-day exchange closures).
     *
     * **The candidate is tested in UTC, because that is the frame it is
     * returned in.** This used to test with `safe_localtime` and return the raw
     * `candidate`, which every caller then formats with `gmtime` -- the two
     * frames agree only while a candidate's local and UTC calendar dates are the
     * same day. Since `95679ea2` the equity runner's run date is UTC MIDNIGHT
     * (`parse_utc_date`; `now_tm` is `gmtime_r` at every consumer), so on a
     * negative-offset host the candidate's local date is the day BEFORE its UTC
     * date: the walk validated day D-1 and handed back day D. Measured on the
     * 2026-06-15 replay in America/New_York -- Monday resolved to Saturday
     * 2026-06-13, no T-1 close existed, the Day T-1 finalization was refused for
     * the whole window, and every day-T row was marked from the widened fallback
     * (E2-F45).
     *
     * Testing in the returned frame makes the function self-consistent for any
     * caller. The two futures runners still parse their CLI date with
     * `std::mktime` (local midnight, E2-F42); on a negative-offset host that
     * lands at 04:00/05:00Z on the SAME calendar day, so the walk sees the same
     * days and their answer does not move -- pinned by
     * `PreviousTradingDayFrameTest.LocalMidnightRunDateResolvesTheSameCalendarDate`.
     *
     * The previous comment justified local time as consistency with
     * `EquityInstrument::is_market_open`. That function has no production
     * callers (only tests), so it was never a constraint on this one.
     *
     * @param start Reference timestamp; the search begins at `start - 24h`.
     * @param max_lookback_days Maximum days to walk back before giving up.
     * @return time_point of the previous trading day, or std::nullopt if the
     *         bound was exhausted (caller should fail closed, not fall back).
     */
    std::optional<std::chrono::system_clock::time_point>
    find_previous_trading_day(std::chrono::system_clock::time_point start,
                              int max_lookback_days = 14) const {
        auto candidate = start - std::chrono::hours(24);
        for (int i = 0; i < max_lookback_days; ++i) {
            auto t = std::chrono::system_clock::to_time_t(candidate);
            std::tm tm{};
            if (gmtime_r(&t, &tm) == nullptr) {
                return std::nullopt;
            }
            bool is_weekend = (tm.tm_wday == 0 || tm.tm_wday == 6);

            char ds_buf[11];
            std::strftime(ds_buf, sizeof(ds_buf), "%Y-%m-%d", &tm);

            if (!is_weekend && !is_holiday(std::string(ds_buf))) {
                return candidate;
            }
            candidate -= std::chrono::hours(24);
        }
        return std::nullopt;
    }

private:
    std::string json_path_;
    std::unordered_map<std::string, HolidayInfo> holidays_;
    std::set<int> covered_years_;
    bool loaded_ = false;

    // is_holiday is const and may be called per-bar, so the warn-once state is
    // mutable and guarded. One warning per out-of-range year, not per query.
    mutable std::mutex warn_mutex_;
    mutable std::unordered_set<int> warned_years_;

    void warn_out_of_range_once(const std::string& date) const {
        int year = 0;
        try {
            year = std::stoi(date.substr(0, 4));
        } catch (const std::exception&) {
            return;
        }
        std::lock_guard<std::mutex> lock(warn_mutex_);
        if (warned_years_.insert(year).second) {
            WARN("Holiday calendar does not cover " + std::to_string(year) +
                 " (loaded: " + coverage_description() + "). Dates in that year "
                 "report as non-holidays because the answer is unknown, not "
                 "because the market was open. Extend " + json_path_ +
                 " via scripts/generate_market_holidays.py.");
        }
    }

    /**
     * @brief Load holidays from JSON file
     * @return true if successful, false otherwise
     */
    bool load_holidays() {
        // BA-1: stage into locals and swap only on FULL success.
        //
        // This used to clear the live maps up front and fill them in place, and
        // it inserted each year key BEFORE parsing that year's entries. A file
        // that threw part way through therefore left the object advertising
        // coverage for years whose closures were missing -- and the outer catch
        // returned false without undoing any of it. `covers_date` then answered
        // true, every fail-closed caller passed, `is_holiday` returned false
        // with no warning because the year WAS covered, and
        // `find_previous_trading_day` walked onto a closed day. The guard only
        // fired when the load failed TOTALLY, which is the easy case.
        //
        // Staging also makes a failed reload() non-destructive: the calendar
        // already in force survives.
        std::unordered_map<std::string, HolidayInfo> staged_holidays;
        std::set<int> staged_years;

        try {
            std::ifstream file(json_path_);
            if (!file.is_open()) {
                ERROR("Could not open holidays file: " + json_path_);
                return false;
            }

            nlohmann::json j;
            file >> j;

            // Iterate through each year
            for (auto& [year, holidays_array] : j.items()) {
                int year_number = 0;
                try {
                    year_number = std::stoi(year);
                } catch (const std::exception&) {
                    // A key that is not a year is the whole file's problem, not
                    // one year's: we cannot tell what range the calendar covers,
                    // so we refuse the file rather than load part of it.
                    ERROR("Holiday calendar has a non-numeric year key: " + year);
                    return false;
                }

                for (auto& holiday : holidays_array) {
                    HolidayInfo info;
                    info.date = holiday["date"].get<std::string>();
                    info.name = holiday["name"].get<std::string>();
                    info.type = holiday["type"].get<std::string>();

                    if (holiday.contains("day_of_week")) {
                        info.day_of_week = holiday["day_of_week"].get<std::string>();
                    }

                    if (holiday.contains("note")) {
                        info.note = holiday["note"].get<std::string>();
                    }

                    staged_holidays[info.date] = info;
                }

                // Only after every entry for the year parsed.
                staged_years.insert(year_number);
            }

        } catch (const std::exception& e) {
            ERROR("Exception loading holidays: " + std::string(e.what()) +
                  " - the calendar was NOT loaded and no coverage is claimed");
            return false;
        }

        holidays_ = std::move(staged_holidays);
        covered_years_ = std::move(staged_years);
        INFO("Loaded " + std::to_string(holidays_.size()) + " holidays from " + json_path_);
        return true;
    }
};

} // namespace trade_ngin