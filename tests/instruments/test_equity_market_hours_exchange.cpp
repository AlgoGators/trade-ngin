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

// ---------------------------------------------------------------------------
// C-5 §9-A3 -- the pins above cannot see the dispatch they claim to pin.
//
// THE TEST DEFECT: every assertion in this file is
// `EXPECT_TRUE(is_market_open(trading_hours_instant()))`. The pre-dispatch code ignored
// `spec_.exchange` entirely and applied US weekday-plus-holiday rules, which ALSO return
// true on a weekday inside trading hours. Executed revert (C-5 L-CLOSURE): with
// src/instruments/equity.{cpp,hpp} reverted to 4210ab14^, all five PASS:
//
//     [----------] 5 tests from EquityMarketHoursExchange  ... [  PASSED  ]
//
// The two branches only disagree where the US calendar would say CLOSED and the fail-open
// dispatch says OPEN: a non-US exchange on a weekend, or on a US market holiday. That is
// the discriminator, and it is what these assert.
// ---------------------------------------------------------------------------

namespace {

// Saturday 2026-04-18, 14:30 UTC -- inside US trading hours on a day the US market is shut.
Timestamp saturday_instant() {
    std::tm tm{};
    tm.tm_year = 126;  // 2026
    tm.tm_mon = 3;     // April
    tm.tm_mday = 18;
    tm.tm_hour = 14;
    tm.tm_min = 30;
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}

}  // namespace

TEST(EquityMarketHoursExchangeDispatch, AUsExchangeIsClosedOnASaturday) {
    // The control: US rules are still applied where they apply.
    EquitySpec spec;
    spec.exchange = "NYSE";
    EquityInstrument nyse("AAPL", spec);
    EXPECT_FALSE(nyse.is_market_open(saturday_instant()))
        << "a US exchange still takes the weekday check";
}

TEST(EquityMarketHoursExchangeDispatch, AnUnknownExchangeFailsOpenOnASaturday) {
    // THE DISCRIMINATOR. Pre-dispatch this returned FALSE (the weekday check ran for every
    // instrument); with the dispatch it returns TRUE, because this build ships no LSE
    // calendar and silently applying NYSE rules to a foreign listing is the error being
    // prevented. Reverting the dispatch flips this assertion.
    EquitySpec spec;
    spec.exchange = "LSE";
    EquityInstrument lse("VOD", spec);
    EXPECT_TRUE(lse.is_market_open(saturday_instant()))
        << "an exchange with no calendar must fail OPEN, not inherit the NYSE weekend";
}

TEST(EquityMarketHoursExchangeDispatch, TheClassificationIsWhatDrivesTheBranch) {
    EXPECT_TRUE(is_us_equities_exchange(""));       // unspecified defaults to US
    EXPECT_TRUE(is_us_equities_exchange("NYSE"));
    EXPECT_TRUE(is_us_equities_exchange("NASDAQ"));
    EXPECT_TRUE(is_us_equities_exchange("ARCA"));
    EXPECT_TRUE(is_us_equities_exchange("AMEX"));
    EXPECT_TRUE(is_us_equities_exchange("BATS"));

    EXPECT_FALSE(is_us_equities_exchange("LSE"));
    EXPECT_FALSE(is_us_equities_exchange("TSE"));
    EXPECT_FALSE(is_us_equities_exchange("HKEX"));
    EXPECT_FALSE(is_us_equities_exchange("nyse"))
        << "matched exact-case on both sides, like every other vendor label in this tree";
}
