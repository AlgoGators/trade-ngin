#include <gtest/gtest.h>
#include "trade_ngin/instruments/equity.hpp"

using namespace trade_ngin;

// Audit tests T2.1-T2.3 + T2.5 -- exercise the new
// EquityInstrument::get_margin_requirement(price, qty) overload and the
// short-selling gate. Closes audit §1.3 + §3.1 + §3.2 verification surface.

namespace {

EquitySpec make_cash_spec() {
    EquitySpec spec;
    spec.exchange = "NASDAQ";
    spec.currency = "USD";
    spec.tick_size = 0.01;
    spec.account_mode = EquityAccountMode::CASH;
    spec.short_selling_allowed = false;
    return spec;
}

EquitySpec make_regt_spec(bool shorts_allowed) {
    EquitySpec spec;
    spec.exchange = "NASDAQ";
    spec.currency = "USD";
    spec.tick_size = 0.01;
    spec.account_mode = EquityAccountMode::REG_T;
    spec.short_selling_allowed = shorts_allowed;
    return spec;
}

}  // namespace

// T2.1: Cash account, long 100 AAPL @ $150 -> full notional posted.
TEST(EquityCashAccountLongTest, FullNotionalIsPosted) {
    EquityInstrument aapl("AAPL", make_cash_spec());
    EXPECT_DOUBLE_EQ(aapl.get_margin_requirement(150.0, 100.0), 15000.0);
}

// T2.2: Reg T account, long 100 AAPL @ $150 -> 50% notional posted.
TEST(EquityRegTLongTest, FiftyPercentNotionalIsPosted) {
    EquityInstrument aapl("AAPL", make_regt_spec(false));
    EXPECT_DOUBLE_EQ(aapl.get_margin_requirement(150.0, 100.0), 7500.0);
}

// T2.3: Reg T account with shorts allowed, short 100 AAPL @ $150 ->
// 150% notional (100% proceeds + 50% maintenance).
TEST(EquityRegTShortTest, OneHundredFiftyPercentNotionalIsPosted) {
    EquityInstrument aapl("AAPL", make_regt_spec(true));
    EXPECT_DOUBLE_EQ(aapl.get_margin_requirement(150.0, -100.0), 22500.0);
}

// T2.5: Cash account rejects shorts via is_short_allowed.
// Strategy clamps when this is false; verifying the accessor pins the gate.
TEST(EquityShortDisabledInCashTest, CashAccountRejectsShorts) {
    EquityInstrument aapl("AAPL", make_cash_spec());
    EXPECT_FALSE(aapl.is_short_allowed())
        << "Cash account must always reject shorts regardless of short_selling_allowed flag.";
}

TEST(EquityShortDisabledInCashTest, RegTWithoutFlagRejectsShorts) {
    EquityInstrument aapl("AAPL", make_regt_spec(false));
    EXPECT_FALSE(aapl.is_short_allowed())
        << "Reg T without short_selling_allowed should still reject shorts.";
}

TEST(EquityShortDisabledInCashTest, RegTWithFlagAllowsShorts) {
    EquityInstrument aapl("AAPL", make_regt_spec(true));
    EXPECT_TRUE(aapl.is_short_allowed())
        << "Reg T with short_selling_allowed=true must allow shorts.";
}

// Account-mode accessor sanity.
TEST(EquityAccountModeTest, GetAccountModeReturnsConfigured) {
    EquityInstrument cash_inst("AAPL", make_cash_spec());
    EquityInstrument regt_inst("MSFT", make_regt_spec(true));
    EXPECT_EQ(cash_inst.get_account_mode(), EquityAccountMode::CASH);
    EXPECT_EQ(regt_inst.get_account_mode(), EquityAccountMode::REG_T);
}

// Legacy no-arg overload returns 0.0 (sentinel) for equities -- forces
// migration to the price/qty overload. Documents the contract for any
// caller that hasn't migrated yet.
TEST(EquityMarginLegacyApiTest, NoArgOverloadReturnsZeroSentinel) {
    EquityInstrument aapl("AAPL", make_cash_spec());
    EXPECT_DOUBLE_EQ(aapl.get_margin_requirement(), 0.0)
        << "Legacy no-arg margin must return 0 sentinel; callers must use "
           "get_margin_requirement(price, quantity).";
}
