// tests/live/test_finalized_books_read.cpp
//
// What a futures live run prints of yesterday (the email's "Yesterday's Finalized Position
// Results" table and the attached <T-1>_positions_asof_<T>.csv) is the Day T-1 rows as stored
// AFTER the run's finalize write. The runner loads the stored T-1 books early, for the no-session
// hold, and from the commit that moved that load ahead of the finalize the same early rows were
// handed to the email and the CSV: every realized cell printed the placeholder 0.00 while the
// stored row carried the settled figure.
//
// The read-back is a helper (finalized_books_read.hpp) and is tested here; that the runners call it
// after the finalize and print its rows is tested in test_yesterday_rows_source.cpp.

#include <gtest/gtest.h>

#include <string>

#include "trade_ngin/live/finalized_books_read.hpp"

using namespace trade_ngin;

namespace {

Position row(const std::string& symbol, double quantity, double realized) {
    Position p;
    p.symbol = symbol;
    p.quantity = Decimal(quantity);
    p.average_price = Decimal(100.0);
    p.realized_pnl = Decimal(realized);
    return p;
}

}  // namespace

TEST(FinalizedBooksRead, TheRowsReadBackCarryTheSettledRealizedNotThePlaceholder) {
    const SleeveBooks early = {
        {"TREND_FOLLOWING", {{"ZF.v.0", row("ZF.v.0", -2.0, 0.0)}, {"KE.v.0", row("KE.v.0", 1.0, 0.0)}}},
        {"TREND_FOLLOWING_FAST", {{"6A.v.0", row("6A.v.0", 3.0, 0.0)}}}};
    const SleeveBooks stored = {
        {"TREND_FOLLOWING", {{"ZF.v.0", row("ZF.v.0", -2.0, -375.0)}, {"KE.v.0", row("KE.v.0", 1.0, -550.0)}}},
        {"TREND_FOLLOWING_FAST", {{"6A.v.0", row("6A.v.0", 3.0, 210.0)}}}};
    int reads = 0;
    const SleeveBooks out = read_back_finalized_books(early, [&](const std::string& sleeve) {
        ++reads;
        return Result<std::unordered_map<std::string, Position>>(stored.at(sleeve));
    });
    EXPECT_EQ(reads, 2) << "one read per sleeve of the early books";
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(static_cast<double>(out.at("TREND_FOLLOWING").at("ZF.v.0").realized_pnl), -375.0);
    EXPECT_EQ(static_cast<double>(out.at("TREND_FOLLOWING").at("KE.v.0").realized_pnl), -550.0);
    EXPECT_EQ(static_cast<double>(out.at("TREND_FOLLOWING_FAST").at("6A.v.0").realized_pnl), 210.0);
    // The early books are the hold's and the execution diff's: untouched.
    EXPECT_EQ(static_cast<double>(early.at("TREND_FOLLOWING").at("ZF.v.0").realized_pnl), 0.0);
}

TEST(FinalizedBooksRead, ASleeveThatCannotBeReadBackKeepsItsEarlyRows) {
    const SleeveBooks early = {{"TREND_FOLLOWING", {{"ZF.v.0", row("ZF.v.0", -2.0, 0.0)}}},
                               {"TREND_FOLLOWING_FAST", {{"6A.v.0", row("6A.v.0", 3.0, 0.0)}}}};
    const SleeveBooks out = read_back_finalized_books(early, [&](const std::string& sleeve) {
        if (sleeve == "TREND_FOLLOWING") {
            return make_error<std::unordered_map<std::string, Position>>(
                ErrorCode::DATABASE_ERROR, "the connection dropped", "test");
        }
        return Result<std::unordered_map<std::string, Position>>(
            std::unordered_map<std::string, Position>{{"6A.v.0", row("6A.v.0", 3.0, 210.0)}});
    });
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(static_cast<double>(out.at("TREND_FOLLOWING").at("ZF.v.0").quantity), -2.0);
    EXPECT_EQ(static_cast<double>(out.at("TREND_FOLLOWING").at("ZF.v.0").realized_pnl), 0.0);
    EXPECT_EQ(static_cast<double>(out.at("TREND_FOLLOWING_FAST").at("6A.v.0").realized_pnl), 210.0);
}
