// src/instruments/equity.cpp
#include "trade_ngin/instruments/equity.hpp"
#include <algorithm>
#include <cmath>
#include <regex>
#include "trade_ngin/core/time_utils.hpp"

namespace trade_ngin {

EquityInstrument::EquityInstrument(std::string symbol, EquitySpec spec)
    : symbol_(std::move(symbol)), spec_(std::move(spec)) {}

bool EquityInstrument::is_tradeable() const {
    // Basic checks for trading eligibility
    return !symbol_.empty() && !spec_.exchange.empty() && spec_.tick_size > 0.0 &&
           spec_.lot_size > 0.0;
}

bool EquityInstrument::is_market_open(const Timestamp& timestamp) const {
    try {
        // Convert timestamp to local time
        std::time_t time = std::chrono::system_clock::to_time_t(timestamp);
        std::tm local_time_buffer;
        std::tm* local_time = trade_ngin::core::safe_localtime(&time, &local_time_buffer);

        // Check for weekdays only
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

        // Convert to minutes since midnight
        int current_minutes = local_time->tm_hour * 60 + local_time->tm_min;
        int start_minutes = start_hour * 60 + start_min;
        int end_minutes = end_hour * 60 + end_min;

        return current_minutes >= start_minutes && current_minutes <= end_minutes;

    } catch (const std::exception&) {
        return false;
    }
}

double EquityInstrument::round_price(double price) const {
    return std::round(price / spec_.tick_size) * spec_.tick_size;
}

double EquityInstrument::get_notional_value(double quantity, double price) const {
    return std::abs(quantity) * price;
}

double EquityInstrument::calculate_commission(double quantity) const {
    return std::abs(quantity) * spec_.commission_per_share;
}

std::optional<DividendInfo> EquityInstrument::get_next_dividend(const Timestamp& from) const {
    auto it = std::find_if(spec_.dividends.begin(), spec_.dividends.end(),
                           [&from](const DividendInfo& div) { return div.ex_date > from; });

    if (it != spec_.dividends.end()) {
        return *it;
    }

    return std::nullopt;
}

}  // namespace trade_ngin