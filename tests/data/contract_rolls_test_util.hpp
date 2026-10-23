// tests/data/contract_rolls_test_util.hpp: shared pieces of the per-contract roll tests.
#pragma once

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

#include "trade_ngin/data/contract_bars.hpp"
#include "trade_ngin/data/roll_calendar.hpp"

namespace contract_rolls_test {

using trade_ngin::contract_rolls::Day;
using trade_ngin::contract_rolls::parse_ymd;

/// The shipped futures calendar (futures_holidays.json), found by walking up from the working
/// directory (ctest runs in the build tree). Empty when not found.
inline std::string shipped_futures_calendar_path() {
    namespace fs = std::filesystem;
    fs::path dir = fs::current_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        const fs::path p = dir / "include/trade_ngin/core/futures_holidays.json";
        if (fs::exists(p)) return p.string();
        dir = dir.parent_path();
    }
    return "";
}

/// The shipped futures calendars, loaded once for the test binary.
inline const trade_ngin::contract_rolls::FuturesCalendar& shipped_calendars() {
    static const auto cals = std::make_unique<trade_ngin::contract_rolls::FuturesCalendar>(
        trade_ngin::contract_rolls::FuturesCalendar::load(shipped_futures_calendar_path()).value());
    return *cals;
}

/// One root's trading-day calendar (GC's by default).
inline trade_ngin::contract_rolls::TradingCalendar shipped_calendar(const std::string& root = "GC") {
    return shipped_calendars().for_root(root).value();
}

inline Day d(const std::string& s) {
    return parse_ymd(s);
}

inline std::string dsn() {
    const char* v = std::getenv("TRADE_NGIN_TEST_DSN");
    return v && *v ? std::string(v) : std::string();
}

inline trade_ngin::contract_rolls::ContractBar bar(const std::string& root, const std::string& raw,
                                                   const std::string& id, const std::string& date,
                                                   const std::string& expiration, double close,
                                                   int64_t volume) {
    trade_ngin::contract_rolls::ContractBar b;
    b.root = root;
    b.raw_symbol = raw;
    b.instrument_id = id;
    b.date = parse_ymd(date);
    if (!expiration.empty()) b.expiration = parse_ymd(expiration);
    b.open = b.high = b.low = b.close = close;
    b.volume = volume;
    return b;
}

}  // namespace contract_rolls_test
