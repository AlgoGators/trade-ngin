// The contract metadata the runners load, after migration 019 (LOOP_SPEC v6.2 section 7.6):
//   6A and 6L carry the exchange tick, 0.00005 in price and 5 dollars a contract, and the two
//     columns agree with the contract size (the registry's own consistency rule);
//   6B, 6E, 6S, ZS and ZW carry the broker symbol of the full-size contract;
//   HO, NG and 6L list every month.
// Read through PostgresDatabase::get_contract_metadata, the table InstrumentRegistry builds every
// futures contract from. Through TRADE_NGIN_TEST_DSN only; reads, never writes.
#include <gtest/gtest.h>
#include <arrow/api.h>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include "trade_ngin/data/conversion_utils.hpp"
#include "trade_ngin/data/postgres_database.hpp"

using namespace trade_ngin;

namespace {
std::string dsn() {
    const char* v = std::getenv("TRADE_NGIN_TEST_DSN");
    return v && *v ? std::string(v) : std::string();
}
}  // namespace

class ContractMetadataFixesDbTest : public ::testing::Test {
protected:
    void SetUp() override {
        const std::string c = dsn();
        if (c.empty()) GTEST_SKIP() << "TRADE_NGIN_TEST_DSN not set";
        db_ = std::make_shared<PostgresDatabase>(c);
        if (db_->connect().is_error()) GTEST_SKIP() << "database unreachable";
        auto table = db_->get_contract_metadata();
        ASSERT_TRUE(table.is_ok()) << table.error()->what();
        table_ = table.value();
        auto symbols = table_->GetColumnByName("Databento Symbol");
        ASSERT_NE(symbols, nullptr);
        for (int64_t row = 0; row < table_->num_rows(); ++row) {
            auto s = DataConversionUtils::safe_get_string(symbols, row, "Databento Symbol");
            if (s.is_ok()) row_of_[s.value()] = row;
        }
    }
    void TearDown() override {
        if (db_) db_->disconnect();
    }
    std::string text(const std::string& symbol, const std::string& column) {
        auto it = row_of_.find(symbol);
        if (it == row_of_.end()) return "<no row for " + symbol + ">";
        auto col = table_->GetColumnByName(column);
        if (!col) return "<no column " + column + ">";
        auto r = DataConversionUtils::safe_get_string(col, it->second, column);
        return r.is_ok() ? r.value() : "<unreadable>";
    }
    double number(const std::string& symbol, const std::string& column) {
        auto it = row_of_.find(symbol);
        if (it == row_of_.end()) return std::nan("");
        auto col = table_->GetColumnByName(column);
        if (!col) return std::nan("");
        auto r = DataConversionUtils::safe_get_double(col, it->second, column);
        return r.is_ok() ? r.value() : std::nan("");
    }
    std::shared_ptr<PostgresDatabase> db_;
    std::shared_ptr<arrow::Table> table_;
    std::map<std::string, int64_t> row_of_;
};

TEST_F(ContractMetadataFixesDbTest, TheTickOf6AAnd6LIsTheExchangeTick) {
    for (const std::string symbol : {"6A", "6L"}) {
        const double tick = number(symbol, "Tick Size");
        const double tick_value = number(symbol, "Minimum Price Fluctuation");
        const double contract_size = number(symbol, "Contract Size");
        EXPECT_DOUBLE_EQ(tick, 0.00005) << symbol;
        EXPECT_DOUBLE_EQ(tick_value, 5.0) << symbol;
        // The registry's consistency rule: one tick in dollars is the tick times the contract size.
        EXPECT_NEAR(tick * contract_size, tick_value, 1e-9) << symbol;
    }
}

TEST_F(ContractMetadataFixesDbTest, TheFullSizeContractsCarryTheirOwnBrokerSymbol) {
    const std::map<std::string, std::string> expected = {
        {"6B", "GBP"}, {"6E", "EUR"}, {"6S", "CHF"}, {"ZS", "ZS"}, {"ZW", "ZW"}};
    for (const auto& [symbol, ib] : expected) {
        EXPECT_EQ(text(symbol, "IB Symbol"), ib) << symbol;
    }
}

TEST_F(ContractMetadataFixesDbTest, TheMonthlyContractsListEveryMonth) {
    for (const std::string symbol : {"HO", "NG", "6L"}) {
        EXPECT_EQ(text(symbol, "Contract Months"), "All Months") << symbol;
    }
    EXPECT_EQ(text("CL", "Contract Months"), "All Months");
}
