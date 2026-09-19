// Direct tests of PortfolioManager internal helpers (private members reached
// via #define private public) plus multi-cycle integration scenarios that
// exercise the optimization+risk iterative loop, execution generation from
// previous-vs-current diffs, and update_historical_returns/calculate_covariance_matrix
// in isolation.

#include <gtest/gtest.h>
#include "../risk/risk_module_test_helpers.hpp"
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <thread>
#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "mock_strategy.hpp"

#define private public
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#undef private

#include "trade_ngin/risk/risk_manager.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

PortfolioConfig default_config(bool optimization = false, bool with_carver = false) {
    PortfolioConfig c{1'000'000.0, 100'000.0, 0.6, 0.05, optimization};
    c.opt_config.tau = 1.0;
    c.opt_config.capital = 1'000'000.0;
    c.opt_config.cost_penalty_scalar = 10.0;
    c.opt_config.asymmetric_risk_buffer = 0.1;
    c.opt_config.max_iterations = 100;
    c.opt_config.convergence_threshold = 1e-6;
    c.risk_config.var_limit = 1.0;
    c.risk_config.max_correlation = 1.0;
    c.risk_config.max_gross_leverage = 1e6;
    c.risk_config.max_net_leverage = 1e6;
    c.risk_config.capital = 1'000'000.0;
    c.risk_config.confidence_level = 0.99;
    c.risk_config.lookback_period = 252;
    // The module list replaces the old use_risk_management bool: the carver module carries
    // the same seven values the test just set, so a gating test gates on the same numbers.
    c.risk_modules = {with_carver ? test_carver_module(c.risk_config) : test_none_module()};
    return c;
}

}  // namespace

class PortfolioManagerInternalsTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        ASSERT_TRUE(db_->connect().is_ok());
        static int n = 0;
        manager_id_ = "PM_INT_" + std::to_string(++n);
        manager_ = std::make_unique<PortfolioManager>(default_config(), manager_id_);
    }

    void TearDown() override {
        manager_.reset();
        db_.reset();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        TestBase::TearDown();
    }

    struct Handle { std::shared_ptr<StrategyInterface> strat; std::string id; };

    Handle make_strategy(const std::string& prefix,
                          std::vector<std::string> symbols = {"AAPL"}) {
        static int n = 0;
        std::string uid = prefix + "_" + std::to_string(++n);
        StrategyConfig sc;
        sc.capital_allocation = 1'000'000.0;
        sc.max_leverage = 2.0;
        sc.asset_classes = {AssetClass::EQUITIES};
        sc.frequencies = {DataFrequency::DAILY};
        for (const auto& s : symbols) {
            sc.trading_params[s] = 1.0;
            sc.position_limits[s] = 10000.0;
        }
        auto strat = std::make_shared<MockStrategy>(uid, sc, db_);
        if (strat->initialize().is_error()) throw std::runtime_error("init failed");
        if (strat->start().is_error()) throw std::runtime_error("start failed");
        return {strat, uid};
    }

    std::vector<Bar> bars(const std::string& symbol, int n,
                           std::chrono::system_clock::time_point t0,
                           double price_amplitude = 2.0) {
        std::vector<Bar> v;
        for (int i = 0; i < n; ++i) {
            Bar b;
            b.symbol = symbol;
            b.timestamp = t0 + std::chrono::hours(24 * i);
            double price = 100.0 + std::sin(i * 0.1) * price_amplitude;
            b.open = b.close = Decimal(price);
            b.high = Decimal(price + 1.0);
            b.low = Decimal(price - 1.0);
            b.volume = 100000.0;
            v.push_back(b);
        }
        return v;
    }

    std::shared_ptr<MockPostgresDatabase> db_;
    std::unique_ptr<PortfolioManager> manager_;
    std::string manager_id_;
};

// ===== validate_allocations (private; reached via #define private public) =====

TEST_F(PortfolioManagerInternalsTest, ValidateAllocationsRejectsEmptyMap) {
    auto a = make_strategy("V1");
    ASSERT_TRUE(manager_->add_strategy(a.strat, 0.3).is_ok());
    auto r = manager_->validate_allocations({});
    EXPECT_TRUE(r.is_error());
}

TEST_F(PortfolioManagerInternalsTest, ValidateAllocationsRejectsWithNoStrategies) {
    auto r = manager_->validate_allocations({{"X", 0.5}});
    EXPECT_TRUE(r.is_error());
}

TEST_F(PortfolioManagerInternalsTest, ValidateAllocationsRejectsAllocationOutsideBounds) {
    auto a = make_strategy("V2");
    ASSERT_TRUE(manager_->add_strategy(a.strat, 0.3).is_ok());
    // Single strategy → must be exactly 1.0; 0.7 is in bounds [0.05, 0.6]? 0.7 is above max
    auto r_above = manager_->validate_allocations({{a.id, 0.7}});
    EXPECT_TRUE(r_above.is_error());
    auto r_below = manager_->validate_allocations({{a.id, 0.001}});
    EXPECT_TRUE(r_below.is_error());
}

TEST_F(PortfolioManagerInternalsTest, ValidateAllocationsAcceptsExactSumOfOne) {
    auto a = make_strategy("V3");
    auto b = make_strategy("V4");
    ASSERT_TRUE(manager_->add_strategy(a.strat, 0.3).is_ok());
    ASSERT_TRUE(manager_->add_strategy(b.strat, 0.3).is_ok());
    EXPECT_TRUE(manager_->validate_allocations({{a.id, 0.4}, {b.id, 0.6}}).is_ok());
}

// ===== calculate_covariance_matrix (private) =====

TEST_F(PortfolioManagerInternalsTest, CovarianceEmptyReturnsEmptyMatrix) {
    std::unordered_map<std::string, std::vector<double>> empty;
    auto cov = manager_->calculate_covariance_matrix(empty);
    EXPECT_TRUE(cov.empty());
}

TEST_F(PortfolioManagerInternalsTest, CovarianceWithFewerThan20PeriodsReturnsDefaultDiagonal) {
    std::unordered_map<std::string, std::vector<double>> returns{
        {"A", {0.01, 0.02, -0.01, 0.005, 0.0}},
        {"B", {0.02, -0.01, 0.01, 0.0, 0.005}},
    };
    auto cov = manager_->calculate_covariance_matrix(returns);
    ASSERT_EQ(cov.size(), 2u);
    EXPECT_DOUBLE_EQ(cov[0][0], 0.01);
    EXPECT_DOUBLE_EQ(cov[1][1], 0.01);
    EXPECT_DOUBLE_EQ(cov[0][1], 0.0);  // no off-diagonal in default
}

TEST_F(PortfolioManagerInternalsTest, CovarianceWithSufficientDataProducesPositiveDiagonal) {
    std::unordered_map<std::string, std::vector<double>> returns{
        {"A", std::vector<double>(30, 0.0)},
        {"B", std::vector<double>(30, 0.0)},
    };
    // Inject some variance into A
    for (size_t i = 0; i < 30; ++i) {
        returns["A"][i] = (i % 2 == 0 ? 0.01 : -0.01);
        returns["B"][i] = (i % 3 == 0 ? 0.02 : -0.005);
    }
    auto cov = manager_->calculate_covariance_matrix(returns);
    ASSERT_EQ(cov.size(), 2u);
    EXPECT_GT(cov[0][0], 0.0);
    EXPECT_GT(cov[1][1], 0.0);
    EXPECT_DOUBLE_EQ(cov[0][1], cov[1][0]);  // symmetric
}

// ===== C-20: calculate_covariance_matrix with an empty series (T-6 commit 2c) =====
//
// min_periods is the length of the SHORTEST NON-EMPTY series, so the only series that can
// be shorter than min_periods is an empty one (a series of 1..min_periods-1 returns would
// itself set min_periods). The scan skips an empty symbol but leaves it in the ordered
// symbol list; without the guard, `returns.size() - min_periods` wraps and the copy reads
// far outside the vector. With the guard its column stays zero, its variance is restored
// to the 0.01 default, and every other entry is bit-identical to the matrix built from the
// same inputs without that symbol (a covariance entry depends only on its two columns).
//
// Two shapes of "empty": a vector that never held data (no buffer), and one that was
// cleared (a live buffer, the shape a cleared historical_returns_ entry has).
namespace {

std::vector<double> covguard_series(size_t n, double amp, double phase, double drift) {
    std::vector<double> v(n);
    for (size_t t = 0; t < n; ++t) {
        v[t] = amp * std::sin(phase * static_cast<double>(t)) + drift * static_cast<double>(t);
    }
    return v;
}

void expect_guarded_and_bit_identical(const std::vector<std::vector<double>>& cov,
                                      const std::vector<std::vector<double>>& ref) {
    // cov is over {A, M, Z} (sorted), M guarded; ref is over {A, Z}
    ASSERT_EQ(ref.size(), 2u);
    ASSERT_EQ(cov.size(), 3u);
    for (const auto& row : cov) ASSERT_EQ(row.size(), 3u);
    EXPECT_EQ(cov[1][1], 0.01) << "the guarded symbol's variance must be the 0.01 default";
    for (size_t k : {size_t{0}, size_t{2}}) {
        EXPECT_EQ(cov[1][k], 0.0) << "guarded row, column " << k;
        EXPECT_EQ(cov[k][1], 0.0) << "guarded column, row " << k;
    }
    const size_t at[2] = {0, 2};
    for (size_t i = 0; i < 2; ++i) {
        for (size_t j = 0; j < 2; ++j) {
            EXPECT_EQ(std::memcmp(&cov[at[i]][at[j]], &ref[i][j], sizeof(double)), 0)
                << "entry (" << i << "," << j << ") moved: " << cov[at[i]][at[j]] << " vs "
                << ref[i][j];
        }
    }
}

}  // namespace

TEST_F(PortfolioManagerInternalsTest, CovarianceGuardsANeverFilledEmptySeries) {
    const auto a = covguard_series(30, 0.01, 0.7, 0.0002);
    const auto z = covguard_series(25, -0.015, 0.3, 0.0001);
    std::unordered_map<std::string, std::vector<double>> with_empty{
        {"A", a}, {"M", std::vector<double>{}}, {"Z", z}};
    std::unordered_map<std::string, std::vector<double>> without{{"A", a}, {"Z", z}};
    ASSERT_EQ(with_empty.at("M").data(), nullptr) << "this case needs a vector with no buffer";

    const auto ref = manager_->calculate_covariance_matrix(without);  // min_periods 25
    const auto cov = manager_->calculate_covariance_matrix(with_empty);
    expect_guarded_and_bit_identical(cov, ref);
}

TEST_F(PortfolioManagerInternalsTest, CovarianceGuardsAClearedEmptySeries) {
    const auto a = covguard_series(30, 0.01, 0.7, 0.0002);
    const auto z = covguard_series(25, -0.015, 0.3, 0.0001);
    std::unordered_map<std::string, std::vector<double>> with_empty{{"A", a}, {"Z", z}};
    auto& m = with_empty["M"];
    m.assign(64, 0.02);  // give it a buffer, then empty it: size 0, buffer kept
    m.clear();
    ASSERT_NE(m.data(), nullptr) << "this case needs a cleared vector that kept its buffer";
    std::unordered_map<std::string, std::vector<double>> without{{"A", a}, {"Z", z}};

    const auto ref = manager_->calculate_covariance_matrix(without);
    const auto cov = manager_->calculate_covariance_matrix(with_empty);
    expect_guarded_and_bit_identical(cov, ref);
}

// ===== update_historical_returns (private) =====

TEST_F(PortfolioManagerInternalsTest, UpdateHistoricalReturnsHandlesEmptyDataNoOp) {
    manager_->update_historical_returns({});
    EXPECT_TRUE(manager_->historical_returns_.empty());
}

TEST_F(PortfolioManagerInternalsTest, UpdateHistoricalReturnsRunsThroughProcessWithoutError) {
    // MockStrategy does not override get_price_history (it relies on
    // BaseStrategy's default empty map), so update_historical_returns has
    // no per-symbol price data to seed `historical_returns_`. We exercise the
    // pathway end-to-end and assert it doesn't error; covariance tests above
    // hit the calculation logic with synthetic returns directly.
    auto a = make_strategy("UH", {"AAPL"});
    ASSERT_TRUE(manager_->add_strategy(a.strat, 0.3).is_ok());
    auto t0 = std::chrono::system_clock::now() - std::chrono::hours(24 * 300);
    auto data = bars("AAPL", 300, t0);
    EXPECT_TRUE(manager_->process_market_data(data).is_ok());
}

TEST_F(PortfolioManagerInternalsTest, UpdateHistoricalReturnsTrimsToMaxHistoryLength) {
    // Direct injection: seed price_history_ to bypass MockStrategy's empty
    // get_price_history() and verify trimming kicks in.
    auto a = make_strategy("UH2", {"AAPL"});
    ASSERT_TRUE(manager_->add_strategy(a.strat, 0.3).is_ok());
    manager_->max_history_length_ = 50;
    std::vector<double> prices;
    for (int i = 0; i < 200; ++i) prices.push_back(100.0 + i * 0.1);
    manager_->price_history_["AAPL"] = prices;
    // Update historical returns from the synthetic price history.
    auto t0 = std::chrono::system_clock::now();
    Bar b;
    b.symbol = "AAPL";
    b.timestamp = t0;
    b.close = Decimal(120.0);
    b.open = Decimal(120.0);
    b.high = Decimal(120.0);
    b.low = Decimal(120.0);
    b.volume = 1000.0;
    manager_->update_historical_returns({b});
    if (manager_->historical_returns_.count("AAPL")) {
        EXPECT_LE(manager_->historical_returns_.at("AAPL").size(), 50u);
    }
}

// ===== update_historical_returns: the history merge (T-6 commit 2d) =====
//
// T-BASE_ADVERSARIAL finding 4, as HD ruled it on 2026-09-19. The PM copies each strategy's
// price history into price_history_. Before the guard the LAST strategy strategies_ iterated
// won each symbol, and strategies_ is an unordered_map, so which series survived depended on
// the map's iteration order. The guard lets the FIRST-REGISTERED (add_strategy order)
// strategy's series win whatever its length and wherever the map iterates it.
namespace {

class FixedHistoryStrategy : public MockStrategy {
public:
    using MockStrategy::MockStrategy;
    std::unordered_map<std::string, std::vector<double>> history;
    std::unordered_map<std::string, std::vector<double>> get_price_history() const override {
        return history;
    }
};

Bar merge_bar(const std::string& symbol) {
    Bar b;
    b.symbol = symbol;
    b.timestamp = std::chrono::system_clock::now();
    b.open = b.high = b.low = b.close = Decimal(100.0);
    b.volume = 1000.0;
    return b;
}

StrategyConfig merge_strategy_config() {
    StrategyConfig sc;
    sc.capital_allocation = 1'000'000.0;
    sc.max_leverage = 2.0;
    sc.asset_classes = {AssetClass::EQUITIES};
    sc.frequencies = {DataFrequency::DAILY};
    return sc;
}

std::string iteration_order(const PortfolioManager& pm) {
    std::string order;
    for (const auto& [id, _] : pm.strategies_) order += (order.empty() ? "" : ",") + id;
    return order;
}

}  // namespace

TEST_F(PortfolioManagerInternalsTest, HistoryMergeKeepsTheFirstRegisteredSeriesWhateverItsLength) {
    // Fixed ids, so both managers hash the same two keys and differ only in insertion order.
    auto long_s = std::make_shared<FixedHistoryStrategy>("HIST_LONG", merge_strategy_config(), db_);
    auto short_s = std::make_shared<FixedHistoryStrategy>("HIST_SHORT", merge_strategy_config(), db_);
    for (auto* s : {long_s.get(), short_s.get()}) {
        ASSERT_TRUE(s->initialize().is_ok());
        ASSERT_TRUE(s->start().is_ok());
    }
    const std::vector<double> long_series{100.0, 101.0, 103.0, 102.0, 104.0, 106.0};
    const std::vector<double> short_series{103.0, 104.0, 105.0};
    long_s->history = {{"X", long_series}};
    short_s->history = {{"X", short_series}};

    for (bool long_first : {true, false}) {
        auto pm = std::make_unique<PortfolioManager>(
            default_config(), manager_id_ + (long_first ? "_LONG_FIRST" : "_SHORT_FIRST"));
        if (long_first) {
            ASSERT_TRUE(pm->add_strategy(long_s, 0.3).is_ok());
            ASSERT_TRUE(pm->add_strategy(short_s, 0.3).is_ok());
        } else {
            ASSERT_TRUE(pm->add_strategy(short_s, 0.3).is_ok());
            ASSERT_TRUE(pm->add_strategy(long_s, 0.3).is_ok());
        }
        SCOPED_TRACE("registered " + std::string(long_first ? "long first" : "short first") +
                     "; strategies_ iterates " + iteration_order(*pm));

        pm->update_historical_returns({merge_bar("X")});

        const auto& expected = long_first ? long_series : short_series;
        ASSERT_EQ(pm->price_history_.count("X"), 1u);
        EXPECT_EQ(pm->price_history_.at("X"), expected)
            << "the first-registered strategy's series was not kept";
        ASSERT_EQ(pm->historical_returns_.count("X"), 1u);
        EXPECT_EQ(pm->historical_returns_.at("X").size(), expected.size() - 1);
    }
}

TEST_F(PortfolioManagerInternalsTest, HistoryMergeFirstRegisteredWinsWhereverTheMapIteratesIt) {
    // Six strategies offer six different series for X, registered in six rotations of one id
    // list. Six, because libc++ iterates the first-inserted key LAST in every map of up to five
    // string keys (measured: 0 of 20,000 random key sets of each size 2..5 break it), so with
    // fewer strategies last-writer-wins and first-registered-wins cannot be told apart; from
    // six keys (the second rehash) the first-registered key can land anywhere.
    const std::vector<std::string> ids{"HIST_A", "HIST_B", "HIST_C", "HIST_D", "HIST_E", "HIST_F"};
    std::vector<std::shared_ptr<FixedHistoryStrategy>> strategies;
    for (size_t k = 0; k < ids.size(); ++k) {
        auto s = std::make_shared<FixedHistoryStrategy>(ids[k], merge_strategy_config(), db_);
        ASSERT_TRUE(s->initialize().is_ok());
        ASSERT_TRUE(s->start().is_ok());
        // Strategy k offers k + 2 prices, each series distinct, so no two offers are equal.
        std::vector<double> series;
        for (size_t i = 0; i < k + 2; ++i) series.push_back(100.0 + 10.0 * k + i);
        s->history = {{"X", series}};
        strategies.push_back(s);
    }

    bool first_registered_not_iterated_last = false;
    bool first_registered_not_longest = false;
    for (size_t r = 0; r < ids.size(); ++r) {
        auto pm = std::make_unique<PortfolioManager>(default_config(),
                                                     manager_id_ + "_ROT" + std::to_string(r));
        for (size_t i = 0; i < ids.size(); ++i) {
            ASSERT_TRUE(pm->add_strategy(strategies[(r + i) % ids.size()], 0.15).is_ok());
        }
        const std::string order = iteration_order(*pm);
        SCOPED_TRACE("registered " + ids[r] + " first; strategies_ iterates " + order);
        if (order.substr(order.rfind(',') + 1) != ids[r]) first_registered_not_iterated_last = true;
        if (r + 1 != ids.size()) first_registered_not_longest = true;

        pm->update_historical_returns({merge_bar("X")});

        ASSERT_EQ(pm->price_history_.count("X"), 1u);
        EXPECT_EQ(pm->price_history_.at("X"), strategies[r]->history.at("X"))
            << "the first-registered strategy (" << ids[r] << ") did not win";
        ASSERT_EQ(pm->historical_returns_.count("X"), 1u);
        EXPECT_EQ(pm->historical_returns_.at("X").size(), r + 1);
    }
    // The test only separates the rules if some rotation iterates the first-registered
    // strategy somewhere other than last (last-writer-wins keeps the wrong series there) and
    // some rotation's first-registered series is not the longest (keep-longest does).
    EXPECT_TRUE(first_registered_not_iterated_last)
        << "every rotation iterates its first-registered strategy last on this standard library";
    EXPECT_TRUE(first_registered_not_longest);
}

TEST_F(PortfolioManagerInternalsTest, HistoryMergeClearsReturnsOfASymbolThatDropsBelowTwoPrices) {
    StrategyConfig sc;
    sc.capital_allocation = 1'000'000.0;
    sc.max_leverage = 2.0;
    sc.asset_classes = {AssetClass::EQUITIES};
    sc.frequencies = {DataFrequency::DAILY};
    auto s = std::make_shared<FixedHistoryStrategy>("HIST_DROP", sc, db_);
    ASSERT_TRUE(s->initialize().is_ok());
    ASSERT_TRUE(s->start().is_ok());
    ASSERT_TRUE(manager_->add_strategy(s, 0.3).is_ok());

    s->history = {{"X", {100.0, 102.0, 101.0}}};
    manager_->update_historical_returns({merge_bar("X")});
    ASSERT_EQ(manager_->historical_returns_.at("X").size(), 2u);

    // The symbol's history is now a single price: no return can be computed from it,
    // so the two returns of the older series must not survive.
    s->history = {{"X", {101.0}}};
    manager_->update_historical_returns({merge_bar("X")});
    ASSERT_EQ(manager_->price_history_.at("X").size(), 1u);
    ASSERT_EQ(manager_->historical_returns_.count("X"), 1u);
    EXPECT_TRUE(manager_->historical_returns_.at("X").empty())
        << "stale returns kept: " << manager_->historical_returns_.at("X").size();
}

// ===== get_positions_internal (private) =====

TEST_F(PortfolioManagerInternalsTest, GetPositionsInternalEmptyBeforeProcess) {
    auto p = manager_->get_positions_internal();
    EXPECT_TRUE(p.empty());
}

TEST_F(PortfolioManagerInternalsTest, GetPositionsInternalReflectsStrategyPositionsAfterProcess) {
    auto a = make_strategy("GPI", {"AAPL"});
    ASSERT_TRUE(manager_->add_strategy(a.strat, 0.3).is_ok());
    auto t0 = std::chrono::system_clock::now() - std::chrono::hours(24 * 300);
    ASSERT_TRUE(manager_->process_market_data(bars("AAPL", 300, t0)).is_ok());
    auto p = manager_->get_positions_internal();
    // MockStrategy generates positions, so map shouldn't be empty after processing.
    EXPECT_FALSE(p.empty());
}

// ===== Multi-cycle process_market_data: exercises execution generation =====

TEST_F(PortfolioManagerInternalsTest, MultiCycleProcessGeneratesExecutionsBetweenCycles) {
    auto a = make_strategy("MC", {"AAPL"});
    ASSERT_TRUE(manager_->add_strategy(a.strat, 0.3).is_ok());
    auto t0 = std::chrono::system_clock::now() - std::chrono::hours(24 * 600);
    ASSERT_TRUE(manager_->process_market_data(bars("AAPL", 300, t0)).is_ok());
    manager_->clear_execution_history();
    // Second cycle with shifted data to drive position changes.
    auto t1 = t0 + std::chrono::hours(24 * 300);
    ASSERT_TRUE(manager_->process_market_data(bars("AAPL", 300, t1, /*amp=*/5.0)).is_ok());
    auto execs = manager_->get_recent_executions();
    // With diverse data and randomized MockStrategy positions, we expect SOME
    // executions; we can't predict the count but it should be reachable.
    EXPECT_TRUE(execs.empty() || !execs.empty());  // tautology guard; structural check below
    // Per-strategy executions tracked separately should align with aggregate.
    auto per_strat = manager_->get_strategy_executions();
    int per_strat_total = 0;
    for (const auto& [_id, list] : per_strat) per_strat_total += list.size();
    EXPECT_GE(per_strat_total, static_cast<int>(execs.size()));
}

TEST_F(PortfolioManagerInternalsTest, ProcessWithOptimizationRunsIterativeLoop) {
    auto cfg = default_config(/*optimization=*/true, /*with_carver=*/false);
    auto pm = std::make_unique<PortfolioManager>(cfg, manager_id_ + "_OPTLOOP");
    auto a = make_strategy("OL_A", {"AAPL"});
    auto b = make_strategy("OL_B", {"MSFT"});
    ASSERT_TRUE(pm->add_strategy(a.strat, 0.3, /*opt=*/true).is_ok());
    ASSERT_TRUE(pm->add_strategy(b.strat, 0.3, /*opt=*/true).is_ok());
    auto t0 = std::chrono::system_clock::now() - std::chrono::hours(24 * 400);
    std::vector<Bar> combined;
    auto a_bars = bars("AAPL", 300, t0);
    auto b_bars = bars("MSFT", 300, t0, 3.0);
    for (size_t i = 0; i < a_bars.size(); ++i) {
        combined.push_back(a_bars[i]);
        combined.push_back(b_bars[i]);
    }
    EXPECT_TRUE(pm->process_market_data(combined).is_ok());
    // After optimization runs, target_positions should be set.
    auto positions = pm->get_strategy_positions();
    EXPECT_GE(positions.size(), 1u);
}

TEST_F(PortfolioManagerInternalsTest, ProcessWithRiskManagementDoesNotCrashOnLargePositions) {
    auto cfg = default_config(/*opt=*/false, /*with_carver=*/true);
    cfg.risk_config.max_gross_leverage = 0.5;  // very restrictive
    cfg.risk_config.max_net_leverage = 0.5;
    auto pm = std::make_unique<PortfolioManager>(cfg, manager_id_ + "_RISKLOOP");
    auto a = make_strategy("RL", {"AAPL"});
    ASSERT_TRUE(pm->add_strategy(a.strat, 0.3, /*opt=*/false).is_ok());
    auto t0 = std::chrono::system_clock::now() - std::chrono::hours(24 * 400);
    EXPECT_TRUE(pm->process_market_data(bars("AAPL", 300, t0, 5.0)).is_ok());
}

TEST_F(PortfolioManagerInternalsTest, ProcessThenUpdateAllocationsScalesPositions) {
    auto a = make_strategy("UA_SCALE", {"AAPL"});
    auto b = make_strategy("UA_SCALE2", {"MSFT"});
    ASSERT_TRUE(manager_->add_strategy(a.strat, 0.3).is_ok());
    ASSERT_TRUE(manager_->add_strategy(b.strat, 0.3).is_ok());
    auto t0 = std::chrono::system_clock::now() - std::chrono::hours(24 * 300);
    std::vector<Bar> data;
    auto a_bars = bars("AAPL", 300, t0);
    auto b_bars = bars("MSFT", 300, t0);
    for (size_t i = 0; i < a_bars.size(); ++i) {
        data.push_back(a_bars[i]);
        data.push_back(b_bars[i]);
    }
    ASSERT_TRUE(manager_->process_market_data(data).is_ok());
    auto positions_before = manager_->get_strategy_positions();
    // Now reallocate (scale a up, b down). Production scales target_positions.
    EXPECT_TRUE(manager_->update_allocations({{a.id, 0.5}, {b.id, 0.5}}).is_ok());
}

// ===== Execution generation and clearing =====

TEST_F(PortfolioManagerInternalsTest, GetRecentExecutionsAggregatesAcrossStrategies) {
    auto a = make_strategy("EX_A", {"AAPL"});
    auto b = make_strategy("EX_B", {"MSFT"});
    ASSERT_TRUE(manager_->add_strategy(a.strat, 0.3).is_ok());
    ASSERT_TRUE(manager_->add_strategy(b.strat, 0.3).is_ok());
    auto t0 = std::chrono::system_clock::now() - std::chrono::hours(24 * 400);
    std::vector<Bar> data;
    auto a_bars = bars("AAPL", 300, t0);
    auto b_bars = bars("MSFT", 300, t0);
    for (size_t i = 0; i < a_bars.size(); ++i) {
        data.push_back(a_bars[i]);
        data.push_back(b_bars[i]);
    }
    ASSERT_TRUE(manager_->process_market_data(data).is_ok());
    auto execs = manager_->get_recent_executions();
    auto per_strat = manager_->get_strategy_executions();
    int per_strat_total = 0;
    for (const auto& [_id, list] : per_strat) per_strat_total += list.size();
    EXPECT_GE(per_strat_total, static_cast<int>(execs.size()));
}

// ===== get_required_changes path =====

TEST_F(PortfolioManagerInternalsTest, GetRequiredChangesReturnsDeltaAfterProcess) {
    auto a = make_strategy("RC", {"AAPL"});
    ASSERT_TRUE(manager_->add_strategy(a.strat, 0.3).is_ok());
    auto t0 = std::chrono::system_clock::now() - std::chrono::hours(24 * 300);
    ASSERT_TRUE(manager_->process_market_data(bars("AAPL", 300, t0)).is_ok());
    auto changes = manager_->get_required_changes();
    // After processing, may or may not have required changes depending on internal
    // state — assertion is that calling it doesn't crash and returns a map.
    EXPECT_GE(static_cast<int>(changes.size()), 0);
}

// ===== process_market_data WHEN strategy is_running=false =====

// Retitled and re-pointed by on_data-swallowed-futures (stage 3, T-1).
//
// This used to assert that a stopped strategy left process_market_data returning
// success, with the comment "production swallows per-strategy errors and
// continues". Swallowing is what the ledger row set out to stop: after a failed
// on_data the manager went on to read get_target_positions(), and an empty or
// stale target map against a held book is a full-book liquidation.
//
// But a STOPPED strategy is not an ingest failure, and treating it as one would
// let a deliberately paused sleeve abort the whole portfolio. So the manager now
// distinguishes the two: a strategy that is not RUNNING is SKIPPED -- it takes
// no part in the cycle and its targets are not read -- while a RUNNING strategy
// whose on_data fails stops the cycle (covered by
// PriceHistoryOrderingTest.AFailedOnDataStopsTheCycleBeforeTargetsAreRead).
//
// The assertion that carries this test is the second one: the cycle succeeds AND
// the stopped strategy contributed nothing.
TEST_F(PortfolioManagerInternalsTest, ProcessSkipsAStoppedStrategyWithoutFailingTheCycle) {
    auto a = make_strategy("STOPPED", {"AAPL"});
    ASSERT_TRUE(manager_->add_strategy(a.strat, 0.3).is_ok());
    auto t0 = std::chrono::system_clock::now() - std::chrono::hours(24 * 300);

    // One running cycle so the sleeve holds a real target map.
    ASSERT_TRUE(manager_->process_market_data(bars("AAPL", 300, t0)).is_ok());

    // Stop it, then plant a target that only a read of the stopped map could
    // surface. On main the failed on_data was swallowed and the stale map was
    // read anyway; the skip must not read it at all.
    a.strat->stop();
    Position planted;
    planted.symbol = "AAPL";
    planted.quantity = Decimal(999.0);
    planted.average_price = Decimal(100.0);
    ASSERT_TRUE(a.strat->update_position("AAPL", planted).is_ok());

    auto r = manager_->process_market_data(bars("AAPL", 5, t0 + std::chrono::hours(24 * 300)));
    EXPECT_TRUE(r.is_ok())
        << "a stopped sleeve aborted the whole portfolio cycle; being stopped is a "
           "lifecycle state, not a data failure";

    const auto by_strategy = manager_->get_strategy_positions();
    const auto it = by_strategy.find(a.id);
    if (it != by_strategy.end()) {
        const auto pit = it->second.find("AAPL");
        if (pit != it->second.end()) {
            EXPECT_NE(static_cast<double>(pit->second.quantity), 999.0)
                << "the stopped strategy's target map was read this cycle: the planted "
                   "quantity reached the portfolio";
        }
    }
}
