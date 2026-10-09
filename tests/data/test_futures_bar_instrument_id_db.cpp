// The futures loader joins each kept bar's vendor instrument id (LOOP_SPEC v6.1 section 2.1; T-ROLLX
// commit 1): futures_data.ohlcv_1d_raw.instrument_id LEFT JOINed on the exact print, carried through
// the Arrow table into Bar::instrument_id. Read-only on the clone, through TRADE_NGIN_TEST_DSN only.
//
//   * NG.v.0 rolls on 2025-10-27 (id 864 -> 863 at the vendor's switch): the loaded bars carry the
//     ids, the series flags the change bar and confirms it on 10-28, and the adjusted return on the
//     change bar is 0 while the raw splice step is 3.376 -> 3.965.
//   * 6A.v.0 2025-11-05: the kept 76,895-lot print has no raw match (the raw table holds the 196-lot
//     copy the loader drops): its id is empty, and the series is not a change there (not judged).
//   * an equity load carries no instrument_id column: every equity bar's id is empty.
// The oracle's series comparison reads the backtest's OWN record of the bars it consumed
// (include/trade_ngin/backtest/consumed_series_record.hpp), not a series built here on every loaded
// bar: a JUNK bar or a thin first print the engine withholds (K-01) is in no consumed sequence.
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

TEST_F(FuturesBarInstrumentIdTest, AKeptPrintTheRawTableLacksHasNoIdAndIsNotJudged) {
    const auto bars = load({"6A.v.0"}, "2025-11-03", "2025-11-07");
    ASSERT_EQ(bars.size(), 5u);
    std::vector<double> closes;
    std::vector<std::string> ids;
    for (const auto& b : bars) {
        closes.push_back(static_cast<double>(b.close));
        ids.push_back(b.instrument_id);
    }
    EXPECT_EQ(ids, (std::vector<std::string>{"7103", "7103", "", "7103", "7103"}));
    const auto s = roll_series::build_series(closes, ids);
    for (size_t t = 0; t < 5; ++t) EXPECT_FALSE(s.flags.change[t]) << t;
    EXPECT_EQ(s.adjusted, closes);
}

TEST_F(FuturesBarInstrumentIdTest, AnEquityBarHasNoInstrumentId) {
    auto symbols = db_->get_symbols(AssetClass::EQUITIES);
    if (symbols.is_error() || symbols.value().empty()) GTEST_SKIP() << "no equity symbols";
    const auto bars = load({symbols.value().front()}, "2026-06-01", "2026-06-30",
                           AssetClass::EQUITIES);
    for (const auto& b : bars) EXPECT_TRUE(b.instrument_id.empty());
}
