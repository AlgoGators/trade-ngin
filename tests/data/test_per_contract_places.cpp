// Rolls step 3, plan section 6 ("how we know we did not break it"): each place that builds its own
// series from its own bars (the PortfolioManager's history and covariance, the risk reader and the
// gate's MarketData, the cost model's volatility window) produces, given the same bars, the same
// returns and levels as the library (roll_series::build_series), on a series with no roll and on a
// per-contract series with every roll shape: a vendor switch the next bar does not confirm, a roll
// known on its bar, a vendor switch the next bar confirms, the AFTER_ROLL field after each roll.
// The strategy's own test is in tests/strategy/test_trend_adjusted_series.cpp, the engines' P&L in
// the backtest and live suites.
#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "trade_ngin/data/roll_series.hpp"
#include "trade_ngin/live/futures_cost_feed.hpp"
#include "trade_ngin/transaction_cost/transaction_cost_manager.hpp"

#define private public
#include "../risk/risk_module_test_helpers.hpp"
#include "trade_ngin/risk/risk_manager.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#undef private

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

constexpr int kDays = 40;

Timestamp at(int day) {
    return std::chrono::system_clock::time_point(std::chrono::hours(24 * (20000 + day)));
}

/// The three contracts' own closes on a day (each its own walk, B above A, C above B).
double own(char contract, int day) {
    const double base = contract == 'A' ? 3.0 : contract == 'B' ? 3.3 : 3.55;
    return base * (1.0 + 0.012 * std::sin(day * 0.7 + (contract - 'A'))) + 0.002 * day;
}

/// One symbol's bars as the per-contract source hands them out. `rolls` false: A every day.
struct Series {
    std::vector<Bar> bars;
    std::vector<double> raw;
    std::vector<std::string> ids;
    roll_series::RollFields rf;
};

Series per_contract(const std::string& symbol, bool rolls) {
    Series s;
    char held = 'A';
    for (int d = 1; d <= kDays; ++d) {
        Bar b;
        b.symbol = symbol;
        b.timestamp = at(d);
        b.volume = 1000.0;
        char shown = held;
        if (rolls && d == 8) {
            // a vendor switch to B that day 9 does not confirm: A's own bar, the field naming B
            b.roll_from = RollFrom{own('B', d), "B", RollFromKind::AWAITS_NEXT};
        } else if (rolls && d == 15) {
            // a crossover: known on its bar
            shown = held = 'B';
            b.roll_from = RollFrom{own('A', d), "A", RollFromKind::KNOWN};
        } else if (rolls && d == 16) {
            b.roll_from = RollFrom{own('A', d), "A", RollFromKind::AFTER_ROLL};
        } else if (rolls && d == 27) {
            // a vendor switch to C that day 28 confirms: B's own bar, the field naming C
            b.roll_from = RollFrom{own('C', d), "C", RollFromKind::AWAITS_NEXT};
        } else if (rolls && d == 28) {
            b.roll_from = RollFrom{own('B', d), "B", RollFromKind::AFTER_ROLL};
        }
        const double c = own(shown, d);
        b.open = b.high = b.low = b.close = Decimal(c);
        b.instrument_id = std::string(1, shown);
        if (rolls && d == 27) held = 'C';  // the switch is confirmed by day 28
        s.raw.push_back(static_cast<double>(b.close));
        s.ids.push_back(b.instrument_id);
        s.rf.push_back(b.roll_from);
        s.bars.push_back(b);
    }
    return s;
}

/// The library's series of the same bars.
roll_series::Series library(const Series& s) {
    return roll_series::build_series(s.raw, s.ids, s.rf);
}

std::vector<Bar> with_es(const Series& ng) {
    std::vector<Bar> out;
    double es = 5000.0;
    for (int d = 1; d <= kDays; ++d) {
        out.push_back(ng.bars[static_cast<size_t>(d - 1)]);
        es *= 1.0 + 0.004 * std::cos(d * 0.5);
        Bar b;
        b.symbol = "ES";
        b.timestamp = at(d);
        b.open = b.high = b.low = b.close = Decimal(es);
        b.volume = 1000.0;
        b.instrument_id = "1";
        out.push_back(b);
    }
    return out;
}

PortfolioConfig pm_config() {
    PortfolioConfig c{1'000'000.0, 0.6, 0.05, false};
    c.opt_config.tau = 1.0;
    c.opt_config.capital = 1'000'000.0;
    c.risk_config.capital = 1'000'000.0;
    c.risk_config.lookback_period = 252;
    c.risk_modules = {test_none_module()};
    return c;
}

RiskConfig risk_config() {
    RiskConfig c;
    c.var_limit = 0.15;
    c.jump_risk_limit = 0.10;
    c.max_correlation = 0.7;
    c.max_gross_leverage = 4.0;
    c.max_net_leverage = 2.0;
    c.capital = 1000000.0;
    c.confidence_level = 0.99;
    c.lookback_period = 252;
    return c;
}

}  // namespace

TEST(PerContractPlaces, TheSeriesHasEveryRollShape) {
    const Series ng = per_contract("NG", true);
    const auto s = library(ng);
    // bar t is day t + 1; returns[t - 1] is bar t's
    EXPECT_TRUE(s.flags.pending[7]) << "day 8: the unconfirmed switch, held";
    EXPECT_FALSE(s.flags.base_close[7].has_value());
    EXPECT_TRUE(s.flags.confirm[14]) << "day 15: known on its bar";
    EXPECT_TRUE(s.flags.confirm[27]) << "day 28 confirms day 27";
    EXPECT_EQ(*s.flags.base_close[26], ng.rf[26]->close);
    EXPECT_EQ(ng.ids[7], "A") << "day 8's bar, and so its level, is A's own";
    EXPECT_EQ(ng.ids[26], "B") << "day 27's bar is B's own";
    EXPECT_DOUBLE_EQ(s.returns[13], (ng.rf[14]->close - ng.raw[13]) / ng.raw[13])
        << "day 15: A's own move";
    EXPECT_DOUBLE_EQ(s.returns[25], (ng.raw[26] - ng.raw[25]) / ng.raw[25])
        << "day 27: B's own move";
    EXPECT_DOUBLE_EQ(s.returns[26], (ng.raw[27] - ng.rf[26]->close) / ng.rf[26]->close)
        << "day 28 reads from C's close on day 27";
    EXPECT_DOUBLE_EQ(s.returns[7], (ng.raw[8] - ng.raw[7]) / ng.raw[7]) << "day 9: A's own move";
}

class PerContractPmTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        static int n = 0;
        manager_ = std::make_unique<PortfolioManager>(pm_config(), "PM_PC_" + std::to_string(++n));
    }
    void TearDown() override {
        manager_.reset();
        StateManager::reset_instance();
        TestBase::TearDown();
    }
    void expect_library(bool rolls) {
        SCOPED_TRACE(rolls ? "per-contract rolls" : "no roll");
        const Series ng = per_contract("NG", rolls);
        const auto lib = library(ng);
        manager_->update_historical_returns(with_es(ng));
        // The stored history's returns: the library's, bit for bit.
        EXPECT_EQ(manager_->historical_returns_.at("NG"), lib.returns);
        // The adjusted levels the covariance reads: the library's.
        std::unordered_map<std::string, std::map<int64_t, double>> closes;
        for (const auto& sym : {"NG", "ES"}) closes[sym] = manager_->closes_by_date_.at(sym);
        const auto adjusted = manager_->adjusted_closes_by_symbol(closes);
        std::vector<double> levels;
        for (const auto& [day, level] : adjusted.at("NG")) levels.push_back(level);
        EXPECT_EQ(levels, lib.adjusted);
        // The date-aligned returns (every date shared): the library's returns.
        const auto r = manager_->date_aligned_returns(manager_->base_closes_by_symbol(closes),
                                                      adjusted);
        ASSERT_EQ(r.at("NG").size(), lib.returns.size());
        for (size_t t = 0; t < lib.returns.size(); ++t) {
            EXPECT_NEAR(r.at("NG")[t], lib.returns[t], 1e-13) << "return " << t;
        }
    }
    std::unique_ptr<PortfolioManager> manager_;
};

TEST_F(PerContractPmTest, TheHistoryAndTheCovarianceReturnsAreTheLibrarys) {
    expect_library(false);
    TearDown();
    SetUp();
    expect_library(true);
}

TEST(PerContractPlaces, TheRiskReadersReturnsAreTheLibrarys) {
    for (bool rolls : {false, true}) {
        SCOPED_TRACE(rolls ? "per-contract rolls" : "no roll");
        const Series ng = per_contract("NG", rolls);
        const auto lib = library(ng);
        RiskManager rm(risk_config());
        const auto bars = with_es(ng);
        const auto md = rm.create_market_data(bars);
        const size_t i = md.symbol_indices.at("NG");
        ASSERT_EQ(md.returns.size(), lib.returns.size());
        for (size_t t = 0; t < lib.returns.size(); ++t) {
            EXPECT_NEAR(md.returns[t][i], lib.returns[t], 1e-13) << "row " << t;
        }
        // The gate's form: the levels and divisors of a whole pool handed in.
        const auto adjusted = RiskManager::adjusted_levels_of(bars);
        const auto resolved = RiskManager::base_closes_of(bars);
        EXPECT_EQ(resolved.count("NG"), rolls ? 1u : 0u);
        const auto md2 = rm.create_market_data(bars, &adjusted, &resolved);
        for (size_t t = 0; t < lib.returns.size(); ++t) {
            EXPECT_EQ(md2.returns[t][i], md.returns[t][i]);
        }
        std::vector<double> levels;
        for (const auto& [ts, level] : adjusted.at("NG")) levels.push_back(level);
        EXPECT_EQ(levels, lib.adjusted);
        // calculate_returns reads the same rows.
        const auto rows = rm.calculate_returns(bars);
        ASSERT_EQ(rows.size(), lib.returns.size());
        for (size_t t = 0; t < lib.returns.size(); ++t) {
            // the row's symbols in name order: ES, NG
            EXPECT_NEAR(rows[t][1], lib.returns[t], 1e-13) << "row " << t;
        }
    }
}

TEST(PerContractPlaces, TheCostModelsVolatilityWindowIsTheLibrarysLogReturns) {
    using transaction_cost::TransactionCostManager;
    for (bool rolls : {false, true}) {
        SCOPED_TRACE(rolls ? "per-contract rolls" : "no roll");
        const Series ng = per_contract("NG.v.0", rolls);
        const auto lib = library(ng);
        // By hand from the library: the log of each bar's move, 0 on a change bar with no old close.
        TransactionCostManager expected;
        const std::vector<double> base = roll_series::return_bases(ng.raw, lib.flags);
        expected.record_log_return("NG.v.0", ng.raw[0], 0.0);
        for (size_t t = 1; t < ng.raw.size(); ++t) {
            const double to = lib.flags.old_close[t] ? *lib.flags.old_close[t] : ng.raw[t];
            expected.record_log_return(
                "NG.v.0", to, base[t - 1],
                lib.flags.change[t] && !lib.flags.old_close[t] && !lib.flags.base_close[t - 1]);
        }
        expected.set_own_day_volume("NG.v.0", ng.bars.back().volume);
        TransactionCostManager window, stepped;
        feed_futures_cost_model(window, ng.bars, ng.bars.back().timestamp + std::chrono::hours(24));
        FuturesCostFeedCarry carry;
        for (const auto& b : ng.bars) {
            feed_futures_cost_model_step(stepped, {b}, b.timestamp + std::chrono::hours(24), carry);
        }
        EXPECT_EQ(window.get_volatility_multiplier("NG.v.0"),
                  expected.get_volatility_multiplier("NG.v.0"));
        EXPECT_EQ(stepped.get_volatility_multiplier("NG.v.0"),
                  expected.get_volatility_multiplier("NG.v.0"));
        if (rolls) {
            // the field is read: the same bars without it give another window
            std::vector<Bar> stripped = ng.bars;
            for (auto& b : stripped) b.roll_from.reset();
            TransactionCostManager blind;
            feed_futures_cost_model(blind, stripped, ng.bars.back().timestamp + std::chrono::hours(24));
            EXPECT_NE(blind.get_volatility_multiplier("NG.v.0"),
                      expected.get_volatility_multiplier("NG.v.0"));
        }
    }
}
