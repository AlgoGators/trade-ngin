#pragma once

#include <cstdlib>
#include <filesystem>
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
 * @brief Federal holiday checker using JSON configuration
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
        if (!load_holidays()) {
            ERROR("Failed to load holidays from: " + json_path_);
        }
    }

    /**
     * @brief Check if a date is a federal holiday
     * @param date Date string in format "YYYY-MM-DD"
     * @return true if holiday, false otherwise
     */
    bool is_holiday(const std::string& date) const {
        return holidays_.find(date) != holidays_.end();
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
        return load_holidays();
    }

    /**
     * @brief Find the most recent trading day strictly before `start`.
     *
     * Walks back day-by-day skipping weekends and holidays. Bound of 14
     * covers worst-case US closure stacks (Christmas + week-of-holidays +
     * weekends, or 9/11-style multi-day exchange closures).
     *
     * @param start Reference timestamp; the search begins at `start - 24h`.
     * @param max_lookback_days Maximum days to walk back before giving up.
     * @return time_point of the previous trading day, or std::nullopt if the
     *         bound was exhausted (caller should fail closed, not fall back).
     */
    std::optional<std::chrono::system_clock::time_point>
    find_previous_trading_day(std::chrono::system_clock::time_point start,
                              int max_lookback_days = 14) const {
        // Ultrareview follow-up (Phase 6 §6b carry-over): use the thread-safe
        // safe_localtime wrapper instead of std::localtime. Local-time
        // semantics are preserved to stay consistent with
        // EquityInstrument::is_market_open which also uses safe_localtime.
        auto candidate = start - std::chrono::hours(24);
        for (int i = 0; i < max_lookback_days; ++i) {
            auto t = std::chrono::system_clock::to_time_t(candidate);
            std::tm tm{};
            if (!trade_ngin::core::safe_localtime(&t, &tm)) {
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

    /**
     * @brief Load holidays from JSON file
     * @return true if successful, false otherwise
     */
    bool load_holidays() {
        try {
            std::ifstream file(json_path_);
            if (!file.is_open()) {
                ERROR("Could not open holidays file: " + json_path_);
                return false;
            }

            nlohmann::json j;
            file >> j;

            holidays_.clear();

            // Iterate through each year
            for (auto& [year, holidays_array] : j.items()) {
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

                    holidays_[info.date] = info;
                }
            }

            INFO("Loaded " + std::to_string(holidays_.size()) + " holidays from " + json_path_);
            return true;

        } catch (const std::exception& e) {
            ERROR("Exception loading holidays: " + std::string(e.what()));
            return false;
        }
    }
};

} // namespace trade_ngin