#pragma once

#include <cstdlib>
#include <filesystem>
#include <set>
#include <chrono>
#include "time_utils.hpp"
#include <string>
#include <unordered_map>
#include <optional>
#include <fstream>
#include <nlohmann/json.hpp>
#include "logger.hpp"

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
        if (load_holidays()) { loaded_ = true; return true; }
        return false;
    }

    bool loaded() const { return loaded_; }
    bool covers_year(int year) const {
        return loaded_ && covered_years_.find(year) != covered_years_.end();
    }

    /**
     * @brief Whether a "YYYY-MM-DD" date falls inside the calendar's coverage.
     *
     * Callers whose correctness depends on holiday awareness (previous-trading-
     * day walks, non-trading-day skips) should check this and fail closed; a
     * false answer from `is_holiday` outside coverage means "unknown", not "no".
     */
    bool covers_date(const std::string& date) const {
        if (!loaded_ || !valid_calendar_date(date)) return false;
        return covers_year(std::stoi(date.substr(0,4)));
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

    std::optional<std::chrono::system_clock::time_point>
    find_previous_trading_day(std::chrono::system_clock::time_point start,
                              int max_lookback_days = 14) const {
        if (!loaded_ || max_lookback_days <= 0) return std::nullopt;
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

            if (!covers_date(std::string(ds_buf))) return std::nullopt;
            if (!is_weekend && !is_holiday(std::string(ds_buf))) {
                return candidate;
            }
            candidate -= std::chrono::hours(24);
        }
        return std::nullopt;
    }

private:
    static bool valid_calendar_date(const std::string& date) {
        if (date.size()!=10 || date[4]!='-' || date[7]!='-') return false;
        for (size_t i=0;i<date.size();++i)
            if(i!=4 && i!=7 && (date[i]<'0' || date[i]>'9')) return false;
        if(date.substr(0,4)=="0000") return false;
        Timestamp parsed;
        return core::parse_utc_date(date,parsed) && core::format_utc_date(parsed)==date;
    }
    std::string json_path_;
    std::unordered_map<std::string, HolidayInfo> holidays_;
    std::set<int> covered_years_;
    bool loaded_{false};

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

            if (!j.is_object()) return false;
            // Iterate through each year
            for (auto& [year, holidays_array] : j.items()) {
                if (year.size()!=4 || !holidays_array.is_array()) return false;
                for (char digit:year) if(digit<'0' || digit>'9') return false;
                const int year_number=std::stoi(year);
                if(year_number==0) return false;

                for (auto& holiday : holidays_array) {
                    HolidayInfo info;
                    info.date = holiday["date"].get<std::string>();
                    if (!valid_calendar_date(info.date) || info.date.substr(0,4)!=year) return false;
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