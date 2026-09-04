#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>

#include "trade_ngin/core/holiday_checker.hpp"
#include "trade_ngin/instruments/equity.hpp"

using namespace trade_ngin;

// Phase 6 §6b -- pins exchange-aware is_market_open dispatch.
//
// The audit flagged that is_market_open ignored spec_.exchange and
// silently applied NYSE rules to every equity (including LSE/TSE/etc.).
// Phase 6 adds a dispatch:
//   - US exchanges (NYSE/NASDAQ/ARCA/AMEX/BATS, empty default) -> US calendar
//   - any other -> fail OPEN with a one-time WARN per unknown exchange
//
// The fail-open choice is intentional: blocking trade on a real foreign
// venue we just haven't added a calendar for would be MORE wrong than
// allowing the trade (the operator's order-management layer is the
// chokepoint for legitimacy).

namespace {

EquitySpec make_spec(const std::string& exchange) {
    EquitySpec s;
    s.exchange = exchange;
    s.currency = "USD";
    s.trading_hours = "09:30-16:00";
    return s;
}

// 2025-06-18 (Wednesday) at 10:00 EDT == 14:00 UTC, within the
// 09:30-16:00 local trading window. Pick an instant that is unambiguously
// "open" for any US-style calendar with no holiday loaded.
std::chrono::system_clock::time_point trading_hours_instant() {
    // 2025-06-18T14:00:00Z
    return std::chrono::system_clock::from_time_t(1750255200);
}

}  // namespace

// US exchanges with no holiday checker registered: weekday + within hours
// returns true (only weekday/hours logic is consulted).
TEST(EquityMarketHoursExchange, NyseFallsThroughToTradingHours) {
    EquityInstrument::set_holiday_checker(nullptr);
    EquityInstrument inst("AAPL", make_spec("NYSE"));
    EXPECT_TRUE(inst.is_market_open(trading_hours_instant()));
}

// Empty exchange string is treated as US default (preserves pre-Phase-6
// behavior for instruments where the exchange wasn't populated).
TEST(EquityMarketHoursExchange, EmptyExchangeTreatedAsUs) {
    EquityInstrument::set_holiday_checker(nullptr);
    EquityInstrument inst("FOO", make_spec(""));
    EXPECT_TRUE(inst.is_market_open(trading_hours_instant()));
}

// Unknown / non-US exchange: fail-open. We can't load a TSE calendar yet,
// so blocking the trade would silently break valid trading.
TEST(EquityMarketHoursExchange, UnknownExchangeFailsOpen) {
    EquityInstrument::set_holiday_checker(nullptr);
    EquityInstrument inst("8316.T", make_spec("TSE"));
    EXPECT_TRUE(inst.is_market_open(trading_hours_instant()));
}

// Unknown exchange WARN fires only once per unique exchange name (avoids
// log spam in a tight per-tick loop). We can't easily intercept the
// logger from this layer, but the dispatch is exercised here and the
// fail-open semantic is the contract worth pinning.
TEST(EquityMarketHoursExchange, UnknownExchangeIsIdempotentAcrossCalls) {
    EquityInstrument::set_holiday_checker(nullptr);
    EquityInstrument inst("8316.T", make_spec("HKEX"));
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(inst.is_market_open(trading_hours_instant()));
    }
}

// NASDAQ is also a US exchange and dispatches into the calendar path.
TEST(EquityMarketHoursExchange, NasdaqIsUs) {
    EquityInstrument::set_holiday_checker(nullptr);
    EquityInstrument inst("MSFT", make_spec("NASDAQ"));
    EXPECT_TRUE(inst.is_market_open(trading_hours_instant()));
}
