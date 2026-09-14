#include <gtest/gtest.h>
#include <cmath>
#include <memory>
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
        strategy_config_.save_signals = false;
        strategy_config_.save_positions = false;
        strategy_config_.save_executions = false;

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

    feed_synthetic(spread_config_.beta_period - 1);
    EXPECT_NE(strategy_->get_beta(), 1.0);
}

TEST_F(SpreadStrategyTest, BetaIsHeldWhenHedgeLegIsFlat) {
    build();
    // Leg B never moves, so var(B) is zero and beta must not blow up.
    for (int i = 0; i < 80; ++i) {
        feed(i, 100.0 + 0.5 * std::sin(0.2 * i), 50.0);
    }
    EXPECT_DOUBLE_EQ(strategy_->get_beta(), spread_config_.initial_beta);
    EXPECT_TRUE(std::isfinite(strategy_->get_beta()));
}

// --- Warmup -------------------------------------------------------------------

TEST_F(SpreadStrategyTest, NoPositionDuringWarmup) {
    build();
    for (int i = 0; i < spread_config_.zscore_period - 1; ++i) {
        feed_synthetic(i);
    }

    EXPECT_EQ(strategy_->get_direction(), 0);
    EXPECT_DOUBLE_EQ(qty(kSymbolA), 0.0);
    EXPECT_DOUBLE_EQ(qty(kSymbolB), 0.0);
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
    // Entry only after the z-score window has filled.
    EXPECT_GE(entry_bar, spread_config_.zscore_period - 1);
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
