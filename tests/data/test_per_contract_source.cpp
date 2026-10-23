// The per-contract bar source (rolls step 3, per_contract_source.hpp): what each grid date hands
// out, and that no bar depends on a later bar, on hand-built GC fixtures (the shipped calendar), then
// on the clone (TRADE_NGIN_TEST_DSN, read only): the loader's own call with the switch on and off.
// GCQ6: D* = Fri 2026-07-31, window Fri 07-24 to the LAB Wed 07-29.
#include <gtest/gtest.h>

#include <map>

#include "contract_rolls_test_util.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/data/conversion_utils.hpp"
#include "trade_ngin/data/per_contract_source.hpp"
#include "trade_ngin/data/postgres_database.hpp"

using namespace trade_ngin;
using namespace trade_ngin::contract_rolls;
using contract_rolls_test::bar;
using contract_rolls_test::d;

namespace {

struct Spec {
    const char* raw;
    const char* id;
    const char* expiration;
};

const std::vector<Spec> kSpecs = {{"GCQ6", "q", "2026-08-27"},
                                  {"GCU6", "u", "2026-09-28"},
                                  {"GCZ6", "z", "2026-12-29"},
                                  {"GCG7", "g", "2027-02-24"}};

const std::vector<std::string> kDates = {
    "2026-07-15", "2026-07-16", "2026-07-17", "2026-07-19", "2026-07-20", "2026-07-21",
    "2026-07-22", "2026-07-23", "2026-07-24", "2026-07-26", "2026-07-27", "2026-07-28",
    "2026-07-29", "2026-07-30", "2026-07-31", "2026-08-02", "2026-08-03", "2026-08-04",
    "2026-08-05"};

/// Contract rows and a vendor grid, run through the rule up to a date (rows and grid both cut there,
/// as build_held_series reads them).
class World {
public:
    World& print(const std::string& raw, const std::string& date, int64_t volume, double close) {
        for (const auto& s : kSpecs) {
            if (raw == s.raw) rows_.push_back(bar("GC", raw, s.id, date, s.expiration, close, volume));
        }
        return *this;
    }
    World& vendor(const std::string& date, const std::string& raw) {
        VendorBar v;
        v.date = d(date);
        v.symbol = "GC.v.0";
        for (const auto& s : kSpecs) {
            if (raw == s.raw) v.instrument_id = s.id;
        }
        v.open = v.high = v.low = v.close = 1.0;
        v.volume = 1;
        grid_.push_back(v);
        return *this;
    }
    HeldSeries run_until(const std::string& last) {
        std::vector<ContractBar> rows;
        for (const auto& r : rows_) {
            if (r.date <= d(last)) rows.push_back(r);
        }
        RuleInputs in;
        in.root = "GC";
        for (const auto& g : grid_) {
            if (g.date <= d(last)) in.grid.push_back(g);
        }
        book_ = std::make_unique<ContractBook>(ContractBook::build(rows).value());
        cal_ = std::make_unique<TradingCalendar>(contract_rolls_test::shipped_calendar("GC"));
        in.book = book_.get();
        in.cal = cal_.get();
        in.cycle = roll_cycle("GC").value();
        auto r = apply_roll_rule(in);
        EXPECT_TRUE(r.is_ok()) << r.error()->what();
        return r.value();
    }
    std::map<Day, SourceBar> bars_until(const std::string& last) {
        const HeldSeries s = run_until(last);
        std::map<Day, std::string> window;
        for (const auto& day : s.days) window[day.date] = ymd(day.date) + " 00:00:00+00";
        auto bars = bars_of_series(s, window);
        EXPECT_TRUE(bars.is_ok()) << bars.error()->what();
        std::map<Day, SourceBar> out;
        for (const auto& b : bars.value()) out[b.date] = b;
        return out;
    }

private:
    std::vector<ContractBar> rows_;
    std::vector<VendorBar> grid_;
    std::unique_ptr<ContractBook> book_;
    std::unique_ptr<TradingCalendar> cal_;
};

double close_of(const std::string& raw, const std::string& date) {
    // distinct closes per contract and date, so a bar read from the wrong contract or day shows
    const int day = std::stoi(date.substr(8, 2));
    if (raw == "GCQ6") return 2000.0 + day;
    if (raw == "GCU6") return 2100.0 + day;
    if (raw == "GCZ6") return 2200.0 + day;
    return 2300.0 + day;
}

/// The vendor's shapes: a one-bar forward stray (Thu 07-16), a confirmed switch to Z (Mon 07-20), a
/// two-bar visit back to Q (07-23, 07-24) and a switch on the last bar (08-05) nothing can confirm.
/// U is off the cycle and out-trades Z, so a source that ignored the cycle would read it.
World shapes_world() {
    World w;
    for (const auto& dt : kDates) {
        w.print("GCQ6", dt, 80000, close_of("GCQ6", dt));
        w.print("GCU6", dt, 30000, close_of("GCU6", dt));
        w.print("GCZ6", dt, 9000, close_of("GCZ6", dt));
        w.print("GCG7", dt, 500, close_of("GCG7", dt));
        std::string v = dt >= "2026-07-20" ? "GCZ6" : "GCQ6";
        if (dt == "2026-07-16") v = "GCZ6";
        if (dt == "2026-07-23" || dt == "2026-07-24") v = "GCQ6";
        if (dt == "2026-08-05") v = "GCG7";
        w.vendor(dt, v);
    }
    return w;
}

/// No vendor switch; Z out-trades Q from Mon 07-27, inside GCQ6's window: a crossover.
World crossover_world() {
    World w;
    for (const auto& dt : kDates) {
        const bool z_leads = dt >= "2026-07-27";
        w.print("GCQ6", dt, z_leads ? 9000 : 80000, close_of("GCQ6", dt));
        w.print("GCZ6", dt, z_leads ? 80000 : 9000, close_of("GCZ6", dt));
        w.vendor(dt, "GCQ6");
    }
    return w;
}

bool same(const SourceBar& a, const SourceBar& b) {
    return a.date == b.date && a.symbol == b.symbol && a.open == b.open && a.high == b.high &&
           a.low == b.low && a.close == b.close && a.volume == b.volume &&
           a.instrument_id == b.instrument_id && a.roll_from == b.roll_from &&
           a.time_text == b.time_text;
}

}  // namespace

TEST(PerContractSource, EachGridDateIsTheHeldContractsBarAndARollBarNamesTheContractLeft) {
    auto bars = shapes_world().bars_until("2026-08-05");
    ASSERT_EQ(bars.size(), kDates.size());
    // An ordinary day: the held contract's own bar and id, no roll field.
    const SourceBar& b15 = bars.at(d("2026-07-15"));
    EXPECT_EQ(b15.symbol, "GC.v.0");
    EXPECT_EQ(b15.instrument_id, "q");
    EXPECT_EQ(b15.close, close_of("GCQ6", "2026-07-15"));
    EXPECT_EQ(b15.volume, 80000.0);
    EXPECT_FALSE(b15.roll_from.has_value());
    // The one-bar stray: handed out as the switch it would have been (Z's bar, Q's close, awaiting).
    const SourceBar& b16 = bars.at(d("2026-07-16"));
    EXPECT_EQ(b16.instrument_id, "z");
    EXPECT_EQ(b16.close, close_of("GCZ6", "2026-07-16"));
    ASSERT_TRUE(b16.roll_from.has_value());
    EXPECT_EQ(b16.roll_from->close, close_of("GCQ6", "2026-07-16"));
    EXPECT_EQ(b16.roll_from->instrument_id, "q");
    EXPECT_EQ(b16.roll_from->kind, RollFromKind::AWAITS_NEXT);
    // The next bar shows it reverted: Q's own bar.
    EXPECT_EQ(bars.at(d("2026-07-17")).instrument_id, "q");
    EXPECT_FALSE(bars.at(d("2026-07-17")).roll_from.has_value());
    // The confirmed switch of Mon 07-20: the same shape as the stray, the next bar keeps Z.
    const SourceBar& b20 = bars.at(d("2026-07-20"));
    EXPECT_EQ(b20.instrument_id, "z");
    ASSERT_TRUE(b20.roll_from.has_value());
    EXPECT_EQ(b20.roll_from->close, close_of("GCQ6", "2026-07-20"));
    EXPECT_EQ(b20.roll_from->kind, RollFromKind::AWAITS_NEXT);
    // The bar after it is Z's own, carrying Q's close of that date for a consumer that withheld the
    // roll bar.
    const SourceBar& b21 = bars.at(d("2026-07-21"));
    EXPECT_EQ(b21.instrument_id, "z");
    EXPECT_EQ(b21.close, close_of("GCZ6", "2026-07-21"));
    ASSERT_TRUE(b21.roll_from.has_value());
    EXPECT_EQ(b21.roll_from->kind, RollFromKind::AFTER_ROLL);
    EXPECT_EQ(b21.roll_from->close, close_of("GCQ6", "2026-07-21"));
    EXPECT_EQ(b21.roll_from->instrument_id, "q");
    EXPECT_FALSE(bars.at(d("2026-07-22")).roll_from.has_value());
    // The vendor's backward visit to Q: ignored, Z's own bar.
    EXPECT_EQ(bars.at(d("2026-07-23")).instrument_id, "z");
    EXPECT_EQ(bars.at(d("2026-07-23")).close, close_of("GCZ6", "2026-07-23"));
    EXPECT_FALSE(bars.at(d("2026-07-23")).roll_from.has_value());
    // The last bar names G7: unconfirmable, handed out as the switch.
    const SourceBar& last = bars.at(d("2026-08-05"));
    EXPECT_EQ(last.instrument_id, "g");
    ASSERT_TRUE(last.roll_from.has_value());
    EXPECT_EQ(last.roll_from->instrument_id, "z");
    EXPECT_EQ(last.roll_from->kind, RollFromKind::AWAITS_NEXT);
}

TEST(PerContractSource, ACrossoverRollIsKnownOnItsOwnBar) {
    auto bars = crossover_world().bars_until("2026-08-05");
    const SourceBar& roll = bars.at(d("2026-07-27"));
    EXPECT_EQ(roll.instrument_id, "z");
    EXPECT_EQ(roll.close, close_of("GCZ6", "2026-07-27"));
    ASSERT_TRUE(roll.roll_from.has_value());
    EXPECT_EQ(roll.roll_from->close, close_of("GCQ6", "2026-07-27"));
    EXPECT_EQ(roll.roll_from->instrument_id, "q");
    EXPECT_EQ(roll.roll_from->kind, RollFromKind::KNOWN);
    EXPECT_EQ(bars.at(d("2026-07-24")).instrument_id, "q");
    EXPECT_EQ(bars.at(d("2026-07-28")).instrument_id, "z");
    ASSERT_TRUE(bars.at(d("2026-07-28")).roll_from.has_value());
    EXPECT_EQ(bars.at(d("2026-07-28")).roll_from->kind, RollFromKind::AFTER_ROLL);
    EXPECT_EQ(bars.at(d("2026-07-28")).roll_from->close, close_of("GCQ6", "2026-07-28"));
}

// The no-look-ahead property: the bar handed out for a date is the same whatever window ends at or
// after it, so a backtest's single load and a chain of dated runs read the same bars.
TEST(PerContractSource, NoBarDependsOnALaterBar) {
    World shapes = shapes_world();
    World crossover = crossover_world();
    for (World* w : {&shapes, &crossover}) {
        const auto full = w->bars_until(kDates.back());
        for (const auto& last : kDates) {
            const auto prefix = w->bars_until(last);
            ASSERT_EQ(prefix.size(), static_cast<size_t>(std::count_if(
                                         kDates.begin(), kDates.end(),
                                         [&](const std::string& x) { return x <= last; })));
            for (const auto& [date, b] : prefix) {
                EXPECT_TRUE(same(b, full.at(date)))
                    << "the bar of " << ymd(date) << " in the window ending " << last
                    << " differs from the full window's";
            }
        }
    }
}

TEST(PerContractSource, AFallbackRollReadsThePreviousCloseTimesTheNewContractsMove) {
    HeldSeries s;
    s.root = "GC";
    HeldDay a;
    a.date = d("2026-07-28");
    a.held = ContractKey::make("GC", "GCQ6", d("2026-08-27")).value();
    a.bar_id = "q";
    a.open = a.high = a.low = a.close = 2000.0;
    HeldDay b;
    b.date = d("2026-07-29");
    b.held = ContractKey::make("GC", "GCZ6", d("2026-12-29")).value();
    b.bar_id = "z";
    b.open = b.high = b.low = b.close = 2210.0;
    b.roll = true;
    b.old = a.held;
    b.fallback = RollFallback::NEW_CONTRACT_MOVE;
    b.fallback_new_previous_close = 2200.0;
    s.days = {a, b};
    std::map<Day, std::string> window{{a.date, "2026-07-28 00:00:00+00"},
                                      {b.date, "2026-07-29 00:00:00+00"}};
    auto bars = bars_of_series(s, window);
    ASSERT_TRUE(bars.is_ok()) << bars.error()->what();
    const SourceBar& roll = bars.value().back();
    ASSERT_TRUE(roll.roll_from.has_value());
    EXPECT_DOUBLE_EQ(roll.roll_from->close, 2000.0 * 2210.0 / 2200.0);
    EXPECT_EQ(roll.roll_from->instrument_id, "q");
    EXPECT_EQ(roll.roll_from->kind, RollFromKind::KNOWN);
    // A zero fallback: the old contract's close is read as unchanged.
    s.days[1].fallback = RollFallback::ZERO;
    s.days[1].fallback_new_previous_close.reset();
    bars = bars_of_series(s, window);
    ASSERT_TRUE(bars.is_ok());
    EXPECT_EQ(bars.value().back().roll_from->close, 2000.0);
    // Refused: a window date the series lacks, and a carried day with nothing to carry.
    window[d("2026-07-30")] = "2026-07-30 00:00:00+00";
    EXPECT_TRUE(bars_of_series(s, window).is_error());
    window.erase(d("2026-07-30"));
    s.days[0].has_close = false;
    EXPECT_TRUE(bars_of_series(s, window).is_error());
}

TEST(PerContractSource, TheSwitchTurnsTheSourceOnAndOffAndTheMicrosStayOnTheVendorsSeries) {
    auto& source = PerContractSource::instance();
    source.disable();
    EXPECT_FALSE(source.enabled());
    source.enable(contract_rolls_test::shipped_calendars());
    EXPECT_TRUE(source.enabled());
    EXPECT_TRUE(source.calendar().for_root("GC").is_ok());
    EXPECT_TRUE(source.first_micros_warning());
    EXPECT_FALSE(source.first_micros_warning());
    source.disable();
    EXPECT_FALSE(source.enabled());
    EXPECT_TRUE(configure_per_contract_source(false).is_ok());
    EXPECT_FALSE(source.enabled());
    for (const char* micro : {"MES", "MNQ", "MYM", "M2K"}) EXPECT_TRUE(reads_vendor_series(micro));
    for (const char* root : {"GC", "ES", "ZN", "MBT"}) EXPECT_FALSE(reads_vendor_series(root));
}

// ------------------------------------------------------------------------------------------------
// On the clone: PostgresDatabase::get_market_data with the switch on and off.
// ------------------------------------------------------------------------------------------------

namespace {

class PerContractSourceDbTest : public ::testing::Test {
protected:
    void SetUp() override {
        const std::string dsn = contract_rolls_test::dsn();
        if (dsn.empty()) GTEST_SKIP() << "TRADE_NGIN_TEST_DSN not set";
        db_ = std::make_shared<PostgresDatabase>(dsn);
        if (db_->connect().is_error() || !db_->is_connected()) GTEST_SKIP() << "database unreachable";
        pqxx::connection c(dsn);
        pqxx::read_transaction t(c);
        if (t.exec("SELECT count(*) FROM futures_data.contract_ohlcv_1d WHERE root = 'GC' AND "
                   "expiration IS NULL")[0][0]
                .as<int64_t>() > 0) {
            GTEST_SKIP() << "the clone's expiration is not filled (fill_expiration.py)";
        }
    }
    void TearDown() override {
        PerContractSource::instance().disable();
        if (db_ && db_->is_connected()) db_->disconnect();
    }
    std::vector<Bar> load(const std::vector<std::string>& symbols, const std::string& from,
                          const std::string& to) {
        auto ts = [](const std::string& s) {
            return std::chrono::system_clock::time_point(std::chrono::sys_days(d(s)));
        };
        auto table = db_->get_market_data(symbols, ts(from), ts(to), AssetClass::FUTURES);
        EXPECT_TRUE(table.is_ok()) << table.error()->what();
        auto bars = DataConversionUtils::arrow_table_to_bars(table.value());
        EXPECT_TRUE(bars.is_ok()) << bars.error()->what();
        return bars.value();
    }
    std::shared_ptr<PostgresDatabase> db_;
};

std::string key_of(const Bar& b) {
    return b.symbol + "|" + std::to_string(std::chrono::system_clock::to_time_t(b.timestamp));
}

std::map<std::string, const Bar*> by_key(const std::vector<Bar>& bars) {
    std::map<std::string, const Bar*> out;
    for (const auto& b : bars) out[key_of(b)] = &b;
    return out;
}

}  // namespace

TEST_F(PerContractSourceDbTest, TheGcExampleOnTheLoadersOwnCall) {
    PerContractSource::instance().disable();
    const auto vendor = load({"GC.v.0", "MES.v.0"}, "2026-07-01", "2026-08-31");
    PerContractSource::instance().enable(contract_rolls_test::shipped_calendars());
    const auto ours = load({"GC.v.0", "MES.v.0"}, "2026-07-01", "2026-08-31");
    // The grid stays the vendor's: the same symbols and instants.
    const auto v = by_key(vendor);
    const auto o = by_key(ours);
    ASSERT_EQ(v.size(), o.size());
    for (const auto& [key, bar] : v) ASSERT_TRUE(o.count(key)) << key;
    for (const auto& b : vendor) EXPECT_FALSE(b.roll_from.has_value());
    int rolls = 0;
    for (const auto& b : ours) {
        if (b.symbol == "MES.v.0") {
            // the micros stay on the vendor's bars
            const Bar* vb = v.at(key_of(b));
            EXPECT_EQ(b.close, vb->close);
            EXPECT_EQ(b.instrument_id, vb->instrument_id);
            EXPECT_FALSE(b.roll_from.has_value());
            continue;
        }
        if (!b.roll_from) continue;
        // a vendor switch only the next bar confirms is handed out awaiting it, and the bar after a
        // roll carries the contract left
        if (b.roll_from->kind != RollFromKind::KNOWN) continue;
        ++rolls;
        // R2's GC example: GCQ6 to GCZ6 on the crossover of Wed 2026-07-29, known on the bar
        EXPECT_EQ(core::format_utc_date(b.timestamp), "2026-07-29");
        EXPECT_NE(b.roll_from->instrument_id, b.instrument_id);
    }
    EXPECT_EQ(rolls, 1);
    // A window that starts later reads the same bars on the dates both hold.
    const auto later_bars = load({"GC.v.0"}, "2026-07-25", "2026-08-31");
    const auto later = by_key(later_bars);
    for (const auto& [key, bar] : later) {
        const Bar* full = o.at(key);
        EXPECT_EQ(bar->close, full->close) << key;
        EXPECT_EQ(bar->instrument_id, full->instrument_id) << key;
        EXPECT_EQ(bar->roll_from.has_value(), full->roll_from.has_value()) << key;
    }
    // The classifier's id rows are the bars' ids.
    const auto ts = [](const std::string& s) {
        return std::chrono::system_clock::time_point(std::chrono::sys_days(d(s)));
    };
    auto ids = db_->get_futures_instrument_ids({"GC.v.0"}, ts("2026-07-01"), ts("2026-08-31"));
    ASSERT_TRUE(ids.is_ok()) << ids.error()->what();
    size_t gc_bars = 0;
    for (const auto& b : ours) gc_bars += b.symbol == "GC.v.0";
    ASSERT_EQ(ids.value().size(), gc_bars);
    for (const auto& row : ids.value()) {
        bool found = false;
        for (const auto& b : ours) {
            if (b.symbol == row.symbol && core::format_utc_date(b.timestamp) == row.date) {
                EXPECT_EQ(b.instrument_id, row.instrument_id) << row.date;
                found = true;
            }
        }
        EXPECT_TRUE(found) << row.date;
    }
}
