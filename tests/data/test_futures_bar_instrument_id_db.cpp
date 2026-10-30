// The futures loader joins each kept bar's vendor instrument id:
// futures_data.ohlcv_1d_raw.instrument_id LEFT JOINed on the exact print, carried through
// the Arrow table into Bar::instrument_id. Read-only on the clone, through TRADE_NGIN_TEST_DSN only.
//
//   * NG.v.0 rolls on 2025-10-27 (id 864 -> 863 at the vendor's switch): the loaded bars carry the
//     ids, the series flags the change bar and confirms it on 10-28, and the adjusted return on the
//     change bar is 0 while the raw splice step is 3.376 -> 3.965.
//   * 6A.v.0 2025-11-05: the kept 76,895-lot print has no raw match (the raw table holds the 196-lot
//     copy the loader drops): its id is empty, and the series is not a change there (not judged).
//   * an equity load carries no instrument_id column: every equity bar's id is empty.
// A reference checker's series comparison reads the backtest's OWN record of the bars it consumed
// (include/trade_ngin/backtest/consumed_series_record.hpp), not a series built here on every loaded
// bar: a JUNK bar or a thin first print the engine withholds is in no consumed sequence.
#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/conversion_utils.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/data/roll_series.hpp"

using namespace trade_ngin;

namespace {

std::string dsn() {
    const char* v = std::getenv("TRADE_NGIN_TEST_DSN");
    return v && *v ? std::string(v) : std::string();
}

Timestamp utc_midnight(int y, int m, int d) {
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}

Timestamp parse_ymd(const std::string& s) {
    return utc_midnight(std::stoi(s.substr(0, 4)), std::stoi(s.substr(5, 2)),
                        std::stoi(s.substr(8, 2)));
}

std::string ymd(const Timestamp& t) {
    const std::time_t tt = std::chrono::system_clock::to_time_t(t);
    std::tm tm{};
    gmtime_r(&tt, &tm);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

}  // namespace

class FuturesBarInstrumentIdTest : public ::testing::Test {
protected:
    void SetUp() override {
        const std::string c = dsn();
        if (c.empty()) GTEST_SKIP() << "TRADE_NGIN_TEST_DSN not set";
        db_ = std::make_shared<PostgresDatabase>(c);
        auto r = db_->connect();
        if (r.is_error()) GTEST_SKIP() << "database unreachable: " << r.error()->what();
    }
    void TearDown() override {
        if (db_) db_->disconnect();
    }

    std::vector<Bar> load(const std::vector<std::string>& symbols, const std::string& start,
                          const std::string& end, AssetClass asset_class = AssetClass::FUTURES) {
        auto table = db_->get_market_data(symbols, parse_ymd(start), parse_ymd(end), asset_class,
                                          DataFrequency::DAILY, "ohlcv");
        EXPECT_TRUE(table.is_ok()) << (table.is_error() ? table.error()->what() : "");
        if (table.is_error()) return {};
        if (asset_class != AssetClass::FUTURES) {
            EXPECT_EQ(table.value()->GetColumnByName("instrument_id"), nullptr)
                << "only the futures query carries the vendor id";
        }
        auto bars = DataConversionUtils::arrow_table_to_bars(table.value());
        EXPECT_TRUE(bars.is_ok()) << (bars.is_error() ? bars.error()->what() : "");
        return bars.is_ok() ? bars.value() : std::vector<Bar>{};
    }

    std::shared_ptr<PostgresDatabase> db_;
};

TEST_F(FuturesBarInstrumentIdTest, TheLoaderCarriesTheKeptPrintsInstrumentIdOnEveryFuturesBar) {
    const auto bars = load({"NG.v.0"}, "2025-10-20", "2025-11-03");
    ASSERT_GE(bars.size(), 10u);
    std::map<std::string, const Bar*> by_date;
    for (const auto& b : bars) by_date[ymd(b.timestamp)] = &b;
    ASSERT_TRUE(by_date.count("2025-10-24") && by_date.count("2025-10-27") &&
                by_date.count("2025-10-28"));
    EXPECT_EQ(by_date.at("2025-10-24")->instrument_id, "864");
    EXPECT_EQ(by_date.at("2025-10-27")->instrument_id, "863") << "the vendor's switch";
    EXPECT_EQ(by_date.at("2025-10-28")->instrument_id, "863");
    for (const auto& b : bars) {
        EXPECT_FALSE(b.instrument_id.empty()) << "NG.v.0 " << ymd(b.timestamp);
    }

    std::vector<double> closes;
    std::vector<std::string> ids;
    for (const auto& b : bars) {
        closes.push_back(static_cast<double>(b.close));
        ids.push_back(b.instrument_id);
    }
    const auto s = roll_series::build_series(closes, ids);
    size_t change_index = 0;
    for (size_t i = 0; i < bars.size(); ++i) {
        if (ymd(bars[i].timestamp) == "2025-10-27") change_index = i;
    }
    ASSERT_GT(change_index, 0u);
    EXPECT_TRUE(s.flags.change[change_index]);
    EXPECT_TRUE(s.flags.confirm[change_index + 1]) << "confirmed on the next consumed bar, 10-28";
    EXPECT_EQ(s.flags.leg_from[change_index + 1], static_cast<int>(change_index) - 1);
    EXPECT_EQ(s.flags.leg_to[change_index + 1], static_cast<int>(change_index));
    EXPECT_DOUBLE_EQ(s.returns[change_index - 1], 0.0) << "the splice step is not a return";
    EXPECT_NEAR(closes[change_index] - closes[change_index - 1], 3.965 - 3.376, 1e-9);
    EXPECT_DOUBLE_EQ(s.adjusted[change_index] - s.adjusted[change_index - 1], 0.0);
    // Every other return is the raw simple return.
    for (size_t t = 1; t < closes.size(); ++t) {
        if (t == change_index) continue;
        EXPECT_DOUBLE_EQ(s.returns[t - 1], (closes[t] - closes[t - 1]) / closes[t - 1]) << t;
    }
}

// A kept print the raw table does not hold has no id and is not judged. A copy of the database may hold
// no such bar (6A 2025-11-05, the raw table's 196-lot copy against the kept 76,895-lot one, can be one
// print there). So the rule is shown twice: on the loader's own SQL (build_futures_bar_query, its raw
// table read from a TEMP copy in an aborted transaction: nothing is written) over a series whose middle
// kept print differs from its raw row; and on the real table, where every 6A bar of the week has its id
// exactly when its raw row is the kept print (derived from the tables, so it holds on any copy).
TEST_F(FuturesBarInstrumentIdTest, AKeptPrintTheRawTableLacksHasNoIdAndIsNotJudged) {
    {
        pqxx::connection c(dsn());
        pqxx::work w(c);
        w.exec("SET LOCAL TimeZone = 'UTC'");
        w.exec("CREATE TEMP TABLE fake_bars (time timestamptz NOT NULL, symbol text NOT NULL, volume "
               "double precision, open double precision, high double precision, low double precision, "
               "close double precision)");
        w.exec("CREATE TEMP TABLE fake_raw (ts_event timestamptz NOT NULL, symbol text NOT NULL, volume "
               "double precision, open double precision, high double precision, low double precision, "
               "close double precision, instrument_id text)");
        const std::vector<std::tuple<std::string, double, double, double>> days = {
            {"2025-11-03", 59844, 0.6541, 59844}, {"2025-11-04", 78356, 0.6487, 78356},
            {"2025-11-05", 76895, 0.6512, 196},  // the raw table holds another print
            {"2025-11-06", 69067, 0.6486, 69067}, {"2025-11-07", 66120, 0.6497, 66120}};
        for (const auto& [d, v, close, raw_v] : days) {
            w.exec("INSERT INTO fake_bars VALUES ($1::date, 'XA.v.0', $2, $3, $3, $3, $3)",
                   pqxx::params{d, v, close});
            w.exec("INSERT INTO fake_raw VALUES ($1::date, 'XA.v.0', $2, $3, $3, $3, $3, '7103')",
                   pqxx::params{d, raw_v, raw_v == v ? close : 0.65095});
        }
        std::string q = market_data_utils::build_futures_bar_query("fake_bars", true);
        const std::string raw = market_data_utils::kFuturesRawBarTable;
        ASSERT_NE(q.find(raw), std::string::npos);
        q.replace(q.find(raw), raw.size(), "fake_raw");
        const auto rows = w.exec(q, pqxx::params{"2025-11-01", "2025-11-30", std::vector<std::string>{"XA.v.0"}});
        ASSERT_EQ(rows.size(), 5u);
        std::vector<std::string> ids;
        std::vector<double> closes;
        for (const auto& r : rows) {
            ids.push_back(r["instrument_id"].is_null() ? std::string() : r["instrument_id"].as<std::string>());
            closes.push_back(r["close"].as<double>());
        }
        EXPECT_EQ(ids, (std::vector<std::string>{"7103", "7103", "", "7103", "7103"}));
        const auto s = roll_series::build_series(closes, ids);
        for (size_t t = 0; t < 5; ++t) EXPECT_FALSE(s.flags.change[t]) << t;
        EXPECT_EQ(s.adjusted, closes);
    }
    // The real table: 6A's week, an id exactly where the raw row is the kept print.
    const auto bars = load({"6A.v.0"}, "2025-11-03", "2025-11-07");
    ASSERT_EQ(bars.size(), 5u);
    pqxx::connection c(dsn());
    pqxx::read_transaction t(c);
    std::vector<double> closes;
    std::vector<std::string> ids;
    for (const auto& b : bars) {
        closes.push_back(static_cast<double>(b.close));
        ids.push_back(b.instrument_id);
    }
    const auto same = t.exec(
        "SELECT (r.volume = k.volume AND r.close = k.close) AS same, r.instrument_id FROM ("
        "SELECT DISTINCT ON (time) time, volume, close FROM futures_data.ohlcv_1d WHERE symbol = '6A.v.0' "
        "AND time >= '2025-11-03 00:00:00+00' AND time < '2025-11-08 00:00:00+00' ORDER BY time, volume DESC, close, open, high, low) k "
        "LEFT JOIN futures_data.ohlcv_1d_raw r ON r.symbol = '6A.v.0' AND r.ts_event = k.time ORDER BY k.time");
    ASSERT_EQ(same.size(), 5u);
    for (size_t i = 0; i < 5; ++i) {
        const bool is_same = !same[i][0].is_null() && same[i][0].as<bool>();
        EXPECT_EQ(ids[i], is_same ? same[i][1].as<std::string>() : std::string()) << i;
    }
    const auto s = roll_series::build_series(closes, ids);
    for (size_t i = 0; i < 5; ++i) EXPECT_FALSE(s.flags.change[i]) << i;
    EXPECT_EQ(s.adjusted, closes);
}

TEST_F(FuturesBarInstrumentIdTest, AnEquityBarHasNoInstrumentId) {
    auto symbols = db_->get_symbols(AssetClass::EQUITIES);
    if (symbols.is_error() || symbols.value().empty()) GTEST_SKIP() << "no equity symbols";
    const auto bars = load({symbols.value().front()}, "2026-06-01", "2026-06-30",
                           AssetClass::EQUITIES);
    for (const auto& b : bars) EXPECT_TRUE(b.instrument_id.empty());
}
