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
    // The kept 6A print of 11-05 is not the raw row's print, and its id is read regardless.
    const auto raw = t.exec(
        "SELECT volume FROM futures_data.ohlcv_1d_raw WHERE symbol = '6A.v.0'"
        " AND ts_event = '2025-11-05 00:00:00+00'");
    ASSERT_EQ(raw.size(), 1u);
    EXPECT_NE(raw[0][0].as<int64_t>(), grid[1].volume);
    // The id names a contract of the root that day.
    const auto rows = read_contract_bars(t, {"6A"}, d("2025-11-05"), d("2025-11-05"));
    auto book = ContractBook::build(rows);
    ASSERT_TRUE(book.is_ok());
    EXPECT_NE(book.value().named_by_id("6A", grid[1].instrument_id, grid[1].date), nullptr);
}
