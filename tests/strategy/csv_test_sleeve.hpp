// A trend sleeve a CSV exporter test can read: the one trend class on a given configuration, fed a
// deterministic walk of daily bars of one symbol, so its forecast, its sizing volatility and its
// moving averages are real figures. No database (the mock) and nothing read from disk.
#pragma once

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "../data/test_db_utils.hpp"
#include "trade_ngin/instruments/futures.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/strategy/trend_following.hpp"

namespace trade_ngin::testing {

struct CsvTestSleeve {
    std::shared_ptr<MockPostgresDatabase> db;
    std::shared_ptr<TrendFollowingStrategy> strategy;
    std::string symbol;

    /// `tilt` is added to every bar's return, so two sleeves can be told apart by their figures.
    CsvTestSleeve(const std::string& id, const std::string& sym, TrendFollowingConfig config,
                  double tilt = 0.0, int bars = 400)
        : symbol(sym) {
        db = std::make_shared<MockPostgresDatabase>("mock://testdb");
        EXPECT_TRUE(db->connect().is_ok());
        StrategyConfig sc;
        sc.capital_allocation = 1'000'000.0;
        sc.max_leverage = 100.0;
        sc.asset_classes = {AssetClass::FUTURES};
        sc.frequencies = {DataFrequency::DAILY};
        sc.trading_params[sym] = 5.0;
        sc.position_limits[sym] = 1000.0;
        auto& registry = InstrumentRegistry::instance();
        if (!registry.has_instrument(sym)) {
            FuturesSpec spec;
            spec.root_symbol = sym;
            spec.exchange = "CME";
            spec.currency = "USD";
            spec.multiplier = 5.0;
            spec.tick_size = 0.25;
            spec.commission_per_contract = 2.0;
            spec.initial_margin = 10000.0;
            spec.maintenance_margin = 8000.0;
            spec.weight = 1.0;
            spec.trading_hours = "09:30-16:00";
            registry.register_instrument(sym, std::make_shared<FuturesInstrument>(sym, spec));
        }
        auto registry_ptr =
            std::shared_ptr<InstrumentRegistry>(&registry, [](InstrumentRegistry*) {});
        strategy =
            std::make_shared<TrendFollowingStrategy>(id, sc, std::move(config), db, registry_ptr);
        EXPECT_TRUE(strategy->initialize().is_ok());
        RiskLimits limits;
        limits.max_position_size = 1000.0;
        limits.max_notional_value = 1e9;
        limits.max_drawdown = 0.5;
        limits.max_leverage = 100.0;
        EXPECT_TRUE(strategy->update_risk_limits(limits).is_ok());
        EXPECT_TRUE(strategy->start().is_ok());
        EXPECT_TRUE(strategy->on_data(walk(bars, tilt)).is_ok());
    }
    ~CsvTestSleeve() {
        strategy->stop();
        db->disconnect();
    }

    std::vector<Bar> walk(int n, double tilt) const {
        std::vector<Bar> out;
        const Timestamp day0 = std::chrono::system_clock::from_time_t(1262563200);  // 2010-01-04
        double p = 100.0;
        unsigned state = 2463534242u;
        for (int k = 0; k < n; ++k) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            const double u = static_cast<double>(state % 100000u) / 100000.0 - 0.5;
            p *= 1.0 + 0.012 * (1.0 + 0.7 * std::sin(k / 61.0)) * u + 0.0003 + tilt;
            Bar b;
            b.symbol = symbol;
            b.timestamp = day0 + std::chrono::hours(24 * k);
            b.open = Decimal(p);
            b.high = Decimal(p * 1.002);
            b.low = Decimal(p * 0.998);
            b.close = Decimal(p);
            b.volume = 100000.0;
            b.instrument_id = "A";
            out.push_back(b);
        }
        return out;
    }
};

}  // namespace trade_ngin::testing
