// Tests for MacroDataLoader: the contracts of load() (forward fill, the
// partial-parse rejection, the all-NaN column drop, the column types and the
// date filters) and of load_single() (the 90-day window and the non-release
// day row, L-24 / L-27 case 6). A fake PostgresDatabase returns hand-built
// Arrow tables, so no database is needed.

#include <gtest/gtest.h>

#include "trade_ngin/regime_detection/macro/macro_data_loader.hpp"

#include <arrow/api.h>

#include <cmath>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace trade_ngin;
using namespace trade_ngin::statistics;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Returns a fixed table for every query and records the last SQL it saw.
class FakeMacroDatabase : public PostgresDatabase {
public:
    FakeMacroDatabase() : PostgresDatabase("fake://macro") {}

    bool is_connected() const override { return connected; }

    Result<std::shared_ptr<arrow::Table>> execute_query(const std::string& query) override {
        last_query = query;
        ++query_count;
        if (fail_query) {
            return make_error<std::shared_ptr<arrow::Table>>(ErrorCode::DATABASE_ERROR,
                                                             "simulated failure");
        }
        return Result<std::shared_ptr<arrow::Table>>(table);
    }

    bool connected = true;
    bool fail_query = false;
    std::shared_ptr<arrow::Table> table;
    std::string last_query;
    int query_count = 0;
};

std::shared_ptr<arrow::Array> string_array(const std::vector<std::optional<std::string>>& v) {
    arrow::StringBuilder b;
    for (const auto& x : v) {
        if (x) {
            EXPECT_TRUE(b.Append(*x).ok());
        } else {
            EXPECT_TRUE(b.AppendNull().ok());
        }
    }
    std::shared_ptr<arrow::Array> out;
    EXPECT_TRUE(b.Finish(&out).ok());
    return out;
}

std::shared_ptr<arrow::Array> double_array(const std::vector<double>& v) {
    arrow::DoubleBuilder b;
    for (double x : v) {
        if (std::isnan(x)) {
            EXPECT_TRUE(b.AppendNull().ok());
        } else {
            EXPECT_TRUE(b.Append(x).ok());
        }
    }
    std::shared_ptr<arrow::Array> out;
    EXPECT_TRUE(b.Finish(&out).ok());
    return out;
}

std::shared_ptr<arrow::Table> make_table(
    const std::vector<std::string>& dates,
    const std::vector<std::pair<std::string, std::shared_ptr<arrow::Array>>>& cols) {
    std::vector<std::shared_ptr<arrow::Field>> fields{arrow::field("date", arrow::utf8())};
    std::vector<std::shared_ptr<arrow::Array>> arrays{
        string_array({dates.begin(), dates.end()})};
    for (const auto& [name, arr] : cols) {
        fields.push_back(arrow::field(name, arr->type()));
        arrays.push_back(arr);
    }
    return arrow::Table::Make(arrow::schema(fields), arrays);
}

// Sets TZ for the lifetime of the guard and restores it after.
class ScopedTZ {
public:
    explicit ScopedTZ(const char* tz) {
        if (const char* old = std::getenv("TZ")) old_ = old;
        setenv("TZ", tz, 1);
        tzset();
    }
    ~ScopedTZ() {
        if (old_) {
            setenv("TZ", old_->c_str(), 1);
        } else {
            unsetenv("TZ");
        }
        tzset();
    }

private:
    std::optional<std::string> old_;
};

}  // namespace

// ---------------------------------------------------------------------------
// load(): error paths
// ---------------------------------------------------------------------------

TEST(MacroDataLoader, NotConnectedIsConnectionError) {
    FakeMacroDatabase db;
    db.connected = false;
    auto r = MacroDataLoader(db).load();
    ASSERT_TRUE(r.is_error());
    EXPECT_EQ(r.error()->code(), ErrorCode::CONNECTION_ERROR);
    EXPECT_EQ(db.query_count, 0);
}

TEST(MacroDataLoader, QueryFailureIsDatabaseError) {
    FakeMacroDatabase db;
    db.fail_query = true;
    auto r = MacroDataLoader(db).load();
    ASSERT_TRUE(r.is_error());
    EXPECT_EQ(r.error()->code(), ErrorCode::DATABASE_ERROR);
}

TEST(MacroDataLoader, EmptyResultIsInvalidArgument) {
    FakeMacroDatabase db;
    db.table = make_table({}, {{"cpi", double_array({})}});
    auto r = MacroDataLoader(db).load();
    ASSERT_TRUE(r.is_error());
    EXPECT_EQ(r.error()->code(), ErrorCode::INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// load(): fill, parse and drop contracts
// ---------------------------------------------------------------------------

TEST(MacroDataLoader, ForwardFillCarriesLastReleaseAndKeepsLeadingNaN) {
    FakeMacroDatabase db;
    db.table = make_table({"2024-01-01", "2024-01-02", "2024-01-03", "2024-01-04", "2024-01-05",
                           "2024-01-06"},
                          {{"cpi", double_array({kNaN, 1.0, kNaN, kNaN, 2.0, kNaN})}});
    auto r = MacroDataLoader(db).load();
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    const auto& p = r.value();
    ASSERT_EQ(p.T, 6);
    ASSERT_EQ(p.N, 1);
    // Before the first release there is nothing to carry: no back-fill.
    EXPECT_TRUE(std::isnan(p.data(0, 0)));
    EXPECT_EQ(p.data(1, 0), 1.0);
    EXPECT_EQ(p.data(2, 0), 1.0);
    EXPECT_EQ(p.data(3, 0), 1.0);
    EXPECT_EQ(p.data(4, 0), 2.0);
    EXPECT_EQ(p.data(5, 0), 2.0);
    EXPECT_EQ(p.dates.front(), "2024-01-01");
    EXPECT_EQ(p.dates.back(), "2024-01-06");
}

TEST(MacroDataLoader, PartialParseIsRejectedNotTruncated) {
    // String columns are parsed with stod; "5.2%" must not become 5.2 and
    // "1e3x" must not become 1000. A rejected cell is missing, so it takes
    // the forward-filled value (or stays NaN when nothing came before).
    FakeMacroDatabase db;
    db.table = make_table({"2024-01-01", "2024-01-02", "2024-01-03", "2024-01-04", "2024-01-05"},
                          {{"vix", string_array({"5.2%", "3.0", "1e3x", "", "4.5"})}});
    auto r = MacroDataLoader(db).load();
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    const auto& p = r.value();
    EXPECT_TRUE(std::isnan(p.data(0, 0)));
    EXPECT_EQ(p.data(1, 0), 3.0);
    EXPECT_EQ(p.data(2, 0), 3.0);
    EXPECT_EQ(p.data(3, 0), 3.0);
    EXPECT_EQ(p.data(4, 0), 4.5);
}

TEST(MacroDataLoader, AllNaNColumnIsDroppedWithItsName) {
    FakeMacroDatabase db;
    db.table = make_table({"2024-01-01", "2024-01-02", "2024-01-03"},
                          {{"cpi", double_array({1.0, 2.0, 3.0})},
                           {"wti_crude", double_array({kNaN, kNaN, kNaN})},
                           {"vix", double_array({10.0, kNaN, 12.0})}});
    auto r = MacroDataLoader(db).load();
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    const auto& p = r.value();
    ASSERT_EQ(p.N, 2);
    ASSERT_EQ(p.data.cols(), 2);
    EXPECT_EQ(p.column_names, (std::vector<std::string>{"cpi", "vix"}));
    EXPECT_EQ(p.data(2, 0), 3.0);
    EXPECT_EQ(p.data(1, 1), 10.0);  // vix forward-filled
    EXPECT_EQ(p.data(2, 1), 12.0);
}

TEST(MacroDataLoader, NumericColumnTypesAreRead) {
    arrow::Int64Builder ib;
    ASSERT_TRUE(ib.AppendValues({100, 200}).ok());
    std::shared_ptr<arrow::Array> ints;
    ASSERT_TRUE(ib.Finish(&ints).ok());
    arrow::FloatBuilder fb;
    ASSERT_TRUE(fb.AppendValues({0.5f, 1.25f}).ok());
    std::shared_ptr<arrow::Array> floats;
    ASSERT_TRUE(fb.Finish(&floats).ok());

    FakeMacroDatabase db;
    db.table = make_table({"2024-01-01", "2024-01-02"},
                          {{"nonfarm_payrolls", ints}, {"tips_10y", floats}});
    auto r = MacroDataLoader(db).load();
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    EXPECT_EQ(r.value().data(1, 0), 200.0);
    EXPECT_EQ(r.value().data(1, 1), 1.25);
}

TEST(MacroDataLoader, Date32DatesAreFormattedAsUtcCalendarDates) {
    // 2024-03-15 is day 19797 since the epoch. The string must not depend
    // on the host time zone.
    arrow::Date32Builder db32;
    ASSERT_TRUE(db32.AppendValues({19797, 19798}).ok());
    std::shared_ptr<arrow::Array> dates;
    ASSERT_TRUE(db32.Finish(&dates).ok());
    auto table = arrow::Table::Make(
        arrow::schema({arrow::field("date", arrow::date32()), arrow::field("cpi", arrow::float64())}),
        {dates, double_array({1.0, 2.0})});

    for (const char* tz : {"UTC", "America/New_York", "Asia/Tokyo"}) {
        ScopedTZ guard(tz);
        FakeMacroDatabase db;
        db.table = table;
        auto r = MacroDataLoader(db).load();
        ASSERT_TRUE(r.is_ok()) << r.error()->what();
        EXPECT_EQ(r.value().dates, (std::vector<std::string>{"2024-03-15", "2024-03-16"}))
            << "TZ=" << tz;
    }
}

TEST(MacroDataLoader, DateFiltersReachTheQuery) {
    FakeMacroDatabase db;
    db.table = make_table({"2020-06-01"}, {{"cpi", double_array({1.0})}});
    MacroDataLoader loader(db);

    ASSERT_TRUE(loader.load().is_ok());
    EXPECT_EQ(db.last_query.find("WHERE"), std::string::npos);

    ASSERT_TRUE(loader.load("2020-01-01", "2020-12-31").is_ok());
    EXPECT_NE(db.last_query.find("WHERE d.date >= '2020-01-01' AND d.date <= '2020-12-31'"),
              std::string::npos)
        << db.last_query;

    ASSERT_TRUE(loader.load("", "2020-12-31").is_ok());
    EXPECT_NE(db.last_query.find("WHERE d.date <= '2020-12-31'"), std::string::npos);
}

// ---------------------------------------------------------------------------
// load_single(): the 90-day window (L-24)
// ---------------------------------------------------------------------------

TEST(MacroDataLoader, LoadSingleWindowIsNinetyCalendarDaysInAnyTimeZone) {
    // 2024-03-15 minus 90 days is 2023-12-16; 2024-05-30 minus 90 days is
    // 2024-03-01 (across the leap day). The old mktime/gmtime arithmetic
    // gave 2023-12-15 east of UTC.
    for (const char* tz : {"UTC", "America/New_York", "Asia/Tokyo", "Pacific/Kiritimati"}) {
        ScopedTZ guard(tz);
        FakeMacroDatabase db;
        db.table = make_table({"2024-03-15"}, {{"cpi", double_array({1.0})}});
        MacroDataLoader loader(db);

        ASSERT_TRUE(loader.load_single("2024-03-15").is_ok());
        EXPECT_NE(db.last_query.find("d.date >= '2023-12-16' AND d.date <= '2024-03-15'"),
                  std::string::npos)
            << "TZ=" << tz << "\n" << db.last_query;

        ASSERT_TRUE(loader.load_single("2024-05-30").is_ok());
        EXPECT_NE(db.last_query.find("d.date >= '2024-03-01' AND d.date <= '2024-05-30'"),
                  std::string::npos)
            << "TZ=" << tz << "\n" << db.last_query;
    }
}

TEST(MacroDataLoader, LoadSingleOnNonReleaseDayReturnsCarriedValues_L27) {
    // L-27 case 6. A monthly series released on 2024-01-01 and a daily
    // series; the requested date is a non-release day for the monthly one.
    // The row must be fully finite and carry the last release.
    FakeMacroDatabase db;
    db.table = make_table(
        {"2024-01-01", "2024-01-02", "2024-01-03", "2024-01-04"},
        {{"cpi", double_array({300.0, kNaN, kNaN, kNaN})},
         {"vix", double_array({12.0, 13.0, 14.0, 15.0})},
         {"gdp", double_array({kNaN, 27000.0, kNaN, kNaN})}});
    auto r = MacroDataLoader(db).load_single("2024-01-03");
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    const auto& [row, names] = r.value();
    ASSERT_EQ(row.size(), 3);
    EXPECT_EQ(names, (std::vector<std::string>{"cpi", "vix", "gdp"}));
    EXPECT_TRUE(row.allFinite());
    EXPECT_EQ(row(0), 300.0);
    EXPECT_EQ(row(1), 14.0);  // the 2024-01-03 row, not the later one
    EXPECT_EQ(row(2), 27000.0);
}

TEST(MacroDataLoader, LoadSingleOnWeekendTakesTheLastEarlierRow) {
    FakeMacroDatabase db;
    db.table = make_table({"2024-01-04", "2024-01-05"},
                          {{"vix", double_array({14.0, 15.0})}});
    auto r = MacroDataLoader(db).load_single("2024-01-06");  // a Saturday
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    EXPECT_EQ(r.value().first(0), 15.0);
}
