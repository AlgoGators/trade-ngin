// tests/live/test_futures_cost_feed.cpp
//
// T-7b-1 C8a: K2 redesigned, the live futures runners' cost feed with two inputs (HD 2026-09-17
// "thin sessions are traded", 2026-09-19 "K2 with two volume inputs"):
//
//   * the participation term and the k_bps impact tier are priced off the FILL DAY'S OWN volume
//     (the T-1 bar whose close prices the fill), so a thin session is charged its own cost;
//   * the volatility term (the spread's vol_mult) is the walk of the symbol's last 20 log returns
//     ending at that T-1 bar, on the loader's one-bar-per-instant feed, never de-duplicated again;
//   * the first bar contributes no return (no fabricated log(close/close) = 0), and with fewer than
//     2 returns the model's neutral 1.0 stands;
//   * commissions never move, and the impact term is the parent's to the bit (the parent fed ONE
//     bar through ExecutionManager's 3-arg form: the same own-day volume, one zero return, so
//     vol_mult was always 1.0).
//
// The pure cases run feed_futures_cost_model (futures_cost_feed.hpp) against the real
// TransactionCostManager; the wiring cases read the two runner sources (they are main()s and
// cannot be linked here, tests/live/test_day_t_write_ordering.cpp).

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/live/execution_manager.hpp"
#include "trade_ngin/live/futures_cost_feed.hpp"
#include "trade_ngin/live/live_daily_cycle.hpp"
#include "trade_ngin/transaction_cost/transaction_cost_manager.hpp"

using namespace trade_ngin;
using trade_ngin::transaction_cost::TransactionCostManager;
using trade_ngin::transaction_cost::TransactionCostResult;

namespace {

constexpr auto npos = std::string::npos;

// NG: baseline 2 ticks, min 1, max 10, spread multiplier 0.5, tick 0.001, point value 10,000,
// max impact 80 bps (asset_cost_config.cpp). A two-tick baseline lets vol_mult move the spread
// both ways, so a wrong vol_mult cannot hide behind the one-tick floor.
const std::string kNg = "NG.v.0";

Bar bar_at(const std::string& symbol, int day_index, double close, double volume) {
    const Timestamp ts = std::chrono::system_clock::from_time_t(
        1700000000LL + static_cast<long long>(day_index) * 86400LL);
    return Bar(ts, close, close, close, close, volume, symbol);
}

/// n bars, the close stepping +step / -step alternately, every volume `volume`.
std::vector<Bar> zigzag(const std::string& symbol, int n, double close, double step,
                        double volume, int first_day = 0) {
    std::vector<Bar> bars;
    for (int i = 0; i < n; ++i) {
        bars.push_back(bar_at(symbol, first_day + i, close, volume));
        close *= (i % 2 == 0) ? (1.0 + step) : (1.0 - step);
    }
    return bars;
}

std::vector<double> log_returns(const std::vector<Bar>& bars) {
    std::vector<double> r;
    for (size_t i = 1; i < bars.size(); ++i) {
        r.push_back(std::log(static_cast<double>(bars[i].close) /
                             static_cast<double>(bars[i - 1].close)));
    }
    return r;
}

/// spread_model.cpp calculate_volatility_multiplier, by hand: sample stdev, 1 % baseline,
/// 0.5 % sigma-of-sigma, lambda 0.15, z clipped to [-2, 2], result to [0.8, 1.5].
double vol_mult_by_hand(const std::vector<double>& r) {
    if (r.size() < 2) return 1.0;
    const double mean = std::accumulate(r.begin(), r.end(), 0.0) / static_cast<double>(r.size());
    double ss = 0.0;
    for (double x : r) ss += (x - mean) * (x - mean);
    const double sd = std::sqrt(ss / static_cast<double>(r.size() - 1));
    const double z = std::clamp((sd - 0.01) / 0.005, -2.0, 2.0);
    return std::clamp(1.0 + 0.15 * z, 0.8, 1.5);
}

std::vector<double> last_n(const std::vector<double>& v, size_t n) {
    return std::vector<double>(v.end() - static_cast<long>(std::min(n, v.size())), v.end());
}

/// k_bps tier, impact_model.cpp get_impact_k_bps.
double k_bps(double adv) {
    if (adv > 1000000.0) return 10.0;
    if (adv > 200000.0) return 20.0;
    if (adv > 50000.0) return 40.0;
    if (adv > 20000.0) return 60.0;
    return 80.0;
}

/// The parent's feed: the latest bar only, through the 3-arg ExecutionManager form, which in a
/// fresh process passes prev_close = close (execution_manager.cpp update_market_data).
void parent_one_bar_feed(TransactionCostManager& tcm, const std::vector<Bar>& bars) {
    const Bar& last = bars.back();
    const double close = static_cast<double>(last.close);
    tcm.update_market_data(last.symbol, last.volume, close, close);
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

}  // namespace

// -----------------------------------------------------------------------------------------------
// The impact term: participation and tier on the fill day's own volume
// -----------------------------------------------------------------------------------------------

TEST(K2FuturesCostFeed, ParticipationAndTheTierArePricedOnTheFillDaysOwnVolume) {
    // Twenty sessions at 1.2M lots (the 10 bps tier), then a thin T-1 session of 30,000 lots
    // (the 60 bps tier). The fill is priced off the thin session alone.
    auto bars = zigzag(kNg, 21, 3.0, 0.01, 1'200'000.0);
    bars.back().volume = 30'000.0;

    TransactionCostManager tcm;
    const auto feed = feed_futures_cost_model(tcm, bars);
    ASSERT_EQ(feed.symbols.size(), 1u);
    EXPECT_EQ(feed.symbols[0].own_day_volume, 30'000.0);
    EXPECT_EQ(feed.symbols[0].own_day_time, bars.back().timestamp);
    EXPECT_EQ(tcm.get_adv(kNg), 30'000.0) << "the impact model's one observation is T-1's volume";

    const double price = static_cast<double>(bars.back().close);
    const double q = 3.0;
    const auto cost = tcm.calculate_costs(kNg, q, price);
    const double expected_impact = (60.0 * std::sqrt(q / 30'000.0)) / 10000.0 * price;
    EXPECT_DOUBLE_EQ(cost.market_impact_price_impact, expected_impact);
    EXPECT_EQ(k_bps(30'000.0), 60.0);
    EXPECT_EQ(k_bps(1'200'000.0), 10.0) << "the 20-bar mean would have keyed the 10 bps tier";
}

TEST(K2FuturesCostFeed, AThinOwnDayBarIsChargedMoreThanANormalOneAndMoreThanTheTwentyBarMean) {
    auto normal = zigzag(kNg, 21, 3.0, 0.01, 1'200'000.0);
    auto thin = normal;
    thin.back().volume = 30'000.0;
    const double price = static_cast<double>(thin.back().close);

    TransactionCostManager on_thin;
    TransactionCostManager on_normal;
    feed_futures_cost_model(on_thin, thin);
    feed_futures_cost_model(on_normal, normal);
    const auto c_thin = on_thin.calculate_costs(kNg, 3.0, price);
    const auto c_normal = on_normal.calculate_costs(kNg, 3.0, price);
    EXPECT_GT(c_thin.total_transaction_costs, c_normal.total_transaction_costs);
    EXPECT_GT(c_thin.market_impact_price_impact, c_normal.market_impact_price_impact);

    // The equity-style feed averages the 20 volumes, which hides the thin session.
    TransactionCostManager on_mean;
    std::unordered_map<std::string, std::vector<Bar>> by_symbol{{kNg, thin}};
    LiveDailyCycle::feed_cost_model(on_mean, {kNg}, by_symbol);
    const auto c_mean = on_mean.calculate_costs(kNg, 3.0, price);
    EXPECT_GT(c_thin.market_impact_price_impact, c_mean.market_impact_price_impact);
}

TEST(K2FuturesCostFeed, TheImpactTermAndTheCommissionAreTheParentsToTheBit) {
    // The parent priced participation and the tier on the same own-day volume; only vol_mult
    // (the spread) may move. A 2 % zigzag is far above the 1 % baseline, so vol_mult is 1.3.
    const auto bars = zigzag(kNg, 30, 3.0, 0.02, 90'000.0);
    const double price = static_cast<double>(bars.back().close);

    TransactionCostManager parent;
    TransactionCostManager fixed;
    parent_one_bar_feed(parent, bars);
    feed_futures_cost_model(fixed, bars);

    for (double q : {1.0, -2.0, 7.0}) {
        SCOPED_TRACE(q);
        const auto a = parent.calculate_costs(kNg, q, price);
        const auto b = fixed.calculate_costs(kNg, q, price);
        EXPECT_EQ(a.commissions_fees, b.commissions_fees);
        EXPECT_EQ(b.commissions_fees, 1.5 * std::abs(q));
        EXPECT_EQ(a.market_impact_price_impact, b.market_impact_price_impact);
        EXPECT_NE(a.spread_price_impact, b.spread_price_impact) << "vol_mult never measured";
        EXPECT_DOUBLE_EQ(b.slippage_market_impact,
                         b.implicit_price_impact * std::abs(q) * 10000.0);
        EXPECT_DOUBLE_EQ(b.total_transaction_costs,
                         b.commissions_fees + b.slippage_market_impact);
    }
}

// -----------------------------------------------------------------------------------------------
// The volatility term: the 20-return walk ending at T-1
// -----------------------------------------------------------------------------------------------

TEST(K2FuturesCostFeed, TheVolatilityTermIsTheLastTwentyReturnsEndingAtTheT1Bar) {
    // 40 bars: a calm first half (0.2 %) and a wild second half (2 %), so any window reaching
    // back past the last 20 returns lands on a different multiplier.
    auto bars = zigzag(kNg, 20, 3.0, 0.002, 90'000.0);
    auto wild = zigzag(kNg, 20, static_cast<double>(bars.back().close) * 1.02, 0.02, 90'000.0, 20);
    bars.insert(bars.end(), wild.begin(), wild.end());

    TransactionCostManager tcm;
    const auto feed = feed_futures_cost_model(tcm, bars);
    ASSERT_EQ(feed.symbols.size(), 1u);
    EXPECT_EQ(feed.symbols[0].returns, 39u);

    const double expected = vol_mult_by_hand(last_n(log_returns(bars), 20));
    EXPECT_DOUBLE_EQ(tcm.get_volatility_multiplier(kNg), expected);
    EXPECT_NE(expected, 1.0);
    EXPECT_NE(expected, vol_mult_by_hand(log_returns(bars))) << "the whole walk is not the window";

    // The spread: 0.5 x clamp(2 x vol_mult, 1, 10) x 0.001.
    const auto cost = tcm.calculate_costs(kNg, 1.0, static_cast<double>(bars.back().close));
    EXPECT_DOUBLE_EQ(cost.spread_price_impact, 0.5 * std::clamp(2.0 * expected, 1.0, 10.0) * 0.001);
}

TEST(K2FuturesCostFeed, OnlyTheLastTwentyReturnsCountAndTheT1CloseDoes) {
    auto a = zigzag(kNg, 45, 3.0, 0.012, 90'000.0);
    auto b = a;
    // A bar 30 sessions before T-1 moves: outside the 21 closes the window reads.
    b[10] = bar_at(kNg, 10, static_cast<double>(b[10].close) * 1.10, 90'000.0);
    auto c = a;
    // The T-1 close moves: inside the window.
    c.back() = bar_at(kNg, 44, static_cast<double>(c.back().close) * 1.03, 90'000.0);

    TransactionCostManager ta, tb, tc;
    feed_futures_cost_model(ta, a);
    feed_futures_cost_model(tb, b);
    feed_futures_cost_model(tc, c);
    EXPECT_EQ(ta.get_volatility_multiplier(kNg), tb.get_volatility_multiplier(kNg));
    EXPECT_NE(ta.get_volatility_multiplier(kNg), tc.get_volatility_multiplier(kNg));
    EXPECT_DOUBLE_EQ(tc.get_volatility_multiplier(kNg),
                     vol_mult_by_hand(last_n(log_returns(c), 20)));
}

TEST(K2FuturesCostFeed, TheFirstBarContributesNoFabricatedReturn) {
    // Three bars, two real returns. A fabricated log(close/close) = 0 in front would give three
    // returns with a lower stdev and a different multiplier.
    const std::vector<Bar> bars = {bar_at(kNg, 0, 3.00, 50'000.0), bar_at(kNg, 1, 3.03, 50'000.0),
                                   bar_at(kNg, 2, 2.97, 50'000.0)};
    TransactionCostManager tcm;
    const auto feed = feed_futures_cost_model(tcm, bars);
    ASSERT_EQ(feed.symbols.size(), 1u);
    EXPECT_EQ(feed.symbols[0].returns, 2u);
    EXPECT_EQ(feed.returns_fed, 2u);

    const auto real = log_returns(bars);
    std::vector<double> with_zero{0.0};
    with_zero.insert(with_zero.end(), real.begin(), real.end());
    ASSERT_NE(vol_mult_by_hand(real), vol_mult_by_hand(with_zero));
    EXPECT_DOUBLE_EQ(tcm.get_volatility_multiplier(kNg), vol_mult_by_hand(real));
}

TEST(K2FuturesCostFeed, FewerBarsFallBackToTheModelsNeutralValues) {
    TransactionCostManager tcm;
    std::vector<Bar> feed = {bar_at("ES.v.0", 5, 5000.0, 1'500'000.0)};  // one bar: no return
    const auto two = std::vector<Bar>{bar_at(kNg, 0, 3.0, 40'000.0), bar_at(kNg, 1, 3.3, 45'000.0)};
    feed.insert(feed.end(), two.begin(), two.end());
    const auto out = feed_futures_cost_model(tcm, feed);

    ASSERT_EQ(out.symbols.size(), 2u);
    EXPECT_EQ(out.symbols[0].symbol, "ES.v.0");
    EXPECT_EQ(out.symbols[0].returns, 0u);
    EXPECT_EQ(out.symbols[1].returns, 1u);
    EXPECT_EQ(out.returns_fed, 1u);
    EXPECT_EQ(tcm.get_volatility_multiplier("ES.v.0"), 1.0);
    EXPECT_EQ(tcm.get_volatility_multiplier(kNg), 1.0) << "one return is not a volatility";
    EXPECT_EQ(tcm.get_adv("ES.v.0"), 1'500'000.0);
    EXPECT_EQ(tcm.get_adv(kNg), 45'000.0);
    EXPECT_EQ(out.thin, (std::vector<std::string>{"ES.v.0", kNg}));

    // A symbol absent from the feed keeps the manager's fallbacks: ADV 100,000, vol_mult 1.0.
    EXPECT_EQ(tcm.get_adv("ZN.v.0"), 0.0);
    const auto c = tcm.calculate_costs("ZN.v.0", 1.0, 110.0);
    const auto f = tcm.calculate_costs("ZN.v.0", 1.0, 110.0, 100000.0, 1.0);
    EXPECT_EQ(c.total_transaction_costs, f.total_transaction_costs);
}

// -----------------------------------------------------------------------------------------------
// The feed as given: one bar per instant, sorted here, never de-duplicated again
// -----------------------------------------------------------------------------------------------

TEST(K2FuturesCostFeed, ADuplicateFreeFeedIsWalkedOnceAndARepeatedInstantIsReportedNotDropped) {
    const auto clean = zigzag(kNg, 25, 3.0, 0.015, 60'000.0);
    TransactionCostManager t_clean;
    const auto out_clean = feed_futures_cost_model(t_clean, clean);
    EXPECT_TRUE(out_clean.repeated_instants.empty());
    EXPECT_EQ(out_clean.returns_fed, 24u);

    // A second copy of one bar (what the loader's max-volume DISTINCT ON removes, T-6c B0).
    auto dup = clean;
    Bar copy = dup[12];
    copy.volume = 1.0;
    dup.insert(dup.begin() + 13, copy);
    TransactionCostManager t_dup;
    const auto out_dup = feed_futures_cost_model(t_dup, dup);
    EXPECT_EQ(out_dup.repeated_instants, (std::vector<std::string>{kNg}));
    EXPECT_EQ(out_dup.returns_fed, 25u) << "fed as given: the helper does not de-duplicate";
    EXPECT_NE(t_dup.get_volatility_multiplier(kNg), t_clean.get_volatility_multiplier(kNg))
        << "the repeated instant injects a log(1) = 0 return; the report is what exposes it";
}

TEST(K2FuturesCostFeed, AnUnsortedInterleavedFeedEqualsEachSymbolFedInDateOrder) {
    const auto ng = zigzag(kNg, 23, 3.0, 0.013, 70'000.0);
    const auto es = zigzag("ES.v.0", 23, 5000.0, 0.011, 1'400'000.0);
    std::vector<Bar> mixed;
    for (size_t i = 0; i < ng.size(); ++i) {
        mixed.push_back(es[ng.size() - 1 - i]);
        mixed.push_back(ng[(i * 7) % ng.size()]);
    }

    TransactionCostManager t_mixed, t_ng, t_es;
    const auto out = feed_futures_cost_model(t_mixed, mixed);
    feed_futures_cost_model(t_ng, ng);
    feed_futures_cost_model(t_es, es);
    ASSERT_EQ(out.symbols.size(), 2u);
    EXPECT_EQ(out.symbols[0].symbol, "ES.v.0");
    EXPECT_EQ(out.symbols[1].symbol, kNg);
    EXPECT_EQ(t_mixed.get_volatility_multiplier(kNg), t_ng.get_volatility_multiplier(kNg));
    EXPECT_EQ(t_mixed.get_volatility_multiplier("ES.v.0"), t_es.get_volatility_multiplier("ES.v.0"));
    EXPECT_EQ(t_mixed.get_adv(kNg), ng.back().volume);
    EXPECT_EQ(t_mixed.get_adv("ES.v.0"), es.back().volume);
}

// -----------------------------------------------------------------------------------------------
// The shared manager: the two halves are exactly update_market_data
// -----------------------------------------------------------------------------------------------

TEST(K2FuturesCostFeed, UpdateMarketDataIsRecordVolumeThenRecordLogReturn) {
    // The backtest's two managers and the equity feed keep calling update_market_data; the split
    // must leave them where they were.
    const auto bars = zigzag(kNg, 30, 3.0, 0.017, 55'000.0);
    TransactionCostManager whole, halves;
    double prev = 0.0;
    for (const auto& b : bars) {
        const double c = static_cast<double>(b.close);
        whole.update_market_data(kNg, b.volume + c, c, prev);
        halves.record_volume(kNg, b.volume + c);
        halves.record_log_return(kNg, c, prev);
        prev = c;
    }
    halves.record_log_return(kNg, 3.0, 0.0);  // prev_close 0: nothing recorded
    halves.record_log_return(kNg, 0.0, 3.0);  // close 0: nothing recorded
    EXPECT_EQ(whole.get_adv(kNg), halves.get_adv(kNg));
    EXPECT_EQ(whole.get_volatility_multiplier(kNg), halves.get_volatility_multiplier(kNg));
    const auto a = whole.calculate_costs(kNg, 4.0, 3.1);
    const auto b = halves.calculate_costs(kNg, 4.0, 3.1);
    EXPECT_EQ(a.total_transaction_costs, b.total_transaction_costs);
}

// -----------------------------------------------------------------------------------------------
// A fill through the live ExecutionManager, end to end
// -----------------------------------------------------------------------------------------------

TEST(K2FuturesCostFeed, ALiveFillIsPricedOffOwnDayVolumeAndTheWalkedVolatility) {
    auto bars = zigzag(kNg, 26, 3.0, 0.018, 80'000.0);
    bars.back().volume = 12'000.0;  // a thin T-1 session: the 80 bps tier
    const double price = static_cast<double>(bars.back().close);

    ExecutionManager em;
    feed_futures_cost_model(em.get_transaction_cost_manager(), bars);
    const auto exec = em.generate_execution(kNg, -2.0, price, bars.back().timestamp);

    const double vm = vol_mult_by_hand(last_n(log_returns(bars), 20));
    const double spread = 0.5 * std::clamp(2.0 * vm, 1.0, 10.0) * 0.001;
    const double impact = (80.0 * std::sqrt(2.0 / 12'000.0)) / 10000.0 * price;
    const double slippage = (spread + impact) * 2.0 * 10000.0;
    EXPECT_NE(vm, 1.0);
    EXPECT_NEAR(exec.commissions_fees.as_double(), 3.0, 1e-9);
    EXPECT_NEAR(exec.implicit_price_impact.as_double(), spread + impact, 2e-8);
    EXPECT_NEAR(exec.slippage_market_impact.as_double(), slippage, 1e-6);
    EXPECT_NEAR(exec.total_transaction_costs.as_double(), 3.0 + slippage, 1e-6);
}

// -----------------------------------------------------------------------------------------------
// The wiring: both twins feed the execution manager's cost model through the two-input feed
// -----------------------------------------------------------------------------------------------

TEST(K2FuturesCostFeedRunnerSource, BothTwinsFeedTheExecutionManagersCostModelThroughTheTwoInputFeed) {
    std::vector<std::string> blocks;
    for (const char* runner : kFuturesRunners) {
        SCOPED_TRACE(runner);
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found";
        const std::string block = between(src, "// UPDATE TRANSACTION COST MANAGER WITH MARKET DATA",
                                          "// NORMAL TRADING DAY PROCESSING");
        ASSERT_FALSE(block.empty());
        EXPECT_NE(block.find("auto& cost_model = execution_manager->get_transaction_cost_manager();"),
                  npos);
        EXPECT_NE(block.find("feed_futures_cost_model(cost_model, strategy_feed_bars);"), npos)
            << "the cost model is fed one bar per symbol";
        EXPECT_EQ(block.find("execution_manager->update_market_data("), npos)
            << "the one-bar 3-arg feed is still there";
        // H-2 (T-7b-1 C8d) feeds the PortfolioManager's cost manager through the same two-input
        // feed (test_optimizer_cost_vector.cpp), never through update_cost_manager_market_data.
        EXPECT_EQ(src.find("update_cost_manager_market_data"), npos);
        blocks.push_back(block);
    }
    ASSERT_EQ(blocks.size(), 2u);
    EXPECT_EQ(blocks[0], blocks[1]);
}
