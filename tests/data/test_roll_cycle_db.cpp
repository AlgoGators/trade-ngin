// The fixed roll-cycle table, recomputed from the data (read only, through TRADE_NGIN_TEST_DSN; skips
// without it or when the clone's expiration column is not filled). Every root of the vendor's
// grid, over the whole history to the last per-contract bar, must give the table's cycle. The
// micros' bars before 2026-02-22 name full-size contracts that are not loaded yet; they are counted
// as unresolved and the cycle is measured on the rest.
#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include "contract_rolls_test_util.hpp"
#include "trade_ngin/data/roll_cycle.hpp"

using namespace trade_ngin::contract_rolls;
using contract_rolls_test::d;

TEST(RollCycleDb, TheFixedTableMatchesTheData) {
    const std::string c = contract_rolls_test::dsn();
    if (c.empty()) GTEST_SKIP() << "TRADE_NGIN_TEST_DSN not set";
    std::unique_ptr<pqxx::connection> conn;
    try {
        conn = std::make_unique<pqxx::connection>(c);
    } catch (const std::exception& e) {
        GTEST_SKIP() << "database unreachable: " << e.what();
    }
    pqxx::read_transaction t(*conn);
    const auto filled = t.exec(
        "SELECT count(*) FILTER (WHERE expiration IS NULL), count(*), max(date)::text"
        " FROM futures_data.contract_ohlcv_1d");
    if (filled[0][1].as<int64_t>() == 0 || filled[0][0].as<int64_t>() > 0) {
        GTEST_SKIP() << "contract_ohlcv_1d empty or expiration not filled";
    }
    const Day end = parse_ymd(filled[0][2].as<std::string>());
    const auto roots = t.exec(
        "SELECT DISTINCT replace(symbol, '.v.0', '') FROM futures_data.ohlcv_1d ORDER BY 1");
    int checked = 0;
    for (const auto& r : roots) {
        const std::string root = r[0].as<std::string>();
        auto want = roll_cycle(root);
        ASSERT_TRUE(want.is_ok()) << root;
        const auto grid = read_vendor_grid(t, root + ".v.0", d("2010-01-01"), end);
        auto book = ContractBook::build(read_contract_bars(t, {root}, d("2010-01-01"), end));
        ASSERT_TRUE(book.is_ok()) << root << ": " << book.error()->what();
        auto m = measure_cycle(root, grid, book.value(), [&](Day) { return root; });
        ASSERT_TRUE(m.is_ok()) << root << ": " << m.error()->what();
        EXPECT_EQ(m.value().cycle(), want.value()) << root << " years " << m.value().years;
        ++checked;
    }
    EXPECT_EQ(checked, 36);
}
