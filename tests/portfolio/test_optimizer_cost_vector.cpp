// tests/portfolio/test_optimizer_cost_vector.cpp
//
// T-7b-1 C8d: H-2 + F4, the optimizer's cost vector (HD 2026-09-17 "thin sessions are traded";
// STAGE3_PLAN section 28, "Commit 10 not taken": the cost penalty was unfed and mis-scaled).
//
// The dynamic optimizer's objective is  sqrt(e' S e) + scalar x sum_i |dw_i| x costs[i],  with
// the positions dw in WEIGHT (notional / capital). A change of n contracts of symbol i is
// dw = n x notional_i / capital, and it costs n x cost_per_contract_i dollars, which as a fraction
// of capital (the tracking error's unit) is n x cost_per_contract_i / capital. So
//
//     costs[i] = cost_per_contract_i / notional_per_contract_i        (F4, the unit that is right)
//
// makes |dw| x costs[i] = n x cost_per_contract_i / capital. The parent divided by capital, so
// |dw| x costs[i] was n x cost_per_contract_i x notional_i / capital^2, understated by
// capital / notional_i (13.8x for MES at 7,252.5 on $500,000; 4.6x for ZF at 107.85).
//
// H-2: PortfolioManager::calculate_trading_costs prices every entry through the PM's OWN cost
// manager. The backtest feeds it (backtest_coordinator.cpp); the live futures runners never did,
// so live priced every entry off the fallbacks (ADV 100,000, vol_mult 1.0). They now feed it the
// same K2 feed C8a gives the execution manager (futures_cost_feed.hpp), before
// process_market_data runs the optimizer.
//
// The scalar: DynamicOptConfig::from_json read cost_penalty_scalar with get<int>(), so a
// fractional value (a sweep's 50 / 14 = 3.571...) was floored to 3 without a word.
//
// The PM cases reach the private cost vector through `#define private public` (as
// test_portfolio_manager_internals.cpp does); the wiring cases read the runner sources (they are
// main()s and cannot be linked here).

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#define private public
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#undef private

#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "../risk/risk_module_test_helpers.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/live/futures_cost_feed.hpp"
#include "trade_ngin/optimization/dynamic_optimizer.hpp"
#include "trade_ngin/strategy/trend_following.hpp"
#include "trade_ngin/transaction_cost/transaction_cost_manager.hpp"
#include "../transaction_cost/session_metadata_rows.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;
using trade_ngin::transaction_cost::TransactionCostManager;

namespace {

constexpr auto npos = std::string::npos;
constexpr double kCapital = 500'000.0;

// Two real contracts at their 2026-05-01 closes (OPTIMIZER_CLAIMS_VERIFICATION_2026-09-23 section 6):
// MES 5 x 7,252.5 = $36,262.50 of notional, ZF 1,000 x 107.85 = $107,850.
const std::string kMes = "MES.v.0";
const std::string kZf = "ZF.v.0";
constexpr double kMesSize = 5.0;
constexpr double kMesPrice = 7252.5;
constexpr double kZfSize = 1000.0;
constexpr double kZfPrice = 107.85;

Bar bar_at(const std::string& symbol, int day_index, double close, double volume) {
    const Timestamp ts = std::chrono::system_clock::from_time_t(
        1700000000LL + static_cast<long long>(day_index) * 86400LL);
    return Bar(ts, close, close, close, close, volume, symbol);
}

/// The date of the run whose T-1 bar is the feed's latest bar: the day after it (the fill day the
/// feed's weekend merge reads, T-7b-2 C8c3).
Timestamp run_date_after(const std::vector<Bar>& bars) {
    Timestamp latest{};
    for (const auto& b : bars) latest = std::max(latest, b.timestamp);
    return latest + std::chrono::hours(24);
}

/// n bars, the close stepping +step / -step alternately, every volume `volume`.
std::vector<Bar> zigzag(const std::string& symbol, int n, double close, double step,
                        double volume) {
    std::vector<Bar> bars;
    for (int i = 0; i < n; ++i) {
        bars.push_back(bar_at(symbol, i, close, volume));
        close *= (i % 2 == 0) ? (1.0 + step) : (1.0 - step);
    }
    return bars;
}

/// spread_model.cpp calculate_volatility_multiplier by hand (as test_futures_cost_feed.cpp).
double vol_mult_by_hand(const std::vector<Bar>& bars, size_t window = 20) {
    std::vector<double> r;
    for (size_t i = 1; i < bars.size(); ++i) {
        r.push_back(std::log(static_cast<double>(bars[i].close) /
                             static_cast<double>(bars[i - 1].close)));
    }
    if (r.size() > window) r.erase(r.begin(), r.end() - static_cast<long>(window));
    if (r.size() < 2) return 1.0;
    const double mean = std::accumulate(r.begin(), r.end(), 0.0) / static_cast<double>(r.size());
    double ss = 0.0;
    for (double x : r) ss += (x - mean) * (x - mean);
    const double sd = std::sqrt(ss / static_cast<double>(r.size() - 1));
    const double z = std::clamp((sd - 0.01) / 0.005, -2.0, 2.0);
    return std::clamp(1.0 + 0.15 * z, 0.8, 1.5);
}

std::filesystem::path find_repo_file(const std::string& relative) {
    namespace fs = std::filesystem;
    fs::path dir = fs::current_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (fs::exists(dir / relative)) return dir / relative;
        dir = dir.parent_path();
    }
    return {};
}

std::string read_source(const std::string& relative) {
    auto path = find_repo_file(relative);
    if (path.empty()) return {};
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string between(const std::string& s, const std::string& from, const std::string& to) {
    const auto a = s.find(from);
    if (a == npos) return {};
    const auto b = s.find(to, a);
    if (b == npos) return {};
    return s.substr(a, b - a);
}

const char* const kFuturesRunners[] = {"apps/strategies/live_portfolio_conservative.cpp",
                                       "apps/strategies/live_portfolio.cpp"};

PortfolioConfig cost_vector_config() {
    PortfolioConfig pc{kCapital, 1.0, 0.0, /*use_optimization=*/true};
    pc.opt_config.capital = kCapital;
    pc.opt_config.cost_penalty_scalar = 50.0;
    pc.risk_config.capital = kCapital;
    pc.risk_modules = {test_none_module()};
    return pc;
}

class OptimizerCostVector : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        // CM1: the cost model prices MES and ZF with their metadata rows.
        trade_ngin::testing::register_session_metadata_futures();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        mock_db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        ASSERT_TRUE(mock_db_->connect().is_ok());
        static int n = 0;
        pm_ = std::make_unique<PortfolioManager>(cost_vector_config(),
                                                 "PM_COSTVEC_" + std::to_string(++n));
        StrategyConfig sc;
        sc.capital_allocation = kCapital;
        sc.max_leverage = 10.0;
        sc.asset_classes = {AssetClass::FUTURES};
        sc.frequencies = {DataFrequency::DAILY};
        trend_ = std::make_shared<TrendFollowingStrategy>(
            "TF_COSTVEC_" + std::to_string(n), sc, TrendFollowingConfig{}, mock_db_, nullptr);
        // The two instruments' data as the trend sleeve holds it: calculate_trading_costs reads
        // contract_size and price_history.back() from here, as optimize_positions' weights do.
        trend_->instrument_data_[kMes].contract_size = kMesSize;
        trend_->instrument_data_[kMes].price_history = {kMesPrice - 10.0, kMesPrice};
        trend_->instrument_data_[kZf].contract_size = kZfSize;
        trend_->instrument_data_[kZf].price_history = {kZfPrice + 0.25, kZfPrice};
        pm_->strategies_["TF"] = PortfolioManager::StrategyInfo{trend_, 1.0, true, {}, {}};
    }

    void TearDown() override {
        pm_.reset();
        trend_.reset();
        mock_db_.reset();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        TestBase::TearDown();
    }

    double cost_per_contract(const std::string& symbol, double price) const {
        return pm_->cost_manager_.calculate_costs(symbol, 1.0, price).total_transaction_costs;
    }

    std::shared_ptr<MockPostgresDatabase> mock_db_;
    std::unique_ptr<PortfolioManager> pm_;
    std::shared_ptr<TrendFollowingStrategy> trend_;
};

}  // namespace

// -----------------------------------------------------------------------------------------------
// The generic optimiser step's cost vector reads no trend sleeve
// -----------------------------------------------------------------------------------------------

// F4's unit (the cost of one contract over its notional) lived in the branch of
// calculate_trading_costs that read a trend sleeve's contract size and price. A book with a trend
// sleeve names an overlay sleeve and is rebalanced by the one pass, which prices each contract
// through the cost model itself (test_one_pass_book.cpp), so no book reached that branch and it is
// gone: the generic step's vector is zero for every symbol, the trend sleeve's own included.
TEST_F(OptimizerCostVector, TheGenericStepsVectorReadsNoTrendSleeve) {
    ASSERT_GT(cost_per_contract(kMes, kMesPrice), 0.0) << "the cost model itself prices MES";
    const auto costs = pm_->calculate_trading_costs({kMes, kZf, "NOT_HELD.v.0"}, kCapital);
    ASSERT_EQ(costs.size(), 3u);
    EXPECT_EQ(costs, (std::vector<double>{0.0, 0.0, 0.0}));
}

// -----------------------------------------------------------------------------------------------
// H-2: the runner's feed reaches the PortfolioManager's cost model, so the vector is priced off
// the fill day's own volume and the 20-return walk instead of the fallbacks
// -----------------------------------------------------------------------------------------------

TEST_F(OptimizerCostVector, TheK2FeedPricesThePortfolioManagersCostModelOffTheFedBars) {
    // The member itself. The runners reach it through get_transaction_cost_manager(), which the
    // parent lacks; reading the member keeps this case compilable on the parent. Its feed half
    // passes there too (the helper always worked; nobody called it on this manager: the wiring
    // case is H-2's RED); its last assertions are F4's unit.
    auto& tcm = pm_->cost_manager_;

    // Unfed, the manager prices from the fallbacks: ADV 100,000 and vol_mult 1.0.
    const double unfed = cost_per_contract(kMes, kMesPrice);
    EXPECT_DOUBLE_EQ(unfed,
                     tcm.calculate_costs(kMes, 1.0, kMesPrice, 100000.0, 1.0).total_transaction_costs);

    // Twenty-five sessions at 150,000 lots, then a thin T-1 session of 12,000 lots.
    auto bars = zigzag(kMes, 26, kMesPrice, 0.02, 150'000.0);
    bars.back().volume = 12'000.0;
    const double t1_close = static_cast<double>(bars.back().close);
    const auto fed = feed_futures_cost_model(tcm, bars, run_date_after(bars));
    ASSERT_EQ(fed.symbols.size(), 1u);
    EXPECT_EQ(fed.symbols[0].returns, 25u);

    const double vm = vol_mult_by_hand(bars);
    EXPECT_NE(vm, 1.0);
    EXPECT_DOUBLE_EQ(tcm.get_adv(kMes), 12'000.0) << "the fill day's own volume";
    EXPECT_DOUBLE_EQ(tcm.get_volatility_multiplier(kMes), vm) << "the last 20 returns";

    // The manager's cost of one contract is now priced off the fed state: what the one pass reads
    // for its cost vector and prices its fills with.
    const double fed_cost =
        tcm.calculate_costs(kMes, 1.0, t1_close, 12'000.0, vm).total_transaction_costs;
    EXPECT_DOUBLE_EQ(cost_per_contract(kMes, t1_close), fed_cost);
    EXPECT_TRUE(tcm.has_volume_history(kMes));
    EXPECT_FALSE(tcm.has_volume_history(kZf)) << "ZF was never fed";
    EXPECT_GT(fed_cost, tcm.calculate_costs(kMes, 1.0, t1_close, 100000.0, 1.0).total_transaction_costs)
        << "a thin session costs more than the 100,000-lot fallback";
}

// -----------------------------------------------------------------------------------------------
// The scalar is read from config as the double it is
// -----------------------------------------------------------------------------------------------

TEST(OptimizerCostPenaltyScalarConfig, AFractionalScalarIsReadAsGiven) {
    DynamicOptConfig c;
    c.from_json(nlohmann::json{{"cost_penalty_scalar", 3.5}});
    EXPECT_DOUBLE_EQ(c.cost_penalty_scalar, 3.5) << "read with get<int>(): floored to 3";

    DynamicOptConfig sweep;
    sweep.cost_penalty_scalar = 50.0 / 14.0;
    DynamicOptConfig back;
    back.from_json(sweep.to_json());
    EXPECT_DOUBLE_EQ(back.cost_penalty_scalar, 50.0 / 14.0) << "the round trip keeps it";
}

TEST(OptimizerCostPenaltyScalarConfig, AnIntegerScalarIsUnchangedAndTheDefaultIsFifty) {
    DynamicOptConfig c;
    EXPECT_DOUBLE_EQ(c.cost_penalty_scalar, 50.0);
    c.from_json(nlohmann::json{{"cost_penalty_scalar", 50}});
    EXPECT_DOUBLE_EQ(c.cost_penalty_scalar, 50.0);
    c.from_json(nlohmann::json{{"cost_penalty_scalar", 0}});
    EXPECT_DOUBLE_EQ(c.cost_penalty_scalar, 0.0);
}

// LOOP_SPEC sections 5.2 and 12 (D38): the search's cost multiplier is 100.
TEST(OptimizerCostPenaltyScalarConfig, TheTrackedTemplateCarriesOneHundred) {
    const std::string src = read_source("config_template/defaults.json");
    if (src.empty()) GTEST_SKIP() << "config_template/defaults.json not found";
    const auto j = nlohmann::json::parse(src);
    ASSERT_TRUE(j.contains("optimization"));
    EXPECT_DOUBLE_EQ(j.at("optimization").at("cost_penalty_scalar").get<double>(), 100.0);
}

// -----------------------------------------------------------------------------------------------
// The wiring: both twins feed the PortfolioManager's cost model with the K2 feed, before
// process_market_data; the equity runner does not (its optimizer is off)
// -----------------------------------------------------------------------------------------------

TEST(OptimizerCostVectorRunnerSource, BothTwinsFeedThePortfolioManagersCostModelBeforeTheOptimizer) {
    std::vector<std::string> blocks;
    for (const char* runner : kFuturesRunners) {
        SCOPED_TRACE(runner);
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found";
        const std::string block = between(src, "// UPDATE TRANSACTION COST MANAGER WITH MARKET DATA",
                                          "// NORMAL TRADING DAY PROCESSING");
        ASSERT_FALSE(block.empty());
        const auto feed = block.find(
            "feed_futures_cost_model(optimizer_cost_model, strategy_feed_bars, now);");
        EXPECT_NE(feed, npos) << "the PortfolioManager's cost model is never fed in live";
        EXPECT_NE(block.find("auto& optimizer_cost_model = portfolio->get_transaction_cost_manager();"),
                  npos);
        // The execution manager's feed (C8a) is still there, on the same feed.
        EXPECT_NE(block.find("feed_futures_cost_model(cost_model, strategy_feed_bars, now);"), npos);
        const auto block_at = src.find("// UPDATE TRANSACTION COST MANAGER WITH MARKET DATA");
        const auto process = src.find("portfolio->process_market_data(strategy_feed_bars);");
        ASSERT_NE(process, npos);
        if (feed != npos) {
            EXPECT_LT(block_at + feed, process) << "fed after the optimizer has run";
        }
        blocks.push_back(block);
    }
    ASSERT_EQ(blocks.size(), 2u);
    EXPECT_EQ(blocks[0], blocks[1]) << "the twins differ";
}

TEST(OptimizerCostVectorRunnerSource, TheEquityRunnerDoesNotFeedThePortfolioManagersCostModel) {
    const std::string src = read_source("apps/strategies/live_equity_mean_reversion.cpp");
    if (src.empty()) GTEST_SKIP() << "runner source not found";
    EXPECT_EQ(src.find("portfolio->get_transaction_cost_manager()"), npos)
        << "the equity book's optimizer is off: its cost vector is never read";
}

TEST_F(OptimizerCostVector, TheRunnersAccessorIsThePortfolioManagersOwnCostModel) {
    // The runners feed portfolio->get_transaction_cost_manager(); it must be the manager that
    // calculate_trading_costs prices from, not a copy.
    EXPECT_EQ(&pm_->get_transaction_cost_manager(), &pm_->cost_manager_);
}
