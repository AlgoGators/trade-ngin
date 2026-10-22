// The equity slow rule (LOOP_SPEC v6.2 section 2.5, D40): on the symbols the rule names, a NEGATIVE
// combined forecast of a ruled sleeve stands only when the scaled forecast of every pair the rule
// names, (32,128) and (64,256), is negative; otherwise the forecast, and the position sized from
// it, is 0. A positive forecast, a symbol the rule does not name and a sleeve that is not ruled (the
// FAST sleeve, which carries neither pair) are untouched.
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "../risk/risk_module_test_helpers.hpp"
#include "trade_ngin/data/listing_dates.hpp"
#include "trade_ngin/instruments/futures.hpp"

#define private public
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/strategy/trend_following.hpp"
#undef private
#include "trade_ngin/strategy/sleeve_config.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;


namespace {

const std::string kSym = "TSTSLOW";
const std::vector<std::pair<int, int>> kSix = {{2, 8}, {4, 16}, {8, 32}, {16, 64}, {32, 128}, {64, 256}};
const std::vector<std::pair<int, int>> kFast = {{2, 8}, {4, 16}, {8, 32}, {16, 64}};
const std::vector<std::pair<int, int>> kSlowPairs = {{32, 128}, {64, 256}};
const Timestamp kDay0 = std::chrono::system_clock::from_time_t(1262563200);  // Monday 2010-01-04

// The rule's fields exist from the commit that adds the rule. Set through a constraint so that this
// file also compiles on the commit before it, where a sleeve cannot be ruled and the zeroing test
// fails: that run is the test's proof that it tests the rule.
template <class Config>
void rule(Config& config, const std::vector<std::string>& symbols,
          const std::vector<std::pair<int, int>>& pairs) {
    if constexpr (requires { config.equity_slow_symbols; }) {
        config.equity_slow_symbols = symbols;
        config.equity_slow_pairs = pairs;
    }
}

// n daily bars: `up` bars drifting up by `drift` a bar, then the rest drifting down by `fall` a bar,
// with a small deterministic wobble so the volatility is not on its floor.
std::vector<Bar> path(int n, int up, double drift, double fall) {
    std::vector<Bar> bars;
    double p = 100.0;
    unsigned state = 88172645u;
    for (int k = 0; k < n; ++k) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        const double u = static_cast<double>(state % 100000u) / 100000.0 - 0.5;
        p *= 1.0 + 0.006 * u + (k < up ? drift : -fall);
        Bar b;
        b.symbol = kSym;
        b.timestamp = kDay0 + std::chrono::hours(24 * k);
        b.open = Decimal(p);
        b.high = Decimal(p * 1.002);
        b.low = Decimal(p * 0.998);
        b.close = Decimal(p);
        b.volume = 100000.0;
        b.instrument_id = "A";
        bars.push_back(b);
    }
    return bars;
}

struct Sleeve {
    std::shared_ptr<MockPostgresDatabase> db;
    std::unique_ptr<TrendFollowingStrategy> strategy;
    Result<void> initialized;

    Sleeve(const std::string& id, const std::vector<std::pair<int, int>>& pairs,
           const std::vector<std::string>& ruled_symbols,
           const std::vector<std::pair<int, int>>& rule_pairs = kSlowPairs, bool start = true,
           const std::map<std::string, std::vector<std::pair<int, int>>>& removals = {},
           const std::vector<std::pair<int, double>>& fdm = {{1, 1.0},  {2, 1.03}, {3, 1.08},
                                                             {4, 1.13}, {5, 1.19}, {6, 1.26}}) {
        db = std::make_shared<MockPostgresDatabase>("mock://testdb");
        EXPECT_TRUE(db->connect().is_ok());
        StrategyConfig sc;
        sc.capital_allocation = 1'000'000.0;
        sc.max_leverage = 100.0;
        sc.asset_classes = {AssetClass::FUTURES};
        sc.frequencies = {DataFrequency::DAILY};
        sc.trading_params[kSym] = 5.0;
        sc.position_limits[kSym] = 1000.0;
        TrendFollowingConfig tc;
        tc.risk_target = 0.2;
        tc.idm = 2.5;
        tc.ema_windows = pairs;
        tc.vol_lookback_short = pairs.size() == 4 ? 16 : 32;
        tc.fdm = fdm;
        if (!ruled_symbols.empty()) rule(tc, ruled_symbols, rule_pairs);
        tc.rule_removals = removals;
        auto& registry = InstrumentRegistry::instance();
        FuturesSpec spec;
        spec.root_symbol = kSym;
        spec.exchange = "CME";
        spec.currency = "USD";
        spec.multiplier = 5.0;
        spec.tick_size = 0.25;
        spec.commission_per_contract = 2.0;
        spec.initial_margin = 10000.0;
        spec.maintenance_margin = 8000.0;
        spec.trading_hours = "09:30-16:00";
        registry.instruments_[kSym] = std::make_shared<FuturesInstrument>(kSym, spec);
        // further contracts a removal list may name: one the fixture feeds no bar of, and the
        // two sides of a listing-date pair
        for (const char* other : {"OTHER", "MES", "ES"}) {
            FuturesSpec named = spec;
            named.root_symbol = other;
            registry.instruments_[other] = std::make_shared<FuturesInstrument>(other, named);
        }
        registry.initialized_ = true;
        auto registry_ptr = std::shared_ptr<InstrumentRegistry>(&registry, [](InstrumentRegistry*) {});
        strategy = std::make_unique<TrendFollowingStrategy>(id, sc, tc, db, registry_ptr);
        initialized = strategy->initialize();
        if (!start) return;
        EXPECT_TRUE(initialized.is_ok());
        RiskLimits limits;
        limits.max_position_size = 1000.0;
        limits.max_notional_value = 1e9;
        limits.max_drawdown = 0.5;
        limits.max_leverage = 100.0;
        EXPECT_TRUE(strategy->update_risk_limits(limits).is_ok());
        EXPECT_TRUE(strategy->start().is_ok());
    }
    ~Sleeve() {
        strategy->stop();
        strategy.reset();
        db->disconnect();
        auto& registry = InstrumentRegistry::instance();
        registry.instruments_.clear();
        registry.initialized_ = false;
    }
    double forecast(const std::vector<Bar>& bars) {
        EXPECT_TRUE(strategy->on_data(bars).is_ok());
        return strategy->get_forecast(kSym);
    }
    double position() const { return strategy->get_all_instrument_data().at(kSym).raw_position; }
    std::vector<double> scaled() const {
        return strategy->get_all_instrument_data().at(kSym).estimate.scaled;
    }
};

// A long fall: every speed is negative.
std::vector<Bar> falling() { return path(700, 0, 0.0, 0.0012); }
// A long rise and then a sharp fall of thirty bars: the fast speeds are negative and strong, the
// combined forecast is negative, and the slowest speed (64,256) is still positive.
std::vector<Bar> rise_then_drop() { return path(700, 670, 0.0015, 0.0080); }
// A long rise: every speed is positive.
std::vector<Bar> rising() { return path(700, 700, 0.0012, 0.0); }

}  // namespace

// Both slow speeds negative: the equity short stands, equal to the unruled forecast.
TEST(EquitySlowRule, AShortWithBothSlowSpeedsNegativeStands) {
    StateManager::reset_instance();
    const auto bars = falling();
    Sleeve plain("SLOW_stands_plain", kSix, {});
    Sleeve ruled("SLOW_stands_ruled", kSix, {kSym});
    const double unruled = plain.forecast(bars);
    ASSERT_LT(unruled, -1.0) << "the fixture is not a short";
    EXPECT_EQ(ruled.forecast(bars), unruled);
    EXPECT_LT(ruled.position(), 0.0);
}

// One slow speed positive: the negative combined forecast is set to 0, and so is the position.
TEST(EquitySlowRule, AShortWithOneSlowSpeedPositiveIsZeroed) {
    StateManager::reset_instance();
    const auto bars = rise_then_drop();
    Sleeve plain("SLOW_zero_plain", kSix, {});
    Sleeve ruled("SLOW_zero_ruled", kSix, {kSym});
    const double unruled = plain.forecast(bars);
    ASSERT_LT(unruled, -1.0) << "the fixture's combined forecast is not negative";
    // The fixture's slowest speed is positive: a sleeve of that pair alone has a positive forecast.
    Sleeve slowest("SLOW_zero_slowest", {{64, 256}}, {});
    ASSERT_GT(slowest.forecast(bars), 0.0) << "the fixture's (64,256) speed is not positive";
    EXPECT_EQ(ruled.forecast(bars), 0.0);
    EXPECT_EQ(ruled.position(), 0.0);
}

// A positive forecast is untouched.
TEST(EquitySlowRule, APositiveForecastIsUntouched) {
    StateManager::reset_instance();
    const auto bars = rising();
    Sleeve plain("SLOW_long_plain", kSix, {});
    Sleeve ruled("SLOW_long_ruled", kSix, {kSym});
    const double unruled = plain.forecast(bars);
    ASSERT_GT(unruled, 1.0);
    EXPECT_EQ(ruled.forecast(bars), unruled);
}

// The FAST sleeve carries neither slow pair and is not ruled: its equity short stands with no
// slow agreement behind it.
TEST(EquitySlowRule, AFastSleeveShortWithNoSlowAgreementIsUntouched) {
    StateManager::reset_instance();
    const auto bars = rise_then_drop();
    Sleeve fast("SLOW_fast", kFast, {});
    const double forecast = fast.forecast(bars);
    EXPECT_LT(forecast, -1.0) << "the fast sleeve's short stands";
    EXPECT_LT(fast.position(), 0.0);
}

// A symbol the rule does not name is untouched, whatever its slow speeds say.
TEST(EquitySlowRule, ASymbolTheRuleDoesNotNameIsUntouched) {
    StateManager::reset_instance();
    const auto bars = rise_then_drop();
    Sleeve plain("SLOW_other_plain", kSix, {});
    Sleeve ruled("SLOW_other_ruled", kSix, {"MES", "MNQ", "MYM", "M2K"});
    const double unruled = plain.forecast(bars);
    ASSERT_LT(unruled, -1.0);
    EXPECT_EQ(ruled.forecast(bars), unruled);
}

// A ruled sleeve carries the pairs the rule reads: the rule on a sleeve without them (the FAST
// sleeve put first in a book) is refused when the sleeve is built.
TEST(EquitySlowRule, ARuledSleeveWithoutTheSlowPairsIsRefused) {
    StateManager::reset_instance();
    Sleeve fast("SLOW_refused", kFast, {kSym}, kSlowPairs, /*start=*/false);
    ASSERT_TRUE(fast.initialized.is_error());
    EXPECT_NE(std::string(fast.initialized.error()->what()).find("(32, 128)"), std::string::npos);
}

// Both futures books' templates carry the rule as LOOP_SPEC section 7.5.1 writes it.
TEST(EquitySlowRule, TheFuturesTemplatesCarryTheRule) {
    std::filesystem::path root = std::filesystem::current_path();
    while (!(std::filesystem::exists(root / "CMakeLists.txt") &&
             std::filesystem::exists(root / "config_template"))) {
        ASSERT_NE(root, root.parent_path()) << "repository root not found";
        root = root.parent_path();
    }
    for (const char* book : {"conservative", "base"}) {
        std::ifstream in(root / "config_template" / "portfolios" / book / "portfolio.json");
        ASSERT_TRUE(in.good()) << book;
        const nlohmann::json portfolio = nlohmann::json::parse(in);
        ASSERT_TRUE(portfolio.contains("equity_slow_rule")) << book;
        EXPECT_EQ(portfolio["equity_slow_rule"]["symbols"],
                  nlohmann::json({"M2K", "MES", "MNQ", "MYM"}))
            << book;
        EXPECT_EQ(portfolio["equity_slow_rule"]["pairs"],
                  nlohmann::json::array({{32, 128}, {64, 256}}))
            << book;
    }
}

// ---------------------------------------------------------------------------------------------
// Trading rules removed from a contract by cost (Carver, strategy nine, "Removing expensive
// trading rules"; portfolio.json's trading_rule_removals). A contract the list names runs the
// pairs left at equal weight with the multiplier for their number; every other contract, and
// every contract without a list, runs all six exactly as before.
// ---------------------------------------------------------------------------------------------
namespace {

using Removals = std::map<std::string, std::vector<std::pair<int, int>>>;
const double kMultiplier[] = {0.0, 1.0, 1.03, 1.08, 1.13, 1.19, 1.26};

// A noisy rise and a fall of twenty bars: the six speeds all differ, the fast ones are negative
// and the slow ones positive, and none sits on the cap (the first test asserts it), so each
// speed's presence and each multiplier shows in the combined forecast.
std::vector<Bar> mixed() {
    std::vector<Bar> bars = path(700, 680, 0.0, 0.0);
    double p = 100.0;
    unsigned state = 88172645u;
    for (int k = 0; k < 700; ++k) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        const double u = static_cast<double>(state % 100000u) / 100000.0 - 0.5;
        p *= 1.0 + 0.05 * u + (k < 680 ? 0.0015 : -0.0050);
        bars[k].open = bars[k].close = Decimal(p);
        bars[k].high = Decimal(p * 1.002);
        bars[k].low = Decimal(p * 0.998);
    }
    return bars;
}

Removals fastest(std::size_t n) {
    return {{kSym, std::vector<std::pair<int, int>>(kSix.begin(), kSix.begin() + static_cast<long>(n))}};
}

std::string refusal_of(const Removals& removals, const std::vector<std::string>& ruled = {},
                       const std::vector<std::pair<int, double>>& fdm = {
                           {1, 1.0}, {2, 1.03}, {3, 1.08}, {4, 1.13}, {5, 1.19}, {6, 1.26}}) {
    StateManager::reset_instance();
    Sleeve sleeve("RULES_refused", kSix, ruled, kSlowPairs, /*start=*/false, removals, fdm);
    return sleeve.initialized.is_error() ? std::string(sleeve.initialized.error()->what()) : "";
}

}  // namespace

// The removed speeds contribute nothing: the combined forecast is the multiplier for the number
// left times the plain mean of the speeds left, each of them the value it has among all six.
TEST(TradingRuleRemovals, TheForecastIsTheEqualWeightMeanOfThePairsLeftTimesTheirMultiplier) {
    const auto bars = mixed();
    StateManager::reset_instance();
    Sleeve plain("RULES_plain", kSix, {});
    const double six = plain.forecast(bars);
    const std::vector<double> all = plain.scaled();
    ASSERT_EQ(all.size(), 6u);
    for (std::size_t k = 0; k < 6; ++k) {
        ASSERT_LT(std::abs(all[k]), 20.0) << "speed " << k << " sits on the cap";
        for (std::size_t j = 0; j < k; ++j) ASSERT_NE(all[k], all[j]) << "two speeds agree";
    }
    const double plain_position = plain.position();
    ASSERT_NE(plain_position, 0.0);
    for (std::size_t removed = 1; removed <= 5; ++removed) {
        StateManager::reset_instance();
        Sleeve cut("RULES_cut_" + std::to_string(removed), kSix, {}, kSlowPairs, true, fastest(removed));
        const double forecast = cut.forecast(bars);
        const std::vector<double> left = cut.scaled();
        ASSERT_EQ(left.size(), 6 - removed);
        double sum = 0.0;
        for (std::size_t k = 0; k < left.size(); ++k) {
            EXPECT_EQ(left[k], all[k + removed]) << removed << " removed, pair " << k + removed;
            sum += all[k + removed];
        }
        const double expected =
            std::clamp(kMultiplier[left.size()] * (sum / static_cast<double>(left.size())), -20.0, 20.0);
        EXPECT_EQ(forecast, expected) << removed << " removed";
        EXPECT_NE(forecast, six) << "the fixture does not tell " << removed << " removed from none";
        EXPECT_LT(std::abs(forecast), 20.0) << "the cap hides the multiplier";
        // the position follows the forecast and nothing else moves
        EXPECT_NEAR(cut.position() / plain_position, forecast / six, 1e-12) << removed << " removed";
    }
}

// A contract the list does not name is untouched to the last bit, and so is every contract when
// the list is empty.
TEST(TradingRuleRemovals, AContractTheListDoesNotNameIsUntouched) {
    const auto bars = mixed();
    StateManager::reset_instance();
    Sleeve plain("RULES_other_plain", kSix, {});
    const double six = plain.forecast(bars);
    StateManager::reset_instance();
    Sleeve other("RULES_other", kSix, {}, kSlowPairs, true, {{"OTHER", {{2, 8}, {4, 16}}}});
    EXPECT_EQ(other.forecast(bars), six);
    EXPECT_EQ(other.scaled(), plain.scaled());
    EXPECT_EQ(other.position(), plain.position());
    StateManager::reset_instance();
    Sleeve empty("RULES_empty", kSix, {}, kSlowPairs, true, Removals{});
    EXPECT_EQ(empty.forecast(bars), six);
    EXPECT_EQ(empty.scaled(), plain.scaled());
    EXPECT_EQ(empty.position(), plain.position());
}

// The multiplier is the table's row for the number of rules left, read from the sleeve's table.
TEST(TradingRuleRemovals, TheMultiplierIsTheTablesRowForTheNumberLeft) {
    const auto bars = mixed();
    StateManager::reset_instance();
    Sleeve table("RULES_table", kSix, {}, kSlowPairs, true, fastest(2));
    const double with_table = table.forecast(bars);
    StateManager::reset_instance();
    Sleeve other_row("RULES_other_row", kSix, {}, kSlowPairs, true, fastest(2),
                   {{1, 1.0}, {2, 1.03}, {3, 1.08}, {4, 1.50}, {5, 1.19}, {6, 1.26}});
    ASSERT_LT(std::abs(with_table), 13.0) << "the fixture is near the cap";
    EXPECT_NEAR(other_row.forecast(bars) / with_table, 1.50 / 1.13, 1e-12);
    // Carver's table 36, the rows the sleeve's default table carries
    const TrendFollowingConfig defaults;
    EXPECT_EQ(defaults.fdm, (std::vector<std::pair<int, double>>{
                                {1, 1.0}, {2, 1.03}, {3, 1.08}, {4, 1.13}, {5, 1.19}, {6, 1.26}}));
}

// A list the sleeve cannot run as written is refused when the sleeve is built, never bent.
TEST(TradingRuleRemovals, AListThatIsNotTheFastestRulesIsRefused) {
    EXPECT_EQ(refusal_of(fastest(1)), "");
    EXPECT_EQ(refusal_of(fastest(5)), "");
    EXPECT_NE(refusal_of({{kSym, {{4, 16}}}}).find("(2, 8) is kept while a slower pair is removed"),
              std::string::npos);
    EXPECT_NE(refusal_of(fastest(6)).find("no pair is left"), std::string::npos);
    EXPECT_NE(refusal_of({{kSym, {{128, 512}}}}).find("not one of the sleeve's pairs"), std::string::npos);
    EXPECT_NE(refusal_of({{kSym, {{2, 8}, {2, 8}}}}).find("named twice"), std::string::npos);
    EXPECT_NE(refusal_of({{kSym, {}}}).find("no pair is named"), std::string::npos);
    EXPECT_NE(refusal_of({{kSym + ".v.0", {{2, 8}}}}).find("base symbol"), std::string::npos);
    // a mistyped contract would remove nothing and say nothing
    EXPECT_NE(refusal_of({{"RZ", {{2, 8}}}}).find("holds no contract of that name"), std::string::npos);
    // the table has no row for the number left: without the refusal the multiplier would be 1
    EXPECT_NE(refusal_of(fastest(2), {}, {{1, 1.0}, {2, 1.03}, {3, 1.08}, {5, 1.19}, {6, 1.26}})
                  .find("no row for 4 rules"),
              std::string::npos);
}

// The equity slow rule reads the two slow speeds: a list may take a ruled symbol's fast rules, and
// the rule goes on acting on what is left, but it may not take a speed the rule reads.
TEST(TradingRuleRemovals, ASpeedTheEquitySlowRuleReadsIsNeverRemoved) {
    EXPECT_NE(refusal_of(fastest(5), {kSym}).find("read by the equity slow rule"), std::string::npos);
    EXPECT_EQ(refusal_of(fastest(4), {kSym}), "");
    // the same list on a symbol the rule does not name is usable
    EXPECT_EQ(refusal_of(fastest(5), {"MES"}), "");

    const auto bars = rise_then_drop();
    StateManager::reset_instance();
    Sleeve unruled("RULES_slow_unruled", kSix, {}, kSlowPairs, true, fastest(1));
    ASSERT_LT(unruled.forecast(bars), -1.0) << "the fixture's five-speed forecast is not negative";
    StateManager::reset_instance();
    Sleeve ruled("RULES_slow_ruled", kSix, {kSym}, kSlowPairs, true, fastest(1));
    EXPECT_EQ(ruled.forecast(bars), 0.0) << "the slowest speed is positive: the short is zeroed";
    EXPECT_EQ(ruled.position(), 0.0);
    const auto all_down = falling();
    StateManager::reset_instance();
    Sleeve stands_plain("RULES_slow_stands_plain", kSix, {}, kSlowPairs, true, fastest(1));
    const double short_forecast = stands_plain.forecast(all_down);
    StateManager::reset_instance();
    Sleeve stands("RULES_slow_stands", kSix, {kSym}, kSlowPairs, true, fastest(1));
    EXPECT_EQ(stands.forecast(all_down), short_forecast);
}

// The two contracts of a listing-date pair run one price history as one instrument: a list that
// takes a rule from one side only would step the forecast on the listing date, and is refused.
TEST(TradingRuleRemovals, BothContractsOfAListingDatePairLoseTheSameRules) {
    struct Listed {
        Listed() { ListingDates::instance().set({{"MES", "ES", "2019-05-06", 10.0}}); }
        ~Listed() { ListingDates::instance().clear(); }
    } listed;
    const std::vector<std::pair<int, int>> one = {{2, 8}}, two = {{2, 8}, {4, 16}};
    EXPECT_NE(refusal_of({{"MES", one}}).find("must lose the same rules"), std::string::npos);
    EXPECT_NE(refusal_of({{"ES", one}}).find("must lose the same rules"), std::string::npos);
    EXPECT_NE(refusal_of({{"MES", one}, {"ES", two}}).find("must lose the same rules"), std::string::npos);
    EXPECT_EQ(refusal_of({{"MES", two}, {"ES", {{4, 16}, {2, 8}}}}), "");
    EXPECT_EQ(refusal_of({{kSym, one}}), "") << "a contract outside every pair is not concerned";
    // the equity slow rule covers the pair under the listed contract's name
    EXPECT_NE(refusal_of({{"MES", std::vector<std::pair<int, int>>(kSix.begin(), kSix.begin() + 5)},
                          {"ES", std::vector<std::pair<int, int>>(kSix.begin(), kSix.begin() + 5)}},
                         {"MES"})
                  .find("read by the equity slow rule"),
              std::string::npos);
}
// The list names a contract by its base symbol and the bars carry the continuous symbol: "X" in
// the list is "X.v.0" in the feed.
TEST(TradingRuleRemovals, TheListsBaseSymbolIsTheFeedsContinuousSymbol) {
    const std::string fed = kSym + ".v.0";
    auto bars = mixed();
    for (auto& bar : bars) bar.symbol = fed;
    auto run = [&](const std::string& id, const Removals& removals) {
        StateManager::reset_instance();
        Sleeve sleeve(id, kSix, {}, kSlowPairs, true, removals);
        EXPECT_TRUE(sleeve.strategy->on_data(bars).is_ok());
        return std::make_pair(sleeve.strategy->get_forecast(fed),
                              sleeve.strategy->get_all_instrument_data().at(fed).estimate.scaled);
    };
    const auto plain = run("RULES_fed_plain", {});
    const auto cut = run("RULES_fed_cut", fastest(2));
    ASSERT_EQ(plain.second.size(), 6u);
    ASSERT_EQ(cut.second.size(), 4u) << "the list was not found under the feed's symbol";
    const double mean = (plain.second[2] + plain.second[3] + plain.second[4] + plain.second[5]) / 4.0;
    EXPECT_EQ(cut.first, std::clamp(1.13 * mean, -20.0, 20.0));
    EXPECT_NE(cut.first, plain.first);
}

// The sleeve's own record keeps one column a pair of the SLEEVE: a pair removed from the contract
// prints nan under its own header and the pairs left keep theirs.
TEST(TradingRuleRemovals, TheEstimatorRecordKeepsEveryPairsColumn) {
    const auto dir = std::filesystem::temp_directory_path() / "tn_rule_removals_record";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    setenv("TRADE_NGIN_SERIES_DUMP_DIR", dir.c_str(), 1);
    std::vector<double> left;
    {
        StateManager::reset_instance();
        Sleeve cut("RULES_record", kSix, {}, kSlowPairs, true, fastest(2));
        cut.forecast(mixed());
        left = cut.scaled();
    }
    unsetenv("TRADE_NGIN_SERIES_DUMP_DIR");
    std::ifstream in(dir / "estimator_RULES_record.csv");
    ASSERT_TRUE(in.good());
    std::string header, row, line;
    std::getline(in, header);
    while (std::getline(in, line)) {
        if (!line.empty()) row = line;
    }
    auto cells = [](const std::string& text) {
        std::vector<std::string> out;
        std::stringstream ss(text);
        std::string cell;
        while (std::getline(ss, cell, ',')) out.push_back(cell);
        return out;
    };
    const auto names = cells(header), values = cells(row);
    ASSERT_EQ(names.size(), values.size());
    ASSERT_GE(names.size(), 6u);
    const std::size_t at = names.size() - 6;
    EXPECT_EQ(names[at], "scaled_2_8");
    EXPECT_EQ(names[at + 5], "scaled_64_256");
    EXPECT_EQ(values[at], "nan");
    EXPECT_EQ(values[at + 1], "nan");
    ASSERT_EQ(left.size(), 4u);
    for (std::size_t k = 0; k < 4; ++k) {
        EXPECT_EQ(std::stod(values[at + 2 + k]), left[k]) << names[at + 2 + k];
    }
    std::filesystem::remove_all(dir);
}

// The list reaches the sleeve through one function, called by every futures runner. The function
// carries the loaded list across, and a sleeve built from what it fills runs the pairs left.
TEST(TradingRuleRemovals, TheHandOverCarriesTheLoadedListToTheSleeve) {
    AppConfig app;
    app.trading_rule_removals = fastest(2);
    TrendFollowingConfig handed;
    ASSERT_TRUE(handed.rule_removals.empty());
    hand_over_trading_rule_removals(app, handed);
    EXPECT_EQ(handed.rule_removals, fastest(2)) << "the list did not reach the sleeve's config";

    const auto bars = mixed();
    StateManager::reset_instance();
    Sleeve plain("RULES_handed_plain", kSix, {});
    const double six = plain.forecast(bars);
    StateManager::reset_instance();
    Sleeve cut("RULES_handed_cut", kSix, {}, kSlowPairs, true, handed.rule_removals);
    EXPECT_NE(cut.forecast(bars), six);
    EXPECT_EQ(cut.scaled().size(), 4u);

    // an absent block hands over nothing and leaves the sleeve's config as it was
    TrendFollowingConfig untouched;
    hand_over_trading_rule_removals(AppConfig{}, untouched);
    EXPECT_TRUE(untouched.rule_removals.empty());
}

// Every futures runner makes the hand-over, once, where it builds its trend sleeve, and none
// assigns the list by itself: a runner that lost the call would parse the block and ignore it.
TEST(TradingRuleRemovals, EveryFuturesRunnerCallsTheHandOverOnce) {
    std::filesystem::path root = std::filesystem::current_path();
    while (!(std::filesystem::exists(root / "CMakeLists.txt") &&
             std::filesystem::exists(root / "config_template"))) {
        ASSERT_NE(root, root.parent_path()) << "repository root not found";
        root = root.parent_path();
    }
    auto count = [](const std::string& text, const std::string& what) {
        std::size_t n = 0;
        for (std::size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++n;
        return n;
    };
    for (const char* runner : {"apps/backtest/bt_portfolio.cpp", "apps/backtest/bt_portfolio_conservative.cpp",
                               "apps/strategies/live_portfolio.cpp",
                               "apps/strategies/live_portfolio_conservative.cpp"}) {
        std::ifstream in(root / runner);
        ASSERT_TRUE(in.good()) << runner;
        std::stringstream text;
        text << in.rdbuf();
        const std::string source = text.str();
        EXPECT_EQ(count(source, "hand_over_trading_rule_removals(app_config, trend_config);"), 1u) << runner;
        EXPECT_EQ(count(source, "rule_removals ="), 0u) << runner << " assigns the list itself";
        // the call sits in the branch that builds the TrendFollowingStrategy sleeve, before the
        // fast sleeve's branch
        const std::size_t call = source.find("hand_over_trading_rule_removals(");
        const std::size_t trend = source.find("strategy_type == \"TrendFollowingStrategy\"");
        const std::size_t fast = source.find("strategy_type == \"TrendFollowingFastStrategy\"");
        ASSERT_NE(call, std::string::npos) << runner;
        ASSERT_NE(trend, std::string::npos) << runner;
        ASSERT_NE(fast, std::string::npos) << runner;
        EXPECT_LT(trend, call) << runner;
        EXPECT_LT(call, fast) << runner;
    }
}
