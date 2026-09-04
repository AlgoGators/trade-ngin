// E2-F1 regression pins.
//
// The optimizer/risk loop in PortfolioManager::process_market_data iterates until
// target positions are integral, applying the risk scale IN PLACE on every lap
// (portfolio_manager.cpp: `pos.quantity *= risk_result.recommended_scale`).
//
// Futures trade whole contracts, so the loop converges on lap 1 and the scale is
// applied exactly once -- the single application the design comment at that site
// reasons about. Equities with fractional shares can never satisfy an
// integrality test, so pre-fix the loop ran all 5 laps, compounded the scale
// (the gate is scale-invariant, so shrinking never satisfies it -- see E2-F2),
// and then force-rounded the decayed remainder to an integer, i.e. to zero.
//
// Observed in the first ever bt_equity_mr run: META target 2.42425398 decayed
// 2.424 -> 0.767 -> 0.243 -> 0.077 -> 0.024 -> rounded to 0. Across that run 220
// days hit max iterations and 107 of 448 forced roundings drove a nonzero target
// to exactly zero.
//
// These tests pin BOTH directions, because pinning only one is what let this
// survive: the false case is the futures invariant (must keep rounding), the
// true case is the equities fix (must keep the fraction).

#include <gtest/gtest.h>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>
#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"

#define private public
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#undef private

#include "trade_ngin/strategy/base_strategy.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

// Deterministic strategy that always targets the same fractional quantity.
// Deliberately NOT MockStrategy: that one calls rand(), so it cannot support an
// exact assertion about a quantity.
class FixedFractionalStrategy : public BaseStrategy {
public:
    FixedFractionalStrategy(std::string id, StrategyConfig config,
                            std::shared_ptr<DatabaseInterface> db, double qty)
        : BaseStrategy(std::move(id), std::move(config),
                       std::static_pointer_cast<trade_ngin::PostgresDatabase>(db)),
          qty_(qty) {
        metadata_.name = "Fixed Fractional Strategy";
    }

    Result<void> on_data(const std::vector<Bar>& data) override {
        auto base = BaseStrategy::on_data(data);
        if (base.is_error()) return base;
        for (const auto& bar : data) {
            Position pos;
            pos.symbol = bar.symbol;
            pos.quantity = Decimal(qty_);
            pos.average_price = bar.close;
            pos.last_update = bar.timestamp;
            positions_[bar.symbol] = pos;
            last_signals_[bar.symbol] = 1.0;
        }
        return Result<void>();
    }

    std::unordered_map<std::string, Position> get_target_positions() const override {
        return positions_;
    }

private:
    double qty_;
};

constexpr double kFractionalQty = 2.42425398;  // the observed META target

std::vector<Bar> flat_bars(const std::string& symbol, int n,
                           std::chrono::system_clock::time_point t0) {
    std::vector<Bar> v;
    for (int i = 0; i < n; ++i) {
        Bar b;
        b.symbol = symbol;
        b.timestamp = t0 + std::chrono::hours(24 * i);
        b.open = b.close = Decimal(100.0);
        b.high = Decimal(101.0);
        b.low = Decimal(99.0);
        b.volume = 100000.0;
        v.push_back(b);
    }
    return v;
}

PortfolioConfig config_with(bool allow_fractional) {
    PortfolioConfig c{1'000'000.0, 100'000.0, 1.0, 0.0, /*optimization=*/false,
                      /*risk=*/false};
    c.allow_fractional_positions = allow_fractional;
    c.risk_config.capital = 1'000'000.0;
    return c;
}

class FractionalConvergenceTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        ASSERT_TRUE(db_->connect().is_ok());
    }

    void TearDown() override {
        db_.reset();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        TestBase::TearDown();
    }

    // Runs one process cycle and returns the quantity the loop settled on.
    double settled_quantity(bool allow_fractional) {
        static int n = 0;
        const std::string uid = "FRAC_S_" + std::to_string(++n);
        auto manager = std::make_unique<PortfolioManager>(config_with(allow_fractional),
                                                          "PM_FRAC_" + std::to_string(n));

        StrategyConfig sc;
        sc.capital_allocation = 1'000'000.0;
        sc.max_leverage = 2.0;
        sc.asset_classes = {AssetClass::EQUITIES};
        sc.frequencies = {DataFrequency::DAILY};
        sc.trading_params["AAPL"] = 1.0;
        sc.position_limits["AAPL"] = 10000.0;

        auto strat = std::make_shared<FixedFractionalStrategy>(uid, sc, db_, kFractionalQty);
        EXPECT_TRUE(strat->initialize().is_ok());
        EXPECT_TRUE(strat->start().is_ok());
        EXPECT_TRUE(manager->add_strategy(strat, 1.0, false, false).is_ok());

        auto t0 = std::chrono::system_clock::now() - std::chrono::hours(24 * 30);
        EXPECT_TRUE(manager->process_market_data(flat_bars("AAPL", 25, t0)).is_ok());

        const auto& info = manager->strategies_.at(uid);
        auto it = info.target_positions.find("AAPL");
        if (it == info.target_positions.end()) return 0.0;
        return static_cast<double>(it->second.quantity);
    }

    std::shared_ptr<MockPostgresDatabase> db_;
};

}  // namespace

// The equities fix: a fractional target is a legitimate end state and must
// survive the loop intact. Pre-fix this position was force-rounded away.
TEST_F(FractionalConvergenceTest, FractionalTargetSurvivesWhenPortfolioPermitsIt) {
    const double q = settled_quantity(/*allow_fractional=*/true);

    EXPECT_NEAR(q, kFractionalQty, 1e-9)
        << "A permitted fractional target was altered by the optimizer/risk loop. "
           "If it came back integral the terminal force-rounding block ran, which "
           "means the loop did not treat the fraction as converged.";

    // State the property directly, not just the value: the fraction must remain.
    EXPECT_GT(std::abs(q - std::round(q)), 1e-6)
        << "Target came back integral; allow_fractional_positions was not honoured.";
}

// The futures invariant. This is the half that must NOT change: whole-contract
// portfolios leave the flag false and must still be driven to integers.
TEST_F(FractionalConvergenceTest, IntegralityStillEnforcedWhenFractionsAreNotPermitted) {
    const double q = settled_quantity(/*allow_fractional=*/false);

    EXPECT_LT(std::abs(q - std::round(q)), 1e-6)
        << "A portfolio that does NOT permit fractional positions returned a "
           "fractional quantity. The futures integrality guarantee has been broken.";
}

// The two paths must genuinely differ, or both assertions above could be passing
// for a reason unrelated to the flag (e.g. the loop never ran at all).
TEST_F(FractionalConvergenceTest, TheFlagIsWhatDecidesTheOutcome) {
    const double permitted = settled_quantity(true);
    const double forbidden = settled_quantity(false);

    EXPECT_NE(permitted, forbidden)
        << "allow_fractional_positions changed nothing. Both paths agreeing means "
           "the flag is not wired into the convergence test, so this suite would "
           "not catch a regression of E2-F1.";
}
