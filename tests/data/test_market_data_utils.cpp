// Coverage for market_data_utils.cpp. Targets:
// - get_market_data_columns(AssetClass::EQUITIES) returns adjusted columns
//   with correct aliases (adj_open AS open, etc.)
// - get_market_data_columns(AssetClass::FUTURES) returns plain columns,
//   no adj_* columns mentioned
// - All other asset classes return plain columns
// - Correctness of column names per ADR-000 C-2 contract

#include <gtest/gtest.h>
#include <string>
#include "trade_ngin/data/market_data_utils.hpp"
#include "trade_ngin/core/types.hpp"

using namespace trade_ngin;

class MarketDataUtilsTest : public ::testing::Test {};

// ===== get_market_data_columns for EQUITIES =====

TEST_F(MarketDataUtilsTest, EquitiesReturnsAdjustedColumnsWithAliases) {
    auto columns = market_data_utils::get_market_data_columns(AssetClass::EQUITIES);

    // Must contain adjusted column names from ADR-000 C-2 contract
    EXPECT_NE(columns.find("adj_open"), std::string::npos)
        << "adj_open not found in equities columns";
    EXPECT_NE(columns.find("adj_high"), std::string::npos)
        << "adj_high not found in equities columns";
    EXPECT_NE(columns.find("adj_low"), std::string::npos)
        << "adj_low not found in equities columns";
    EXPECT_NE(columns.find("adjusted_close"), std::string::npos)
        << "adjusted_close not found in equities columns";
    EXPECT_NE(columns.find("adj_volume"), std::string::npos)
        << "adj_volume not found in equities columns";

    // Must alias to plain names so downstream consumers are unchanged
    EXPECT_NE(columns.find("AS open"), std::string::npos)
        << "adj_open must be aliased AS open";
    EXPECT_NE(columns.find("AS high"), std::string::npos)
        << "adj_high must be aliased AS high";
    EXPECT_NE(columns.find("AS low"), std::string::npos)
        << "adj_low must be aliased AS low";
    EXPECT_NE(columns.find("AS close"), std::string::npos)
        << "adjusted_close must be aliased AS close";
    EXPECT_NE(columns.find("AS volume"), std::string::npos)
        << "adj_volume must be aliased AS volume";

    // Must include time and symbol
    EXPECT_NE(columns.find("time"), std::string::npos)
        << "time column missing";
    EXPECT_NE(columns.find("symbol"), std::string::npos)
        << "symbol column missing";
}

// ===== get_market_data_columns for FUTURES =====

TEST_F(MarketDataUtilsTest, FuturesReturnsPlainColumns) {
    auto columns = market_data_utils::get_market_data_columns(AssetClass::FUTURES);

    // Must contain plain OHLCV columns
    EXPECT_NE(columns.find("open"), std::string::npos)
        << "open not found in futures columns";
    EXPECT_NE(columns.find("high"), std::string::npos)
        << "high not found in futures columns";
    EXPECT_NE(columns.find("low"), std::string::npos)
        << "low not found in futures columns";
    EXPECT_NE(columns.find("close"), std::string::npos)
        << "close not found in futures columns";
    EXPECT_NE(columns.find("volume"), std::string::npos)
        << "volume not found in futures columns";

    // Must NOT contain adjusted column names
    EXPECT_EQ(columns.find("adj_open"), std::string::npos)
        << "futures should not have adj_open";
    EXPECT_EQ(columns.find("adj_high"), std::string::npos)
        << "futures should not have adj_high";
    EXPECT_EQ(columns.find("adjusted_close"), std::string::npos)
        << "futures should not have adjusted_close";

    // Must include time and symbol
    EXPECT_NE(columns.find("time"), std::string::npos)
        << "time column missing";
    EXPECT_NE(columns.find("symbol"), std::string::npos)
        << "symbol column missing";
}

// ===== get_market_data_columns for other asset classes =====

TEST_F(MarketDataUtilsTest, FixedIncomeReturnsPlainColumns) {
    auto columns = market_data_utils::get_market_data_columns(AssetClass::FIXED_INCOME);
    EXPECT_NE(columns.find("open"), std::string::npos);
    EXPECT_NE(columns.find("close"), std::string::npos);
    EXPECT_EQ(columns.find("adj_open"), std::string::npos);
}

TEST_F(MarketDataUtilsTest, CurrenciesReturnsPlainColumns) {
    auto columns = market_data_utils::get_market_data_columns(AssetClass::CURRENCIES);
    EXPECT_NE(columns.find("open"), std::string::npos);
    EXPECT_NE(columns.find("close"), std::string::npos);
    EXPECT_EQ(columns.find("adj_open"), std::string::npos);
}

TEST_F(MarketDataUtilsTest, CommoditiesReturnsPlainColumns) {
    auto columns = market_data_utils::get_market_data_columns(AssetClass::COMMODITIES);
    EXPECT_NE(columns.find("open"), std::string::npos);
    EXPECT_NE(columns.find("close"), std::string::npos);
    EXPECT_EQ(columns.find("adj_open"), std::string::npos);
}

TEST_F(MarketDataUtilsTest, CryptoReturnsPlainColumns) {
    auto columns = market_data_utils::get_market_data_columns(AssetClass::CRYPTO);
    EXPECT_NE(columns.find("open"), std::string::npos);
    EXPECT_NE(columns.find("close"), std::string::npos);
    EXPECT_EQ(columns.find("adj_open"), std::string::npos);
}

// ===== get_schema_name cases =====

// These tests verify the types.hpp get_schema_name() function
// They test both the new OPTIONS case and that other cases are unchanged

TEST_F(MarketDataUtilsTest, GetSchemaNameEquitiesReturnsCorrectSchema) {
    auto schema = trade_ngin::get_schema_name(AssetClass::EQUITIES);
    EXPECT_EQ(schema, "equities_data");
}

TEST_F(MarketDataUtilsTest, GetSchemaNameFuturesReturnsCorrectSchema) {
    auto schema = trade_ngin::get_schema_name(AssetClass::FUTURES);
    EXPECT_EQ(schema, "futures_data");
}

TEST_F(MarketDataUtilsTest, GetSchemaNameOptionsReturnsCorrectSchema) {
    auto schema = trade_ngin::get_schema_name(AssetClass::OPTIONS);
    EXPECT_EQ(schema, "options_data")
        << "OPTIONS should resolve to options_data per ADR-000 C-2, not unknown_data";
}

TEST_F(MarketDataUtilsTest, GetSchemaNameFixedIncomeReturnsCorrectSchema) {
    auto schema = trade_ngin::get_schema_name(AssetClass::FIXED_INCOME);
    EXPECT_EQ(schema, "fixed_income_data");
}

TEST_F(MarketDataUtilsTest, GetSchemaNameCurrenciesReturnsCorrectSchema) {
    auto schema = trade_ngin::get_schema_name(AssetClass::CURRENCIES);
    EXPECT_EQ(schema, "currencies_data");
}

TEST_F(MarketDataUtilsTest, GetSchemaNameCommoditiesReturnsCorrectSchema) {
    auto schema = trade_ngin::get_schema_name(AssetClass::COMMODITIES);
    EXPECT_EQ(schema, "commodities_data");
}

TEST_F(MarketDataUtilsTest, GetSchemaNameCryptoReturnsCorrectSchema) {
    auto schema = trade_ngin::get_schema_name(AssetClass::CRYPTO);
    EXPECT_EQ(schema, "crypto_data");
}
