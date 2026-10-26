// The per-contract reader on a scratch clone (read only, through TRADE_NGIN_TEST_DSN only; skips
// without it). The clone's expiration column must be filled (R2's clone: from R0's derived
// expiries); production's is NULL until the data side fills it.
//
//   * GC on 2026-07-29: GCQ6 and GCZ6 print, with the volumes of the worked example.
//   * The vendor's contract is read from ohlcv_1d_raw by symbol and instant: on 6A 2025-11-05 the
//     kept print differs from the raw row (R0 class d), and the id is still there.
#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include "contract_rolls_test_util.hpp"

using namespace trade_ngin::contract_rolls;
using contract_rolls_test::d;

class ContractBarsDbTest : public ::testing::Test {
protected:
    void SetUp() override {
        const std::string c = contract_rolls_test::dsn();
        if (c.empty()) GTEST_SKIP() << "TRADE_NGIN_TEST_DSN not set";
        try {
            conn_ = std::make_unique<pqxx::connection>(c);
            pqxx::work w(*conn_);
            const auto r = w.exec(
                "SELECT count(*) FROM information_schema.tables WHERE table_schema = 'futures_data'"
                " AND table_name = 'contract_ohlcv_1d'");
            if (r[0][0].as<int>() == 0) GTEST_SKIP() << "no futures_data.contract_ohlcv_1d";
        } catch (const std::exception& e) {
            GTEST_SKIP() << "database unreachable: " << e.what();
        }
    }
    std::unique_ptr<pqxx::connection> conn_;
};

TEST_F(ContractBarsDbTest, ReadsTheContractBarsOfARoot) {
    pqxx::read_transaction t(*conn_);
    const auto rows = read_contract_bars(t, {"GC"}, d("2026-07-29"), d("2026-07-29"));
    const ContractBar* q6 = nullptr;
    const ContractBar* z6 = nullptr;
    for (const auto& b : rows) {
        EXPECT_EQ(b.root, "GC");
        EXPECT_EQ(b.date, d("2026-07-29"));
        if (b.raw_symbol == "GCQ6") q6 = &b;
        if (b.raw_symbol == "GCZ6") z6 = &b;
    }
    ASSERT_NE(q6, nullptr);
    ASSERT_NE(z6, nullptr);
    EXPECT_EQ(q6->volume, 22870);
    EXPECT_EQ(z6->volume, 117650);
    if (!q6->expiration) GTEST_SKIP() << "expiration not filled on this database";
    EXPECT_EQ(ymd(*q6->expiration), "2026-08-27");
    auto book = ContractBook::build(rows);
    ASSERT_TRUE(book.is_ok());
    EXPECT_EQ(book.value().printing_on("GC", d("2026-07-29")).size(), rows.size());
}

TEST_F(ContractBarsDbTest, ReadsTheVendorsContractBySymbolAndInstant) {
    pqxx::read_transaction t(*conn_);
    const auto grid = read_vendor_grid(t, "6A.v.0", d("2025-11-04"), d("2025-11-06"));
    ASSERT_EQ(grid.size(), 3u);
    for (const auto& b : grid) {
        EXPECT_EQ(b.symbol, "6A.v.0");
        EXPECT_FALSE(b.instrument_id.empty()) << ymd(b.date);
    }
    EXPECT_EQ(grid[1].date, d("2025-11-05"));
    // The id is the raw row's of the same symbol and instant, whatever its print (R0 class d).
    const auto raw = t.exec(
        "SELECT volume, instrument_id FROM futures_data.ohlcv_1d_raw WHERE symbol = '6A.v.0'"
        " AND ts_event = '2025-11-05 00:00:00+00'");
    ASSERT_EQ(raw.size(), 1u);
    EXPECT_EQ(grid[1].instrument_id, raw[0][1].as<std::string>());
    // On the reference clone the kept 6A print of 11-05 (76,895 lots) is not the raw row's (196) and
    // its id is read regardless. R6: the 213-row fix of algogators #36 made them one print on the
    // tuning copy, and no bar there has a raw row of another print; so the grid's own SQL
    // (vendor_grid_query) is also run on TEMP copies of the two tables, in an aborted transaction
    // (nothing is written), where the middle kept print is not the raw row's: its id is still read.
    {
        pqxx::connection c(contract_rolls_test::dsn());
        pqxx::work w(c);
        w.exec("SET LOCAL TimeZone = 'UTC'");
        w.exec("CREATE TEMP TABLE fake_bars (time timestamptz NOT NULL, symbol text NOT NULL, volume "
               "double precision, open double precision, high double precision, low double precision, "
               "close double precision)");
        w.exec("CREATE TEMP TABLE fake_raw (ts_event timestamptz NOT NULL, symbol text NOT NULL, volume "
               "double precision, close double precision, instrument_id text)");
        for (const auto& [day, kept, raw_volume] : std::vector<std::tuple<std::string, double, double>>{
                 {"2025-11-04", 78356, 78356}, {"2025-11-05", 76895, 196}, {"2025-11-06", 69067, 69067}}) {
            w.exec("INSERT INTO fake_bars VALUES ($1::date, 'XA.v.0', $2, 1, 1, 1, 1)", pqxx::params{day, kept});
            w.exec("INSERT INTO fake_raw VALUES ($1::date, 'XA.v.0', $2, 2, '7103')", pqxx::params{day, raw_volume});
        }
        std::string q = vendor_grid_query();
        for (const auto& [from, to] : std::vector<std::pair<std::string, std::string>>{
                 {"futures_data.ohlcv_1d_raw", "fake_raw"}, {"futures_data.ohlcv_1d", "fake_bars"}}) {
            const auto at = q.find(from);
            ASSERT_NE(at, std::string::npos) << from;
            q.replace(at, from.size(), to);
        }
        const auto fake = w.exec(q, pqxx::params{"XA.v.0", "2025-11-04", "2025-11-06"});
        ASSERT_EQ(fake.size(), 3u);
        EXPECT_EQ(fake[1][6].as<double>(), 76895.0) << "the kept print";
        ASSERT_FALSE(fake[1][7].is_null()) << "the raw row's id, though another print";
        EXPECT_EQ(fake[1][7].as<std::string>(), "7103");
    }
    // The id names a contract of the root that day.
    const auto rows = read_contract_bars(t, {"6A"}, d("2025-11-05"), d("2025-11-05"));
    auto book = ContractBook::build(rows);
    ASSERT_TRUE(book.is_ok());
    EXPECT_NE(book.value().named_by_id("6A", grid[1].instrument_id, grid[1].date), nullptr);
}
