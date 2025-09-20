// src/instruments/futures.cpp
#include "trade_ngin/instruments/futures.hpp"
#include <cmath>
#include <regex>
#include <sstream>
#include "trade_ngin/core/time_utils.hpp"

namespace trade_ngin {

FuturesInstrument::FuturesInstrument(std::string symbol, FuturesSpec spec)
    : symbol_(std::move(symbol)), spec_(std::move(spec)) {}

bool FuturesInstrument::is_tradeable() const {
    // Check if contract is expired
    if (spec_.expiry) {
        if (is_expired(std::chrono::system_clock::now())) {
            return false;
        }
    }
    return true;
}

bool FuturesInstrument::is_market_open(const Timestamp& timestamp) const {
    try {
        // Convert timestamp to local time
        std::time_t time = std::chrono::system_clock::to_time_t(timestamp);
        std::tm local_time_buffer;
        std::tm* local_time = trade_ngin::core::safe_localtime(&time, &local_time_buffer);

        // Early return for weekends
        if (local_time->tm_wday == 0 || local_time->tm_wday == 6) {
            return false;
        }

        // Parse trading hours (format: "HH:MM-HH:MM")
        std::regex time_pattern("(\\d{2}):(\\d{2})-(\\d{2}):(\\d{2})");
        std::smatch matches;
        if (!std::regex_match(spec_.trading_hours, matches, time_pattern)) {
            return false;
        }

        // Extract hours and minutes
        int start_hour = std::stoi(matches[1]);
        int start_min = std::stoi(matches[2]);
        int end_hour = std::stoi(matches[3]);
        int end_min = std::stoi(matches[4]);

        // Convert current time to minutes since midnight
        int current_minutes = local_time->tm_hour * 60 + local_time->tm_min;
        int start_minutes = start_hour * 60 + start_min;
        int end_minutes = end_hour * 60 + end_min;

        // Handle overnight sessions
        if (end_minutes < start_minutes) {
            return current_minutes >= start_minutes || current_minutes <= end_minutes;
        }

        return current_minutes >= start_minutes && current_minutes <= end_minutes;

    } catch (const std::exception&) {
        return false;  // Default to closed on any error
    }
}

double FuturesInstrument::round_price(double price) const {
    return std::round(price / spec_.tick_size) * spec_.tick_size;
}

double FuturesInstrument::get_notional_value(double quantity, double price) const {
    return std::abs(quantity) * price * spec_.multiplier;
}

double FuturesInstrument::calculate_commission(double quantity) const {
    return std::abs(quantity) * spec_.commission_per_contract;
}

std::optional<int> FuturesInstrument::days_to_expiry(const Timestamp& from) const {
    if (!spec_.expiry) {
        return std::nullopt;
    }

    auto duration = spec_.expiry.value() - from;
    return std::chrono::duration_cast<std::chrono::hours>(duration).count() / 24;
}

bool FuturesInstrument::is_expired(const Timestamp& timestamp) const {
    if (!spec_.expiry) {
        return false;
    }
    return timestamp >= spec_.expiry.value();
}

std::pair<int, int> FuturesInstrument::parse_time(const std::string& timestr) const {
    std::istringstream iss(timestr);
    std::string hour_str, minute_str;

    std::getline(iss, hour_str, ':');
    std::getline(iss, minute_str);

    return {std::stoi(hour_str), std::stoi(minute_str)};
}

}  // namespace trade_ngin
