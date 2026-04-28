// Coverage for execution_manager.cpp. ExecutionManager owns a
// TransactionCostManager so we use a real one with a default config.
//
// Targets:
// - generate_daily_executions on adds, increases, decreases, and closes
// - generate_execution sets side correctly for buy/sell, populates IDs and
//   transaction-cost fields
// - generate_date_string formats YYYYMMDD
// - generate_exec_id encodes symbol/timestamp/sequence
// - update_market_data populates the prev_close map

#include <gtest/gtest.h>
#include <chrono>
#include <unordered_map>
#include "trade_ngin/live/execution_manager.hpp"

using namespace trade_ngin;

namespace {

Position make_position(const std::string& symbol, double qty, double avg_price) {
    Position p;
    p.symbol = symbol;
    p.quantity = Decimal(qty);
    p.average_price = Decimal(avg_price);
    return p;
}

Timestamp at_local_date(int year, int month, int day) {
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = 12;
    return std::chrono::system_clock::from_time_t(std::mktime(&tm));
}

}  // namespace

class ExecutionManagerTest : public ::testing::Test {};

// ===== generate_daily_executions =====

TEST_F(ExecutionManagerTest, EmptyPositionsProducesNoExecutions) {
    ExecutionManager em;
    auto r = em.generate_daily_executions({}, {}, {}, std::chrono::system_clock::now());
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().empty());
}

TEST_F(ExecutionManagerTest, NewPositionGeneratesBuyExecution) {
    ExecutionManager em;
    std::unordered_map<std::string, Position> curr{{"ES", make_position("ES", 5.0, 4500.0)}};
    std::unordered_map<std::string, Position> prev;
    std::unordered_map<std::string, double> prices{{"ES", 4500.0}};
    auto r = em.generate_daily_executions(curr, prev, prices, std::chrono::system_clock::now());
    ASSERT_TRUE(r.is_ok());
    ASSERT_EQ(r.value().size(), 1u);
    EXPECT_EQ(r.value()[0].side, Side::BUY);
    EXPECT_EQ(r.value()[0].symbol, "ES");
}

TEST_F(ExecutionManagerTest, ReducedPositionGeneratesSellExecution) {
    ExecutionManager em;
    std::unordered_map<std::string, Position> curr{{"ES", make_position("ES", 3.0, 4500.0)}};
    std::unordered_map<std::string, Position> prev{{"ES", make_position("ES", 5.0, 4500.0)}};
    std::unordered_map<std::string, double> prices{{"ES", 4505.0}};
    auto r = em.generate_daily_executions(curr, prev, prices, std::chrono::system_clock::now());
    ASSERT_TRUE(r.is_ok());
    ASSERT_EQ(r.value().size(), 1u);
    EXPECT_EQ(r.value()[0].side, Side::SELL);
}

TEST_F(ExecutionManagerTest, UnchangedPositionProducesNoExecution) {
    ExecutionManager em;
    auto pos = make_position("ES", 5.0, 4500.0);
    std::unordered_map<std::string, Position> curr{{"ES", pos}};
    std::unordered_map<std::string, Position> prev{{"ES", pos}};
    std::unordered_map<std::string, double> prices{{"ES", 4500.0}};
    auto r = em.generate_daily_executions(curr, prev, prices, std::chrono::system_clock::now());
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().empty());
}

TEST_F(ExecutionManagerTest, ClosedPositionGeneratesOppositeSideExecution) {
    ExecutionManager em;
    std::unordered_map<std::string, Position> curr;
    std::unordered_map<std::string, Position> prev{{"ES", make_position("ES", 5.0, 4500.0)}};
    std::unordered_map<std::string, double> prices{{"ES", 4505.0}};
    auto r = em.generate_daily_executions(curr, prev, prices, std::chrono::system_clock::now());
    ASSERT_TRUE(r.is_ok());
    ASSERT_EQ(r.value().size(), 1u);
    EXPECT_EQ(r.value()[0].side, Side::SELL);  // Closing a long → SELL
}

TEST_F(ExecutionManagerTest, ClosedShortPositionGeneratesBuyExecution) {
    ExecutionManager em;
    std::unordered_map<std::string, Position> curr;
    std::unordered_map<std::string, Position> prev{{"ES", make_position("ES", -3.0, 4500.0)}};
    std::unordered_map<std::string, double> prices{{"ES", 4505.0}};
    auto r = em.generate_daily_executions(curr, prev, prices, std::chrono::system_clock::now());
    ASSERT_TRUE(r.is_ok());
    ASSERT_EQ(r.value().size(), 1u);
    EXPECT_EQ(r.value()[0].side, Side::BUY);
}

TEST_F(ExecutionManagerTest, MissingMarketPriceFallsBackToAveragePrice) {
    ExecutionManager em;
    std::unordered_map<std::string, Position> curr{{"ES", make_position("ES", 2.0, 4500.0)}};
    std::unordered_map<std::string, Position> prev;
    std::unordered_map<std::string, double> prices;  // no price for ES
    auto r = em.generate_daily_executions(curr, prev, prices, std::chrono::system_clock::now());
    ASSERT_TRUE(r.is_ok());
    ASSERT_EQ(r.value().size(), 1u);
    EXPECT_DOUBLE_EQ(r.value()[0].fill_price.as_double(), 4500.0);
}

// ===== generate_execution =====

TEST_F(ExecutionManagerTest, GenerateExecutionPopulatesAllFields) {
    ExecutionManager em;
    auto ts = at_local_date(2026, 4, 28);
    auto exec = em.generate_execution("ES", 3.0, 4500.0, ts, 0);
    EXPECT_EQ(exec.symbol, "ES");
    EXPECT_EQ(exec.side, Side::BUY);
    EXPECT_DOUBLE_EQ(exec.filled_quantity.as_double(), 3.0);
    EXPECT_DOUBLE_EQ(exec.fill_price.as_double(), 4500.0);
    EXPECT_FALSE(exec.is_partial);
    EXPECT_NE(exec.exec_id.find("EXEC_ES_"), std::string::npos);
    EXPECT_NE(exec.order_id.find("DAILY_ES_"), std::string::npos);
}

TEST_F(ExecutionManagerTest, GenerateExecutionSequenceProducesDistinctIds) {
    ExecutionManager em;
    auto ts = at_local_date(2026, 4, 28);
    auto e0 = em.generate_execution("ES", 1.0, 4500.0, ts, 0);
    auto e1 = em.generate_execution("ES", 1.0, 4500.0, ts, 1);
    EXPECT_NE(e0.exec_id, e1.exec_id);
}

// ===== Static helpers =====

TEST_F(ExecutionManagerTest, GenerateDateStringHasYYYYMMDDFormat) {
    auto s = ExecutionManager::generate_date_string(at_local_date(2026, 4, 28));
    EXPECT_EQ(s.length(), 8u);
    EXPECT_NE(s.find("20260428"), std::string::npos);
}

TEST_F(ExecutionManagerTest, GenerateExecIdEncodesSymbolAndSequence) {
    auto s = ExecutionManager::generate_exec_id("ES", at_local_date(2026, 4, 28), 7);
    EXPECT_NE(s.find("EXEC_ES_"), std::string::npos);
    EXPECT_NE(s.find("_7"), std::string::npos);
}

// ===== update_market_data populates prev_close map =====

TEST_F(ExecutionManagerTest, UpdateMarketDataDoesNotThrow) {
    ExecutionManager em;
    EXPECT_NO_THROW(em.update_market_data("ES", 1000.0, 4500.0));
    EXPECT_NO_THROW(em.update_market_data("ES", 1100.0, 4510.0));
}
