// Listing dates (data/listing_dates.hpp): a contract trades only from its listing date,
// and before it the book trades the contract that existed, on the same price history. The rule is
// off unless contracts are set; set, the predecessor signals only on signal bars dated before the
// listing date and the listed contract only on bars dated on or after it, both on the whole history.
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "../risk/risk_module_test_helpers.hpp"
#include "trade_ngin/data/listing_dates.hpp"
#include "trade_ngin/instruments/futures.hpp"
#include "trade_ngin/optimization/one_pass.hpp"

#define private public
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/strategy/trend_following.hpp"
#undef private

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

const std::string kMicro = "TMIC.v.0";
const std::string kBig = "TBIG.v.0";
const std::string kOther = "TOTH.v.0";
const Timestamp kDay0 = std::chrono::system_clock::from_time_t(1262563200);  // Monday 2010-01-04
constexpr int kBars = 400;        // bars 0..399; the listing date is bar 397's date
constexpr int kListedBar = 397;

Timestamp day(int k) { return kDay0 + std::chrono::hours(24 * k); }

std::string ymd(const Timestamp& ts) {
    const std::chrono::year_month_day d{std::chrono::floor<std::chrono::days>(ts)};
    char text[11];
    std::snprintf(text, sizeof(text), "%04d-%02u-%02u", static_cast<int>(d.year()),
                  static_cast<unsigned>(d.month()), static_cast<unsigned>(d.day()));
    return text;
}

std::vector<ListedContract> pair() { return {{"TMIC", "TBIG", ymd(day(kListedBar)), 10.0}}; }

// One rising index path, the same closes for every symbol that reads it.
std::vector<Bar> path(const std::string& symbol, int n = kBars) {
    std::vector<Bar> bars;
    double p = 2000.0;
    unsigned state = 88172645u;
    for (int k = 0; k < n; ++k) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        const double u = static_cast<double>(state % 100000u) / 100000.0 - 0.5;
        p *= 1.0 + 0.006 * u + 0.0012;
        Bar b;
        b.symbol = symbol;
        b.timestamp = day(k) + std::chrono::hours(5);  // some hours into its date, as loaded bars are
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

struct Guard {
    Guard() { reset(); }
    ~Guard() { reset(); }
    static void reset() {
        ListingDates::instance().clear();
        ListingDates::instance().set_relabels({});
        ListingDates::instance().set_switch_rule(ListingSwitchRule::kOpenAtTarget);
    }
};

// A trend sleeve over the micro, its predecessor and one unrelated symbol.
struct Sleeve {
    std::shared_ptr<MockPostgresDatabase> db;
    std::unique_ptr<TrendFollowingStrategy> strategy;

    explicit Sleeve(const std::string& id) {
        db = std::make_shared<MockPostgresDatabase>("mock://testdb");
        EXPECT_TRUE(db->connect().is_ok());
        StrategyConfig sc;
        sc.capital_allocation = 500'000.0;
        sc.max_leverage = 100.0;
        sc.asset_classes = {AssetClass::FUTURES};
        sc.frequencies = {DataFrequency::DAILY};
        TrendFollowingConfig tc;
        tc.risk_target = 0.2;
        tc.idm = 2.5;
        tc.ema_windows = {{2, 8}, {4, 16}, {8, 32}, {16, 64}, {32, 128}, {64, 256}};
        tc.vol_lookback_short = 32;
        tc.fdm = {{1, 1.0}, {2, 1.03}, {3, 1.08}, {4, 1.13}, {5, 1.19}, {6, 1.26}};
        auto& registry = InstrumentRegistry::instance();
        const std::pair<std::string, double> terms[] = {{"TMIC", 5.0}, {"TBIG", 50.0}, {"TOTH", 5.0}};
        for (const auto& [root, multiplier] : terms) {
            sc.trading_params[root + ".v.0"] = multiplier;
            sc.position_limits[root + ".v.0"] = 1000.0;
            FuturesSpec spec;
            spec.root_symbol = root;
            spec.exchange = "CME";
            spec.currency = "USD";
            spec.multiplier = multiplier;
            spec.tick_size = 0.25;
            spec.commission_per_contract = 2.0;
            spec.initial_margin = 10000.0;
            spec.maintenance_margin = 8000.0;
            spec.trading_hours = "09:30-16:00";
            registry.instruments_[root] = std::make_shared<FuturesInstrument>(root, spec);
        }
        registry.initialized_ = true;
        auto registry_ptr = std::shared_ptr<InstrumentRegistry>(&registry, [](InstrumentRegistry*) {});
        strategy = std::make_unique<TrendFollowingStrategy>(id, sc, tc, db, registry_ptr);
        EXPECT_TRUE(strategy->initialize().is_ok());
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
    // Feeds the three symbols' bars [from, to) as one call per date (bar `from` onward one date a
    // call; the first call is the bulk history).
    void feed_history(int to) {
        std::vector<Bar> all;
        for (const auto& symbol : {kMicro, kBig, kOther}) {
            auto bars = path(symbol, to);
            all.insert(all.end(), bars.begin(), bars.end());
        }
        ASSERT_TRUE(strategy->on_data(all).is_ok());
    }
    void feed_day(int k) {
        std::vector<Bar> group;
        for (const auto& symbol : {kMicro, kBig, kOther}) group.push_back(path(symbol, k + 1).back());
        ASSERT_TRUE(strategy->on_data(group).is_ok());
    }
    double target(const std::string& symbol) const {
        return strategy->get_all_instrument_data().at(symbol).final_position;
    }
    StrategyInterface::OverlaySeries series(const std::string& symbol) const {
        StrategyInterface::OverlaySeries s;
        EXPECT_TRUE(strategy->overlay_series(symbol, &s));
        return s;
    }
};

}  // namespace

// Default off: nothing is a predecessor, every symbol is tradeable on every date, no symbol or row is
// added and none is taken away.
TEST(ListingDates, OffByDefaultIsTheIdentity) {
    Guard guard;
    const auto& rule = ListingDates::instance();
    EXPECT_FALSE(rule.enabled());
    EXPECT_TRUE(rule.tradeable("MES.v.0", day(0)));
    EXPECT_TRUE(rule.tradeable("ES.v.0", day(4000)));
    EXPECT_EQ(rule.pair_root("ES"), "ES");
    const std::vector<std::string> symbols = {"MES.v.0", "ES.v.0", "ZN.v.0"};
    EXPECT_EQ(rule.stored_symbols(symbols), symbols);
    EXPECT_TRUE(rule.predecessor_symbols(symbols, day(0)).empty());
    std::vector<Bar> bars = path("MES.v.0", 5);
    rule.add_predecessor_bars(symbols, bars);
    EXPECT_EQ(bars.size(), 5u);
    std::vector<market_data_utils::FuturesInstrumentId> ids = {{"MES.v.0", "2019-05-03", "736"}};
    rule.add_predecessor_ids(symbols, ids);
    EXPECT_EQ(ids.size(), 1u);
}

// The day before, the switch day and the day after, for both contracts of a pair and a third symbol.
TEST(ListingDates, TheWindowTurnsOnTheListingDate) {
    Guard guard;
    auto& rule = ListingDates::instance();
    rule.set({{"MES", "ES", "2019-05-06", 10.0}, {"M2K", "RTY", "2019-05-06", 10.0}});
    using std::chrono::sys_days;
    using namespace std::chrono_literals;
    const Timestamp before = sys_days{2019y / 5 / 5} + std::chrono::hours(23);  // late on the day before
    const Timestamp on = sys_days{2019y / 5 / 6};                               // 00:00 of the listing date
    const Timestamp after = sys_days{2019y / 5 / 7} + std::chrono::hours(5);
    EXPECT_TRUE(rule.tradeable("ES.v.0", before));
    EXPECT_FALSE(rule.tradeable("MES.v.0", before));
    EXPECT_FALSE(rule.tradeable("ES.v.0", on));
    EXPECT_TRUE(rule.tradeable("MES.v.0", on));
    EXPECT_FALSE(rule.tradeable("ES.v.0", after));
    EXPECT_TRUE(rule.tradeable("MES.v.0", after));
    EXPECT_TRUE(rule.tradeable("RTY.v.0", before));
    EXPECT_FALSE(rule.tradeable("M2K.v.0", before));
    for (const auto& ts : {before, on, after}) EXPECT_TRUE(rule.tradeable("ZN.v.0", ts));
    // exactly one of a pair is tradeable on any instant
    for (const auto& ts : {before, on, after}) {
        EXPECT_NE(rule.tradeable("ES.v.0", ts), rule.tradeable("MES.v.0", ts));
    }
    EXPECT_EQ(rule.pair_root("ES"), "MES");
    EXPECT_EQ(rule.pair_root("RTY"), "M2K");
    EXPECT_EQ(rule.pair_root("MES"), "MES");
    EXPECT_EQ(rule.pair_root("ZN"), "ZN");
}

// A window that starts on or after the listing date adds no symbol (the run is the baseline's run);
// one that starts before it adds the predecessor of each listed contract the run trades, once.
TEST(ListingDates, PredecessorsAreAddedOnlyToAWindowThatStartsBeforeTheListing) {
    Guard guard;
    auto& rule = ListingDates::instance();
    rule.set({{"MES", "ES", "2019-05-06", 10.0}, {"MNQ", "NQ", "2019-05-06", 10.0}, {"MBT", "BTC", "2021-05-03", 10.0}});
    using std::chrono::sys_days;
    using namespace std::chrono_literals;
    const std::vector<std::string> symbols = {"6A.v.0", "MES.v.0", "MNQ.v.0", "ZN.v.0"};
    EXPECT_EQ(rule.predecessor_symbols(symbols, sys_days{2016y / 10 / 7}),
              (std::vector<std::string>{"ES.v.0", "NQ.v.0"}));
    EXPECT_TRUE(rule.predecessor_symbols(symbols, sys_days{2019y / 5 / 6}).empty());
    EXPECT_TRUE(rule.predecessor_symbols(symbols, sys_days{2021y / 10 / 7}).empty());
    std::vector<std::string> with = symbols;
    with.push_back("ES.v.0");
    EXPECT_EQ(rule.predecessor_symbols(with, sys_days{2016y / 10 / 7}),
              (std::vector<std::string>{"NQ.v.0"}));
    with.push_back("NQ.v.0");
    EXPECT_EQ(rule.stored_symbols(with), symbols);
}

// The predecessor's bars and vendor ids are the rows stored under the listed contract's symbol,
// handed on unchanged under its own symbol, after the stored rows and in their order.
TEST(ListingDates, ThePredecessorReadsTheRowsStoredUnderTheListedSymbol) {
    Guard guard;
    auto& rule = ListingDates::instance();
    rule.set({{"MES", "ES", "2019-05-06", 10.0}});
    std::vector<Bar> bars = path("MES.v.0", 4);
    const auto other = path("ZN.v.0", 4);
    bars.insert(bars.end(), other.begin(), other.end());
    const std::vector<Bar> stored = bars;
    rule.add_predecessor_bars({"MES.v.0", "ZN.v.0"}, bars);  // the run does not trade ES
    EXPECT_EQ(bars.size(), stored.size());
    rule.add_predecessor_bars({"MES.v.0", "ZN.v.0", "ES.v.0"}, bars);
    ASSERT_EQ(bars.size(), stored.size() + 4);
    for (size_t i = 0; i < stored.size(); ++i) {
        EXPECT_EQ(bars[i].symbol, stored[i].symbol);
        EXPECT_EQ(bars[i].close, stored[i].close);
    }
    for (size_t i = 0; i < 4; ++i) {
        const Bar& copy = bars[stored.size() + i];
        EXPECT_EQ(copy.symbol, "ES.v.0");
        EXPECT_EQ(copy.timestamp, stored[i].timestamp);
        EXPECT_EQ(copy.open, stored[i].open);
        EXPECT_EQ(copy.high, stored[i].high);
        EXPECT_EQ(copy.low, stored[i].low);
        EXPECT_EQ(copy.close, stored[i].close);
        EXPECT_EQ(copy.volume, stored[i].volume);
        EXPECT_EQ(copy.instrument_id, stored[i].instrument_id);
    }
    std::vector<market_data_utils::FuturesInstrumentId> ids = {
        {"MES.v.0", "2019-05-03", "736"}, {"ZN.v.0", "2019-05-03", "11"}, {"MES.v.0", "2019-05-06", "736"}};
    rule.add_predecessor_ids({"MES.v.0", "ZN.v.0", "ES.v.0"}, ids);
    ASSERT_EQ(ids.size(), 5u);
    EXPECT_EQ(ids[3].symbol, "ES.v.0");
    EXPECT_EQ(ids[3].date, "2019-05-03");
    EXPECT_EQ(ids[3].instrument_id, "736");
    EXPECT_EQ(ids[4].date, "2019-05-06");
}

TEST(ListingDates, AnUnusableBlockIsRefused) {
    Guard guard;
    auto& rule = ListingDates::instance();
    EXPECT_THROW(rule.set({{"MES", "ES", "2019-5-6", 10.0}}), std::invalid_argument);
    EXPECT_THROW(rule.set({{"MES", "ES", "2019-13-06", 10.0}}), std::invalid_argument);
    EXPECT_THROW(rule.set({{"MES.v.0", "ES", "2019-05-06", 10.0}}), std::invalid_argument);
    EXPECT_THROW(rule.set({{"MES", "", "2019-05-06", 10.0}}), std::invalid_argument);
    EXPECT_THROW(rule.set({{"MES", "ES", "2019-05-06", 10.0}, {"MNQ", "ES", "2019-05-06", 10.0}}),
                 std::invalid_argument);
    EXPECT_FALSE(rule.enabled()) << "a refused block leaves the rule as it was";
    EXPECT_THROW(ListingDates::validate({{"MES", "ES", "nope", 10.0}}), std::invalid_argument);
    EXPECT_THROW(ListingDates::validate({{"MES", "ES", "2019-05-06", 0.0}}), std::invalid_argument);
    ListingDates::validate({{"MES", "ES", "2019-05-06", 10.0}});
    EXPECT_FALSE(rule.enabled()) << "validate changes nothing";
}

// The registry's legacy rewrite of a full-size root to its micro's row is on by default and off on
// request: off, the full-size symbol resolves to its own row.
TEST(ListingDates, TheRegistryGivesTheFullSizeRowWhenTheRewriteIsOff) {
    auto& registry = InstrumentRegistry::instance();
    FuturesSpec micro;
    micro.root_symbol = "MES";
    micro.multiplier = 5.0;
    FuturesSpec full;
    full.root_symbol = "ES";
    full.multiplier = 50.0;
    registry.instruments_["MES"] = std::make_shared<FuturesInstrument>("MES", micro);
    registry.instruments_["ES"] = std::make_shared<FuturesInstrument>("ES", full);
    registry.initialized_ = true;
    ASSERT_TRUE(registry.full_size_remap_) << "the rewrite is on by default";
    EXPECT_EQ(registry.get_instrument("ES.v.0")->get_multiplier(), 5.0);
    registry.set_full_size_remap(false);
    EXPECT_EQ(registry.get_instrument("ES.v.0")->get_multiplier(), 50.0);
    EXPECT_TRUE(registry.has_instrument("ES.v.0"));
    EXPECT_EQ(registry.get_instrument("MES.v.0")->get_multiplier(), 5.0);
    registry.set_full_size_remap(true);
    EXPECT_EQ(registry.get_instrument("ES.v.0")->get_multiplier(), 5.0);
    registry.instruments_.clear();
    registry.initialized_ = false;
}

// Off: the three symbols read one path and are sized alike (the predecessor at a tenth of the
// micro's contracts), on every day. This is the sleeve's behaviour before the arm.
TEST(ListingDates, SleeveWithTheRuleOffSignalsEverySymbolOnEveryDay) {
    Guard guard;
    StateManager::reset_instance();
    Sleeve sleeve("LISTING_off");
    sleeve.feed_history(kListedBar - 1);
    for (int k = kListedBar - 1; k <= kListedBar + 1; ++k) {
        sleeve.feed_day(k);
        EXPECT_TRUE(sleeve.strategy->is_signalling(kMicro));
        EXPECT_TRUE(sleeve.strategy->is_signalling(kBig));
        ASSERT_GT(sleeve.target(kMicro), 1.0);
        EXPECT_DOUBLE_EQ(sleeve.target(kMicro), sleeve.target(kOther));
        EXPECT_NEAR(sleeve.target(kBig) * 10.0, sleeve.target(kMicro), 1e-9 * sleeve.target(kMicro));
    }
}

// On: before the listing date the predecessor signals and the micro publishes nothing; from the
// listing date the micro signals, at exactly the target the same sleeve gives a symbol that was
// never restricted (its history was kept), and the predecessor publishes nothing.
TEST(ListingDates, SleeveSwitchesContractOnTheListingDateAndKeepsTheHistory) {
    Guard guard;
    StateManager::reset_instance();
    ListingDates::instance().set(pair());
    Sleeve sleeve("LISTING_on");
    sleeve.feed_history(kListedBar - 1);  // bars 0..395: the last bar is two days before the listing

    // the day before the listing date
    sleeve.feed_day(kListedBar - 1);
    EXPECT_TRUE(sleeve.strategy->is_signalling(kBig));
    EXPECT_FALSE(sleeve.strategy->is_signalling(kMicro));
    EXPECT_TRUE(sleeve.series(kBig).signalling);
    EXPECT_FALSE(sleeve.series(kMicro).signalling);
    ASSERT_GT(sleeve.target(kOther), 1.0);
    EXPECT_NEAR(sleeve.target(kBig) * 10.0, sleeve.target(kOther), 1e-9 * sleeve.target(kOther));
    EXPECT_EQ(sleeve.target(kMicro), 0.0);
    EXPECT_EQ(sleeve.series(kMicro).optimal_position, 0.0);
    EXPECT_EQ(sleeve.series(kBig).multiplier, 50.0);
    EXPECT_EQ(sleeve.strategy->get_all_instrument_data().at(kMicro).price_history.size(),
              static_cast<size_t>(kListedBar))
        << "the micro keeps every bar while it may not trade";

    // the listing date
    sleeve.feed_day(kListedBar);
    EXPECT_FALSE(sleeve.strategy->is_signalling(kBig));
    EXPECT_TRUE(sleeve.strategy->is_signalling(kMicro));
    EXPECT_FALSE(sleeve.series(kBig).signalling);
    EXPECT_TRUE(sleeve.series(kMicro).signalling);
    EXPECT_EQ(sleeve.target(kBig), 0.0);
    EXPECT_EQ(sleeve.series(kBig).optimal_position, 0.0);
    ASSERT_GT(sleeve.target(kOther), 1.0);
    EXPECT_DOUBLE_EQ(sleeve.target(kMicro), sleeve.target(kOther))
        << "the micro's first target is the full-history target";
    EXPECT_EQ(sleeve.series(kMicro).multiplier, 5.0);
    EXPECT_EQ(sleeve.series(kBig).multiplier, 50.0) << "the predecessor is still weighed for its close";
    EXPECT_GT(sleeve.series(kBig).close, 0.0);

    // the day after
    sleeve.feed_day(kListedBar + 1);
    EXPECT_FALSE(sleeve.strategy->is_signalling(kBig));
    EXPECT_TRUE(sleeve.strategy->is_signalling(kMicro));
    EXPECT_EQ(sleeve.target(kBig), 0.0);
    EXPECT_DOUBLE_EQ(sleeve.target(kMicro), sleeve.target(kOther));
}

// A held E-mini at the switch: on the listing date's rebalance the sleeve's answers make the
// predecessor a close-out (one fill to flat, LOOP_SPEC section 6.2) and the micro a free row opened
// from flat, in the one pass the engine runs.
TEST(ListingDates, AHeldPredecessorIsClosedAndTheMicroOpenedOnTheListingDatesRebalance) {
    Guard guard;
    StateManager::reset_instance();
    ListingDates::instance().set(pair());
    Sleeve sleeve("LISTING_pass");
    sleeve.feed_history(kListedBar - 1);
    sleeve.feed_day(kListedBar - 1);

    auto inputs = [&](double held_big, double held_micro, bool big_ever, bool micro_ever) {
        one_pass::DayInputs in;
        in.capital = 500000.0;
        in.limits = {0.45, 0.90, 0.80, 8.0, 6.0};
        const std::string symbols[] = {kBig, kMicro};
        const double held[] = {held_big, held_micro};
        const bool ever[] = {big_ever, micro_ever};
        for (int i = 0; i < 2; ++i) {
            const auto s = sleeve.series(symbols[i]);
            in.multiplier.push_back(s.multiplier);
            in.close.push_back(s.close);
            in.held.push_back(held[i]);
            in.target.push_back(s.signalling ? s.optimal_position : 0.0);
            in.first_forecast.push_back(s.forecast);
            in.cost.push_back(i == 0 ? 15.0 : 2.0);
            in.signalling.push_back(s.signalling);
            in.first_signalling.push_back(s.signalling);
            in.hold.push_back(0);
            in.has_bar.push_back(1);
            in.ever_signalled.push_back(ever[i]);
            in.jump_sigma_daily.push_back(0.0);
        }
        return in;
    };

    // the day before: the predecessor is the free row, the micro is nothing (not a participant)
    {
        const one_pass::DayResult r = one_pass::rebalance(inputs(1.0, 0.0, true, false));
        ASSERT_TRUE(r.refusal.empty()) << r.refusal;
        EXPECT_EQ(r.free, (one_pass::Mask{1, 0}));
        EXPECT_EQ(r.closeout, (one_pass::Mask{0, 0}));
        EXPECT_EQ(r.participant, (one_pass::Mask{1, 0}));
        EXPECT_EQ(r.book[1], 0.0) << "no micro before its listing date";
    }
    // the listing date: one E-mini held
    sleeve.feed_day(kListedBar);
    {
        const one_pass::DayResult r = one_pass::rebalance(inputs(1.0, 0.0, true, false));
        ASSERT_TRUE(r.refusal.empty()) << r.refusal;
        EXPECT_EQ(r.closeout, (one_pass::Mask{1, 0}));
        EXPECT_EQ(r.free, (one_pass::Mask{0, 1}));
        EXPECT_EQ(r.book[0], 0.0) << "the held E-mini is closed to flat";
        EXPECT_GT(r.book[1], 0.0) << "the micro is opened from flat";
        EXPECT_LE(r.book[1], std::ceil(sleeve.target(kMicro)));
    }
    // the day after: the predecessor is flat and inert, the micro is held and free
    sleeve.feed_day(kListedBar + 1);
    {
        const one_pass::DayResult r = one_pass::rebalance(inputs(0.0, 3.0, true, true));
        ASSERT_TRUE(r.refusal.empty()) << r.refusal;
        EXPECT_EQ(r.closeout, (one_pass::Mask{0, 0}));
        EXPECT_EQ(r.free, (one_pass::Mask{0, 1}));
        EXPECT_EQ(r.participant, (one_pass::Mask{0, 1}));
        EXPECT_EQ(r.book[0], 0.0);
    }
}

// ---- the switch day: an exact conversion (HD 2026-10-08) ----

namespace {
ListingLegCost test_cost(const std::string& symbol, double signed_qty, double price) {
    // a fee per contract by symbol and a spread cost in the contract's own multiplier
    const double fee = symbol == "ES.v.0" ? 2.247 : 0.614;
    const double multiplier = symbol == "ES.v.0" ? 50.0 : 5.0;
    ListingLegCost c;
    c.commissions_fees = fee * std::abs(signed_qty);
    c.slippage_market_impact = 0.125 * multiplier * std::abs(signed_qty);
    c.implicit_price_impact = c.slippage_market_impact / (multiplier * price);
    c.total_transaction_costs = c.commissions_fees + c.slippage_market_impact;
    return c;
}
const ListingConversion kConversion{"ES.v.0", "MES.v.0", 10.0, 2918.0, 2918.0};
}  // namespace

// A conversion is due on a signal feed that holds both contracts' bars with the listed one dated on
// or after its listing date; not before, and not without the predecessor's bar.
TEST(ListingDates, AConversionIsDueFromTheListingDatesSignalBar) {
    Guard guard;
    auto& rule = ListingDates::instance();
    using std::chrono::sys_days;
    using namespace std::chrono_literals;
    rule.set({{"MES", "ES", "2019-05-06", 10.0}});
    auto bar = [](const std::string& symbol, Timestamp ts, double close) {
        Bar b;
        b.symbol = symbol;
        b.timestamp = ts;
        b.close = Decimal(close);
        return b;
    };
    const Timestamp before = sys_days{2019y / 5 / 5} + std::chrono::hours(5);
    const Timestamp on = sys_days{2019y / 5 / 6} + std::chrono::hours(5);
    EXPECT_TRUE(rule.conversions_due({bar("MES.v.0", before, 2895.25), bar("ES.v.0", before, 2895.25)}).empty());
    EXPECT_TRUE(rule.conversions_due({bar("MES.v.0", on, 2918.0), bar("ZN.v.0", on, 123.0)}).empty());
    const auto due = rule.conversions_due(
        {bar("ZN.v.0", on, 123.0), bar("MES.v.0", on, 2918.0), bar("ES.v.0", on, 2918.0)});
    ASSERT_EQ(due.size(), 1u);
    EXPECT_EQ(due[0].from, "ES.v.0");
    EXPECT_EQ(due[0].to, "MES.v.0");
    EXPECT_EQ(due[0].ratio, 10.0);
    EXPECT_EQ(due[0].from_close, 2918.0);
    EXPECT_EQ(due[0].to_close, 2918.0);
    rule.clear();
    EXPECT_TRUE(rule.conversions_due({bar("MES.v.0", on, 2918.0), bar("ES.v.0", on, 2918.0)}).empty())
        << "nothing is ever due with the rule off";
}

// Rule convert, long q: the E-minis are sold to flat and exactly 10q micros bought at the same close;
// the exposure in dollars is the same before and after; each leg is charged on its own terms.
TEST(ListingDates, ConvertALongPositionIsExactlyTenTimesTheMicros) {
    const Timestamp t = std::chrono::system_clock::from_time_t(1557187200);
    const ListingSwitch plan = plan_listing_switch(ListingSwitchRule::kConvert, 10.0, 2.0, 0.0, 13.7, 40.0);
    EXPECT_EQ(plan.close_from, -2.0);
    EXPECT_EQ(plan.trade_to, 20.0);
    EXPECT_EQ(plan.new_to, 20.0);
    const auto legs = make_listing_switch_fills(kConversion, plan, t, "LC-S-0", "LO-S-0", test_cost);
    ASSERT_EQ(legs.size(), 2u);
    EXPECT_EQ(legs[0].symbol, "ES.v.0");
    EXPECT_EQ(legs[0].side, Side::SELL);
    EXPECT_EQ(static_cast<double>(legs[0].filled_quantity), 2.0);
    EXPECT_EQ(static_cast<double>(legs[0].fill_price), 2918.0);
    EXPECT_EQ(legs[0].exec_id, "LC-S-0");
    EXPECT_EQ(legs[1].symbol, "MES.v.0");
    EXPECT_EQ(legs[1].side, Side::BUY);
    EXPECT_EQ(static_cast<double>(legs[1].filled_quantity), 20.0);
    EXPECT_EQ(static_cast<double>(legs[1].fill_price), 2918.0);
    EXPECT_EQ(legs[1].exec_id, "LO-S-0");
    for (const auto& leg : legs) {
        EXPECT_EQ(leg.execution_type, ExecutionType::STRATEGY);
        EXPECT_EQ(leg.fill_time, t);
        EXPECT_FALSE(leg.is_partial);
    }
    EXPECT_DOUBLE_EQ(2.0 * 50.0 * 2918.0, 20.0 * 5.0 * 2918.0);  // the exposure is unchanged
    EXPECT_NEAR(static_cast<double>(legs[0].commissions_fees), 2.0 * 2.247, 1e-9);
    EXPECT_NEAR(static_cast<double>(legs[0].total_transaction_costs), 2.0 * 2.247 + 2.0 * 6.25, 1e-9);
    EXPECT_NEAR(static_cast<double>(legs[1].commissions_fees), 20.0 * 0.614, 1e-9);
    EXPECT_NEAR(static_cast<double>(legs[1].total_transaction_costs), 20.0 * 0.614 + 20.0 * 0.625, 1e-9);
}

// Rule convert, short q: the E-minis are bought back and 10|q| micros sold; a micro already held is kept.
TEST(ListingDates, ConvertAShortPositionIsExactlyTenTimesTheMicrosShort) {
    const Timestamp t = std::chrono::system_clock::from_time_t(1557187200);
    const ListingSwitch plan = plan_listing_switch(ListingSwitchRule::kConvert, 10.0, -1.0, 0.0, -6.0, 40.0);
    EXPECT_EQ(plan.new_to, -10.0);
    const auto legs = make_listing_switch_fills(kConversion, plan, t, "LC-S-3", "LO-S-3", test_cost);
    ASSERT_EQ(legs.size(), 2u);
    EXPECT_EQ(legs[0].side, Side::BUY);
    EXPECT_EQ(static_cast<double>(legs[0].filled_quantity), 1.0);
    EXPECT_EQ(legs[1].side, Side::SELL);
    EXPECT_EQ(static_cast<double>(legs[1].filled_quantity), 10.0);
    EXPECT_GT(static_cast<double>(legs[0].total_transaction_costs), 0.0);
    EXPECT_GT(static_cast<double>(legs[1].total_transaction_costs), 0.0);
    EXPECT_EQ(plan_listing_switch(ListingSwitchRule::kConvert, 10.0, -1.0, 4.0, 0.0, 0.0).new_to, -6.0);
}

// q = 0 under convert: nothing is converted and nothing is priced. A switch without a usable close
// is refused.
TEST(ListingDates, ConvertAFlatPositionDoesNothing) {
    const Timestamp t = std::chrono::system_clock::from_time_t(1557187200);
    int priced = 0;
    const ListingSwitch plan = plan_listing_switch(ListingSwitchRule::kConvert, 10.0, 0.0, 0.0, 2.0, 40.0);
    EXPECT_EQ(plan.close_from, 0.0);
    EXPECT_EQ(plan.trade_to, 0.0);
    const auto legs = make_listing_switch_fills(
        kConversion, plan, t, "LC-S-0", "LO-S-0", [&](const std::string&, double, double) {
            ++priced;
            return ListingLegCost{};
        });
    EXPECT_TRUE(legs.empty());
    EXPECT_EQ(priced, 0);
    ListingConversion bad = kConversion;
    bad.to_close = 0.0;
    EXPECT_THROW(make_listing_switch_fills(bad, {-1.0, 10.0, 10.0}, t, "a", "b", test_cost),
                 std::invalid_argument);
}

// Rule open_at_target: the predecessor is closed and the micro opened at its rounded target whatever
// was held: HD's cases (0.2 of an E-mini against none held; 0.6 against one held; a short; near zero).
TEST(ListingDates, OpenAtTargetIgnoresWhatWasHeld) {
    const auto rule = ListingSwitchRule::kOpenAtTarget;
    ListingSwitch p = plan_listing_switch(rule, 10.0, 0.0, 0.0, 2.0, 40.0);  // target 0.2 E-mini, held 0
    EXPECT_EQ(p.close_from, 0.0);
    EXPECT_EQ(p.trade_to, 2.0);
    p = plan_listing_switch(rule, 10.0, 1.0, 0.0, 6.3, 40.0);  // target 0.63, held 1
    EXPECT_EQ(p.close_from, -1.0);
    EXPECT_EQ(p.new_to, 6.0);
    p = plan_listing_switch(rule, 10.0, 1.0, 0.0, 14.5, 40.0);  // a half rounds away from zero
    EXPECT_EQ(p.new_to, 15.0);
    p = plan_listing_switch(rule, 10.0, -1.0, 0.0, -7.5, 40.0);  // a short
    EXPECT_EQ(p.close_from, 1.0);
    EXPECT_EQ(p.new_to, -8.0);
    p = plan_listing_switch(rule, 10.0, 1.0, 0.0, 0.3, 40.0);  // target near zero, held 1
    EXPECT_EQ(p.close_from, -1.0);
    EXPECT_EQ(p.trade_to, 0.0);
    const Timestamp t = std::chrono::system_clock::from_time_t(1557187200);
    const auto legs = make_listing_switch_fills(kConversion, p, t, "LC-S-0", "LO-S-0", test_cost);
    ASSERT_EQ(legs.size(), 1u) << "only the close is a fill";
    EXPECT_EQ(legs[0].symbol, "ES.v.0");
    p = plan_listing_switch(rule, 10.0, 0.0, 0.0, 55.2, 40.0);  // bounded by the per-name cap
    EXPECT_EQ(p.new_to, 40.0);
    p = plan_listing_switch(rule, 10.0, 0.0, 0.0, 39.8, 39.9);  // rounding never passes the cap
    EXPECT_EQ(p.new_to, 39.0);
}

// Rule close_reenter plans nothing: the engine's close-out and the pass do the switch.
TEST(ListingDates, CloseReenterPlansNothing) {
    const ListingSwitch p = plan_listing_switch(ListingSwitchRule::kCloseReenter, 10.0, 2.0, 0.0, 13.0, 40.0);
    EXPECT_EQ(p.close_from, 0.0);
    EXPECT_EQ(p.trade_to, 0.0);
    ListingSwitchRule rule = ListingSwitchRule::kConvert;
    EXPECT_TRUE(parse_listing_switch_rule("close_reenter", &rule));
    EXPECT_EQ(rule, ListingSwitchRule::kCloseReenter);
    EXPECT_TRUE(parse_listing_switch_rule("open_at_target", &rule));
    EXPECT_TRUE(parse_listing_switch_rule("convert", &rule));
    EXPECT_EQ(rule, ListingSwitchRule::kConvert);
    EXPECT_FALSE(parse_listing_switch_rule("nope", &rule));
}

// Rule carry_to_target: a held pair is one exit of the E-mini and one entry of the micro at its rounded
// target (with a holding it is rule open_at_target exactly); a pair with nothing held is left to the pass.
TEST(ListingDates, CarryToTargetSwitchesAHeldPairToTheTargetAndLeavesAFlatPairAlone) {
    const auto rule = ListingSwitchRule::kCarryToTarget;
    ListingSwitch p = plan_listing_switch(rule, 10.0, 1.0, 0.0, 4.2, 40.0);  // 1 E-mini against 0.42
    EXPECT_EQ(p.close_from, -1.0);
    EXPECT_EQ(p.trade_to, 4.0);
    EXPECT_EQ(p.new_to, 4.0);
    const Timestamp t = std::chrono::system_clock::from_time_t(1557187200);
    const auto legs = make_listing_switch_fills(kConversion, p, t, "LC-S-0", "LO-S-0", test_cost);
    ASSERT_EQ(legs.size(), 2u) << "one exit, one entry";
    EXPECT_EQ(legs[0].symbol, "ES.v.0");
    EXPECT_EQ(static_cast<double>(legs[0].filled_quantity), 1.0);
    EXPECT_EQ(legs[1].symbol, "MES.v.0");
    EXPECT_EQ(legs[1].side, Side::BUY);
    EXPECT_EQ(static_cast<double>(legs[1].filled_quantity), 4.0);
    EXPECT_NEAR(static_cast<double>(legs[1].total_transaction_costs), 4.0 * 0.614 + 4.0 * 0.625, 1e-9);
    // with a holding it is open_at_target, case by case
    const double cases[][3] = {{1.0, 4.2, 40.0}, {-1.0, -7.5, 40.0}, {1.0, 14.5, 40.0}, {1.0, 0.3, 40.0},
                               {1.0, -3.0, 40.0}, {2.0, 55.2, 40.0}};
    for (const auto& c : cases) {
        const ListingSwitch d = plan_listing_switch(rule, 10.0, c[0], 0.0, c[1], c[2]);
        const ListingSwitch o = plan_listing_switch(ListingSwitchRule::kOpenAtTarget, 10.0, c[0], 0.0, c[1], c[2]);
        EXPECT_EQ(d.close_from, o.close_from);
        EXPECT_EQ(d.trade_to, o.trade_to);
        EXPECT_EQ(d.new_to, o.new_to);
    }
    // a short held against a long target: the short E-mini is bought back and the micro bought
    p = plan_listing_switch(rule, 10.0, -1.0, 0.0, 3.0, 40.0);
    EXPECT_EQ(p.close_from, 1.0);
    EXPECT_EQ(p.new_to, 3.0);
    // nothing held: nothing is planned, whatever the target (open_at_target would open 2)
    p = plan_listing_switch(rule, 10.0, 0.0, 0.0, 2.0, 40.0);
    EXPECT_EQ(p.close_from, 0.0);
    EXPECT_EQ(p.trade_to, 0.0);
    EXPECT_EQ(p.new_to, 0.0);
    ListingSwitchRule parsed = ListingSwitchRule::kConvert;
    EXPECT_TRUE(parse_listing_switch_rule("carry_to_target", &parsed));
    EXPECT_EQ(parsed, ListingSwitchRule::kCarryToTarget);
}
// A held predecessor inside the deferral band (against a forecast weaker than the band) is carried,
// not traded to target, under both target rules: the pass then holds it as it holds any such holding.
TEST(ListingDates, AHoldingInsideTheDeferralBandIsCarriedUnderTheTargetRules) {
    for (const auto rule : {ListingSwitchRule::kCarryToTarget, ListingSwitchRule::kOpenAtTarget}) {
        const ListingSwitch banded = plan_listing_switch(rule, 10.0, -1.0, 0.0, 0.24, 40.0, true);
        EXPECT_EQ(banded.close_from, 1.0);
        EXPECT_EQ(banded.new_to, -10.0) << "carried: short 1 E-mini is short 10 micros";
        const ListingSwitch free = plan_listing_switch(rule, 10.0, -1.0, 0.0, 0.24, 40.0, false);
        EXPECT_EQ(free.new_to, 0.0) << "outside the band the target rule applies";
    }
    // nothing held: the band has nothing to hold
    EXPECT_EQ(plan_listing_switch(ListingSwitchRule::kOpenAtTarget, 10.0, 0.0, 0.0, 2.0, 40.0, true).new_to, 2.0);
    EXPECT_EQ(plan_listing_switch(ListingSwitchRule::kCarryToTarget, 10.0, 0.0, 0.0, 2.0, 40.0, true).trade_to, 0.0);
    // the carry itself is unchanged by the flag
    EXPECT_EQ(plan_listing_switch(ListingSwitchRule::kConvert, 10.0, -1.0, 0.0, 0.24, 40.0, true).new_to, -10.0);
}

// A declared vendor relabelling is not a roll: from its date the new id is read as the old one, for
// that symbol only, so the roll tracker sees no change bar (no hold, no legs) on that day, and still
// sees the real roll that follows. Off when nothing is declared.
#include "trade_ngin/data/roll_series.hpp"
TEST(InstrumentIdRelabel, ADeclaredRelabelIsReadAsTheOldIdAndIsNotARoll) {
    Guard guard;
    auto& rule = ListingDates::instance();
    using std::chrono::sys_days;
    using namespace std::chrono_literals;
    auto bar = [](const std::string& symbol, std::chrono::sys_days d, double close, const std::string& id) {
        Bar b;
        b.symbol = symbol;
        b.timestamp = d + std::chrono::hours(0);
        b.close = Decimal(close);
        b.instrument_id = id;
        return b;
    };
    // MES around the real change: Friday 02-20 on the old id, the Sunday bar 02-22 on the new one
    std::vector<Bar> bars = {bar("MES.v.0", 2026y / 2 / 19, 6880.0, "42140878"),
                             bar("MES.v.0", 2026y / 2 / 20, 6924.75, "42140878"),
                             bar("MES.v.0", 2026y / 2 / 22, 6906.5, "42003800"),
                             bar("MES.v.0", 2026y / 2 / 23, 6857.25, "42003800"),
                             bar("MES.v.0", 2026y / 3 / 17, 6700.0, "42003800"),
                             bar("MES.v.0", 2026y / 3 / 18, 6760.0, "42005163"),
                             bar("MES.v.0", 2026y / 3 / 19, 6765.0, "42005163"),
                             bar("ZN.v.0", 2026y / 2 / 22, 112.0, "42003800")};
    const std::vector<Bar> stored = bars;
    auto walk = [](const std::vector<Bar>& series) {
        roll_series::RollTracker tracker;
        std::vector<int> change;
        int confirms = 0;
        for (const auto& b : series) {
            if (b.symbol != "MES.v.0") continue;
            const auto st = tracker.add(b.instrument_id, static_cast<double>(b.close));
            change.push_back(st.change ? 1 : 0);
            confirms += st.confirm ? 1 : 0;
        }
        return std::make_pair(change, confirms);
    };
    // nothing declared: the engine reads two rolls (02-22 and 03-18)
    rule.apply_relabels(bars);
    EXPECT_EQ(bars[2].instrument_id, "42003800") << "off: the ids are untouched";
    EXPECT_FALSE(rule.has_relabels());
    EXPECT_EQ(walk(bars).first, (std::vector<int>{0, 0, 1, 0, 0, 1, 0}));
    EXPECT_EQ(walk(bars).second, 2);
    // declared: one roll, the real one
    rule.set_relabels({{"MES", "2026-02-22", "42140878", "42003800"}});
    rule.apply_relabels(bars);
    EXPECT_EQ(bars[1].instrument_id, "42140878");
    EXPECT_EQ(bars[2].instrument_id, "42140878");
    EXPECT_EQ(bars[4].instrument_id, "42140878");
    EXPECT_EQ(bars[5].instrument_id, "42005163") << "the next id is a real roll and is kept";
    EXPECT_EQ(bars[7].instrument_id, "42003800") << "another symbol with the same id is not touched";
    EXPECT_EQ(walk(bars).first, (std::vector<int>{0, 0, 0, 0, 0, 1, 0}));
    EXPECT_EQ(walk(bars).second, 1);
    for (size_t i = 0; i < bars.size(); ++i) EXPECT_EQ(bars[i].close, stored[i].close);
    // a bar dated before the relabel that carries the new id is not rewritten
    EXPECT_EQ(rule.read_id("MES.v.0", sys_days{2026y / 2 / 21}, "42003800"), "42003800");
    EXPECT_EQ(rule.read_id("MES.v.0", sys_days{2026y / 2 / 22}, "42003800"), "42140878");
    // the id rows the classifier is fed are read the same way
    std::vector<market_data_utils::FuturesInstrumentId> ids = {
        {"MES.v.0", "2026-02-20", "42140878"}, {"MES.v.0", "2026-02-22", "42003800"}, {"MNQ.v.0", "2026-02-22", "42004946"}};
    rule.apply_relabels(ids);
    EXPECT_EQ(ids[1].instrument_id, "42140878");
    EXPECT_EQ(ids[2].instrument_id, "42004946");
    EXPECT_THROW(rule.set_relabels({{"MES", "2026-2-22", "a", "b"}}), std::invalid_argument);
    EXPECT_THROW(rule.set_relabels({{"MES", "2026-02-22", "a", "a"}}), std::invalid_argument);
    EXPECT_THROW(rule.set_relabels({{"MES.v.0", "2026-02-22", "a", "b"}}), std::invalid_argument);
}

// The id rows of a load are read through one function by the backtest and by live: the identity
// with nothing declared, the relabels applied and a predecessor given its listed contract's rows
// otherwise; an error is passed through untouched.
TEST(InstrumentIdRelabel, TheIdRowsAreReadThroughOneFunction) {
    Guard guard;
    auto& rule = ListingDates::instance();
    using Rows = std::vector<market_data_utils::FuturesInstrumentId>;
    const Rows stored = {{"MES.v.0", "2026-02-20", "42140878"}, {"MES.v.0", "2026-02-22", "42003800"}};
    auto view = [](const Result<Rows>& r) {
        std::vector<std::string> out;
        for (const auto& row : r.value()) out.push_back(row.symbol + " " + row.date + " " + row.instrument_id);
        return out;
    };
    EXPECT_EQ(view(rule.read_ids({"MES.v.0"}, Result<Rows>(stored))),
              (std::vector<std::string>{"MES.v.0 2026-02-20 42140878", "MES.v.0 2026-02-22 42003800"}));
    rule.set_relabels({{"MES", "2026-02-22", "42140878", "42003800"}});
    EXPECT_EQ(view(rule.read_ids({"MES.v.0"}, Result<Rows>(stored))),
              (std::vector<std::string>{"MES.v.0 2026-02-20 42140878", "MES.v.0 2026-02-22 42140878"}));
    rule.set({{"MES", "ES", "2019-05-06", 10.0}});
    EXPECT_EQ(view(rule.read_ids({"MES.v.0", "ES.v.0"}, Result<Rows>(stored))),
              (std::vector<std::string>{"MES.v.0 2026-02-20 42140878", "MES.v.0 2026-02-22 42140878",
                                        "ES.v.0 2026-02-20 42140878", "ES.v.0 2026-02-22 42140878"}));
    // a row stored under the predecessor's own symbol is left out: it reads its listed contract's rows
    Rows with_own = stored;
    with_own.push_back({"ES.v.0", "2026-02-21", "999"});
    EXPECT_EQ(view(rule.read_ids({"MES.v.0", "ES.v.0"}, Result<Rows>(with_own))),
              (std::vector<std::string>{"MES.v.0 2026-02-20 42140878", "MES.v.0 2026-02-22 42140878",
                                        "ES.v.0 2026-02-20 42140878", "ES.v.0 2026-02-22 42140878"}));
    const auto failed = rule.read_ids(
        {"MES.v.0"}, make_error<Rows>(ErrorCode::DATABASE_ERROR, "the id query failed", "test"));
    ASSERT_TRUE(failed.is_error());
    EXPECT_NE(std::string(failed.error()->what()).find("the id query failed"), std::string::npos);
}

// The cap on the short side, no cap at all, and the fill as the difference from what is already held.
TEST(ListingDates, OpenAtTargetOnTheShortSideWithoutACapAndFromAHeldListedContract) {
    const auto rule = ListingSwitchRule::kOpenAtTarget;
    EXPECT_EQ(plan_listing_switch(rule, 10.0, 0.0, 0.0, -55.2, 40.0).new_to, -40.0);
    EXPECT_EQ(plan_listing_switch(rule, 10.0, 0.0, 0.0, -39.8, 39.9).new_to, -39.0);
    EXPECT_EQ(plan_listing_switch(rule, 10.0, 0.0, 0.0, 6.3, 0.0).new_to, 6.0) << "a cap of 0 is no cap";
    const ListingSwitch p = plan_listing_switch(rule, 10.0, 1.0, 3.0, 6.3, 40.0);
    EXPECT_EQ(p.new_to, 6.0);
    EXPECT_EQ(p.trade_to, 3.0) << "the fill is the difference from the 3 already held";
    EXPECT_EQ(p.close_from, -1.0);
}

// With the rewrite off a full-size root that has no row of its own is NOT found (the runner refuses
// a predecessor without metadata on this answer); with it on the micro's row answers for it.
TEST(ListingDates, WithTheRewriteOffAFullSizeRootWithoutARowIsNotFound) {
    Guard guard;
    auto& registry = InstrumentRegistry::instance();
    const auto saved = registry.instruments_;
    const bool saved_init = registry.initialized_;
    registry.instruments_.clear();
    FuturesSpec spec;
    spec.root_symbol = "MES";
    spec.exchange = "CME";
    spec.currency = "USD";
    spec.multiplier = 5.0;
    spec.tick_size = 0.25;
    registry.instruments_["MES"] = std::make_shared<FuturesInstrument>("MES", spec);
    registry.initialized_ = true;
    EXPECT_TRUE(registry.has_instrument("ES.v.0")) << "the legacy rewrite";
    registry.set_full_size_remap(false);
    EXPECT_FALSE(registry.has_instrument("ES.v.0"));
    EXPECT_TRUE(registry.has_instrument("MES.v.0"));
    registry.set_full_size_remap(true);
    registry.instruments_ = saved;
    registry.initialized_ = saved_init;
}

// The ratio is a positive whole number, and it is the two contracts' sizes: one predecessor
// contract is exactly `ratio` listed contracts.
TEST(ListingDates, TheRatioIsWholeAndEqualsTheTwoContractsSizes) {
    EXPECT_THROW(ListingDates::validate({{"MES", "ES", "2019-05-06", 2.5}}), std::invalid_argument);
    EXPECT_THROW(ListingDates::validate({{"MES", "ES", "2019-05-06", 0.1}}), std::invalid_argument);
    ListingDates::validate({{"MES", "ES", "2019-05-06", 10.0}});
    const ListedContract mes{"MES", "ES", "2019-05-06", 10.0};
    EXPECT_EQ(ListingDates::ratio_error(mes, 50.0, 5.0), "");
    EXPECT_EQ(ListingDates::ratio_error({"MYM", "YM", "2019-05-06", 10.0}, 5.0, 0.5), "");
    const std::string wrong = ListingDates::ratio_error({"MES", "ES", "2019-05-06", 5.0}, 50.0, 5.0);
    EXPECT_NE(wrong.find("\"ratio\" of MES is 5.000000 but one ES (contract size 50.000000) is 10.000000 MES"),
              std::string::npos)
        << wrong;
    EXPECT_NE(ListingDates::ratio_error(mes, 50.0, 0.0), "") << "a size that is not positive is refused";
    EXPECT_NE(ListingDates::ratio_error(mes, 20.0, 5.0), "");
}

// A declared relabel that rewrites nothing, or whose date is a bar late, is said out loud: one line
// each, on the bars as stored. The four real entries on their real bars say nothing.
TEST(InstrumentIdRelabel, AnEntryThatMatchesNothingOrIsLateIsReported) {
    Guard guard;
    auto& rule = ListingDates::instance();
    using namespace std::chrono_literals;
    auto bar = [](const std::string& symbol, std::chrono::sys_days d, const std::string& id) {
        Bar b;
        b.symbol = symbol;
        b.timestamp = d + std::chrono::hours(0);
        b.close = Decimal(100.0);
        b.instrument_id = id;
        return b;
    };
    const std::vector<Bar> bars = {bar("MES.v.0", 2026y / 2 / 19, "42140878"), bar("MES.v.0", 2026y / 2 / 20, "42140878"),
                                   bar("MES.v.0", 2026y / 2 / 22, "42003800"), bar("MES.v.0", 2026y / 2 / 23, "42003800"),
                                   bar("ZN.v.0", 2026y / 2 / 20, "1"), bar("ZN.v.0", 2026y / 2 / 23, "1")};
    EXPECT_TRUE(rule.relabel_findings(bars).empty()) << "nothing declared";
    rule.set_relabels({{"MES", "2026-02-22", "42140878", "42003800"}});
    EXPECT_TRUE(rule.relabel_findings(bars).empty()) << "the real entry on the real bars";

    rule.set_relabels({{"MES", "2026-02-22", "42140878", "42003801"}});  // a wrong new id
    auto lines = rule.relabel_findings(bars);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0],
              "RELABEL_UNMATCHED instrument_id_relabels entry MES 2026-02-22 42140878 -> 42003801: no bar of "
              "MES dated on or after 2026-02-22 carries id 42003801 in a load that spans the date; the entry "
              "rewrites nothing");

    rule.set_relabels({{"MES", "2026-02-23", "42140878", "42003800"}});  // declared one bar late
    lines = rule.relabel_findings(bars);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0],
              "RELABEL_LATE instrument_id_relabels entry MES 2026-02-23 42140878 -> 42003800: a bar dated "
              "2026-02-22, before the entry's date, already carries id 42003800; the entry's date is late and "
              "the change would be read as a flip");

    // a load that does not span the date (a window that ends before it) says nothing
    rule.set_relabels({{"MES", "2026-02-22", "42140878", "42003801"}});
    EXPECT_TRUE(rule.relabel_findings({bars[0], bars[1]}).empty());
    // a symbol that is not in the load says nothing
    rule.set_relabels({{"M2K", "2026-02-22", "1", "2"}});
    EXPECT_TRUE(rule.relabel_findings(bars).empty());
}
