// Contract identity of the per-contract table (ROLLS_STEP1_PLAN section 10b point 2): a contract is
// (root, raw_symbol, expiration). The vendor reuses ids across contracts years apart and raw_symbol
// has a one-digit year, so neither names a contract across dates on its own; the expiration
// resolves the year. A contract with no expiration cannot be ordered and is refused, by name.
#include <gtest/gtest.h>

#include "contract_rolls_test_util.hpp"

using namespace trade_ngin::contract_rolls;
using contract_rolls_test::bar;
using contract_rolls_test::d;

TEST(ContractKey, ResolvesTheOneDigitYearByExpiration) {
    // GCZ6 is the December 2016 and the December 2026 contract.
    auto k16 = ContractKey::make("GC", "GCZ6", d("2016-12-28"));
    auto k26 = ContractKey::make("GC", "GCZ6", d("2026-12-29"));
    ASSERT_TRUE(k16.is_ok());
    ASSERT_TRUE(k26.is_ok());
    EXPECT_EQ(k16.value().delivery_year, 2016);
    EXPECT_EQ(k26.value().delivery_year, 2026);
    EXPECT_EQ(k26.value().delivery_month, 12u);
    EXPECT_NE(k16.value(), k26.value());
    EXPECT_TRUE(k16.value() < k26.value());
    // A January contract whose last trading day is in December of the year before (CL, NG).
    auto clf7 = ContractKey::make("CL", "CLF7", d("2026-12-21"));
    ASSERT_TRUE(clf7.is_ok());
    EXPECT_EQ(clf7.value().delivery_year, 2027);
    EXPECT_EQ(clf7.value().delivery_month, 1u);
    // A two-digit year is read as written.
    auto clz31 = ContractKey::make("CL", "CLZ31", d("2031-11-19"));
    ASSERT_TRUE(clz31.is_ok());
    EXPECT_EQ(clz31.value().delivery_year, 2031);
    EXPECT_EQ(clz31.value().to_string(), "CLZ31 (2031-12, expires 2031-11-19)");
}

TEST(ContractKey, RefusesASymbolOrExpirationThatCannotBeAContract) {
    EXPECT_TRUE(ContractKey::make("GC", "SIZ6", d("2026-12-29")).is_error());   // wrong root
    EXPECT_TRUE(ContractKey::make("GC", "GCA6", d("2026-12-29")).is_error());   // no month code
    EXPECT_TRUE(ContractKey::make("GC", "GCZ", d("2026-12-29")).is_error());    // no year
    EXPECT_TRUE(ContractKey::make("GC", "GCZ6x", d("2026-12-29")).is_error());  // not digits
    // An expiration five months before the delivery month is not this contract's.
    EXPECT_TRUE(ContractKey::make("GC", "GCZ6", d("2026-07-29")).is_error());
    EXPECT_EQ(ContractKey::month_code(8), 'Q');
    EXPECT_EQ(ContractKey::month_of_code('Q'), 8u);
    EXPECT_EQ(ContractKey::month_of_code('A'), 0u);
    EXPECT_EQ(ContractKey::month_code(13), '?');
}

TEST(ContractBook, AReusedIdNamesTwoContractsOnTheirOwnDates) {
    // Id 2017 is 6BX0 in 2020 and GCQ4 in 2023-24 (different roots); id 669 is GFK8 then GCM4. Here
    // one root: id 77 is GCZ6 (2016) and, ten years later, GCQ6 (2026).
    const std::vector<ContractBar> rows = {
        bar("GC", "GCZ6", "77", "2016-11-01", "2016-12-28", 1270.0, 100),
        bar("GC", "GCZ6", "77", "2016-11-02", "2016-12-28", 1290.0, 120),
        bar("GC", "GCQ6", "77", "2026-07-01", "2026-08-27", 3350.0, 900),
        bar("GC", "GCZ6", "41271", "2026-07-01", "2026-12-29", 3400.0, 50),
    };
    auto book = ContractBook::build(rows);
    ASSERT_TRUE(book.is_ok()) << book.error()->what();
    const ContractBook& b = book.value();
    EXPECT_EQ(b.contracts().size(), 3u);  // GCZ6 2016, GCQ6 2026, GCZ6 2026
    const Contract* in2016 = b.named_by_id("GC", "77", d("2016-11-02"));
    const Contract* in2026 = b.named_by_id("GC", "77", d("2026-07-01"));
    ASSERT_NE(in2016, nullptr);
    ASSERT_NE(in2026, nullptr);
    EXPECT_EQ(in2016->key->raw_symbol, "GCZ6");
    EXPECT_EQ(in2016->key->delivery_year, 2016);
    EXPECT_EQ(in2026->key->raw_symbol, "GCQ6");
    // The two GCZ6 contracts are two identities.
    const Contract* z26 = b.named_by_id("GC", "41271", d("2026-07-01"));
    ASSERT_NE(z26, nullptr);
    EXPECT_NE(*z26->key, *in2016->key);
    // On a date neither contract carrying id 77 spans, the id names nothing.
    EXPECT_EQ(b.named_by_id("GC", "77", d("2020-01-02")), nullptr);
    EXPECT_EQ(b.named_by_id("GC", "", d("2026-07-01")), nullptr);
    EXPECT_EQ(b.named_by_id("SI", "77", d("2026-07-01")), nullptr);
    EXPECT_EQ(b.printing_on("GC", d("2026-07-01")).size(), 2u);
    EXPECT_TRUE(b.has_bars("GC", d("2016-01-01"), d("2016-12-31")));
    EXPECT_FALSE(b.has_bars("GC", d("2017-01-01"), d("2025-12-31")));
    EXPECT_EQ(b.find(*z26->key), z26);
}

TEST(ContractBook, AnIdInsideTheContractsSpanNamesItOnADateWithNoRow) {
    // R0's 16 Sunday no-row days: the vendor names the held contract's id on a date the table has
    // no row for it. The id is resolved within the contract's own span.
    const std::vector<ContractBar> rows = {
        bar("UB", "UBU4", "500", "2014-08-29", "2014-09-19", 140.0, 10000),
        bar("UB", "UBU4", "500", "2014-09-02", "2014-09-19", 141.0, 10000),
    };
    auto book = ContractBook::build(rows);
    ASSERT_TRUE(book.is_ok());
    const Contract* c = book.value().named_by_id("UB", "500", d("2014-08-31"));
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->bar_on(d("2014-08-31")), nullptr);
    EXPECT_EQ(c->key->raw_symbol, "UBU4");
}

// Rolls step 4 (R3 audit A note 1): past the contract's last row read, up to its expiration, the id still
// names it, so the answer for a day does not depend on how far the rows read reach: a live run whose
// contract bars of its last day are not loaded yet reads what a later backtest reads. After the
// expiration nothing is named.
TEST(ContractBook, AnIdNamesItsContractPastItsLastRowUpToItsExpiration) {
    const std::vector<ContractBar> rows = {
        bar("GC", "GCZ6", "41271", "2026-10-01", "2026-12-29", 4170.0, 150000),
        bar("GC", "GCZ6", "41271", "2026-10-02", "2026-12-29", 4172.1, 153044),
    };
    auto book = ContractBook::build(rows);
    ASSERT_TRUE(book.is_ok());
    for (const char* day : {"2026-10-05", "2026-12-29"}) {
        const Contract* c = book.value().named_by_id("GC", "41271", d(day));
        ASSERT_NE(c, nullptr) << day;
        EXPECT_EQ(c->key->raw_symbol, "GCZ6") << day;
        EXPECT_EQ(c->bar_on(d(day)), nullptr) << day;
    }
    EXPECT_EQ(book.value().named_by_id("GC", "41271", d("2026-12-30")), nullptr) << "past its expiration";
    EXPECT_EQ(book.value().named_by_id("GC", "41271", d("2026-09-30")), nullptr) << "before its first row";
}

TEST(ContractBook, ANullExpirationIsRefusedByName) {
    const std::vector<ContractBar> rows = {
        bar("GC", "GCZ6", "41271", "2026-10-01", "", 4170.0, 150000),
        bar("GC", "GCZ6", "41271", "2026-10-02", "", 4172.1, 153044),
        bar("GC", "GCG7", "42005245", "2026-10-02", "2027-02-24", 4204.5, 1670),
    };
    auto book = ContractBook::build(rows);
    ASSERT_TRUE(book.is_ok());  // the rows group; only a use of the contract is refused
    const Contract* z6 = book.value().named_by_id("GC", "41271", d("2026-10-02"));
    ASSERT_NE(z6, nullptr);
    EXPECT_FALSE(z6->resolved());
    auto refused = ContractBook::require(*z6);
    ASSERT_TRUE(refused.is_error());
    const std::string msg = refused.error()->what();
    EXPECT_NE(msg.find("GCZ6"), std::string::npos) << msg;
    EXPECT_NE(msg.find("41271"), std::string::npos) << msg;
    EXPECT_NE(msg.find("no expiration"), std::string::npos) << msg;
    const Contract* g7 = book.value().named_by_id("GC", "42005245", d("2026-10-02"));
    ASSERT_NE(g7, nullptr);
    EXPECT_TRUE(ContractBook::require(*g7).is_ok());
}

TEST(ContractBook, RefusesTwoRowsOfOneIdOnOneDate) {
    const std::vector<ContractBar> rows = {
        bar("GC", "GCZ6", "41271", "2026-10-02", "2026-12-29", 4172.1, 1),
        bar("GC", "GCQ6", "41271", "2026-10-02", "2026-08-27", 4172.1, 1),
    };
    EXPECT_TRUE(ContractBook::build(rows).is_error());
    // And a key that cannot be made is a build error.
    EXPECT_TRUE(ContractBook::build({bar("GC", "GCA6", "1", "2026-10-02", "2026-12-29", 1, 1)})
                    .is_error());
}
