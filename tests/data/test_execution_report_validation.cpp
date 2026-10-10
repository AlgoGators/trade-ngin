// tests/data/test_execution_report_validation.cpp
//
// T-NETTING fix round 2 (audit A of the fix round, S3 b): the store-side refusal, WITHOUT a
// database. PostgresDatabase::validate_execution_report is called by every execution writer on
// every row before anything is stored; it reads the row only, so it runs on an object that never
// connects. (tests/data/test_executions_netting_column_db.cpp shows, against a database, that a
// refused row is not stored.)

#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/postgres_database.hpp"

using namespace trade_ngin;

namespace {

ExecutionReport row(ExecutionType type, double adjustment) {
    ExecutionReport e;
    e.symbol = "MBT.v.0";
    e.order_id = "DAILY_MBT.v.0_2026-04-28";
    e.exec_id = "EXEC_MBT.v.0_20260427_RC";
    e.side = Side::SELL;
    e.filled_quantity = Quantity(1.0);
    e.fill_price = Price(78270.0);
    e.fill_time = std::chrono::system_clock::from_time_t(1777352400);
    e.commissions_fees = Decimal(1.5);
    e.slippage_market_impact = Decimal(1.05);
    e.total_transaction_costs = Decimal(2.55);
    e.netting_adjustment = Decimal(adjustment);
    e.execution_type = type;
    return e;
}

}  // namespace

// RED if the call of unnetted_row_refusal is removed from validate_execution_report.
TEST(ExecutionReportValidation, ARollOrBorrowRowWithAnAdjustmentIsRefusedWithoutADatabase) {
    const PostgresDatabase db("host=127.0.0.1 port=1 dbname=never_connected");
    for (const ExecutionType type : {ExecutionType::ROLL, ExecutionType::BORROW}) {
        ASSERT_TRUE(db.validate_execution_report(row(type, 0.0)).is_ok())
            << to_string(type) << ": the row itself is valid";
        for (const double adjustment : {1.0, -1.0, 0.00000001}) {
            const auto refused = db.validate_execution_report(row(type, adjustment));
            ASSERT_TRUE(refused.is_error()) << to_string(type) << " " << adjustment;
            const std::string what = refused.error()->what();
            EXPECT_NE(what.find("NETTING_REFUSED"), std::string::npos) << what;
            EXPECT_NE(what.find("EXEC_MBT.v.0_20260427_RC"), std::string::npos) << what;
            EXPECT_NE(what.find(std::string("the ") + to_string(type) + " row"), std::string::npos)
                << what;
        }
    }
}

// A STRATEGY row carries any adjustment, a saving or an extra cost: that is what netting is.
TEST(ExecutionReportValidation, AStrategyRowMayCarryAnyAdjustment) {
    const PostgresDatabase db("host=127.0.0.1 port=1 dbname=never_connected");
    for (const double adjustment : {0.0, 2.55, 1.2, -0.91}) {
        EXPECT_TRUE(db.validate_execution_report(row(ExecutionType::STRATEGY, adjustment)).is_ok())
            << adjustment;
    }
}
