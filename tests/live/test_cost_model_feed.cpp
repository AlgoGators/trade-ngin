// tests/live/test_cost_model_feed.cpp
//
// E2-F62 -- the live equity runner must give its transaction-cost model market data
// before it prices a fill.
//
// The failure shape these pin: TransactionCostManager::calculate_costs reads ADV from
// impact_model_.get_adv() and the spread widening from
// spread_model_.get_volatility_multiplier() (transaction_cost_manager.cpp:26-27). Both
// are populated ONLY by update_market_data. The live equity runner never called it --
// the only runner in the tree that did not -- so every live equity fill was priced
// against the fallbacks at :33 and :37: adv = 100000 shares, vol_mult = 1.0. For a name
// that trades 5.4 M shares a day that is a participation rate 54x too high and the
// 40 bps impact bucket where the truth is 10 (impact_model.cpp get_impact_k_bps).
// Registering the tier config does NOT fix it: that writes spread ticks and caps, not
// the ADV the impact model divides by.
//
// Three properties, each of which the pre-fix tree violates:
//   1. after the feed, ADV is the mean of the last 20 volumes and vol_mult is measured
//   2. the first bar of a symbol contributes NO fabricated log(close/close) = 0 return
//   3. a 100-share TMUS fill at a real 5.4 M ADV is priced in the 10 bps bucket, not 40
//
// Property 3 is written as a before/after pair in one test so it stays a regression
// guard rather than a one-off: delete the feed from the runner and the "after" half
// collapses onto the "before" half.

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <numeric>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/live/live_daily_cycle.hpp"
#include "trade_ngin/transaction_cost/asset_cost_config.hpp"
#include "trade_ngin/transaction_cost/transaction_cost_manager.hpp"

using namespace trade_ngin;
using trade_ngin::transaction_cost::AssetCostConfigRegistry;
using trade_ngin::transaction_cost::TransactionCostManager;

namespace {

// Deque sizes the models keep, so the tests state the window they assert on rather
// than inheriting it silently: ImpactModel::Config::adv_lookback_days and
// SpreadModel::VolatilityConfig::lookback_days are both 20.
constexpr size_t kWindow = 20;

Bar bar_at(const std::string& symbol, int day_index, double close, double volume) {
    Bar b;
    b.symbol = symbol;
    b.timestamp = std::chrono::system_clock::from_time_t(
        1700000000LL + static_cast<long long>(day_index) * 86400LL);
    b.open = Price(close);
    b.high = Price(close);
    b.low = Price(close);
    b.close = Price(close);
    b.volume = volume;
    return b;
}

// Sample stdev (N-1) -- SpreadModel::compute_stdev.
double sample_stdev(const std::vector<double>& v) {
    if (v.size() < 2) return 0.0;
    const double mean = std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
    double ss = 0.0;
    for (double x : v) ss += (x - mean) * (x - mean);
    return std::sqrt(ss / static_cast<double>(v.size() - 1));
}

}  // namespace

// -----------------------------------------------------------------------------
// 1. Twenty-one bars must yield a 20-bar ADV and a MEASURED volatility multiplier.
// -----------------------------------------------------------------------------
TEST(CostModelFeed, TwentyOneBarsGiveTheTwentyBarMeanVolumeAndAMeasuredVolMultiplier) {
    TransactionCostManager tcm;

    // Volumes that rise every day, so the 21-bar mean and the last-20 mean differ by a
    // wide margin: averaging the wrong window cannot pass by luck.
    // Closes step 1.5 % a day, which is a daily sigma far above the model's 1 % baseline,
    // so a correctly measured vol_mult is pushed off 1.0 rather than landing there.
    std::vector<Bar> bars;
    double close = 100.0;
    for (int i = 0; i < 21; ++i) {
        bars.push_back(bar_at("TEST", i, close, 1'000'000.0 + i * 100'000.0));
        close *= (i % 2 == 0) ? 1.015 : 0.985;
    }

    std::unordered_map<std::string, std::vector<Bar>> by_symbol{{"TEST", bars}};
    const auto feed = LiveDailyCycle::feed_cost_model(tcm, {"TEST"}, by_symbol);

    EXPECT_EQ(feed.symbols_fed, 1u);
    EXPECT_EQ(feed.bars_fed, 21u);
    // 21 bars, 20 returns: the first bar contributes volume but no return.
    EXPECT_EQ(feed.returns_fed, 20u);
    EXPECT_TRUE(feed.no_bars.empty());
    EXPECT_TRUE(feed.thin.empty());

    // ADV is the mean of the LAST 20 volumes, not of all 21.
    double last20 = 0.0;
    for (size_t i = bars.size() - kWindow; i < bars.size(); ++i) last20 += bars[i].volume;
    last20 /= static_cast<double>(kWindow);
    EXPECT_NEAR(tcm.get_adv("TEST"), last20, 1e-9);

    double all21 = 0.0;
    for (const auto& b : bars) all21 += b.volume;
    all21 /= static_cast<double>(bars.size());
    EXPECT_GT(std::abs(last20 - all21), 1000.0) << "the two windows must be distinguishable";
    EXPECT_GT(std::abs(tcm.get_adv("TEST") - all21), 1000.0);

    // The volatility multiplier is measured, not the 1.0 default.
    const double vol_mult = tcm.get_volatility_multiplier("TEST");
    EXPECT_NE(vol_mult, 1.0);
    EXPECT_GT(vol_mult, 1.0) << "1.5 % daily steps are well above the 1 % baseline sigma";

    // And an unfed symbol still reports the defaults, so the assertions above are
    // measuring the feed rather than a property of a fresh manager.
    EXPECT_DOUBLE_EQ(tcm.get_adv("NEVER_FED"), 0.0);
    EXPECT_DOUBLE_EQ(tcm.get_volatility_multiplier("NEVER_FED"), 1.0);
}

// -----------------------------------------------------------------------------
// 2. The first bar must not inject a fabricated zero return.
// -----------------------------------------------------------------------------
// Three bars give exactly two real returns. get_annual_volatility is the sample stdev
// of the return deque times sqrt(252), so it pins the deque's CONTENTS exactly: with a
// fabricated leading zero the deque would hold three entries and a different stdev.
TEST(CostModelFeed, FirstBarContributesVolumeButNoFabricatedZeroReturn) {
    TransactionCostManager tcm;

    std::vector<Bar> bars{
        bar_at("TEST", 0, 100.0, 500'000.0),
        bar_at("TEST", 1, 104.0, 500'000.0),
        bar_at("TEST", 2, 101.0, 500'000.0),
    };
    std::unordered_map<std::string, std::vector<Bar>> by_symbol{{"TEST", bars}};

    const auto feed = LiveDailyCycle::feed_cost_model(tcm, {"TEST"}, by_symbol, /*min_bars=*/21);
    EXPECT_EQ(feed.bars_fed, 3u);
    EXPECT_EQ(feed.returns_fed, 2u) << "3 bars is 2 returns, never 3";
    ASSERT_EQ(feed.thin.size(), 1u) << "3 bars is below the 21-bar bar; say so";
    EXPECT_EQ(feed.thin.front(), "TEST");

    // All three volumes are recorded even though the first contributes no return.
    EXPECT_NEAR(tcm.get_adv("TEST"), 500'000.0, 1e-9);

    const std::vector<double> real_returns{std::log(104.0 / 100.0), std::log(101.0 / 104.0)};
    const double expected = sample_stdev(real_returns) * std::sqrt(252.0);
    EXPECT_NEAR(tcm.get_annual_volatility("TEST"), expected, 1e-12);

    // What the fabricated-zero path would have produced, so the assertion above is
    // shown to discriminate rather than merely to pass. This is the number the futures
    // live runner gets from ExecutionManager's 3-arg form
    // (execution_manager.cpp:186 defaults prev_close to the bar's own close).
    const std::vector<double> with_fake_zero{0.0, real_returns[0], real_returns[1]};
    const double fabricated = sample_stdev(with_fake_zero) * std::sqrt(252.0);
    EXPECT_GT(std::abs(expected - fabricated), 0.05)
        << "a fabricated leading zero must be visibly different, or this test proves nothing";
    EXPECT_GT(std::abs(tcm.get_annual_volatility("TEST") - fabricated), 0.05);
}

// -----------------------------------------------------------------------------
// 3. A 100-share TMUS fill at a real 5.4 M ADV must land in the 10 bps bucket.
// -----------------------------------------------------------------------------
// impact_model.cpp get_impact_k_bps: adv > 1e6 -> 10 bps; 50k < adv <= 200k -> 40 bps.
// The unfed manager's adv = 100000 fallback (transaction_cost_manager.cpp:33) lands in
// the 40 bps bucket AND overstates participation by 54x, so it compounds twice.
TEST(CostModelFeed, HundredShareTmusFillIsPricedInTheTenBpsBucketNotForty) {
    const double kAdv = 5'400'000.0;
    const double kPrice = 201.40;
    const double kQty = 100.0;

    // The tier config is the same on both sides -- this test isolates the ADV the
    // impact model divides by, not the spread ticks the tier supplies.
    auto tier = AssetCostConfigRegistry::get_tiered_equity_config(kPrice, kAdv);
    tier.symbol = "TMUS";

    // Closes are held flat so the ONLY thing that varies between the two managers is
    // what each one knows. Note that flat closes do not make the two spreads equal:
    // twenty exact-zero returns give sigma = 0, so z = (0 - 0.01)/0.005 = -2 and the
    // fed manager's vol_mult clamps to the 0.8 floor, while the unfed manager has
    // fewer than two returns and reports the 1.0 default. So the spread is backed out
    // per manager from its OWN multiplier below rather than assumed common -- which is
    // the decomposition calculate_costs actually performs.
    std::vector<Bar> bars;
    for (int i = 0; i < 21; ++i) bars.push_back(bar_at("TMUS", i, kPrice, kAdv));
    std::unordered_map<std::string, std::vector<Bar>> by_symbol{{"TMUS", bars}};

    TransactionCostManager unfed;
    unfed.register_asset_config(tier);
    const auto before = unfed.calculate_costs("TMUS", kQty, kPrice);

    TransactionCostManager fed;
    fed.register_asset_config(tier);
    LiveDailyCycle::feed_cost_model(fed, {"TMUS"}, by_symbol);
    const auto after = fed.calculate_costs("TMUS", kQty, kPrice);

    ASSERT_NEAR(fed.get_adv("TMUS"), kAdv, 1e-9);
    ASSERT_DOUBLE_EQ(unfed.get_adv("TMUS"), 0.0) << "unfed is the pre-fix state";
    ASSERT_DOUBLE_EQ(unfed.get_volatility_multiplier("TMUS"), 1.0)
        << "no returns at all -> the transaction_cost_manager.cpp:37 default";
    ASSERT_DOUBLE_EQ(fed.get_volatility_multiplier("TMUS"), 0.8)
        << "twenty exact-zero returns -> sigma 0 -> z -2 -> 1 + 0.15*-2 = 0.7, floored at 0.8";

    // spread_model.cpp:18-36, per manager, from that manager's own multiplier.
    auto spread_of = [&](double vol_mult) {
        double ticks = std::clamp(tier.baseline_spread_ticks * vol_mult,
                                  tier.min_spread_ticks, tier.max_spread_ticks);
        ticks = std::max(ticks, 1.0);  // equity NMS floor
        return tier.spread_cost_multiplier * ticks * tier.tick_size;
    };

    auto k_bps_of = [&](double implicit, double adv, double vol_mult) {
        const double impact_bps = (implicit - spread_of(vol_mult)) / kPrice * 10000.0;
        return impact_bps / std::sqrt(kQty / adv);
    };

    // THE ASSERTION THIS TEST EXISTS FOR.
    // Fed: the real ADV, so impact_model.cpp's ultra-liquid 10 bps coefficient.
    EXPECT_NEAR(k_bps_of(after.implicit_price_impact, kAdv, 0.8), 10.0, 1e-6);
    // Unfed: the adv = 100000 fallback, so the 40 bps coefficient -- and applied to a
    // participation rate 54x too high, so it compounds twice.
    EXPECT_NEAR(k_bps_of(before.implicit_price_impact, 100000.0, 1.0), 40.0, 1e-6);
    EXPECT_NEAR(kAdv / 100000.0, 54.0, 0.001);

    // Which is the whole point: the pre-fix manager charges strictly more for the same
    // trade, and by a factor rather than a rounding. The expected factor is not guessed
    // -- it is rebuilt from the model constants, so if any of them moves this test says
    // so instead of quietly still passing.
    const double unfed_expected =
        spread_of(1.0) + 40.0 * std::sqrt(kQty / 100000.0) / 10000.0 * kPrice;
    const double fed_expected =
        spread_of(0.8) + 10.0 * std::sqrt(kQty / kAdv) / 10000.0 * kPrice;
    EXPECT_NEAR(before.implicit_price_impact, unfed_expected, 1e-12);
    EXPECT_NEAR(after.implicit_price_impact, fed_expected, 1e-12);
    EXPECT_GT(before.slippage_market_impact, after.slippage_market_impact);
    EXPECT_NEAR(before.slippage_market_impact / after.slippage_market_impact,
                unfed_expected / fed_expected, 1e-9);
    EXPECT_NEAR(before.slippage_market_impact / after.slippage_market_impact, 4.0009652, 1e-6)
        << "the pre-fix runner charged 4x the implicit cost of the same 100-share fill";

    // Commission is untouched by any of this -- $0.005/share floored at $1.00.
    EXPECT_DOUBLE_EQ(before.commissions_fees, 1.0);
    EXPECT_DOUBLE_EQ(after.commissions_fees, 1.0);
}

// -----------------------------------------------------------------------------
// 4. Housekeeping the runner logs on: a universe symbol with no bars is reported,
//    and a stray key in the bar map is never fed.
// -----------------------------------------------------------------------------
TEST(CostModelFeed, ReportsUniverseSymbolsWithNoBarsAndIgnoresStrayKeys) {
    TransactionCostManager tcm;

    std::vector<Bar> bars;
    for (int i = 0; i < 25; ++i) bars.push_back(bar_at("HELD", i, 50.0 + i, 300'000.0));

    std::unordered_map<std::string, std::vector<Bar>> by_symbol{
        {"HELD", bars},
        {"NOT_IN_UNIVERSE", {bar_at("NOT_IN_UNIVERSE", 0, 10.0, 999'999.0)}},
        {"EMPTY", {}},
    };

    const auto feed = LiveDailyCycle::feed_cost_model(tcm, {"HELD", "EMPTY", "MISSING"}, by_symbol);

    EXPECT_EQ(feed.symbols_fed, 1u);
    EXPECT_EQ(feed.bars_fed, 25u);
    ASSERT_EQ(feed.no_bars.size(), 2u);
    EXPECT_EQ(feed.no_bars[0], "EMPTY");
    EXPECT_EQ(feed.no_bars[1], "MISSING");
    EXPECT_TRUE(feed.thin.empty()) << "25 bars clears the 21-bar bar";

    // The stray key was never handed to the model.
    EXPECT_DOUBLE_EQ(tcm.get_adv("NOT_IN_UNIVERSE"), 0.0);
    // 25 bars fed, ADV over the last 20 only.
    EXPECT_NEAR(tcm.get_adv("HELD"), 300'000.0, 1e-9);
}

// -----------------------------------------------------------------------------
// 5. Bars out of order must not change what the model sees.
// -----------------------------------------------------------------------------
// The deques are order-dependent -- returns are consecutive differences and the window
// keeps the LAST 20 -- so the helper sorts rather than trusting its input.
TEST(CostModelFeed, ShuffledBarsProduceTheSameModelAsSortedBars) {
    std::vector<Bar> sorted;
    double close = 80.0;
    for (int i = 0; i < 30; ++i) {
        sorted.push_back(bar_at("TEST", i, close, 400'000.0 + i * 7'000.0));
        close *= (i % 3 == 0) ? 1.02 : 0.99;
    }
    std::vector<Bar> shuffled(sorted.rbegin(), sorted.rend());

    TransactionCostManager a, b;
    std::unordered_map<std::string, std::vector<Bar>> in_order{{"TEST", sorted}};
    std::unordered_map<std::string, std::vector<Bar>> reversed{{"TEST", shuffled}};
    LiveDailyCycle::feed_cost_model(a, {"TEST"}, in_order);
    LiveDailyCycle::feed_cost_model(b, {"TEST"}, reversed);

    EXPECT_NEAR(a.get_adv("TEST"), b.get_adv("TEST"), 1e-9);
    EXPECT_NEAR(a.get_annual_volatility("TEST"), b.get_annual_volatility("TEST"), 1e-12);
    EXPECT_NEAR(a.get_volatility_multiplier("TEST"), b.get_volatility_multiplier("TEST"), 1e-12);
}
