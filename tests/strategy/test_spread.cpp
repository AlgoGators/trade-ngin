#include <gtest/gtest.h>
#include <cmath>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>
#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "trade_ngin/strategy/spread.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

constexpr const char* kSymbolA = "AAA";
constexpr const char* kSymbolB = "BBB";

// Leg B is a deterministic smooth random-walk stand-in; leg A tracks it at a
// known ratio with a slow sinusoidal dislocation layered on top, so the pair is
// cointegrated with beta ~= 2 in log space and the spread reverts on a known
// period.
struct SyntheticPair {
    double price_a;
    double price_b;
};

constexpr double kTrueBeta = 2.0;

double leg_b_price(int i) {
    return 50.0 * std::exp(0.0004 * i + 0.12 * std::sin(0.11 * i));
}

// A pair with no dislocation at all: log(Pa) = log(k) + kTrueBeta*log(Pb), so
// rolling OLS must recover kTrueBeta to floating-point precision over any
// window. Used for the estimator tests, where an approximate answer would hide
// a real error in the regression.
SyntheticPair make_exact_pair(int i) {
    const double base_b = leg_b_price(i);
    const double price_a = std::exp(std::log(0.01) + kTrueBeta * std::log(base_b));
    return {price_a, base_b};
}

// The same pair with a slow sinusoidal dislocation layered onto the spread, so
// the spread is mean-reverting on a known period. Used for the trading-behaviour
// tests. The dislocation is not orthogonal to log(Pb) over a short window, so
// the fitted beta here sits near but not exactly at kTrueBeta -- which is why
// the estimator tests use make_exact_pair() instead.
SyntheticPair make_pair(int i, double spread_amplitude = 0.03, double spread_period = 40.0) {
    const double base_b = leg_b_price(i);
    const double dislocation = spread_amplitude * std::sin(2.0 * M_PI * i / spread_period);
    const double price_a =
        std::exp(std::log(0.01) + kTrueBeta * std::log(base_b) + dislocation);
    return {price_a, base_b};
}

Timestamp day(int i) {
    return std::chrono::system_clock::time_point{} + std::chrono::hours(24 * (i + 1));
}

Bar make_bar(const std::string& symbol, Timestamp ts, double close) {
    return Bar(ts, close, close, close, close, 1000.0, symbol);
}

// First bar at which z can exist: beta is first estimated on bar beta_period - 1, and z needs
// zscore_period estimated spreads.
int earliest_z_bar(const SpreadConfig& c) { return c.beta_period + c.zscore_period - 2; }

}  // namespace

class SpreadStrategyTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();

        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        ASSERT_TRUE(db_->connect().is_ok());

        strategy_config_ = StrategyConfig();
        strategy_config_.capital_allocation = 1000000.0;
        strategy_config_.max_leverage = 2.0;
        strategy_config_.asset_classes = {AssetClass::EQUITIES};
        strategy_config_.frequencies = {DataFrequency::DAILY};

        spread_config_ = SpreadConfig();
        spread_config_.symbol_a = kSymbolA;
        spread_config_.symbol_b = kSymbolB;
        spread_config_.spread_type = SpreadType::LOG_RATIO;
        spread_config_.beta_period = 30;
        spread_config_.zscore_period = 20;
        spread_config_.entry_z = 1.5;
        spread_config_.exit_z = 0.3;
        spread_config_.stop_z = 4.0;
        spread_config_.capital_per_leg_pct = 0.10;
        spread_config_.min_holding_period = 1;
    }

    void TearDown() override {
        if (strategy_) {
            strategy_->stop();
            strategy_.reset();
        }
        if (db_) {
            db_->disconnect();
            db_.reset();
        }
        TestBase::TearDown();
    }

    // Build and start a strategy with the current configs.
    void build() {
        static int test_id = 0;
        strategy_ = std::make_shared<SpreadStrategy>(
            "TEST_SPREAD_" + std::to_string(++test_id), strategy_config_, spread_config_, db_);
        ASSERT_TRUE(strategy_->initialize().is_ok());
        ASSERT_TRUE(strategy_->start().is_ok());
    }

    // Feed one aligned pair of bars.
    void feed(int i, double price_a, double price_b) {
        std::vector<Bar> bars{make_bar(kSymbolA, day(i), price_a),
                              make_bar(kSymbolB, day(i), price_b)};
        ASSERT_TRUE(strategy_->on_data(bars).is_ok());
    }

    void feed_synthetic(int i) {
        const auto p = make_pair(i);
        feed(i, p.price_a, p.price_b);
    }

    double qty(const std::string& symbol) const {
        const auto& positions = strategy_->get_positions();
        auto it = positions.find(symbol);
        return it == positions.end() ? 0.0 : it->second.quantity.as_double();
    }

    std::shared_ptr<MockPostgresDatabase> db_;
    std::shared_ptr<SpreadStrategy> strategy_;
    StrategyConfig strategy_config_;
    SpreadConfig spread_config_;
};

// --- Configuration validation -------------------------------------------------

TEST_F(SpreadStrategyTest, RejectsIdenticalLegs) {
    spread_config_.symbol_b = kSymbolA;
    auto strategy = std::make_shared<SpreadStrategy>("BAD_SAME", strategy_config_, spread_config_, db_);
    EXPECT_TRUE(strategy->initialize().is_error());
}

TEST_F(SpreadStrategyTest, RejectsEmptyLeg) {
    spread_config_.symbol_b = "";
    auto strategy = std::make_shared<SpreadStrategy>("BAD_EMPTY", strategy_config_, spread_config_, db_);
    EXPECT_TRUE(strategy->initialize().is_error());
}

TEST_F(SpreadStrategyTest, RejectsEntryInsideExitBand) {
    spread_config_.entry_z = 0.2;
    spread_config_.exit_z = 0.5;
    auto strategy = std::make_shared<SpreadStrategy>("BAD_Z", strategy_config_, spread_config_, db_);
    EXPECT_TRUE(strategy->initialize().is_error());
}

TEST_F(SpreadStrategyTest, RejectsStopInsideEntryBand) {
    spread_config_.stop_z = 1.0;
    spread_config_.entry_z = 2.0;
    auto strategy = std::make_shared<SpreadStrategy>("BAD_STOP", strategy_config_, spread_config_, db_);
    EXPECT_TRUE(strategy->initialize().is_error());
}

TEST_F(SpreadStrategyTest, RejectsOutOfRangeCapitalPerLeg) {
    spread_config_.capital_per_leg_pct = 1.5;
    auto strategy = std::make_shared<SpreadStrategy>("BAD_CAP", strategy_config_, spread_config_, db_);
    EXPECT_TRUE(strategy->initialize().is_error());
}

TEST_F(SpreadStrategyTest, AcceptsSeedConfigWithOnlySymbols) {
    SpreadConfig seed;
    seed.symbol_a = kSymbolA;
    seed.symbol_b = kSymbolB;
    auto strategy = std::make_shared<SpreadStrategy>("SEED", strategy_config_, seed, db_);
    EXPECT_TRUE(strategy->initialize().is_ok());
}

// --- Rolling OLS beta ---------------------------------------------------------

TEST_F(SpreadStrategyTest, RollingBetaRecoversExactHedgeRatio) {
    build();
    for (int i = 0; i < 120; ++i) {
        const auto p = make_exact_pair(i);
        feed(i, p.price_a, p.price_b);
    }

    // With no dislocation the regression is exact, so this pins the estimator
    // rather than merely checking that it lands in the neighbourhood.
    EXPECT_NEAR(strategy_->get_beta(), kTrueBeta, 1e-6);
}

TEST_F(SpreadStrategyTest, RollingBetaTracksADislocatedPair) {
    build();
    for (int i = 0; i < 120; ++i) {
        feed_synthetic(i);
    }

    // The dislocation biases a short-window fit, but the estimate must stay in
    // the right region rather than drifting off or blowing up.
    EXPECT_NEAR(strategy_->get_beta(), kTrueBeta, 0.75);
    EXPECT_TRUE(std::isfinite(strategy_->get_beta()));
}

TEST_F(SpreadStrategyTest, BetaHoldsSeedValueBeforeWindowFills) {
    spread_config_.initial_beta = 1.0;
    build();

    // One short of the OLS window.
    for (int i = 0; i < spread_config_.beta_period - 1; ++i) {
        feed_synthetic(i);
    }
    EXPECT_DOUBLE_EQ(strategy_->get_beta(), 1.0);
    // The seed is only what get_beta() reports: it never trades.
    EXPECT_FALSE(strategy_->get_state_snapshot().zscore_valid);
    EXPECT_EQ(strategy_->get_direction(), 0);

    feed_synthetic(spread_config_.beta_period - 1);
    EXPECT_NE(strategy_->get_beta(), 1.0);
    EXPECT_TRUE(strategy_->get_state_snapshot().beta_estimated);
}

TEST_F(SpreadStrategyTest, BetaIsHeldWhenHedgeLegIsFlat) {
    build();
    // Leg B never moves, so var(B) is zero and beta must not blow up.
    for (int i = 0; i < 80; ++i) {
        feed(i, 100.0 + 0.5 * std::sin(0.2 * i), 50.0);
    }
    EXPECT_DOUBLE_EQ(strategy_->get_beta(), spread_config_.initial_beta);
    EXPECT_TRUE(std::isfinite(strategy_->get_beta()));
    // Beta is never estimated, so nothing ever opens.
    EXPECT_FALSE(strategy_->get_state_snapshot().beta_estimated);
    EXPECT_FALSE(strategy_->get_state_snapshot().zscore_valid);
    EXPECT_EQ(strategy_->get_direction(), 0);
    EXPECT_DOUBLE_EQ(qty(kSymbolA), 0.0);
    EXPECT_DOUBLE_EQ(qty(kSymbolB), 0.0);
}

// --- Warmup -------------------------------------------------------------------

TEST_F(SpreadStrategyTest, NoPositionDuringWarmup) {
    build();
    // Asserted at every bar, not after the loop: the parent opens at bar 23, stops at bar 29 and
    // is flat again by bar 47, so a check after the loop would pass on it and prove nothing.
    for (int i = 0; i < earliest_z_bar(spread_config_); ++i) {
        feed_synthetic(i);
        ASSERT_EQ(strategy_->get_direction(), 0) << "bar " << i;
        ASSERT_DOUBLE_EQ(qty(kSymbolA), 0.0) << "bar " << i;
        ASSERT_DOUBLE_EQ(qty(kSymbolB), 0.0) << "bar " << i;
    }
}

// --- Entry / exit -------------------------------------------------------------

TEST_F(SpreadStrategyTest, EntersOnFirstBarBeyondEntryThreshold) {
    build();

    int entry_bar = -1;
    for (int i = 0; i < 200 && entry_bar < 0; ++i) {
        feed_synthetic(i);
        if (strategy_->get_direction() != 0) {
            entry_bar = i;
        }
    }

    ASSERT_GE(entry_bar, 0) << "strategy never opened a position on a mean-reverting pair";
    // Entry only once z can exist: after beta is estimated and the z window has filled.
    EXPECT_GE(entry_bar, earliest_z_bar(spread_config_));
    // And the bar it fired on must actually be beyond the entry threshold.
    EXPECT_GE(std::abs(strategy_->get_zscore()), spread_config_.entry_z);
}

TEST_F(SpreadStrategyTest, LegsAreOppositeSignedAndNonZeroWhenOpen) {
    build();

    for (int i = 0; i < 200; ++i) {
        feed_synthetic(i);
        if (strategy_->get_direction() != 0) {
            break;
        }
    }

    ASSERT_NE(strategy_->get_direction(), 0);
    const double a = qty(kSymbolA);
    const double b = qty(kSymbolB);
    EXPECT_NE(a, 0.0);
    EXPECT_NE(b, 0.0);
    EXPECT_LT(a * b, 0.0) << "spread legs must point in opposite directions";
}

TEST_F(SpreadStrategyTest, LongSpreadWhenZScoreIsNegative) {
    build();

    for (int i = 0; i < 200; ++i) {
        feed_synthetic(i);
        if (strategy_->get_direction() != 0) {
            break;
        }
    }

    ASSERT_NE(strategy_->get_direction(), 0);
    // The sign of z decides the side: low z buys A and sells B. Checked directly, so a swap of
    // the two returns in target_direction() fails here.
    const double z_at_entry = strategy_->get_state_snapshot().current_zscore;
    EXPECT_EQ(strategy_->get_direction() > 0, z_at_entry < 0.0) << "z " << z_at_entry;
    if (strategy_->get_direction() > 0) {
        // Long spread: long A, short B, entered on a negative z-score.
        EXPECT_GT(qty(kSymbolA), 0.0);
        EXPECT_LT(qty(kSymbolB), 0.0);
    } else {
        EXPECT_LT(qty(kSymbolA), 0.0);
        EXPECT_GT(qty(kSymbolB), 0.0);
    }
}

TEST_F(SpreadStrategyTest, FlattensWhenSpreadReverts) {
    build();

    int entry_bar = -1;
    for (int i = 0; i < 200 && entry_bar < 0; ++i) {
        feed_synthetic(i);
        if (strategy_->get_direction() != 0) {
            entry_bar = i;
        }
    }
    ASSERT_GE(entry_bar, 0);

    int exit_bar = -1;
    for (int i = entry_bar + 1; i < 400 && exit_bar < 0; ++i) {
        feed_synthetic(i);
        if (strategy_->get_direction() == 0) {
            exit_bar = i;
        }
    }

    ASSERT_GE(exit_bar, 0) << "strategy never closed the position as the spread reverted";
    EXPECT_GT(exit_bar, entry_bar);
    // A reversion exit, not a stop: z is inside the exit band and the stop block is not set.
    const auto& at_exit = strategy_->get_state_snapshot();
    EXPECT_LE(std::abs(at_exit.current_zscore), spread_config_.exit_z) << "z " << at_exit.current_zscore;
    EXPECT_FALSE(at_exit.stopped_out);
    EXPECT_DOUBLE_EQ(qty(kSymbolA), 0.0);
    EXPECT_DOUBLE_EQ(qty(kSymbolB), 0.0);
}

TEST_F(SpreadStrategyTest, RespectsMinimumHoldingPeriod) {
    spread_config_.min_holding_period = 10;
    // Put the stop out of reach so only the reversion exit can fire; the stop
    // deliberately ignores the minimum holding period (see StopOverridesMinimumHolding).
    spread_config_.stop_z = 50.0;
    build();

    int entry_bar = -1;
    for (int i = 0; i < 200 && entry_bar < 0; ++i) {
        feed_synthetic(i);
        if (strategy_->get_direction() != 0) {
            entry_bar = i;
        }
    }
    ASSERT_GE(entry_bar, 0);

    int exit_bar = -1;
    for (int i = entry_bar + 1; i < 400 && exit_bar < 0; ++i) {
        feed_synthetic(i);
        if (strategy_->get_direction() == 0) {
            exit_bar = i;
        }
    }

    ASSERT_GE(exit_bar, 0);
    EXPECT_GE(exit_bar - entry_bar, spread_config_.min_holding_period);
}

// --- Stop-out -----------------------------------------------------------------

TEST_F(SpreadStrategyTest, StopOverridesMinimumHolding) {
    // A blown-out spread must be exited even inside the minimum holding period:
    // the holding rule exists to avoid churn, not to force a losing position open.
    spread_config_.min_holding_period = 50;
    spread_config_.stop_z = 3.0;
    build();

    int entry_bar = -1;
    for (int i = 0; i < 200 && entry_bar < 0; ++i) {
        feed_synthetic(i);
        if (strategy_->get_direction() != 0) {
            entry_bar = i;
        }
    }
    ASSERT_GE(entry_bar, 0);

    const auto p = make_pair(entry_bar + 1);
    const double shocked_a = p.price_a * (strategy_->get_zscore() < 0 ? 0.55 : 1.80);
    feed(entry_bar + 1, shocked_a, p.price_b);

    ASSERT_GE(std::abs(strategy_->get_zscore()), spread_config_.stop_z);
    EXPECT_EQ(strategy_->get_direction(), 0)
        << "stop did not fire inside the minimum holding period";
    EXPECT_DOUBLE_EQ(qty(kSymbolA), 0.0);
    EXPECT_DOUBLE_EQ(qty(kSymbolB), 0.0);
}

TEST_F(SpreadStrategyTest, StopsOutOnSpreadShock) {
    spread_config_.stop_z = 3.0;
    build();

    int entry_bar = -1;
    for (int i = 0; i < 200 && entry_bar < 0; ++i) {
        feed_synthetic(i);
        if (strategy_->get_direction() != 0) {
            entry_bar = i;
        }
    }
    ASSERT_GE(entry_bar, 0);

    // Shock leg A far away from its hedge, well past the stop threshold.
    const auto p = make_pair(entry_bar + 1);
    const double shocked_a =
        p.price_a * (strategy_->get_zscore() < 0 ? 0.55 : 1.80);
    feed(entry_bar + 1, shocked_a, p.price_b);

    EXPECT_GE(std::abs(strategy_->get_zscore()), spread_config_.stop_z);
    EXPECT_EQ(strategy_->get_direction(), 0);
    EXPECT_DOUBLE_EQ(qty(kSymbolA), 0.0);
    EXPECT_DOUBLE_EQ(qty(kSymbolB), 0.0);
}

TEST_F(SpreadStrategyTest, DoesNotReenterImmediatelyAfterStop) {
    spread_config_.stop_z = 3.0;
    build();

    int entry_bar = -1;
    for (int i = 0; i < 200 && entry_bar < 0; ++i) {
        feed_synthetic(i);
        if (strategy_->get_direction() != 0) {
            entry_bar = i;
        }
    }
    ASSERT_GE(entry_bar, 0);

    const auto p = make_pair(entry_bar + 1);
    const double direction = strategy_->get_zscore() < 0 ? 0.55 : 1.80;
    feed(entry_bar + 1, p.price_a * direction, p.price_b);
    ASSERT_EQ(strategy_->get_direction(), 0);

    // Hold the dislocation: |z| is still extreme, so re-entry must stay blocked.
    for (int k = 2; k <= 4; ++k) {
        const auto q = make_pair(entry_bar + k);
        feed(entry_bar + k, q.price_a * direction, q.price_b);
        EXPECT_EQ(strategy_->get_direction(), 0)
            << "re-entered while still stopped out, at offset " << k;
    }
}

// --- Bar alignment ------------------------------------------------------------

TEST_F(SpreadStrategyTest, SingleLegBarsNeverProduceATrade) {
    build();
    // Only leg A ever reports: there is no pair, so nothing can be computed.
    for (int i = 0; i < 200; ++i) {
        std::vector<Bar> bars{make_bar(kSymbolA, day(i), make_pair(i).price_a)};
        ASSERT_TRUE(strategy_->on_data(bars).is_ok());
    }

    EXPECT_EQ(strategy_->get_direction(), 0);
    EXPECT_DOUBLE_EQ(qty(kSymbolA), 0.0);
    EXPECT_DOUBLE_EQ(qty(kSymbolB), 0.0);
    EXPECT_EQ(strategy_->get_state_snapshot().observations, 0u);
}

TEST_F(SpreadStrategyTest, LegsArrivingInSeparateCallsStillAlign) {
    build();
    for (int i = 0; i < 60; ++i) {
        const auto p = make_exact_pair(i);
        std::vector<Bar> a{make_bar(kSymbolA, day(i), p.price_a)};
        std::vector<Bar> b{make_bar(kSymbolB, day(i), p.price_b)};
        ASSERT_TRUE(strategy_->on_data(a).is_ok());
        ASSERT_TRUE(strategy_->on_data(b).is_ok());
    }

    // Splitting the legs across two calls must give exactly the same state as
    // delivering them together.
    EXPECT_EQ(strategy_->get_state_snapshot().observations, 60u);
    EXPECT_NEAR(strategy_->get_beta(), kTrueBeta, 1e-6);
}

TEST_F(SpreadStrategyTest, MismatchedTimestampsDoNotPair) {
    build();
    // Leg B is always one day behind leg A, so no timestamp ever has both legs.
    for (int i = 0; i < 100; ++i) {
        std::vector<Bar> bars{make_bar(kSymbolA, day(2 * i), make_pair(i).price_a),
                              make_bar(kSymbolB, day(2 * i + 1), make_pair(i).price_b)};
        ASSERT_TRUE(strategy_->on_data(bars).is_ok());
    }

    EXPECT_EQ(strategy_->get_state_snapshot().observations, 0u);
    EXPECT_EQ(strategy_->get_direction(), 0);
}

TEST_F(SpreadStrategyTest, UnrelatedSymbolsAreIgnored) {
    build();
    for (int i = 0; i < 60; ++i) {
        const auto p = make_pair(i);
        std::vector<Bar> bars{make_bar("ZZZ", day(i), 10.0),
                              make_bar(kSymbolA, day(i), p.price_a),
                              make_bar("QQQ", day(i), 20.0),
                              make_bar(kSymbolB, day(i), p.price_b)};
        ASSERT_TRUE(strategy_->on_data(bars).is_ok());
    }

    EXPECT_EQ(strategy_->get_state_snapshot().observations, 60u);
    const auto& positions = strategy_->get_positions();
    EXPECT_EQ(positions.count("ZZZ"), 0u);
    EXPECT_EQ(positions.count("QQQ"), 0u);
}

// --- Sizing -------------------------------------------------------------------

TEST_F(SpreadStrategyTest, LogRatioSizesLegBByBetaWeightedNotional) {
    build();

    for (int i = 0; i < 200; ++i) {
        feed_synthetic(i);
        if (strategy_->get_direction() != 0) {
            break;
        }
    }
    ASSERT_NE(strategy_->get_direction(), 0);

    const auto& s = strategy_->get_state_snapshot();
    const double notional = strategy_config_.capital_allocation * spread_config_.capital_per_leg_pct;
    // LOG_RATIO hedges in proportional terms: leg B carries beta x leg A's notional.
    EXPECT_NEAR(std::abs(qty(kSymbolA)) * s.last_price_a, notional, notional * 1e-6);
    EXPECT_NEAR(std::abs(qty(kSymbolB)) * s.last_price_b, s.beta * notional, notional * 1e-6);
}

TEST_F(SpreadStrategyTest, PriceDiffSizesLegBByBetaWeightedShares) {
    spread_config_.spread_type = SpreadType::PRICE_DIFF;
    spread_config_.min_beta = 1e-6;  // PRICE_DIFF has no default bounds
    spread_config_.max_beta = 1e9;
    build();

    for (int i = 0; i < 300; ++i) {
        feed_synthetic(i);
        if (strategy_->get_direction() != 0) {
            break;
        }
    }
    ASSERT_NE(strategy_->get_direction(), 0)
        << "PRICE_DIFF spread never opened a position";

    const auto& s = strategy_->get_state_snapshot();
    // PRICE_DIFF hedges in unit terms: leg B carries beta x leg A's share count.
    EXPECT_NEAR(std::abs(qty(kSymbolB)), s.beta * std::abs(qty(kSymbolA)),
                std::abs(qty(kSymbolA)) * 1e-6);
}

TEST_F(SpreadStrategyTest, BothSpreadTypesTradeTheSamePair) {
    // The two spread constructions are different formulations of the same idea;
    // both should produce trades on a cointegrated pair.
    for (auto type : {SpreadType::LOG_RATIO, SpreadType::PRICE_DIFF}) {
        spread_config_.spread_type = type;
        if (type == SpreadType::PRICE_DIFF) {
            spread_config_.min_beta = 1e-6;  // PRICE_DIFF has no default bounds
            spread_config_.max_beta = 1e9;
        }
        build();

        bool traded = false;
        for (int i = 0; i < 300 && !traded; ++i) {
            feed_synthetic(i);
            traded = strategy_->get_direction() != 0;
        }
        EXPECT_TRUE(traded) << "no trade for spread type "
                            << (type == SpreadType::LOG_RATIO ? "LOG_RATIO" : "PRICE_DIFF");
        strategy_->stop();
        strategy_.reset();
    }
}

// --- Robustness ---------------------------------------------------------------

TEST_F(SpreadStrategyTest, NonPositivePriceIsSkippedUnderLogTransform) {
    build();
    for (int i = 0; i < 40; ++i) {
        feed_synthetic(i);
    }
    const size_t before = strategy_->get_state_snapshot().observations;

    feed(40, -5.0, make_pair(40).price_b);

    EXPECT_EQ(strategy_->get_state_snapshot().observations, before)
        << "a non-positive price must not enter the log-spread series";
    EXPECT_TRUE(std::isfinite(strategy_->get_beta()));
}

TEST_F(SpreadStrategyTest, FlatSpreadProducesNoSignal) {
    build();
    // A perfectly constant ratio means zero spread variance and no z-score.
    for (int i = 0; i < 100; ++i) {
        feed(i, 100.0, 50.0);
    }

    EXPECT_EQ(strategy_->get_direction(), 0);
    EXPECT_DOUBLE_EQ(qty(kSymbolA), 0.0);
    EXPECT_DOUBLE_EQ(qty(kSymbolB), 0.0);
}

TEST_F(SpreadStrategyTest, HistoryIsBoundedByMaxHistory) {
    spread_config_.max_history = 64;
    build();
    for (int i = 0; i < 400; ++i) {
        feed_synthetic(i);
    }

    const auto& s = strategy_->get_state_snapshot();
    EXPECT_LE(s.a_history.size(), spread_config_.max_history);
    EXPECT_LE(s.b_history.size(), spread_config_.max_history);
    EXPECT_LE(s.spread_history.size(), spread_config_.max_history);
    EXPECT_EQ(s.observations, 400u);
}

TEST_F(SpreadStrategyTest, ReportedPriceHistoryIsRawPrices) {
    build();
    for (int i = 0; i < 40; ++i) {
        feed_synthetic(i);
    }

    auto history = strategy_->get_price_history();
    ASSERT_EQ(history.count(kSymbolA), 1u);
    ASSERT_FALSE(history[kSymbolA].empty());
    // Under LOG_RATIO the internal series is log prices; the accessor must undo that.
    EXPECT_NEAR(history[kSymbolA].back(), make_pair(39).price_a,
                make_pair(39).price_a * 1e-6);
    EXPECT_NEAR(history[kSymbolB].back(), make_pair(39).price_b,
                make_pair(39).price_b * 1e-6);
}

TEST_F(SpreadStrategyTest, NoTradingWhileStrategyIsNotRunning) {
    static int id = 0;
    strategy_ = std::make_shared<SpreadStrategy>("NOT_RUNNING_" + std::to_string(++id),
                                                 strategy_config_, spread_config_, db_);
    ASSERT_TRUE(strategy_->initialize().is_ok());
    // Deliberately not started.

    for (int i = 0; i < 100; ++i) {
        const auto p = make_pair(i);
        std::vector<Bar> bars{make_bar(kSymbolA, day(i), p.price_a),
                              make_bar(kSymbolB, day(i), p.price_b)};
        ASSERT_TRUE(strategy_->on_data(bars).is_ok());
    }

    EXPECT_EQ(strategy_->get_state_snapshot().observations, 0u);
    EXPECT_EQ(strategy_->get_direction(), 0);
}

// =============================================================================================
// Issue #100: the fixes before the layer work.
// Every test below fails on d84c9684 (the parent) and passes with the fixes; the PR records, for
// each fix, the tests that fail when only that fix is reverted.
// =============================================================================================

namespace {

// An independent, long-hand model of the fixed strategy. It shares no code with spread.cpp: it
// keeps every price (no cap), re-forms the z window from scratch every bar, and counts estimated
// bars with its own counter. The randomized tests hold the strategy to it bar by bar.
struct RefSpread {
    SpreadConfig c;
    double lo{0.1};
    double hi{10.0};
    std::vector<double> a, b;
    double beta{1.0};
    bool estimated{false};
    size_t est_bars{0};
    int dir{0};
    int hold{0};
    bool stopped{false};
    bool zv{false};
    double z{0.0};
    int breaches{0};

    explicit RefSpread(const SpreadConfig& cfg) : c(cfg) {
        if (c.min_beta) lo = *c.min_beta;
        if (c.max_beta) hi = *c.max_beta;
    }

    std::optional<double> ols() const {
        const size_t bp = static_cast<size_t>(c.beta_period);
        if (a.size() < bp) return std::nullopt;
        double ma = 0, mb = 0;
        for (size_t k = a.size() - bp; k < a.size(); ++k) { ma += a[k]; mb += b[k]; }
        ma /= bp; mb /= bp;
        double cov = 0, vb = 0;
        for (size_t k = a.size() - bp; k < a.size(); ++k) {
            cov += (a[k] - ma) * (b[k] - mb);
            vb += (b[k] - mb) * (b[k] - mb);
        }
        if (vb < 1e-12) return std::nullopt;
        const double r = cov / vb;
        return std::isfinite(r) ? std::optional<double>(r) : std::nullopt;
    }

    int target() const {
        const double az = std::abs(z);
        if (dir != 0) {
            if (az >= c.stop_z) return 0;
            if (az <= c.exit_z && hold >= c.min_holding_period) return 0;
            return dir;
        }
        if (stopped || az >= c.stop_z) return 0;
        if (z <= -c.entry_z) return 1;
        if (z >= c.entry_z) return -1;
        return 0;
    }

    void bar(double pa, double pb) {
        const bool lg = c.spread_type == SpreadType::LOG_RATIO;
        a.push_back(lg ? std::log(pa) : pa);
        b.push_back(lg ? std::log(pb) : pb);
        if (auto nb = ols()) {
            if (*nb < lo || *nb > hi) {
                zv = false;
                ++breaches;
                if (dir != 0 && c.flatten_on_beta_breach) { dir = 0; hold = 0; }
                return;
            }
            beta = *nb;
            estimated = true;
        }
        if (!estimated) { zv = false; return; }
        ++est_bars;
        const size_t zp = static_cast<size_t>(c.zscore_period);
        if (est_bars < zp) { zv = false; return; }
        std::vector<double> w;
        for (size_t k = a.size() - zp; k < a.size(); ++k) w.push_back(a[k] - beta * b[k]);
        double m = 0;
        for (double v : w) m += v;
        m /= zp;
        double ss = 0;
        for (double v : w) ss += (v - m) * (v - m);
        const double sd = std::sqrt(ss / (zp - 1));
        if (!std::isfinite(sd) || sd < 1e-12) { zv = false; return; }
        z = (w.back() - m) / sd;
        zv = true;
        if (stopped && std::abs(z) <= c.exit_z) stopped = false;
        if (dir != 0) ++hold;
        const int prev = dir;
        const int d = target();
        if (d == prev) return;
        if (d == 0) {
            stopped = std::abs(z) >= c.stop_z;
            dir = 0;
            hold = 0;
        } else {
            dir = d;
            hold = 0;
        }
    }
};

// What a state snapshot must agree on between two ways of delivering the same bars.
struct Snap {
    int direction;
    int holding;
    bool stopped;
    bool zvalid;
    double z;
    double beta;
    bool legs_open;
    size_t observations;
    size_t estimated_bars;
    bool beta_estimated;
};

Snap snap_of(const SpreadStrategy& s) {
    const auto& st = s.get_state_snapshot();
    return {st.direction, st.holding_period, st.stopped_out, st.zscore_valid, st.current_zscore,
            st.beta, st.quantity_a != 0.0 || st.quantity_b != 0.0, st.observations,
            st.estimated_bars, st.beta_estimated};
}

void expect_same(const Snap& x, const Snap& y, const std::string& what) {
    EXPECT_EQ(x.direction, y.direction) << what;
    EXPECT_EQ(x.holding, y.holding) << what;
    EXPECT_EQ(x.stopped, y.stopped) << what;
    EXPECT_EQ(x.zvalid, y.zvalid) << what;
    EXPECT_DOUBLE_EQ(x.z, y.z) << what;
    EXPECT_DOUBLE_EQ(x.beta, y.beta) << what;
    EXPECT_EQ(x.legs_open, y.legs_open) << what;
    EXPECT_EQ(x.observations, y.observations) << what;
    EXPECT_EQ(x.estimated_bars, y.estimated_bars) << what;
    EXPECT_EQ(x.beta_estimated, y.beta_estimated) << what;
}

// A random cointegrated-ish pair with occasional shocks, from a seed.
std::vector<std::pair<double, double>> random_pair(uint64_t seed, int n, double true_beta,
                                                    double noise, double shock_prob) {
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> N(0.0, 1.0);
    std::uniform_real_distribution<double> U(0.0, 1.0);
    std::vector<std::pair<double, double>> out;
    double lb = std::log(50.0);
    const double amp = 0.01 + 0.04 * U(rng);
    const double per = 15.0 + 60.0 * U(rng);
    for (int i = 0; i < n; ++i) {
        lb += 0.0004 + 0.01 * N(rng);
        double la = std::log(0.01) + true_beta * lb + amp * std::sin(2.0 * M_PI * i / per) +
                    noise * N(rng);
        if (U(rng) < shock_prob) la += (U(rng) < 0.5 ? -1.0 : 1.0) * (0.05 + 0.15 * U(rng));
        out.emplace_back(std::exp(la), std::exp(lb));
    }
    return out;
}

}  // namespace

// --- Item 2: no spread value from an estimated beta until the estimate exists ----------------

TEST_F(SpreadStrategyTest, NoTradeBeforeBetaIsEstimated) {
    build();
    for (int i = 0; i < spread_config_.beta_period - 1; ++i) {
        feed_synthetic(i);
        const auto& s = strategy_->get_state_snapshot();
        ASSERT_EQ(s.direction, 0) << "bar " << i;
        ASSERT_DOUBLE_EQ(s.quantity_a, 0.0) << "bar " << i;
        ASSERT_DOUBLE_EQ(s.quantity_b, 0.0) << "bar " << i;
        ASSERT_FALSE(s.zscore_valid) << "bar " << i;
        ASSERT_FALSE(s.beta_estimated) << "bar " << i;
    }
}

TEST_F(SpreadStrategyTest, FirstTradeUsesAnEstimatedBetaAndMatchesTheReferenceTrace) {
    build();
    // Reference trace of the fixed logic on this pair (independent Python replica, issue #100
    // Appendix B): open short spread at bar 48 (z +1.715, beta 1.8968), close at bar 58 on the
    // exit band (z -0.059), re-open long at bar 60 (z -1.817, beta 1.9759).
    int open_bar = -1, close_bar = -1;
    double open_beta = 0, open_z = 0, close_z = 0;
    for (int i = 0; i < 70; ++i) {
        feed_synthetic(i);
        const auto& s = strategy_->get_state_snapshot();
        if (open_bar < 0 && s.direction != 0) {
            open_bar = i; open_beta = s.beta; open_z = s.current_zscore;
            EXPECT_TRUE(s.beta_estimated);
            EXPECT_EQ(s.direction, -1);
        } else if (open_bar >= 0 && close_bar < 0 && s.direction == 0) {
            close_bar = i; close_z = s.current_zscore;
        }
    }
    EXPECT_EQ(open_bar, 48);
    EXPECT_NEAR(open_beta, 1.8968, 5e-4);
    EXPECT_NEAR(open_z, 1.715, 5e-3);
    EXPECT_EQ(close_bar, 58);
    EXPECT_NEAR(close_z, -0.059, 5e-3);
}

TEST_F(SpreadStrategyTest, WithAShorterBetaWindowTheFirstTradeMovesToBar38) {
    // Issue #100, item 2 control: beta_period 20 first opens at bar 38 (beta 2.0756, z +1.84).
    spread_config_.beta_period = 20;
    build();
    int open_bar = -1;
    for (int i = 0; i < 60 && open_bar < 0; ++i) {
        feed_synthetic(i);
        if (strategy_->get_direction() != 0) open_bar = i;
    }
    EXPECT_EQ(open_bar, 38);
    EXPECT_NEAR(strategy_->get_beta(), 2.0756, 5e-4);
    EXPECT_NEAR(strategy_->get_zscore(), 1.84, 5e-3);
}

TEST_F(SpreadStrategyTest, PriceDiffNeverTradesOnTheSeedBeta) {
    spread_config_.spread_type = SpreadType::PRICE_DIFF;
    spread_config_.min_beta = 1e-6;
    spread_config_.max_beta = 1e9;
    build();
    for (int i = 0; i < earliest_z_bar(spread_config_); ++i) {
        feed_synthetic(i);
        ASSERT_EQ(strategy_->get_direction(), 0) << "bar " << i;
    }
}

TEST_F(SpreadStrategyTest, ZScoreNeedsZScorePeriodEstimatedBarsNotJustHistory) {
    build();
    const int first = earliest_z_bar(spread_config_);
    for (int i = 0; i < first; ++i) {
        feed_synthetic(i);
        ASSERT_FALSE(strategy_->get_state_snapshot().zscore_valid) << "bar " << i;
    }
    feed_synthetic(first);
    EXPECT_TRUE(strategy_->get_state_snapshot().zscore_valid);
    EXPECT_EQ(strategy_->get_state_snapshot().estimated_bars,
              static_cast<size_t>(spread_config_.zscore_period));
}

TEST_F(SpreadStrategyTest, EstimatedBarCountKeepsGrowingWhenTheHistoryIsCapped) {
    // The histories stop growing at max_history; the count of estimated bars must not.
    spread_config_.max_history = spread_config_.beta_period + spread_config_.zscore_period - 1;
    build();
    for (int i = 0; i < 300; ++i) feed_synthetic(i);
    const auto& s = strategy_->get_state_snapshot();
    EXPECT_EQ(s.a_history.size(), spread_config_.max_history);
    EXPECT_EQ(s.estimated_bars, static_cast<size_t>(300 - (spread_config_.beta_period - 1)));
    EXPECT_TRUE(s.zscore_valid);
}

// --- Item 3: the z window is built with the current beta ------------------------------------

TEST_F(SpreadStrategyTest, ZScoreMatchesAnIndependentRecomputation) {
    build();
    int checked = 0;
    for (int i = 0; i < 250; ++i) {
        feed_synthetic(i);
        const auto& s = strategy_->get_state_snapshot();
        if (!s.zscore_valid) continue;
        const size_t zp = static_cast<size_t>(spread_config_.zscore_period);
        const size_t off = s.a_history.size() - zp;
        std::vector<double> w;
        for (size_t k = off; k < s.a_history.size(); ++k) {
            w.push_back(s.a_history[k] - s.beta * s.b_history[k]);
        }
        double m = 0;
        for (double v : w) m += v;
        m /= zp;
        double ss = 0;
        for (double v : w) ss += (v - m) * (v - m);
        const double sd = std::sqrt(ss / (zp - 1));
        EXPECT_NEAR(s.current_zscore, (w.back() - m) / sd, 1e-9) << "bar " << i;
        EXPECT_NEAR(s.spread_mean, m, 1e-9) << "bar " << i;
        EXPECT_NEAR(s.spread_std, sd, 1e-9) << "bar " << i;
        ++checked;
    }
    EXPECT_GT(checked, 150);
}

// --- Item 4: the stop block clears once per bar -----------------------------------------------

TEST_F(SpreadStrategyTest, BatchAndPerBarDeliveryGiveIdenticalState) {
    auto build_one = [&](std::shared_ptr<SpreadStrategy>& out, const std::string& id) {
        out = std::make_shared<SpreadStrategy>(id, strategy_config_, spread_config_, db_);
        ASSERT_TRUE(out->initialize().is_ok());
        ASSERT_TRUE(out->start().is_ok());
    };
    spread_config_.stop_z = 3.0;
    std::shared_ptr<SpreadStrategy> one, batch;
    build_one(one, "BATCH_ONE");
    build_one(batch, "BATCH_ALL");
    auto deliver = [&](std::shared_ptr<SpreadStrategy>& s, int i, double scale) {
        const auto p = make_pair(i);
        std::vector<Bar> bars{make_bar(kSymbolA, day(i), p.price_a * scale),
                              make_bar(kSymbolB, day(i), p.price_b)};
        ASSERT_TRUE(s->on_data(bars).is_ok());
    };
    // Per bar to the first entry in both, then shock the next bar to force a stop.
    int entry_bar = -1;
    for (int i = 0; i < 200 && entry_bar < 0; ++i) {
        deliver(one, i, 1.0);
        deliver(batch, i, 1.0);
        if (one->get_direction() != 0) entry_bar = i;
    }
    ASSERT_GE(entry_bar, 0);
    const double scale = one->get_zscore() < 0 ? 0.55 : 1.80;
    deliver(one, entry_bar + 1, scale);
    deliver(batch, entry_bar + 1, scale);
    ASSERT_EQ(one->get_direction(), 0);
    ASSERT_TRUE(one->get_state_snapshot().stopped_out);

    // The next 170 bars: one call per bar to `one`, a single call to `batch`.
    std::vector<Bar> all;
    for (int i = entry_bar + 2; i < entry_bar + 172; ++i) {
        const auto p = make_pair(i);
        std::vector<Bar> bars{make_bar(kSymbolA, day(i), p.price_a),
                              make_bar(kSymbolB, day(i), p.price_b)};
        ASSERT_TRUE(one->on_data(bars).is_ok());
        all.insert(all.end(), bars.begin(), bars.end());
    }
    ASSERT_TRUE(batch->on_data(all).is_ok());
    expect_same(snap_of(*one), snap_of(*batch), "after the batch");
    const auto& s1 = one->get_state_snapshot();
    const auto& s2 = batch->get_state_snapshot();
    EXPECT_DOUBLE_EQ(s1.quantity_a, s2.quantity_a);
    EXPECT_DOUBLE_EQ(s1.quantity_b, s2.quantity_b);
    // The stop did clear and the strategy traded again afterwards (a pair that never re-enters
    // would make this test pass for the wrong reason).
    EXPECT_GT(s1.observations, static_cast<size_t>(entry_bar + 100));
}

TEST_F(SpreadStrategyTest, RandomBatchSizesGiveTheSameStateAsOneBarAtATime) {
    // 60 random pairs x random configs x random batch sizes; the single-call run is the bar-by-bar
    // run, compared at every batch boundary.
    int stops_seen = 0, trades_seen = 0;
    for (uint64_t seed = 1; seed <= 60; ++seed) {
        std::mt19937_64 rng(seed * 7919);
        spread_config_ = SpreadConfig();
        spread_config_.symbol_a = kSymbolA;
        spread_config_.symbol_b = kSymbolB;
        spread_config_.beta_period = 6 + static_cast<int>(rng() % 30);
        spread_config_.zscore_period = 4 + static_cast<int>(rng() % 25);
        spread_config_.entry_z = 1.0 + (rng() % 15) / 10.0;
        spread_config_.exit_z = 0.1 + (rng() % 6) / 10.0;
        spread_config_.stop_z = spread_config_.entry_z + 0.5 + (rng() % 20) / 10.0;
        spread_config_.min_holding_period = static_cast<int>(rng() % 5);
        const auto pairs = random_pair(seed, 320, 2.0, 0.004, 0.02);

        auto make = [&](const std::string& id) {
            auto s = std::make_shared<SpreadStrategy>(id, strategy_config_, spread_config_, db_);
            EXPECT_TRUE(s->initialize().is_ok());
            EXPECT_TRUE(s->start().is_ok());
            return s;
        };
        auto bar_of = [&](int i) {
            return std::vector<Bar>{make_bar(kSymbolA, day(i), pairs[i].first),
                                    make_bar(kSymbolB, day(i), pairs[i].second)};
        };
        auto one = make("FZ1_" + std::to_string(seed));
        auto many = make("FZN_" + std::to_string(seed));
        std::vector<Snap> per_bar;
        for (int i = 0; i < 320; ++i) {
            ASSERT_TRUE(one->on_data(bar_of(i)).is_ok());
            per_bar.push_back(snap_of(*one));
            stops_seen += one->get_state_snapshot().stopped_out ? 1 : 0;
            trades_seen += one->get_state_snapshot().direction != 0 ? 1 : 0;
        }
        int i = 0;
        while (i < 320) {
            const int n = 1 + static_cast<int>(rng() % 40);
            std::vector<Bar> chunk;
            int last = i;
            for (int k = 0; k < n && i < 320; ++k, ++i) {
                auto b = bar_of(i);
                chunk.insert(chunk.end(), b.begin(), b.end());
                last = i;
            }
            ASSERT_TRUE(many->on_data(chunk).is_ok());
            expect_same(per_bar[last], snap_of(*many),
                        "seed " + std::to_string(seed) + " at bar " + std::to_string(last));
        }
    }
    EXPECT_GT(trades_seen, 500) << "the fuzz never traded; it proves nothing";
    EXPECT_GT(stops_seen, 20) << "the fuzz never stopped out; it proves nothing about item 4";
}

// --- Item 5: the hedge ratio is guarded ------------------------------------------------------

TEST_F(SpreadStrategyTest, NegativeBetaIsRejected) {
    build();
    for (int i = 0; i < 200; ++i) {
        // Keep the 0.03 dislocation: without it the spread is constant, no z ever exists, and the
        // test would pass for the wrong reason.
        const double b = leg_b_price(i);
        const double a = std::exp(std::log(0.01) - 2.0 * std::log(b) +
                                  0.03 * std::sin(2.0 * M_PI * i / 40.0));
        feed(i, a, b);
        ASSERT_EQ(strategy_->get_direction(), 0) << "bar " << i;
        if (i >= spread_config_.beta_period - 1) {
            ASSERT_FALSE(strategy_->get_state_snapshot().zscore_valid) << "bar " << i;
        }
    }
}

TEST_F(SpreadStrategyTest, ANegativeMinBetaIsRefusedAtConfig) {
    // The bounds themselves must be positive, so a config cannot switch the guard off by
    // allowing negative betas.
    spread_config_.min_beta = -100.0;
    spread_config_.max_beta = 100.0;
    auto s = std::make_shared<SpreadStrategy>("NEG_BOUNDS", strategy_config_, spread_config_, db_);
    EXPECT_TRUE(s->initialize().is_error());
}

TEST_F(SpreadStrategyTest, BetaLeavingItsBoundsFlattensWithoutAStop) {
    spread_config_.min_beta = 1.5;
    spread_config_.max_beta = 2.5;
    build();
    int open_bar = -1;
    for (int i = 0; i < 200 && open_bar < 0; ++i) {
        feed_synthetic(i);
        if (strategy_->get_direction() != 0) open_bar = i;
    }
    ASSERT_GE(open_bar, 0);
    ASSERT_FALSE(strategy_->get_state_snapshot().stopped_out);

    // Re-price leg A so the next window's beta falls well below 1.5 (A barely follows B).
    int flat_bar = -1;
    for (int i = open_bar + 1; i < open_bar + 120; ++i) {
        const auto p = make_pair(i);
        feed(i, 1.0 + 1e-4 * i, p.price_b);
        if (strategy_->get_direction() == 0) { flat_bar = i; break; }
    }
    ASSERT_GE(flat_bar, 0) << "beta left its bounds but the position was never flattened";
    const auto& s = strategy_->get_state_snapshot();
    EXPECT_FALSE(s.stopped_out) << "a beta breach is not a stop";
    EXPECT_EQ(s.holding_period, 0);
    EXPECT_DOUBLE_EQ(s.quantity_a, 0.0);
    EXPECT_DOUBLE_EQ(s.quantity_b, 0.0);
    EXPECT_FALSE(s.zscore_valid);
    EXPECT_DOUBLE_EQ(qty(kSymbolA), 0.0);
    EXPECT_DOUBLE_EQ(qty(kSymbolB), 0.0);
}

TEST_F(SpreadStrategyTest, BreachWithFlattenOffKeepsTheOpenPositionButOpensNothingNew) {
    spread_config_.min_beta = 1.5;
    spread_config_.max_beta = 2.5;
    spread_config_.flatten_on_beta_breach = false;
    build();
    int open_bar = -1;
    for (int i = 0; i < 200 && open_bar < 0; ++i) {
        feed_synthetic(i);
        if (strategy_->get_direction() != 0) open_bar = i;
    }
    ASSERT_GE(open_bar, 0);
    const int held = strategy_->get_direction();
    for (int i = open_bar + 1; i < open_bar + 40; ++i) {
        feed(i, 1.0 + 1e-4 * i, make_pair(i).price_b);
        EXPECT_EQ(strategy_->get_direction(), held) << "bar " << i;
        EXPECT_FALSE(strategy_->get_state_snapshot().zscore_valid) << "bar " << i;
    }
}

TEST_F(SpreadStrategyTest, PriceDiffRequiresExplicitBetaBounds) {
    spread_config_.spread_type = SpreadType::PRICE_DIFF;
    auto none = std::make_shared<SpreadStrategy>("PD_NONE", strategy_config_, spread_config_, db_);
    EXPECT_TRUE(none->initialize().is_error());
    spread_config_.min_beta = 0.001;
    auto half = std::make_shared<SpreadStrategy>("PD_HALF", strategy_config_, spread_config_, db_);
    EXPECT_TRUE(half->initialize().is_error());
    spread_config_.max_beta = 1000.0;
    auto both = std::make_shared<SpreadStrategy>("PD_BOTH", strategy_config_, spread_config_, db_);
    EXPECT_TRUE(both->initialize().is_ok());
}

TEST_F(SpreadStrategyTest, RejectsBadBetaBounds) {
    struct Case { double lo, hi; };
    for (const auto& c : {Case{0.0, 10.0}, Case{-1.0, 10.0}, Case{5.0, 5.0}, Case{5.0, 1.0},
                          Case{std::nan(""), 10.0}, Case{0.1, std::nan("")}}) {
        spread_config_.min_beta = c.lo;
        spread_config_.max_beta = c.hi;
        auto s = std::make_shared<SpreadStrategy>("BAD_BOUNDS", strategy_config_, spread_config_, db_);
        EXPECT_TRUE(s->initialize().is_error()) << c.lo << " .. " << c.hi;
    }
    // Only one bound given on LOG_RATIO: the other defaults, and the pair must still be ordered.
    spread_config_.min_beta = 20.0;
    spread_config_.max_beta.reset();
    auto s = std::make_shared<SpreadStrategy>("BAD_DEFAULT", strategy_config_, spread_config_, db_);
    EXPECT_TRUE(s->initialize().is_error());
}

// --- Item 6: the windows fit inside max_history --------------------------------------------

TEST_F(SpreadStrategyTest, RejectsWindowsLongerThanHistory) {
    auto refused = [&](int beta_period, int zscore_period, size_t max_history) {
        SpreadConfig c = spread_config_;
        c.beta_period = beta_period;
        c.zscore_period = zscore_period;
        c.max_history = max_history;
        auto s = std::make_shared<SpreadStrategy>("WIN", strategy_config_, c, db_);
        return s->initialize().is_error();
    };
    EXPECT_TRUE(refused(101, 20, 100));   // beta_period > max_history
    EXPECT_TRUE(refused(30, 101, 100));   // zscore_period > max_history
    EXPECT_TRUE(refused(60, 42, 100));    // 60 + 42 - 1 = 101 > 100, though each fits alone
    EXPECT_FALSE(refused(60, 41, 100));   // the exact boundary is allowed: 60 + 41 - 1 = 100
    EXPECT_FALSE(refused(60, 40, 100));
    EXPECT_FALSE(refused(30, 20, 49));    // 30 + 20 - 1 = 49
    EXPECT_TRUE(refused(30, 20, 48));
}

// --- Look-ahead, and the reference model ----------------------------------------------------

TEST_F(SpreadStrategyTest, NoLookAheadAddingLaterBarsNeverChangesEarlierRows) {
    const int T = 120;
    auto make = [&](const std::string& id) {
        auto s = std::make_shared<SpreadStrategy>(id, strategy_config_, spread_config_, db_);
        EXPECT_TRUE(s->initialize().is_ok());
        EXPECT_TRUE(s->start().is_ok());
        return s;
    };
    auto a = make("LA_SHORT");
    auto b = make("LA_LONG");
    std::vector<Snap> short_run, long_run;
    for (int i = 0; i < T + 50; ++i) {
        const auto p = make_pair(i);
        std::vector<Bar> bars{make_bar(kSymbolA, day(i), p.price_a),
                              make_bar(kSymbolB, day(i), p.price_b)};
        ASSERT_TRUE(b->on_data(bars).is_ok());
        long_run.push_back(snap_of(*b));
        if (i < T) {
            ASSERT_TRUE(a->on_data(bars).is_ok());
            short_run.push_back(snap_of(*a));
        }
    }
    for (int i = 0; i < T; ++i) {
        expect_same(short_run[i], long_run[i], "bar " + std::to_string(i));
    }
}

TEST_F(SpreadStrategyTest, MatchesTheReferenceModelBarByBarAcrossRandomConfigs) {
    int compared = 0, trades = 0, breaches = 0, stops = 0;
    for (uint64_t seed = 1; seed <= 150; ++seed) {
        std::mt19937_64 rng(seed * 104729);
        spread_config_ = SpreadConfig();
        spread_config_.symbol_a = kSymbolA;
        spread_config_.symbol_b = kSymbolB;
        spread_config_.spread_type =
            (rng() % 4 == 0) ? SpreadType::PRICE_DIFF : SpreadType::LOG_RATIO;
        spread_config_.beta_period = 5 + static_cast<int>(rng() % 36);
        spread_config_.zscore_period = 3 + static_cast<int>(rng() % 28);
        spread_config_.entry_z = 1.0 + (rng() % 16) / 10.0;
        spread_config_.exit_z = 0.1 + (rng() % 7) / 10.0;
        spread_config_.stop_z = spread_config_.entry_z + 0.4 + (rng() % 25) / 10.0;
        spread_config_.min_holding_period = static_cast<int>(rng() % 6);
        spread_config_.flatten_on_beta_breach = rng() % 3 != 0;
        const bool tight = rng() % 3 == 0;
        if (spread_config_.spread_type == SpreadType::PRICE_DIFF) {
            spread_config_.min_beta = 1e-6;
            spread_config_.max_beta = 1e9;
        } else if (tight) {
            spread_config_.min_beta = 1.2 + (rng() % 5) / 10.0;
            spread_config_.max_beta = 2.2 + (rng() % 5) / 10.0;
        }
        // half the runs cap the history at the smallest legal size
        spread_config_.max_history = (rng() % 2) ? static_cast<size_t>(spread_config_.beta_period +
                                                    spread_config_.zscore_period - 1)
                                                 : 512u;
        const double true_beta = (rng() % 5 == 0) ? 1.0 + (rng() % 10) / 10.0 : 2.0;
        const auto pairs = random_pair(seed, 360, true_beta, 0.003 + (rng() % 5) / 1000.0, 0.015);

        auto s = std::make_shared<SpreadStrategy>("REF_" + std::to_string(seed), strategy_config_,
                                                  spread_config_, db_);
        ASSERT_TRUE(s->initialize().is_ok()) << "seed " << seed;
        ASSERT_TRUE(s->start().is_ok());
        RefSpread ref(spread_config_);
        for (int i = 0; i < 360; ++i) {
            std::vector<Bar> bars{make_bar(kSymbolA, day(i), pairs[i].first),
                                  make_bar(kSymbolB, day(i), pairs[i].second)};
            ASSERT_TRUE(s->on_data(bars).is_ok());
            // The strategy reads each close through Bar's Price, which quantizes it; the model is
            // given the same numbers.
            ref.bar(bars[0].close.as_double(), bars[1].close.as_double());
            const auto& st = s->get_state_snapshot();
            const std::string at = "seed " + std::to_string(seed) + " bar " + std::to_string(i);
            ASSERT_EQ(st.direction, ref.dir) << at;
            ASSERT_EQ(st.holding_period, ref.hold) << at;
            ASSERT_EQ(st.stopped_out, ref.stopped) << at;
            ASSERT_EQ(st.zscore_valid, ref.zv) << at;
            ASSERT_EQ(st.beta_estimated, ref.estimated) << at;
            ASSERT_EQ(st.estimated_bars, ref.est_bars) << at;
            if (ref.zv) ASSERT_NEAR(st.current_zscore, ref.z, 1e-9) << at;
            if (ref.estimated) ASSERT_NEAR(st.beta, ref.beta, 1e-9) << at;
            ASSERT_EQ(st.quantity_a != 0.0, ref.dir != 0) << at;
            ++compared;
            trades += ref.dir != 0 ? 1 : 0;
            stops += ref.stopped ? 1 : 0;
        }
        breaches += ref.breaches;
    }
    EXPECT_EQ(compared, 150 * 360);
    EXPECT_GT(trades, 3000) << "the model comparison barely traded";
    EXPECT_GT(stops, 100) << "the model comparison barely stopped out";
    EXPECT_GT(breaches, 100) << "the model comparison barely hit a beta breach";
}
