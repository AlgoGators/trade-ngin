// src/instruments/equity.cpp
//
// Phase 6 audit-status (Phase 6 §1.10 verification):
//   - set_holiday_checker IS called in production (live_equity_mean_reversion.cpp,
//     bt_equity_mean_reversion.cpp) and is consulted by is_market_open below.
//   - calculate_commission returns qty * spec_.commission_per_share where the
//     spec is populated by instrument_registry.cpp:227 (default $0.005/share)
//     or :398 (ADV-tier classifier). Audit's "always returns 0" finding is
//     stale.
//   - get_margin_requirement(price, qty) implements the Phase 2 §1.3 Reg T
//     math (CASH: 100% notional; REG_T long: 50%; REG_T short: 150%).
//   See tests/instruments/test_equity_audit_status.cpp for regression guards.

#include "trade_ngin/instruments/equity.hpp"
#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <regex>
#include <unordered_set>
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/time_utils.hpp"

namespace trade_ngin {

std::shared_ptr<HolidayChecker> EquityInstrument::holiday_checker_ = nullptr;

void EquityInstrument::set_holiday_checker(std::shared_ptr<HolidayChecker> checker) {
    // atomic_store handles the racing-with-reads case (Phase 6 §6b).
    std::atomic_store(&holiday_checker_, std::move(checker));
}

std::shared_ptr<HolidayChecker> EquityInstrument::get_holiday_checker() {
    return std::atomic_load(&holiday_checker_);
}

namespace {

// Phase 6 §6b: exchange-aware is_market_open dispatch.
//
// Today we ship one calendar (US equities). Other exchanges (LSE, TSE, HKEX,
// etc.) are latent -- their holiday calendars aren't part of this PR. To
// avoid silently using NYSE rules for a non-US listing (which the audit
// flagged), we fail-open WITH a WARN that fires once per unique exchange so
// the operator sees the gap without log spam.
bool is_us_equities_exchange(const std::string& exchange) {
    return exchange.empty() ||  // unspecified defaults to US
           exchange == "NYSE" || exchange == "NASDAQ" ||
           exchange == "ARCA" || exchange == "AMEX" || exchange == "BATS";
}

void warn_unknown_exchange_once(const std::string& exchange) {
    static std::mutex m;
    static std::unordered_set<std::string> seen;
    std::lock_guard<std::mutex> lock(m);
    if (seen.insert(exchange).second) {
        WARN("EquityInstrument::is_market_open: no holiday calendar for "
             "exchange '" + exchange + "' -- failing OPEN (allowing trade). "
             "Add a calendar to the HolidayChecker if this is a real venue.");
    }
}

}  // namespace

EquityInstrument::EquityInstrument(std::string symbol, EquitySpec spec)
    : symbol_(std::move(symbol)), spec_(std::move(spec)) {}

bool EquityInstrument::is_tradeable() const {
    // Basic checks for trading eligibility
    return !symbol_.empty() && !spec_.exchange.empty() && spec_.tick_size > 0.0 &&
           spec_.lot_size > 0.0;
}

bool EquityInstrument::is_market_open(const Timestamp& timestamp) const {
    try {
        // Phase 6 §6b: exchange-aware dispatch. Today we only ship a US
        // calendar; non-US exchanges fail OPEN with a one-time WARN so the
        // operator notices the gap rather than getting silent NYSE rules.
        if (!is_us_equities_exchange(spec_.exchange)) {
            warn_unknown_exchange_once(spec_.exchange);
            return true;  // fail-open for unknown exchange
        }

        // Convert timestamp to local time
        std::time_t time = std::chrono::system_clock::to_time_t(timestamp);
        std::tm local_time_buffer;
        std::tm* local_time = trade_ngin::core::safe_localtime(&time, &local_time_buffer);

        // Check for weekdays only
        if (local_time->tm_wday == 0 || local_time->tm_wday == 6) {
            return false;
        }

        // Check for market holidays (atomic snapshot of the registered checker).
        if (auto checker = std::atomic_load(&holiday_checker_)) {
            char date_buf[11];
            std::strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", local_time);
            if (checker->is_holiday(std::string(date_buf))) {
                return false;
            }
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

double EquityInstrument::get_margin_requirement(double price, double quantity) const {
    // Closes audit §1.3: real Reg T-aware margin math, replacing the
    // structurally broken margin_requirement{0.5} treated as $/share.
    const double notional = std::abs(quantity) * price;
    if (spec_.account_mode == EquityAccountMode::CASH) {
        // Cash account: full position value tied up; no margin extension.
        return notional;
    }
    // REG_T:
    if (quantity >= 0.0) {
        // Long: 50% initial margin under Reg T.
        return notional * 0.50;
    }
    // Short: 100% proceeds collateral + 50% maintenance margin = 150% notional.
    return notional * 1.50;
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