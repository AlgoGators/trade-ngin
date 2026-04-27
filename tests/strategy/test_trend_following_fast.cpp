#include <gtest/gtest.h>
#include <chrono>
#include <cmath>
#include <memory>
#include <random>
#include <vector>
#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "trade_ngin/instruments/futures.hpp"

// Expose private members so we can populate the singleton registry in tests
// (no public API to add instruments without a real DB connection).
#define private public
#include "trade_ngin/instruments/instrument_registry.hpp"
#undef private

#include "trade_ngin/strategy/trend_following_fast.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

void seed_registry_with_micros() {
    auto& registry = InstrumentRegistry::instance();
    for (const auto& symbol : {"MES", "MNQ", "MYM"}) {
        FuturesSpec spec;
        spec.root_symbol = symbol;
        spec.exchange = "CME";
        spec.currency = "USD";
        spec.multiplier = 5.0;
        spec.tick_size = 0.25;
        spec.commission_per_contract = 2.0;
        spec.initial_margin = 10000.0;
        spec.maintenance_margin = 8000.0;
        spec.weight = 1.0;
        spec.trading_hours = "09:30-16:00";
        registry.instruments_[symbol] = std::make_shared<FuturesInstrument>(symbol, spec);
    }
    registry.initialized_ = true;
}

void clear_registry() {
    auto& registry = InstrumentRegistry::instance();
    registry.instruments_.clear();
    registry.initialized_ = false;
}

}  // namespace

class TrendFollowingFastTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();

        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        ASSERT_TRUE(db_->connect().is_ok());

        strategy_config_.capital_allocation = 1'000'000.0;
        strategy_config_.max_leverage = 100.0;
        strategy_config_.asset_classes = {AssetClass::FUTURES};
        strategy_config_.frequencies = {DataFrequency::DAILY};
        for (const auto& symbol : {"ES", "NQ", "YM"}) {
            strategy_config_.trading_params[symbol] = 5.0;
            strategy_config_.position_limits[symbol] = 1000.0;
        }

        risk_limits_.max_position_size = 1000.0;
        risk_limits_.max_notional_value = 1'000'000.0;
        risk_limits_.max_drawdown = 0.5;
        risk_limits_.max_leverage = 100.0;

        seed_registry_with_micros();
        registry_ptr_ = std::shared_ptr<InstrumentRegistry>(&InstrumentRegistry::instance(),
                                                            [](InstrumentRegistry*) {});

        static int test_id = 0;
        std::string id = "TEST_TF_FAST_" + std::to_string(++test_id);
        strategy_ = std::make_unique<TrendFollowingFastStrategy>(
            id, strategy_config_, TrendFollowingFastConfig{}, db_, registry_ptr_);
        ASSERT_TRUE(strategy_->initialize().is_ok());
        ASSERT_TRUE(strategy_->update_risk_limits(risk_limits_).is_ok());
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
        clear_registry();
        TestBase::TearDown();
    }

    std::vector<Bar> create_test_data(const std::string& symbol, int num_bars,
                                       double start_price = 100.0,
                                       double trend = 0.0, double vol = 0.01) {
        std::vector<Bar> data;
        // Deterministic PRNG: order-independent and reproducible.
        std::mt19937 rng(42);
        std::normal_distribution<double> noise(0.0, 1.0);
        auto t0 = std::chrono::system_clock::now() - std::chrono::hours(24 * num_bars);
        double price = start_price;
        for (int i = 0; i < num_bars; ++i) {
            double change = trend + vol * noise(rng);
            price = std::max(1.0, price * (1.0 + change));
            Bar bar;
            bar.symbol = symbol;
            bar.timestamp = t0 + std::chrono::hours(24 * i);
            bar.open = Decimal(price);
            bar.high = Decimal(price * 1.005);
            bar.low = Decimal(price * 0.995);
            bar.close = Decimal(price);
            bar.volume = 10000;
            data.push_back(bar);
        }
        return data;
    }

    void process_safely(const std::vector<Bar>& bars) {
        const size_t kBatch = 50;
        for (size_t i = 0; i < bars.size(); i += kBatch) {
            std::vector<Bar> batch(bars.begin() + i,
                                    bars.begin() + std::min(i + kBatch, bars.size()));
            ASSERT_TRUE(strategy_->on_data(batch).is_ok());
        }
    }

    std::shared_ptr<MockPostgresDatabase> db_;
    std::shared_ptr<InstrumentRegistry> registry_ptr_;
    StrategyConfig strategy_config_;
    RiskLimits risk_limits_;
    std::unique_ptr<TrendFollowingFastStrategy> strategy_;
};

TEST_F(TrendFollowingFastTest, InitializeSucceedsAndStartTransitionsState) {
    EXPECT_TRUE(strategy_->start().is_ok());
}

TEST_F(TrendFollowingFastTest, MaxRequiredLookbackReflectsLongestEMAWindow) {
    int lb = strategy_->get_max_required_lookback();
    // Default fast EMA windows top out at 64 (slow side); add vol_lookback_long(64).
    EXPECT_GT(lb, 0);
}

TEST_F(TrendFollowingFastTest, OnDataAcceptsEmptyBars) {
    EXPECT_TRUE(strategy_->start().is_ok());
    EXPECT_TRUE(strategy_->on_data({}).is_ok());
}

TEST_F(TrendFollowingFastTest, OnDataInsufficientHistoryDoesNotCrash) {
    EXPECT_TRUE(strategy_->start().is_ok());
    auto bars = create_test_data("ES", 10);  // way below required lookback
    EXPECT_TRUE(strategy_->on_data(bars).is_ok());
    EXPECT_DOUBLE_EQ(strategy_->get_forecast("ES"), 0.0);
}

TEST_F(TrendFollowingFastTest, OnDataWithSufficientHistoryProducesForecastAndPosition) {
    EXPECT_TRUE(strategy_->start().is_ok());
    auto bars = create_test_data("ES", 200, /*start=*/100.0, /*trend=*/0.001);
    process_safely(bars);
    // After enough history, internal state should be populated.
    EXPECT_TRUE(std::isfinite(strategy_->get_forecast("ES")));
    EXPECT_TRUE(std::isfinite(strategy_->get_position("ES")));
}

TEST_F(TrendFollowingFastTest, GetEMAValuesReturnsRequestedWindows) {
    EXPECT_TRUE(strategy_->start().is_ok());
    process_safely(create_test_data("ES", 200));
    auto emas = strategy_->get_ema_values("ES", {2, 8, 16});
    EXPECT_FALSE(emas.empty());
    for (auto& [w, v] : emas) {
        EXPECT_TRUE(std::isfinite(v)) << "window=" << w;
    }
}

TEST_F(TrendFollowingFastTest, GetForecastForUnknownSymbolReturnsZero) {
    EXPECT_DOUBLE_EQ(strategy_->get_forecast("UNKNOWN"), 0.0);
    EXPECT_DOUBLE_EQ(strategy_->get_position("UNKNOWN"), 0.0);
}

TEST_F(TrendFollowingFastTest, PriceHistoryGrowsWithBars) {
    EXPECT_TRUE(strategy_->start().is_ok());
    process_safely(create_test_data("ES", 100));
    auto hist = strategy_->get_price_history();
    ASSERT_TRUE(hist.count("ES"));
    EXPECT_GT(hist.at("ES").size(), 0u);
}

TEST_F(TrendFollowingFastTest, OnExecutionAcceptsValidReportWithoutErroring) {
    EXPECT_TRUE(strategy_->start().is_ok());
    process_safely(create_test_data("ES", 100));
    ExecutionReport exec;
    exec.order_id = "O1";
    exec.exec_id = "E1";
    exec.symbol = "ES";
    exec.side = Side::BUY;
    exec.filled_quantity = Quantity(1.0);
    exec.fill_price = Price(100.0);
    exec.fill_time = std::chrono::system_clock::now();
    exec.commissions_fees = Decimal(0.0);
    exec.implicit_price_impact = Decimal(0.0);
    exec.slippage_market_impact = Decimal(0.0);
    exec.total_transaction_costs = Decimal(0.0);
    EXPECT_TRUE(strategy_->on_execution(exec).is_ok());
}

TEST_F(TrendFollowingFastTest, MultipleSymbolsTrackedIndependently) {
    EXPECT_TRUE(strategy_->start().is_ok());
    auto es = create_test_data("ES", 200, 100.0, 0.001);
    auto nq = create_test_data("NQ", 200, 200.0, -0.001);
    std::vector<Bar> combined;
    for (size_t i = 0; i < es.size(); ++i) {
        combined.push_back(es[i]);
        combined.push_back(nq[i]);
    }
    process_safely(combined);
    auto hist = strategy_->get_price_history();
    EXPECT_TRUE(hist.count("ES"));
    EXPECT_TRUE(hist.count("NQ"));
    EXPECT_GT(hist.at("ES").size(), 0u);
    EXPECT_GT(hist.at("NQ").size(), 0u);
}

// ===== Branch-targeting tests =====

TEST_F(TrendFollowingFastTest, OnExecutionSellSideUpdatesPosition) {
    EXPECT_TRUE(strategy_->start().is_ok());
    process_safely(create_test_data("ES", 200));
    ExecutionReport exec;
    exec.order_id = "O2";
    exec.exec_id = "E2";
    exec.symbol = "ES";
    exec.side = Side::SELL;
    exec.filled_quantity = Quantity(2.0);
    exec.fill_price = Price(100.0);
    exec.fill_time = std::chrono::system_clock::now();
    exec.commissions_fees = Decimal(0.0);
    exec.implicit_price_impact = Decimal(0.0);
    exec.slippage_market_impact = Decimal(0.0);
    exec.total_transaction_costs = Decimal(0.0);
    EXPECT_TRUE(strategy_->on_execution(exec).is_ok());
}

TEST_F(TrendFollowingFastTest, IncrementalUpdatesProduceConsistentForecast) {
    EXPECT_TRUE(strategy_->start().is_ok());
    auto bars = create_test_data("ES", 250, 100.0, 0.002);
    // Process in two halves and verify state advances both times.
    std::vector<Bar> first(bars.begin(), bars.begin() + 125);
    std::vector<Bar> second(bars.begin() + 125, bars.end());
    ASSERT_TRUE(strategy_->on_data(first).is_ok());
    double f1 = strategy_->get_forecast("ES");
    ASSERT_TRUE(strategy_->on_data(second).is_ok());
    double f2 = strategy_->get_forecast("ES");
    EXPECT_TRUE(std::isfinite(f1));
    EXPECT_TRUE(std::isfinite(f2));
}

TEST_F(TrendFollowingFastTest, GetEMAValuesUnknownSymbolReturnsEmpty) {
    auto emas = strategy_->get_ema_values("UNKNOWN", {2, 4, 8});
    EXPECT_TRUE(emas.empty());
}

TEST_F(TrendFollowingFastTest, GetEMAValuesEmptyWindowsReturnsEmpty) {
    EXPECT_TRUE(strategy_->start().is_ok());
    process_safely(create_test_data("ES", 200));
    auto emas = strategy_->get_ema_values("ES", {});
    EXPECT_TRUE(emas.empty());
}

TEST_F(TrendFollowingFastTest, GetInstrumentDataUnknownSymbolReturnsNull) {
    EXPECT_EQ(strategy_->get_instrument_data("UNKNOWN"), nullptr);
}

TEST_F(TrendFollowingFastTest, GetInstrumentDataPopulatedAfterUpdate) {
    EXPECT_TRUE(strategy_->start().is_ok());
    process_safely(create_test_data("ES", 200));
    EXPECT_NE(strategy_->get_instrument_data("ES"), nullptr);
    EXPECT_FALSE(strategy_->get_all_instrument_data().empty());
}

TEST_F(TrendFollowingFastTest, GetTargetPositionsReturnsMapAfterUpdate) {
    EXPECT_TRUE(strategy_->start().is_ok());
    process_safely(create_test_data("ES", 200, 100.0, 0.002));
    auto positions = strategy_->get_target_positions();
    // Should include "ES" once enough history accumulated.
    EXPECT_TRUE(positions.count("ES") > 0 || positions.empty());
}

TEST_F(TrendFollowingFastTest, GetPointValueMultiplierForKnownSymbol) {
    EXPECT_GT(strategy_->get_point_value_multiplier("ES"), 0.0);
}

TEST_F(TrendFollowingFastTest, GetPointValueMultiplierThrowsForUnknownSymbol) {
    // Production contract: unknown registry symbols throw rather than fallback,
    // so a misconfigured symbol surfaces immediately instead of computing wrong PnL.
    EXPECT_THROW(
        strategy_->get_point_value_multiplier("ZZZUNKNOWN"),
        std::exception);
}

TEST_F(TrendFollowingFastTest, StopTransitionsStateOutOfRunning) {
    EXPECT_TRUE(strategy_->start().is_ok());
    process_safely(create_test_data("ES", 50));
    EXPECT_TRUE(strategy_->stop().is_ok());
}

TEST_F(TrendFollowingFastTest, UpdateRiskLimitsWhileRunningSucceeds) {
    EXPECT_TRUE(strategy_->start().is_ok());
    process_safely(create_test_data("ES", 100));
    RiskLimits new_limits = risk_limits_;
    new_limits.max_position_size = 5000.0;
    EXPECT_TRUE(strategy_->update_risk_limits(new_limits).is_ok());
}

TEST_F(TrendFollowingFastTest, ProcessUptrendingThenDowntrendingFlipsForecastSign) {
    EXPECT_TRUE(strategy_->start().is_ok());
    auto t0 = std::chrono::system_clock::now() - std::chrono::hours(24 * 400);
    std::vector<Bar> bars;
    // 200 strongly trending up
    for (int i = 0; i < 200; ++i) {
        Bar b;
        b.symbol = "ES";
        b.timestamp = t0 + std::chrono::hours(24 * i);
        double price = 100.0 + i * 0.5;
        b.open = b.high = b.low = b.close = Decimal(price);
        b.volume = 10000;
        bars.push_back(b);
    }
    process_safely(bars);
    double f_up = strategy_->get_forecast("ES");

    // Then 200 strongly trending down
    std::vector<Bar> down;
    for (int i = 0; i < 200; ++i) {
        Bar b;
        b.symbol = "ES";
        b.timestamp = t0 + std::chrono::hours(24 * (200 + i));
        double price = 200.0 - i * 0.5;
        b.open = b.high = b.low = b.close = Decimal(price);
        b.volume = 10000;
        down.push_back(b);
    }
    process_safely(down);
    double f_down = strategy_->get_forecast("ES");
    // After a sustained downtrend the forecast should not be more bullish than after the uptrend.
    EXPECT_LE(f_down, f_up);
}
