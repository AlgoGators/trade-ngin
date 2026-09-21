// The futures bar loader returns ONE bar per (symbol, time).
//
// futures_data.ohlcv_1d has no key. It stores 970 extra rows in 306 symbol-dates, all between
// 2025-10-06 and 2026-02-05 (every symbol 9 times on 2026-02-03 and 02-04). The loader used to
// return every copy, and the trend sleeves append every copy as a new day, so each one was a
// zero return inside their EMAs, volatility and the optimiser's covariance. One group
// disagrees: 6A.v.0 2025-11-05, close 0.6512 on volume 76,895 (the front contract) against
// close 0.65095 on volume 196 (a one-day wrong-instrument bar).
//
// The rule (HD 2026-09-21): per (symbol, time) keep the copy with the highest volume; ties keep
// the first by (close, open, high, low), so the pick never depends on physical row order. The
// result comes back ORDER BY time, symbol, as before; a clean series comes back unchanged.
//
// Two layers are tested, both through TRADE_NGIN_TEST_DSN only:
//   * the loader's own SQL (market_data_utils::build_futures_bar_query and the companion
//     build_futures_duplicate_copies_query) run over a TEMP table this test fills with fake
//     rows in a chosen physical order. A temp table lives only in this session and is dropped
//     with the aborted transaction: nothing in the database is written;
//   * PostgresDatabase::get_market_data itself, reading the real futures table read-only, to
//     show the loader runs that SQL and logs what it dropped. Its expectations are computed
//     from the table, so the test stays true once the table is de-duplicated.
//
// Reachability gate as in test_get_trading_days_scope_db.cpp:
//   * TRADE_NGIN_REQUIRE_DB=1 -- an unreachable database FAILS rather than skips.
//   * unset -- skip (local dev without a server).

#include <gtest/gtest.h>
#include <pqxx/pqxx>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/market_data_utils.hpp"
#include "trade_ngin/data/postgres_database.hpp"

using namespace trade_ngin;
namespace mdu = trade_ngin::market_data_utils;

namespace {

std::string discover_connection_string() {
    const char* dsn = std::getenv("TRADE_NGIN_TEST_DSN");
    if (dsn && *dsn) return std::string(dsn);
    return {};
}

struct Row {
    std::string time;
    std::string symbol;
    double open, high, low, close, volume;
    bool operator==(const Row& o) const {
        return time == o.time && symbol == o.symbol && open == o.open && high == o.high &&
               low == o.low && close == o.close && volume == o.volume;
    }
};

std::ostream& operator<<(std::ostream& os, const Row& r) {
    return os << r.time << " " << r.symbol << " o=" << r.open << " h=" << r.high
              << " l=" << r.low << " c=" << r.close << " v=" << r.volume;
}

std::vector<Row> to_rows(const pqxx::result& res) {
    std::vector<Row> out;
    for (const auto& r : res) {
        out.push_back({r["time"].as<std::string>(), r["symbol"].as<std::string>(),
                       r["open"].as<double>(), r["high"].as<double>(), r["low"].as<double>(),
                       r["close"].as<double>(), r["volume"].as<double>()});
    }
    return out;
}

std::vector<mdu::FuturesBarCopy> to_copies(const pqxx::result& res) {
    std::vector<mdu::FuturesBarCopy> out;
    for (const auto& r : res) {
        mdu::FuturesBarCopy c;
        c.symbol = r["symbol"].as<std::string>();
        c.date = r["time"].as<std::string>().substr(0, 10);
        c.open = r["open"].as<double>();
        c.high = r["high"].as<double>();
        c.low = r["low"].as<double>();
        c.close = r["close"].as<double>();
        c.volume = r["volume"].as<double>();
        out.push_back(c);
    }
    return out;
}

// One fake stored row: (date, symbol, volume, open, high, low, close).
struct Fake {
    const char* date;
    const char* symbol;
    long volume;
    double open, high, low, close;
};

Timestamp utc_midnight(int y, int m, int d) {
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}

size_t count_of(const std::string& text, const std::string& needle) {
    size_t n = 0;
    for (size_t p = text.find(needle); p != std::string::npos; p = text.find(needle, p + 1)) ++n;
    return n;
}

// The query the loader ran at 817c0b68, verbatim, for the no-symbol and symbol shapes.
std::string previous_query(const std::string& table, bool with_symbols) {
    return "SELECT time, symbol, open, high, low, close, volume FROM " + table +
           (with_symbols ? " WHERE time BETWEEN $1 AND $2 AND symbol = ANY($3)"
                           " ORDER BY time, symbol"
                         : " WHERE time BETWEEN $1 AND $2 ORDER BY time, symbol");
}

}  // namespace

class FuturesBarDedupTest : public ::testing::Test {
protected:
    void SetUp() override {
        const bool require_db = [] {
            const char* v = std::getenv("TRADE_NGIN_REQUIRE_DB");
            return v && std::string(v) == "1";
        }();
        conn_ = discover_connection_string();
        if (conn_.empty()) {
            if (require_db) FAIL() << "TRADE_NGIN_REQUIRE_DB=1 but TRADE_NGIN_TEST_DSN is not set";
            GTEST_SKIP() << "TRADE_NGIN_TEST_DSN not set";
        }
        try {
            c_ = std::make_unique<pqxx::connection>(conn_);
        } catch (const std::exception& e) {
            if (require_db) FAIL() << "TRADE_NGIN_REQUIRE_DB=1 but the database is unreachable: "
                                   << e.what();
            GTEST_SKIP() << "database unreachable";
        }
    }

    // A temp table shaped like futures_data.ohlcv_1d, filled in the given physical order.
    static void fill(pqxx::work& w, const std::vector<Fake>& rows) {
        w.exec("CREATE TEMP TABLE fake_bars (time timestamptz NOT NULL, symbol text NOT NULL, "
               "volume integer NOT NULL, open double precision, high double precision, "
               "low double precision, close double precision) ON COMMIT DROP");
        w.exec("SET LOCAL TimeZone = 'UTC'");
        for (const auto& r : rows) {
            w.exec("INSERT INTO fake_bars (time, symbol, volume, open, high, low, close) VALUES "
                   "($1::timestamptz, $2, $3, $4, $5, $6, $7)",
                   pqxx::params{std::string(r.date) + " 00:00:00+00", std::string(r.symbol),
                                r.volume, r.open, r.high, r.low, r.close});
        }
    }

    static pqxx::result bars(pqxx::work& w, const std::vector<std::string>& symbols = {}) {
        const std::string q = mdu::build_futures_bar_query("fake_bars", !symbols.empty());
        return symbols.empty() ? w.exec(q, pqxx::params{"2025-01-01", "2026-12-31"})
                               : w.exec(q, pqxx::params{"2025-01-01", "2026-12-31", symbols});
    }

    static mdu::FuturesBarDedupReport report(pqxx::work& w) {
        return mdu::summarise_futures_bar_duplicates(to_copies(
            w.exec(mdu::build_futures_duplicate_copies_query("fake_bars", false),
                   pqxx::params{"2025-01-01", "2026-12-31"})));
    }

    std::string conn_;
    std::unique_ptr<pqxx::connection> c_;
};

// Nine field-identical copies of one bar (MES.v.0 2026-02-03 has exactly this) are ONE bar.
TEST_F(FuturesBarDedupTest, NineIdenticalCopiesReturnOneBar) {
    pqxx::work w(*c_);
    std::vector<Fake> rows = {{"2026-02-02", "MES.v.0", 1500000, 6950.0, 7010.0, 6940.0, 7000.0}};
    for (int i = 0; i < 9; ++i) {
        rows.push_back({"2026-02-03", "MES.v.0", 1600000, 7000.0, 7050.0, 6980.0, 7020.25});
    }
    rows.push_back({"2026-02-04", "MES.v.0", 1400000, 7020.0, 7030.0, 6990.0, 7005.5});
    fill(w, rows);

    const auto got = to_rows(bars(w));
    ASSERT_EQ(got.size(), 3u) << "one bar per (symbol, date): 2026-02-02, 02-03, 02-04";
    EXPECT_EQ(got[1].time.substr(0, 10), "2026-02-03");
    EXPECT_EQ(got[1].close, 7020.25);
    EXPECT_EQ(got[1].volume, 1600000.0);

    const auto rep = report(w);
    EXPECT_EQ(rep.rows_dropped, 8u);
    EXPECT_EQ(rep.symbol_dates, 1u);
    EXPECT_TRUE(rep.conflicts.empty()) << "identical copies are not a conflict";
    EXPECT_EQ(mdu::format_futures_bar_dedup_summary(got.size(), rep),
              "FUTURES_BAR_DEDUP rows_read=11 rows_dropped=8 symbol_dates=1 conflicts=0");
}

// The 6A.v.0 2025-11-05 shape: the 76,895-volume copy is kept whichever copy is stored first,
// and the disagreement is reported with both closes and volumes.
TEST_F(FuturesBarDedupTest, SixAShapeKeepsTheHighVolumeCopyInEitherPhysicalOrder) {
    const Fake good{"2025-11-05", "6A.v.0", 76895, 0.6487, 0.65155, 0.64615, 0.6512};
    const Fake thin{"2025-11-05", "6A.v.0", 196, 0.648, 0.651, 0.646, 0.65095};
    const Fake before{"2025-11-04", "6A.v.0", 78356, 0.65405, 0.6545, 0.6480, 0.6487};
    const Fake after{"2025-11-06", "6A.v.0", 69067, 0.65115, 0.6520, 0.6480, 0.6486};
    const std::string expected_conflict =
        "FUTURES_BAR_DEDUP_CONFLICT symbol=6A.v.0 date=2025-11-05 copies=2 kept close=0.6512 "
        "volume=76895 dropped close=0.65095 volume=196";

    for (const bool thin_first : {false, true}) {
        SCOPED_TRACE(thin_first ? "the 196-volume copy stored first"
                                : "the 76,895-volume copy stored first");
        pqxx::work w(*c_);
        fill(w, thin_first ? std::vector<Fake>{before, thin, good, after}
                           : std::vector<Fake>{before, good, thin, after});
        const auto got = to_rows(bars(w, {"6A.v.0"}));
        ASSERT_EQ(got.size(), 3u);
        EXPECT_EQ(got[1].time.substr(0, 10), "2025-11-05");
        EXPECT_EQ(got[1].close, 0.6512);
        EXPECT_EQ(got[1].volume, 76895.0);
        EXPECT_EQ(got[1].open, 0.6487);

        const auto rep = report(w);
        EXPECT_EQ(rep.rows_dropped, 1u);
        EXPECT_EQ(rep.symbol_dates, 1u);
        ASSERT_EQ(rep.conflicts.size(), 1u);
        EXPECT_EQ(rep.conflicts[0], expected_conflict);
        w.abort();
    }
}

// Equal volume, different prices: the pick is by (close, open, high, low), not by row order.
TEST_F(FuturesBarDedupTest, EqualVolumeTieBreaksOnValuesNotRowOrder) {
    const Fake a{"2026-02-03", "ZN.v.0", 500, 112.0, 112.5, 111.5, 112.25};
    const Fake b{"2026-02-03", "ZN.v.0", 500, 112.0, 112.5, 111.5, 112.125};
    for (const bool b_first : {false, true}) {
        pqxx::work w(*c_);
        fill(w, b_first ? std::vector<Fake>{b, a} : std::vector<Fake>{a, b});
        const auto got = to_rows(bars(w));
        ASSERT_EQ(got.size(), 1u);
        EXPECT_EQ(got[0].close, 112.125) << "lowest close wins an equal-volume tie";
        w.abort();
    }
}

// A series with no repeated (symbol, date) comes back exactly as the previous query returned
// it: same rows, same values, same order, with and without a symbol filter.
TEST_F(FuturesBarDedupTest, CleanSeriesIsReturnedUnchangedAndInTheSameOrder) {
    pqxx::work w(*c_);
    // Stored deliberately out of order.
    fill(w, {{"2026-03-03", "ZN.v.0", 900, 111.0, 111.5, 110.5, 111.25},
             {"2026-03-02", "MES.v.0", 1500, 6000.0, 6010.0, 5990.0, 6005.0},
             {"2026-03-03", "6A.v.0", 70000, 0.70, 0.71, 0.69, 0.705},
             {"2026-03-02", "ZN.v.0", 800, 111.1, 111.4, 110.9, 111.0},
             {"2026-03-03", "MES.v.0", 1700, 6005.0, 6030.0, 6000.0, 6020.0},
             {"2026-03-02", "6A.v.0", 69000, 0.69, 0.70, 0.68, 0.695},
             {"2026-03-04", "6A.v.0", 71000, 0.705, 0.71, 0.70, 0.7075}});

    const auto previous = to_rows(w.exec(previous_query("fake_bars", false),
                                         pqxx::params{"2025-01-01", "2026-12-31"}));
    const auto now = to_rows(bars(w));
    ASSERT_EQ(previous.size(), 7u);
    ASSERT_EQ(now.size(), previous.size());
    for (size_t i = 0; i < now.size(); ++i) EXPECT_EQ(now[i], previous[i]) << "row " << i;

    const std::vector<std::string> some = {"MES.v.0", "6A.v.0"};
    const auto previous_f = to_rows(w.exec(previous_query("fake_bars", true),
                                           pqxx::params{"2025-01-01", "2026-12-31", some}));
    const auto now_f = to_rows(bars(w, some));
    ASSERT_EQ(previous_f.size(), 5u);
    ASSERT_EQ(now_f.size(), previous_f.size());
    for (size_t i = 0; i < now_f.size(); ++i) EXPECT_EQ(now_f[i], previous_f[i]) << "row " << i;

    const auto rep = report(w);
    EXPECT_EQ(rep.rows_dropped, 0u);
    EXPECT_EQ(rep.symbol_dates, 0u);
    EXPECT_TRUE(rep.conflicts.empty());
    EXPECT_EQ(mdu::format_futures_bar_dedup_summary(now.size(), rep),
              "FUTURES_BAR_DEDUP rows_read=7 rows_dropped=0 symbol_dates=0 conflicts=0");
}

// The loader itself: PostgresDatabase::get_market_data on the real futures table (read-only)
// returns one bar per (symbol, date), keeps the highest-volume copy, and logs the counts it
// dropped. Every expectation is computed from the table, not hard-coded.
TEST_F(FuturesBarDedupTest, LoaderReturnsOneBarPerSymbolDateAndLogsWhatItDropped) {
    struct Window {
        std::string symbol;
        Timestamp start, end;
        std::string start_s, end_s;
    };
    const std::vector<Window> windows = {
        {"6A.v.0", utc_midnight(2025, 11, 3), utc_midnight(2025, 11, 7), "2025-11-03",
         "2025-11-07"},
        {"MES.v.0", utc_midnight(2026, 2, 2), utc_midnight(2026, 2, 6), "2026-02-02",
         "2026-02-06"}};

    LoggerConfig lc;
    lc.destination = LogDestination::CONSOLE;
    lc.min_level = LogLevel::INFO;
    lc.include_timestamp = false;
    Logger::instance().initialize(lc);

    auto db = std::make_shared<PostgresDatabase>(conn_);
    ASSERT_TRUE(db->connect().is_ok());

    for (const auto& win : windows) {
        SCOPED_TRACE(win.symbol);
        // What the table holds, straight from SQL.
        size_t stored = 0, distinct = 0, groups = 0;
        double best_close = 0.0, best_volume = 0.0;
        size_t dates_before_1105 = 0;
        {
            pqxx::work w(*c_);
            const auto r = w.exec(
                "SELECT count(*), count(DISTINCT time), "
                "(SELECT count(*) FROM (SELECT time FROM futures_data.ohlcv_1d WHERE symbol = $1 "
                "AND time BETWEEN $2::timestamptz AND $3::timestamptz GROUP BY time "
                "HAVING count(*) > 1) g) "
                "FROM futures_data.ohlcv_1d WHERE symbol = $1 "
                "AND time BETWEEN $2::timestamptz AND $3::timestamptz",
                pqxx::params{win.symbol, win.start_s + " 00:00:00+00", win.end_s + " 00:00:00+00"});
            stored = r[0][0].as<size_t>();
            distinct = r[0][1].as<size_t>();
            groups = r[0][2].as<size_t>();
            if (win.symbol == "6A.v.0") {
                const auto b = w.exec(
                    "SELECT close, volume FROM futures_data.ohlcv_1d WHERE symbol = '6A.v.0' "
                    "AND time = '2025-11-05 00:00:00+00' ORDER BY volume DESC LIMIT 1");
                ASSERT_EQ(b.size(), 1u);
                best_close = b[0][0].as<double>();
                best_volume = b[0][1].as<double>();
                dates_before_1105 =
                    w.exec("SELECT count(DISTINCT time) FROM futures_data.ohlcv_1d WHERE "
                           "symbol = '6A.v.0' AND time >= '2025-11-03 00:00:00+00' AND "
                           "time < '2025-11-05 00:00:00+00'")[0][0]
                        .as<size_t>();
            }
        }
        ASSERT_GT(distinct, 0u) << "the clone holds no " << win.symbol << " bars in the window";

        ::testing::internal::CaptureStdout();
        auto res = db->get_market_data({win.symbol}, win.start, win.end, AssetClass::FUTURES,
                                       DataFrequency::DAILY, "ohlcv");
        const std::string out = ::testing::internal::GetCapturedStdout();
        std::cout << out;  // the loader's own lines, kept in the test log as evidence
        ASSERT_TRUE(res.is_ok()) << res.error()->what();
        const auto table = res.value();

        EXPECT_EQ(static_cast<size_t>(table->num_rows()), distinct)
            << "stored " << stored << " rows over " << distinct << " dates";
        EXPECT_EQ(count_of(out, "FUTURES_BAR_DEDUP rows_read=" + std::to_string(stored) +
                                    " rows_dropped=" + std::to_string(stored - distinct) +
                                    " symbol_dates=" + std::to_string(groups) + " conflicts="),
                  1u)
            << out;

        if (win.symbol == "6A.v.0") {
            // One symbol, ORDER BY time: the 2025-11-05 bar sits after the distinct dates
            // before it. (Positions, not epoch values: the arrow time column carries the
            // converter's local-time round trip, which is not what this test is about.)
            auto close_col = std::static_pointer_cast<arrow::DoubleArray>(
                table->GetColumnByName("close")->chunk(0));
            auto vol_col = std::static_pointer_cast<arrow::DoubleArray>(
                table->GetColumnByName("volume")->chunk(0));
            ASSERT_LT(static_cast<int64_t>(dates_before_1105), table->num_rows());
            EXPECT_EQ(close_col->Value(dates_before_1105), best_close);
            EXPECT_EQ(vol_col->Value(dates_before_1105), best_volume);
            if (stored > distinct && best_volume == 76895.0) {
                EXPECT_EQ(count_of(out, "FUTURES_BAR_DEDUP_CONFLICT symbol=6A.v.0 date=2025-11-05 "
                                        "copies=2 kept close=0.6512 volume=76895 dropped "
                                        "close=0.65095 volume=196"),
                          1u)
                    << out;
            }
        }
    }
    db->disconnect();
}
