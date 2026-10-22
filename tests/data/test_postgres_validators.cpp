#include <gtest/gtest.h>

// Preload the target header's dependencies so the access macro only changes
// PostgresDatabase. These validators are pure and never open the test DSN.
#include <arrow/api.h>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <pqxx/pqxx>
#include <string>
#include <vector>
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/database_interface.hpp"
#include "trade_ngin/data/market_data_utils.hpp"
#include "trade_ngin/data/strategy_config_row.hpp"

#define private public
#include "trade_ngin/data/postgres_database.hpp"
#undef private

#include <limits>

using namespace trade_ngin;

namespace {

void expect_invalid(const Result<void>& result) {
    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_ARGUMENT);
}

ExecutionReport valid_execution() {
    ExecutionReport exec{};
    exec.symbol = "ES-Z6";
    exec.order_id = "order_1";
    exec.exec_id = "exec_1";
    exec.filled_quantity = Quantity(2);
    exec.fill_price = Price(4000);
    exec.total_transaction_costs = Decimal(1);
    return exec;
}

Position valid_position() {
    return Position("ES-Z6", Quantity(-2), Price(4000), Decimal(-10), Decimal(5),
                    Timestamp{});
}

}  // namespace

TEST(PostgresValidators, SymbolAllowsFuturesPunctuationAndRejectsOversize) {
    PostgresDatabase db("dbname=offline_validator_test");
    EXPECT_TRUE(db.validate_symbol("ES-Z6.1").is_ok());
    EXPECT_TRUE(db.validate_symbol(std::string(20, 'A')).is_ok());
    expect_invalid(db.validate_symbol(std::string(21, 'A')));
    expect_invalid(db.validate_symbol("ES/Z6"));
}

TEST(PostgresValidators, SymbolsRejectsAnInvalidMemberAndMoreThanOneThousand) {
    PostgresDatabase db("dbname=offline_validator_test");
    EXPECT_TRUE(db.validate_symbols({}).is_ok());
    EXPECT_TRUE(db.validate_symbols(std::vector<std::string>(1000, "ES")).is_ok());
    expect_invalid(db.validate_symbols({"ES", "bad symbol"}));
    expect_invalid(db.validate_symbols(std::vector<std::string>(1001, "ES")));
}

TEST(PostgresValidators, ExecutionReportChecksIdsQuantityPriceAndCost) {
    PostgresDatabase db("dbname=offline_validator_test");
    const auto valid = valid_execution();
    EXPECT_TRUE(db.validate_execution_report(valid).is_ok());
    auto bad = valid;
    bad.symbol = "bad symbol";
    expect_invalid(db.validate_execution_report(bad));
    bad = valid;
    bad.order_id.clear();
    expect_invalid(db.validate_execution_report(bad));
    bad = valid;
    bad.exec_id = std::string(51, 'x');
    expect_invalid(db.validate_execution_report(bad));
    bad = valid;
    bad.filled_quantity = Quantity(-1);
    expect_invalid(db.validate_execution_report(bad));
    bad = valid;
    bad.fill_price = Price(-1);
    expect_invalid(db.validate_execution_report(bad));
    bad = valid;
    bad.total_transaction_costs = Decimal(-1);
    expect_invalid(db.validate_execution_report(bad));
}

TEST(PostgresValidators, PositionChecksSymbolQuantityPriceAndPnl) {
    PostgresDatabase db("dbname=offline_validator_test");
    const auto valid = valid_position();
    EXPECT_TRUE(db.validate_position(valid).is_ok());
    auto bad = valid;
    bad.symbol = "bad symbol";
    expect_invalid(db.validate_position(bad));
    bad = valid;
    bad.quantity = Quantity(1e9 + 1);
    expect_invalid(db.validate_position(bad));
    bad = valid;
    bad.average_price = Price(-1);
    expect_invalid(db.validate_position(bad));
    bad = valid;
    bad.unrealized_pnl = Decimal(1e9 + 1);
    expect_invalid(db.validate_position(bad));
    bad = valid;
    bad.realized_pnl = Decimal(-1e9 - 1);
    expect_invalid(db.validate_position(bad));
}

TEST(PostgresValidators, SignalChecksSymbolFiniteValueAndMagnitude) {
    PostgresDatabase db("dbname=offline_validator_test");
    EXPECT_TRUE(db.validate_signal_data("ES-Z6", -1e6).is_ok());
    expect_invalid(db.validate_signal_data("bad symbol", 0.5));
    expect_invalid(db.validate_signal_data("ES", std::numeric_limits<double>::quiet_NaN()));
    expect_invalid(db.validate_signal_data("ES", std::numeric_limits<double>::infinity()));
    expect_invalid(db.validate_signal_data("ES", 1e6 + 1));
}

TEST(PostgresValidators, TableNameComponentsCheckTypeAndBothEnums) {
    PostgresDatabase db("dbname=offline_validator_test");
    EXPECT_TRUE(db.validate_table_name_components(AssetClass::EQUITIES, "ohlcv_1d",
                                                  DataFrequency::DAILY).is_ok());
    expect_invalid(db.validate_table_name_components(AssetClass::EQUITIES, "",
                                                     DataFrequency::DAILY));
    expect_invalid(db.validate_table_name_components(AssetClass::EQUITIES, "ohlcv-1d",
                                                     DataFrequency::DAILY));
    expect_invalid(db.validate_table_name_components(static_cast<AssetClass>(11), "ohlcv_1d",
                                                     DataFrequency::DAILY));
    expect_invalid(db.validate_table_name_components(AssetClass::EQUITIES, "ohlcv_1d",
                                                     static_cast<DataFrequency>(-1)));
}

TEST(PostgresValidators, DisconnectedConnectionHasExactErrorCode) {
    PostgresDatabase db("dbname=offline_validator_test");
    auto result = db.validate_connection();
    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::CONNECTION_ERROR);
}
