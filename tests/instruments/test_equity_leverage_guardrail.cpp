#include <gtest/gtest.h>
#include <memory>
#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "trade_ngin/instruments/equity.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

// Audit test T2.4 -- leverage guardrail (audit §3.3). The equity apps refuse
// to start if max_gross_leverage > 1.0 and any CASH-mode equity is in the
// symbol list. Cash accounts can't borrow, so a leverage cap above 1.0 is
// structurally invalid.
//
// The guard logic lives inline in the apps' main(); this test verifies the
// building blocks (instrument account_mode + registry lookup) the guard
// depends on, plus reproduces the same check pattern the apps execute.

namespace {

// Mimic the exact check the equity apps perform after load_equity_instruments.
// Returns true if the guardrail should refuse startup.
bool would_refuse_startup(const std::vector<std::string>& symbols,
                          const InstrumentRegistry& registry,
                          double max_gross_leverage) {
    if (max_gross_leverage <= 1.0) return false;
    for (const auto& symbol : symbols) {
        auto inst = registry.get_equity_instrument(symbol);
        if (inst && inst->get_account_mode() == EquityAccountMode::CASH) {
            return true;
        }
    }
    return false;
}

class LeverageGuardrailTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        auto& registry = InstrumentRegistry::instance();
        auto db = std::make_shared<MockPostgresDatabase>("mock://leverage_guardrail_test");
        ASSERT_TRUE(db->connect().is_ok());
        (void)registry.initialize(db);  // Idempotent.
    }
};

}  // namespace

TEST_F(LeverageGuardrailTest, CashEquityWithLeverageAboveOneRefuses) {
    auto& registry = InstrumentRegistry::instance();
    const std::string sym = "PHASE2_T24_CASH_AAPL";

    EquitySpec spec;
    spec.exchange = "NASDAQ";
    spec.currency = "USD";
    spec.tick_size = 0.01;
    spec.account_mode = EquityAccountMode::CASH;
    registry.register_instrument(sym, std::make_shared<EquityInstrument>(sym, spec));

    EXPECT_TRUE(would_refuse_startup({sym}, registry, 2.0))
        << "Cash equity with max_gross_leverage=2.0 must trip the guardrail.";
}

TEST_F(LeverageGuardrailTest, CashEquityWithLeverageOneAccepts) {
    auto& registry = InstrumentRegistry::instance();
    const std::string sym = "PHASE2_T24_CASH_AAPL_LEV1";

    EquitySpec spec;
    spec.exchange = "NASDAQ";
    spec.currency = "USD";
    spec.tick_size = 0.01;
    spec.account_mode = EquityAccountMode::CASH;
    registry.register_instrument(sym, std::make_shared<EquityInstrument>(sym, spec));

    EXPECT_FALSE(would_refuse_startup({sym}, registry, 1.0))
        << "Cash equity at max_gross_leverage=1.0 (the valid cap) must NOT trip the guardrail.";
}

TEST_F(LeverageGuardrailTest, RegTEquityWithLeverageAboveOneAccepts) {
    auto& registry = InstrumentRegistry::instance();
    const std::string sym = "PHASE2_T24_REGT_MSFT";

    EquitySpec spec;
    spec.exchange = "NASDAQ";
    spec.currency = "USD";
    spec.tick_size = 0.01;
    spec.account_mode = EquityAccountMode::REG_T;
    registry.register_instrument(sym, std::make_shared<EquityInstrument>(sym, spec));

    EXPECT_FALSE(would_refuse_startup({sym}, registry, 2.0))
        << "REG_T equities permit leverage > 1.0; guardrail must not fire.";
}
