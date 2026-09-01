// TransactionCostManager -- all cost paths for the component in one place.
//
// Sections below: registering equity costs from bars (the ADV/spread
// classifier), overnight borrow-fee accrual on short equity, and the
// calculate_costs fallback when a symbol has no registered config.

#include <chrono>
#include <cmath>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/instruments/equity.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/transaction_cost/transaction_cost_manager.hpp"

using namespace trade_ngin;
using namespace trade_ngin::transaction_cost;
using namespace trade_ngin::testing;

// ──────────────────────────────────────────────────────────────────────────
// Equity cost registration from bars -- the ADV classifier.
// register_equity_costs_from_bars derives a liquidity tier from recent
// volume, so a thinly-traded symbol is not charged mega-cap spreads. Pins
// the tier boundaries and that the wiring is actually reached.
// (was: test_adv_classifier_wireup.cpp)
// ──────────────────────────────────────────────────────────────────────────

// Regression test for audit finding §1.1 — Phase 1a-ii wire-up. Before this
// fix, every traded equity needed an explicit hardcoded config in
// TransactionCostManager. The new register_equity_costs_from_bars helper
// computes ADV from recent bars and registers the appropriate tier.

namespace {

std::vector<Bar> synthetic_equity_bars(const std::string& symbol,
                                       int days,
                                       double price,
                                       double volume) {
    std::vector<Bar> bars;
    bars.reserve(days);
    auto now = std::chrono::system_clock::now();
    for (int i = 0; i < days; ++i) {
        Bar b;
        b.symbol = symbol;
        b.timestamp = now - std::chrono::hours(24 * (days - i));
        b.open = price;
        b.high = price * 1.005;
        b.low = price * 0.995;
        b.close = price;
        b.volume = volume;
        bars.push_back(b);
    }
    return bars;
}

}  // namespace

// NVDA-shaped mega-cap: ADV >> 10M, price > $100 → mega-cap tier.
// Expected from get_tiered_equity_config:
//   point_value = 1.0 (per share, not per "contract")
//   commission_per_unit = 0.005 ($0.005/share, IBKR Pro)
//   apply_regulatory_fees = true (SEC + FINRA on sells)
//   baseline_spread_ticks = 1.0 (mega-cap = tight spread)
TEST(ADVClassifierWireupTest, MegaCapSymbolGetsTier1Config) {
    TransactionCostManager::Config tcm_config;
    TransactionCostManager tcm(tcm_config);

    std::unordered_map<std::string, std::vector<Bar>> bars_by_symbol;
    bars_by_symbol["NVDA"] = synthetic_equity_bars("NVDA", /*days=*/30,
                                                    /*price=*/500.0,
                                                    /*volume=*/50'000'000.0);

    int registered = tcm.register_equity_costs_from_bars({"NVDA"}, bars_by_symbol);
    EXPECT_EQ(registered, 1);

    AssetCostConfig cfg = tcm.get_asset_config("NVDA");
    EXPECT_EQ(cfg.asset_type, AssetType::EQUITY);
    EXPECT_DOUBLE_EQ(cfg.point_value, 1.0);
    EXPECT_DOUBLE_EQ(cfg.commission_per_unit, 0.005);
    // E2-C3: false under IBKR Pro Fixed, whose $0.005/share is all-inclusive of exchange,
    // clearing and regulatory fees. Charging SEC/FINRA on top double-charges. It would be
    // true under Tiered, where fees are passed through.
    EXPECT_FALSE(cfg.apply_regulatory_fees);
    EXPECT_LE(cfg.baseline_spread_ticks, 2.0)
        << "Mega-cap should have tight spread (≤2 ticks); got "
        << cfg.baseline_spread_ticks;
}

// E2-F14: a symbol with no bars is registered with the UNTIERED EQUITY DEFAULT, not
// skipped. Skipping left it absent from configs_, and an absent symbol does not fall back
// to the equity default -- get_config() only reaches that when the caller passes
// AssetType::EQUITY, which none of the nine production call sites does. So a skipped equity
// resolved to the FUTURES default: $1.50/share instead of $0.005 and point_value 100
// instead of 1. Worse, configs_ is one flat map shared across asset classes and real tickers
// collide with futures roots (CL = Colgate-Palmolive AND Crude Oil; ES = Eversource AND the
// E-mini S&P). Registering the default makes the futures fallthrough unreachable for a
// configured equity regardless of what any call site passes.
TEST(ADVClassifierWireupTest, SymbolWithNoBarsGetsEquityDefaultNotFuturesPricing) {
    TransactionCostManager::Config tcm_config;
    TransactionCostManager tcm(tcm_config);

    std::unordered_map<std::string, std::vector<Bar>> bars_by_symbol;
    // No entry for "MSFT".

    int registered = tcm.register_equity_costs_from_bars({"MSFT"}, bars_by_symbol);
    EXPECT_EQ(registered, 1) << "A configured equity with no bars must still be registered.";

    AssetCostConfig cfg = tcm.get_asset_config("MSFT");
    EXPECT_DOUBLE_EQ(cfg.point_value, 1.0)
        << "Unregistered equity inherited a futures point_value -- 100x the slippage.";
    EXPECT_DOUBLE_EQ(cfg.commission_per_unit, 0.005)
        << "Unregistered equity inherited the futures per-contract fee, i.e. $1.50/SHARE.";
}

// E2-F14: same reasoning as the no-bars case -- a degenerate ADV must not leave a
// configured equity resolving as a future.
TEST(ADVClassifierWireupTest, ZeroVolumeGetsEquityDefaultNotFuturesPricing) {
    TransactionCostManager::Config tcm_config;
    TransactionCostManager tcm(tcm_config);

    std::unordered_map<std::string, std::vector<Bar>> bars_by_symbol;
    bars_by_symbol["GOOG"] = synthetic_equity_bars("GOOG", 20, 150.0, /*volume=*/0.0);

    int registered = tcm.register_equity_costs_from_bars({"GOOG"}, bars_by_symbol);
    EXPECT_EQ(registered, 1) << "A zero-ADV equity must still be registered.";

    AssetCostConfig cfg = tcm.get_asset_config("GOOG");
    EXPECT_DOUBLE_EQ(cfg.point_value, 1.0);
    EXPECT_DOUBLE_EQ(cfg.commission_per_unit, 0.005);
}

// Smaller bar history than the lookback window still computes ADV from what
// is available (capped by bars.size()) and registers a config.
TEST(ADVClassifierWireupTest, ShortHistoryStillRegisters) {
    TransactionCostManager::Config tcm_config;
    TransactionCostManager tcm(tcm_config);

    std::unordered_map<std::string, std::vector<Bar>> bars_by_symbol;
    bars_by_symbol["AAPL"] = synthetic_equity_bars("AAPL", /*days=*/3,
                                                    /*price=*/180.0,
                                                    /*volume=*/30'000'000.0);

    int registered = tcm.register_equity_costs_from_bars(
        {"AAPL"}, bars_by_symbol, /*adv_lookback_days=*/20);
    EXPECT_EQ(registered, 1);
    AssetCostConfig cfg = tcm.get_asset_config("AAPL");
    EXPECT_EQ(cfg.asset_type, AssetType::EQUITY);
}

// Multiple symbols at once.
TEST(ADVClassifierWireupTest, MultipleSymbolsBatch) {
    TransactionCostManager::Config tcm_config;
    TransactionCostManager tcm(tcm_config);

    std::unordered_map<std::string, std::vector<Bar>> bars_by_symbol;
    bars_by_symbol["AAPL"] = synthetic_equity_bars("AAPL", 30, 180.0, 30'000'000.0);
    bars_by_symbol["MSFT"] = synthetic_equity_bars("MSFT", 30, 400.0, 20'000'000.0);
    bars_by_symbol["NVDA"] = synthetic_equity_bars("NVDA", 30, 500.0, 50'000'000.0);

    int registered = tcm.register_equity_costs_from_bars(
        {"AAPL", "MSFT", "NVDA"}, bars_by_symbol);
    EXPECT_EQ(registered, 3);
    EXPECT_EQ(tcm.get_asset_config("AAPL").asset_type, AssetType::EQUITY);
    EXPECT_EQ(tcm.get_asset_config("MSFT").asset_type, AssetType::EQUITY);
    EXPECT_EQ(tcm.get_asset_config("NVDA").asset_type, AssetType::EQUITY);
}

// ──────────────────────────────────────────────────────────────────────────
// Overnight borrow fees -- short equity carry.
// A short equity position accrues a borrow fee per calendar day held.
// Hard-to-borrow names cost more, and the accrual must not fire for longs
// or on an account mode that forbids shorting.
// (was: test_equity_borrow_fees.cpp)
// ──────────────────────────────────────────────────────────────────────────

// Audit tests T2.6, T2.7, T2.8 -- borrow fee model on TCM (§3.2).
// Multi-factor risk scoring (dollar-volume + price + is_easy_to_borrow flag)
// × volatility multiplier × short notional / 365. Per-symbol
// borrow_rate_override bypasses the formula.

namespace {

// Synthesize bars at constant price/volume so ADV is exactly `volume`.
std::vector<Bar> bars_for(const std::string& symbol, int days, double price, double volume) {
    std::vector<Bar> bars;
    bars.reserve(days);
    auto now = std::chrono::system_clock::now();
    for (int i = 0; i < days; ++i) {
        Bar b;
        b.symbol = symbol;
        b.timestamp = now - std::chrono::hours(24 * (days - i));
        b.open = price;
        b.high = price * 1.001;
        b.low = price * 0.999;
        b.close = price;
        b.volume = volume;
        bars.push_back(b);
    }
    return bars;
}

EquitySpec regt_spec(bool shorts = true) {
    EquitySpec spec;
    spec.exchange = "NASDAQ";
    spec.currency = "USD";
    spec.tick_size = 0.01;
    spec.account_mode = EquityAccountMode::REG_T;
    spec.short_selling_allowed = shorts;
    return spec;
}

class BorrowFeesTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        auto& registry = InstrumentRegistry::instance();
        auto db = std::make_shared<MockPostgresDatabase>("mock://borrow_fees_test");
        ASSERT_TRUE(db->connect().is_ok());
        (void)registry.initialize(db);  // Idempotent.
    }
};

}  // namespace

// T2.6: REG_T short 100 AAPL @ $150, mega-cap (ADV >10M -> dollar volume
// >$1.5B/day, no flag), price > $10 (no flag), default is_easy_to_borrow=true
// (no flag) -> 0 flags -> 25 bps base. Annual vol 25% -> multiplier 1.0.
// Daily fee = 0.0025 * $15,000 / 365 ~= $0.1027.
TEST_F(BorrowFeesTest, MegaCapShortYieldsLowFee) {
    TransactionCostManager::Config tcm_config;
    TransactionCostManager tcm(tcm_config);
    auto& registry = InstrumentRegistry::instance();

    const std::string sym = "PHASE2_T26_AAPL";
    registry.register_instrument(sym, std::make_shared<EquityInstrument>(sym, regt_spec()));

    // Register equity cost config so impact_model knows the ADV.
    std::unordered_map<std::string, std::vector<Bar>> bars;
    bars[sym] = bars_for(sym, 30, 150.0, 50'000'000.0);
    tcm.register_equity_costs_from_bars({sym}, bars);

    // Feed log returns to seed the vol estimate at 25% annualized.
    // daily_log_return for 25% annual vol = 0.25/sqrt(252) ~ 0.01575.
    const double daily_ret = 0.25 / std::sqrt(252.0);
    for (int i = 0; i < 22; ++i) {
        const double r = (i % 2 == 0) ? daily_ret : -daily_ret;
        tcm.update_market_data(sym, 50'000'000.0, 150.0 + r * 150.0, 150.0);
    }

    // Short 100 shares.
    std::unordered_map<std::string, Position> positions;
    Position p;
    p.symbol = sym;
    p.quantity = -100.0;
    p.average_price = 150.0;
    positions[sym] = p;

    std::unordered_map<std::string, double> prices;
    prices[sym] = 150.0;

    auto fees = tcm.calculate_overnight_borrow_fees(positions, prices, registry);
    ASSERT_EQ(fees.size(), 1u);

    const double expected = 0.0025 * 15000.0 / 365.0;  // ~0.1027
    EXPECT_NEAR(fees.at(sym), expected, 0.05)
        << "Mega-cap short fee should be ~$0.10/day (25 bps × $15K / 365); "
           "got " << fees.at(sym);
}

// T2.7: short 100 of small-cap @ $4. ADV 80K -> dollar volume $320K/day -> 1 flag.
// Price < $5 -> 1 flag. is_easy_to_borrow=true so no extra flag. Total 2 flags
// -> 150 bps base. Annual vol set to 75% -> raw multiplier 3.0 (capped).
// Annual rate = 0.015 × 3.0 = 0.045 (4.5%). Daily fee = 0.045 × $400 / 365 ~= $0.0493.
TEST_F(BorrowFeesTest, SmallCapHighVolYieldsScaledFee) {
    TransactionCostManager::Config tcm_config;
    TransactionCostManager tcm(tcm_config);
    auto& registry = InstrumentRegistry::instance();

    const std::string sym = "PHASE2_T27_SMALLCAP";
    registry.register_instrument(sym, std::make_shared<EquityInstrument>(sym, regt_spec()));

    std::unordered_map<std::string, std::vector<Bar>> bars;
    bars[sym] = bars_for(sym, 30, 4.0, 80'000.0);
    tcm.register_equity_costs_from_bars({sym}, bars);

    // Seed 75% annual vol via daily log returns.
    const double daily_ret = 0.75 / std::sqrt(252.0);
    for (int i = 0; i < 22; ++i) {
        const double r = (i % 2 == 0) ? daily_ret : -daily_ret;
        tcm.update_market_data(sym, 80'000.0, 4.0 + r * 4.0, 4.0);
    }

    std::unordered_map<std::string, Position> positions;
    Position p;
    p.symbol = sym;
    p.quantity = -100.0;
    p.average_price = 4.0;
    positions[sym] = p;

    std::unordered_map<std::string, double> prices;
    prices[sym] = 4.0;

    auto fees = tcm.calculate_overnight_borrow_fees(positions, prices, registry);
    ASSERT_EQ(fees.size(), 1u);

    // 2 flags -> 150 bps base. 75% vol -> 3× cap. Annual rate = 0.045.
    // Daily fee = 0.045 × $400 / 365 ~= $0.0493
    const double expected = 0.045 * 400.0 / 365.0;
    EXPECT_NEAR(fees.at(sym), expected, 0.02)
        << "Small-cap high-vol short should scale via 2-flag base × 3× vol mult; "
           "got " << fees.at(sym);
}

// T2.8: borrow_rate_override = 0.50 (50% annual). Formula is bypassed entirely.
// Daily fee = 0.50 × $15,000 / 365 ~= $20.55.
TEST_F(BorrowFeesTest, OverrideBypassesFormula) {
    TransactionCostManager::Config tcm_config;
    TransactionCostManager tcm(tcm_config);
    auto& registry = InstrumentRegistry::instance();

    const std::string sym = "PHASE2_T28_OVERRIDE";
    EquitySpec spec = regt_spec();
    spec.borrow_rate_override = 0.50;
    registry.register_instrument(sym, std::make_shared<EquityInstrument>(sym, spec));

    // No ADV / vol seeding -- override means formula isn't consulted.

    std::unordered_map<std::string, Position> positions;
    Position p;
    p.symbol = sym;
    p.quantity = -100.0;
    p.average_price = 150.0;
    positions[sym] = p;

    std::unordered_map<std::string, double> prices;
    prices[sym] = 150.0;

    auto fees = tcm.calculate_overnight_borrow_fees(positions, prices, registry);
    ASSERT_EQ(fees.size(), 1u);

    const double expected = 0.50 * 15000.0 / 365.0;  // ~$20.55
    EXPECT_NEAR(fees.at(sym), expected, 0.05)
        << "Override should bypass formula; expected " << expected
        << ", got " << fees.at(sym);
}

// Longs and non-equities should be skipped (no borrow fee).
TEST_F(BorrowFeesTest, LongsAndNonEquitiesAreSkipped) {
    TransactionCostManager::Config tcm_config;
    TransactionCostManager tcm(tcm_config);
    auto& registry = InstrumentRegistry::instance();

    const std::string sym = "PHASE2_T28_LONG";
    registry.register_instrument(sym, std::make_shared<EquityInstrument>(sym, regt_spec()));

    std::unordered_map<std::string, Position> positions;
    Position p;
    p.symbol = sym;
    p.quantity = 100.0;  // Long.
    p.average_price = 150.0;
    positions[sym] = p;

    std::unordered_map<std::string, double> prices;
    prices[sym] = 150.0;

    auto fees = tcm.calculate_overnight_borrow_fees(positions, prices, registry);
    EXPECT_TRUE(fees.empty()) << "Long positions must not accrue borrow fees.";
}

// ──────────────────────────────────────────────────────────────────────────
// Unregistered-symbol dispatch -- the calculate_costs fallback.
// An equity symbol with no registered cost config must fall back to a
// documented default rather than silently costing zero, which would make a
// backtest look cheaper than reality.
// (was: test_unknown_equity_costs_dispatch.cpp)
// ──────────────────────────────────────────────────────────────────────────

// Regression test for audit finding §1.1 dispatch dead-end.
//
// Before Phase 1a-i, TransactionCostManager::calculate_costs() called
// asset_configs_.get_config(symbol) without asset_type. The default branch
// at asset_cost_config.cpp:670 gated on `asset_type == EQUITY` but always
// received NONE, so unknown equity symbols fell through to the futures
// default: point_value=100, commission=$1.50/share. ~157× cost overstatement.
//
// Post-fix, calculate_costs accepts an optional `asset_type` parameter that
// threads through to get_config, allowing equity callers to opt into the
// correct fallback even for unregistered symbols.

// Unregistered symbol "ZZZZ", buy 100 shares at $50.
// With AssetType::EQUITY hint: commission = max($1 floor, 100 × $0.005) = $1.00.
// Without the hint: commission would be 100 × $1.50 = $150 (futures fallback).
TEST(UnknownEquityCostsDispatchTest, EquityHintRoutesUnregisteredToEquityDefault) {
    TransactionCostManager::Config tcm_config;
    TransactionCostManager tcm(tcm_config);

    // No registration of "ZZZZ"; calling code passes AssetType::EQUITY so
    // the fallback at AssetCostConfigRegistry::get_config returns equity
    // defaults instead of futures defaults.
    auto result = tcm.calculate_costs("ZZZZ", /*quantity=*/100.0,
                                       /*reference_price=*/50.0,
                                       AssetType::EQUITY);

    // $1.00 min commission floor binds (raw = 100 × $0.005 = $0.50).
    EXPECT_NEAR(result.commissions_fees, 1.00, 0.01)
        << "Unregistered equity should fall back to equity default ($0.005/share, $1 min). "
        << "Got " << result.commissions_fees
        << " -- if this is ~$150, the dispatch dead-end has regressed.";
}

// Larger trade so the per-share rate exceeds the floor. 500 shares × $0.005 = $2.50.
TEST(UnknownEquityCostsDispatchTest, EquityHintScalesByShares) {
    TransactionCostManager::Config tcm_config;
    TransactionCostManager tcm(tcm_config);

    auto result = tcm.calculate_costs("UNKNOWN_LARGE", /*quantity=*/500.0,
                                       /*reference_price=*/100.0,
                                       AssetType::EQUITY);
    EXPECT_NEAR(result.commissions_fees, 2.50, 0.01)
        << "500 × $0.005 = $2.50; got " << result.commissions_fees;
}

// Without the hint, an unknown symbol still gets the legacy futures default.
// This documents the contract: callers MUST pass AssetType::EQUITY for the
// equity fallback to apply.
TEST(UnknownEquityCostsDispatchTest, WithoutHintFallsThroughToFuturesDefault) {
    TransactionCostManager::Config tcm_config;
    TransactionCostManager tcm(tcm_config);

    // No asset_type hint -> defaults to AssetType::NONE -> falls through to
    // get_default_config() (futures: point_value=100, commission absent so
    // global config_.explicit_fee_per_contract kicks in).
    auto result = tcm.calculate_costs("UNKNOWN_NO_HINT", /*quantity=*/100.0,
                                       /*reference_price=*/50.0);
    // Documents that the unhinted path is NOT equity-shaped. Production
    // callers that handle equities should always pass AssetType::EQUITY.
    EXPECT_GT(result.commissions_fees, 1.50)
        << "Without an equity hint, fallback should still be futures-shaped. "
           "If this asserts because the result is ~$1, dispatch defaults changed.";
}

// ===========================================================================
// E2-C1 / E2-C2 / E2-C3 -- the equity commission schedule is IBKR Pro FIXED:
//   $0.005/share, minimum $1.00 per order, maximum 1% of trade value,
//   all-inclusive of exchange, clearing and regulatory fees.
//
// The floor and the cap do different jobs and only collide on very small orders --
// exactly what a fractional-share book trades. The floor protects the broker on
// small-but-normal orders; the cap protects the client on tiny ones. IBKR publishes the
// cap as "Maximum per order", so it is a ceiling and must be applied LAST.
//
// The code computed max(floor, min(cap, raw)), which let the floor override the cap and
// made max_commission_pct provably dead for any stock above $0.50/share: the inner
// min(cap, raw) picks the cap only when pct*price < commission_per_unit, so above that the
// expression collapsed to max(floor, raw) and the configured cap could not change a single
// output value.
//
// Real numbers from EQUITY_MR_PORTFOLIO, 2026-07-24..08-03, so the fixture cannot drift
// from what production produced: the cap should have bound on 5 of 15 executions and did
// not, a 21% over-charge ($15.00 vs $12.35) concentrated on the smallest clips.
// ===========================================================================

namespace {
// AAPL et al. are pre-registered by initialize_default_configs(), so these exercise the
// real production config rather than a synthetic one.
TransactionCostManager make_equity_tcm() {
    TransactionCostManager::Config cfg;
    return TransactionCostManager(cfg);
}
}  // namespace

TEST(EquityCommissionSchedule, CapSupersedesFloorOnATinyOrder) {
    auto tcm = make_equity_tcm();

    // The worst observed case: AMZN 2026-07-29, 0.081001 shares at $230.86 = $18.70 notional.
    //   raw  = 0.081001 * 0.005 = $0.000405
    //   cap  = 1% * 18.70       = $0.187
    //   floor                   = $1.00
    const double qty = 0.081001, px = 230.86;
    auto r = tcm.calculate_costs("AMZN", qty, px);

    EXPECT_NEAR(r.commissions_fees, 0.01 * qty * px, 1e-9)
        << "The 1% cap did not bind. Production charged $1.00 on an $18.70 trade -- 535 bps "
           "where the schedule caps at 100 bps. A maximum a minimum can exceed is not a "
           "maximum; the cap must be applied after the floor.";
    EXPECT_LT(r.commissions_fees, 1.0)
        << "Commission is still pinned at the $1.00 floor, i.e. the old clamp order.";
}

TEST(EquityCommissionSchedule, FloorAppliesWhenTheCapSitsAboveIt) {
    auto tcm = make_equity_tcm();

    // GOOGL 2026-07-24: 7.149959 shares at $317.69 = $2,271.47 notional.
    //   raw = $0.0357, cap = 1% * 2271.47 = $22.71, floor = $1.00  -> floor wins.
    auto r = tcm.calculate_costs("GOOGL", 7.149959, 317.69);

    EXPECT_NEAR(r.commissions_fees, 1.00, 1e-9)
        << "The $1.00 per-order minimum stopped applying on a normal-sized order. The cap "
           "must not be allowed to pull the charge below the floor when it sits above it.";
}

TEST(EquityCommissionSchedule, LargeOrderPaysThePerShareRate) {
    auto tcm = make_equity_tcm();

    // 10,000 shares at $300: raw = $50, floor $1.00, cap 1% * 3,000,000 = $30,000.
    // Neither bound applies -- the per-share rate is what is charged.
    auto r = tcm.calculate_costs("AAPL", 10000.0, 300.0);

    EXPECT_NEAR(r.commissions_fees, 50.0, 1e-9)
        << "A large order should pay the per-share rate, with neither bound binding.";
}

TEST(EquityCommissionSchedule, RegulatoryFeesAreNotChargedOnTopOfFixed) {
    auto tcm = make_equity_tcm();

    // AMZN 2026-08-03 sell: 16.590374 shares at $271.58 = $4,505.61.
    // Under Fixed the $0.005/share already includes exchange, clearing and regulatory fees,
    // so SEC ($0.0928 here) and FINRA TAF ($0.0032) must NOT be added.
    const double qty = 16.590374, px = 271.58;
    auto sell = tcm.calculate_costs("AMZN", -qty, px);
    auto buy = tcm.calculate_costs("AMZN", qty, px);

    EXPECT_NEAR(sell.commissions_fees, buy.commissions_fees, 1e-9)
        << "A sell was charged more than the identical buy, i.e. regulatory fees were added "
           "on top of an all-inclusive Fixed schedule. That is a double-charge. It would be "
           "correct under Tiered, where fees are passed through.";
}
