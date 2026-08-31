// Coverage for market_data_utils.cpp. Targets:
// - get_market_data_columns(AssetClass::EQUITIES) returns the RAW per-bar
//   columns plus the corporate-action primitives (div_cash, split_factor).
//   Phase 4.2 moved equity adjustment in-engine: the loader computes the
//   backward cumulative factor itself instead of reading the vendor's derived
//   adj_*/adjusted_close columns, which can go stale when the vendor's
//   restating job stalls (it did, 2026-08-06). Behavioural coverage of the
//   adjustment maths lives in test_bar_close_uses_adjusted.cpp.
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

TEST_F(MarketDataUtilsTest, EquitiesReturnsRawColumnsPlusAdjustmentPrimitives) {
    auto columns = market_data_utils::get_market_data_columns(AssetClass::EQUITIES);

    // Raw OHLCV: adjustment is applied by the engine, not selected from the vendor.
    EXPECT_NE(columns.find("open"), std::string::npos) << "open column missing";
    EXPECT_NE(columns.find("high"), std::string::npos) << "high column missing";
    EXPECT_NE(columns.find("low"), std::string::npos) << "low column missing";
    EXPECT_NE(columns.find("close"), std::string::npos) << "close column missing";
    EXPECT_NE(columns.find("volume"), std::string::npos) << "volume column missing";

    // The per-bar corporate-action primitives the adjustment recursion needs.
    EXPECT_NE(columns.find("div_cash"), std::string::npos)
        << "div_cash missing; dividend adjustment impossible";
    EXPECT_NE(columns.find("split_factor"), std::string::npos)
        << "split_factor missing; split/spin-off adjustment impossible";

    // The vendor's derived columns must NOT be read.
    EXPECT_EQ(columns.find("adj_open"), std::string::npos)
        << "equities must not read the vendor's derived adj_* columns";
    EXPECT_EQ(columns.find("adjusted_close"), std::string::npos)
        << "equities must not read the vendor's derived adjusted_close";
    EXPECT_EQ(columns.find("closeadj"), std::string::npos)
        << "closeadj belongs to the legacy sharadar table only";

    // Must include time and symbol
    EXPECT_NE(columns.find("time"), std::string::npos)
        << "time column missing";
    EXPECT_NE(columns.find("symbol"), std::string::npos)
        << "symbol column missing";
}

TEST_F(MarketDataUtilsTest, EquityAdjustedQueryAppliesFactorAndBindsParameters) {
    auto query = market_data_utils::build_equity_adjusted_query("equities_data.ohlcv_1d", false);
    EXPECT_NE(query.find("equities_data.ohlcv_1d"), std::string::npos);
    EXPECT_NE(query.find("time BETWEEN $1 AND $2"), std::string::npos);
    EXPECT_EQ(query.find("$3"), std::string::npos)
        << "unfiltered query must not reference a symbol parameter";
    EXPECT_NE(query.find("close * f AS close"), std::string::npos)
        << "adjustment factor must scale close";
    EXPECT_NE(query.find("ORDER BY time, symbol"), std::string::npos);

    auto filtered = market_data_utils::build_equity_adjusted_query("equities_data.ohlcv_1d", true);
    EXPECT_NE(filtered.find("symbol = ANY($3)"), std::string::npos)
        << "symbol filter must bind as a parameter, never interpolate";
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

TEST_F(MarketDataUtilsTest, OptionsReturnsPlainColumns) {
    // OPTIONS asset class should use unadjusted columns.
    // Options pricing does not involve stock splits or dividends,
    // so adjusted prices are not applicable.
    auto columns = market_data_utils::get_market_data_columns(AssetClass::OPTIONS);
    EXPECT_NE(columns.find("open"), std::string::npos)
        << "open column must be present for OPTIONS";
    EXPECT_NE(columns.find("close"), std::string::npos)
        << "close column must be present for OPTIONS";
    EXPECT_EQ(columns.find("adj_open"), std::string::npos)
        << "OPTIONS should not have adj_open; adjusted prices not applicable";
    EXPECT_EQ(columns.find("adjusted_close"), std::string::npos)
        << "OPTIONS should not have adjusted_close; adjusted prices not applicable";
    EXPECT_NE(columns.find("time"), std::string::npos)
        << "time column must be present";
    EXPECT_NE(columns.find("symbol"), std::string::npos)
        << "symbol column must be present";
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

// ===== get_market_data_columns default case =====

TEST_F(MarketDataUtilsTest, UnknownAssetClassDefaultsToPlainColumns) {
    // Test the default case: unknown/invalid asset classes fall back to unadjusted columns.
    // This ensures defensive behavior if new asset classes are added without handler.
    // We cast -1 to AssetClass to force an unknown value.
    auto unknown_class = static_cast<AssetClass>(-1);
    auto columns = market_data_utils::get_market_data_columns(unknown_class);

    // Default case should return plain unadjusted columns
    EXPECT_NE(columns.find("open"), std::string::npos)
        << "Default case should include plain open column";
    EXPECT_NE(columns.find("close"), std::string::npos)
        << "Default case should include plain close column";
    EXPECT_EQ(columns.find("adj_open"), std::string::npos)
        << "Default case should NOT include adjusted columns";
    EXPECT_NE(columns.find("time"), std::string::npos)
        << "time column must be present";
    EXPECT_NE(columns.find("symbol"), std::string::npos)
        << "symbol column must be present";
}
