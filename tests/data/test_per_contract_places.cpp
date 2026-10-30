// Rolls step 3, plan section 6 ("how we know we did not break it"): each place that builds its own
// series from its own bars (the PortfolioManager's history and covariance, the risk reader and the
// gate's MarketData, the cost model's volatility window) produces, given the same bars, the same
// returns and levels as the library (roll_series::build_series), on a series with no roll and on a
// per-contract series with every roll shape: a vendor switch the next bar does not confirm, a roll
// known on its bar, a vendor switch the next bar confirms, the AFTER_ROLL field after each roll.
// The strategy's own test is in tests/strategy/test_trend_adjusted_series.cpp, the engines' P&L in
// the backtest and live suites.
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "trade_ngin/data/roll_series.hpp"
#include "trade_ngin/strategy/base_strategy.hpp"

// Private members read: the cost model's window, the optimizer's covariance.
#define private public
#include "trade_ngin/transaction_cost/spread_model.hpp"
#include "trade_ngin/transaction_cost/transaction_cost_manager.hpp"
#include "trade_ngin/live/futures_cost_feed.hpp"
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

/// `unconfirmed`: the day of the vendor switch the next bar does not confirm (8 by default).
Series per_contract(const std::string& symbol, bool rolls, int unconfirmed = 8) {
    Series s;
    char held = 'A';
    for (int d = 1; d <= kDays; ++d) {
        Bar b;
        b.symbol = symbol;
        b.timestamp = at(d);
        b.volume = 1000.0;
        char shown = held;
        if (rolls && d == unconfirmed) {
            // a vendor switch to the next contract that the next day does not confirm: the held
            // contract's own bar, the field naming the target
            const char target = static_cast<char>(held + 1);
            b.roll_from = RollFrom{own(target, d), std::string(1, target), RollFromKind::AWAITS_NEXT, ""};
        } else if (rolls && d == 15) {
            // a crossover: known on its bar
            shown = held = 'B';
            b.roll_from = RollFrom{own('A', d), "A", RollFromKind::KNOWN, ""};
        } else if (rolls && d == 16) {
            b.roll_from = RollFrom{own('A', d), "A", RollFromKind::AFTER_ROLL, ""};
        } else if (rolls && d == 27) {
            // a vendor switch to C that day 28 confirms: B's own bar, the field naming C
            b.roll_from = RollFrom{own('C', d), "C", RollFromKind::AWAITS_NEXT, ""};
        } else if (rolls && d == 28) {
            b.roll_from = RollFrom{own('B', d), "B", RollFromKind::AFTER_ROLL, ""};
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

// The cost model's own window, return by return, with an unconfirmed switch (day 33) and a confirmed
// one (day 27, confirmed by day 28) inside its last 20 returns: the held contract's own moves, the
// bar that confirms the switch read from the target's close on the switch bar, nothing changed by
// the switch the next bar did not confirm. Both forms of the feed (the live window, the backtest's
// cycle by cycle).
TEST(PerContractPlaces, TheCostModelsWindowReturnsAreTheHeldContractsMovesAroundEverySwitch) {
    using transaction_cost::TransactionCostManager;
    const Series ng = per_contract("NG.v.0", true, 33);
    ASSERT_EQ(ng.ids[32], "C") << "day 33's bar is C's own";
    ASSERT_EQ(ng.rf[32]->instrument_id, "D");
    ASSERT_EQ(ng.ids[33], "C") << "day 34 stays in C: not confirmed";
    // By hand: bar t's log return, t = 1 .. 39 (day t + 1).
    std::vector<double> want;
    for (size_t t = 1; t < ng.raw.size(); ++t) {
        const int day = static_cast<int>(t) + 1;
        double to = ng.raw[t];
        double from = ng.raw[t - 1];
        if (day == 15) to = own('A', 15);  // the crossover bar: A's own move
        if (day == 28) from = own('C', 27);  // confirms day 27: from C's close on the switch bar
        want.push_back(std::log(to / from));
    }
    ASSERT_EQ(want.size(), 39u);
    // the two bars inside the window that read another close than their own previous bar's
    EXPECT_NE(want[26], std::log(ng.raw[27] / ng.raw[26])) << "day 28";
    EXPECT_EQ(want[32], std::log(ng.raw[33] / ng.raw[32])) << "day 34: C's own move";
    const std::deque<double> tail(want.end() - 20, want.end());
    TransactionCostManager window, stepped;
    feed_futures_cost_model(window, ng.bars, ng.bars.back().timestamp + std::chrono::hours(24));
    FuturesCostFeedCarry carry;
    for (const auto& b : ng.bars) {
        feed_futures_cost_model_step(stepped, {b}, b.timestamp + std::chrono::hours(24), carry);
    }
    for (const TransactionCostManager* m : {&window, &stepped}) {
        SCOPED_TRACE(m == &window ? "the live window" : "cycle by cycle");
        const std::deque<double>& got = m->spread_model_.symbol_log_returns_.at("NG.v.0");
        ASSERT_EQ(got.size(), 20u);
        for (size_t i = 0; i < 20; ++i) {
            EXPECT_EQ(got[i], tail[i]) << "return of day " << (40 - 20 + 1 + i);
        }
    }
}

namespace {

/// A sleeve that holds a fixed book from its first bar on and signals every symbol.
class PerContractFixedBook : public BaseStrategy {
public:
    PerContractFixedBook(std::string id, StrategyConfig config,
                         std::shared_ptr<DatabaseInterface> db,
                         std::unordered_map<std::string, Position> book)
        : BaseStrategy(std::move(id), std::move(config),
                       std::static_pointer_cast<trade_ngin::PostgresDatabase>(db)),
          book_(std::move(book)) {
        metadata_.name = "Per-contract Fixed Book Strategy";
    }
    Result<void> on_data(const std::vector<Bar>& data) override {
        (void)data;
        ++calls_;
        return Result<void>();
    }
    std::unordered_map<std::string, Position> get_target_positions() const override {
        return calls_ == 0 ? std::unordered_map<std::string, Position>{} : book_;
    }

private:
    std::unordered_map<std::string, Position> book_;
    size_t calls_{0};
};

}  // namespace

// The optimizer's covariance (optimize_positions, through process_market_data) on a book holding a
// per-contract NG with every roll shape: built on the library's returns, not on returns divided by
// the bars' own closes.
TEST_F(PerContractPmTest, TheOptimizersCovarianceIsBuiltOnTheLibrarysReturns) {
    PortfolioConfig pc{1'000'000.0, 1.0, 0.0, true};
    pc.allow_fractional_positions = false;
    pc.opt_config.tau = 1.0;
    pc.opt_config.capital = 1'000'000.0;
    pc.opt_config.cost_penalty_scalar = 50.0;
    pc.opt_config.max_iterations = 100;
    pc.opt_config.convergence_threshold = 1e-6;
    pc.opt_config.use_buffering = false;
    pc.risk_config = risk_config();
    pc.risk_config.var_limit = 1e6;
    pc.risk_config.jump_risk_limit = 1e6;
    pc.risk_config.max_correlation = 1.0;
    pc.risk_config.max_gross_leverage = 1e6;
    pc.risk_config.max_net_leverage = 1e6;
    pc.risk_modules = {test_none_module()};
    auto db = std::make_shared<MockPostgresDatabase>("mock://testdb");
    ASSERT_TRUE(db->connect().is_ok());
    const Series ng = per_contract("NG", true, 33);
    const auto lib = library(ng);
    std::unordered_map<std::string, Position> book;
    for (const char* symbol : {"NG", "ES"}) {
        Position p;
        p.symbol = symbol;
        p.quantity = Decimal(1.0);
        p.average_price = Decimal(100.0);
        p.last_update = at(0);
        book[symbol] = p;
    }
    StrategyConfig sc;
    sc.capital_allocation = 1'000'000.0;
    sc.max_leverage = 10.0;
    sc.asset_classes = {AssetClass::FUTURES};
    sc.frequencies = {DataFrequency::DAILY};
    auto strategy = std::make_shared<PerContractFixedBook>("TREND_FOLLOWING_PC", sc, db, book);
    ASSERT_TRUE(strategy->initialize().is_ok());
    ASSERT_TRUE(strategy->start().is_ok());
    PortfolioManager pm(pc, "PM_PC_OPT");
    ASSERT_TRUE(pm.add_strategy(strategy, 1.0, true).is_ok());
    ASSERT_TRUE(pm.process_market_data(with_es(ng), false, at(kDays + 1)).is_ok());
    ASSERT_TRUE(pm.covariance_cache_valid_) << "the optimizer did not build a covariance";
    // The library's returns of NG and ES's own returns, every date shared: the covariance.
    std::vector<double> es;
    const auto all = with_es(ng);
    for (size_t i = 3; i < all.size(); i += 2) {
        es.push_back((static_cast<double>(all[i].close) - static_cast<double>(all[i - 2].close)) /
                     static_cast<double>(all[i - 2].close));
    }
    const auto want = pm.calculate_covariance_matrix({{"ES", es}, {"NG", lib.returns}});
    ASSERT_EQ(pm.cached_covariance_.size(), 2u);
    for (size_t i = 0; i < 2; ++i) {
        for (size_t j = 0; j < 2; ++j) {
            EXPECT_NEAR(pm.cached_covariance_[i][j], want[i][j], 1e-12 * std::abs(want[i][j]))
                << "cell (" << i << "," << j << ") (0 = ES, 1 = NG)";
        }
    }
    // the switch the next bar confirmed is inside the covariance's returns: a covariance built on
    // the bars' own closes is another matrix
    std::vector<double> own_closes;
    for (size_t t = 1; t < ng.raw.size(); ++t) {
        own_closes.push_back(lib.returns[t - 1]);
    }
    own_closes[26] = (ng.raw[27] - ng.raw[26]) / ng.raw[26];
    const auto blind = pm.calculate_covariance_matrix({{"ES", es}, {"NG", own_closes}});
    EXPECT_GT(std::abs(blind[1][1] - want[1][1]), 1e-6 * want[1][1]);
}
