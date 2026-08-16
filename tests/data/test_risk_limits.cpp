/**
 * Tests for the Stage 1b risk-limits publication path.
 *
 * Two layers:
 *  1. store_risk_limits() itself — input validation and the not-connected
 *     error path, exercised through the real implementation (the mock database
 *     has no live pqxx connection, so a fully valid call must stop at the
 *     connection check rather than crash into pqxx).
 *  2. The published envelope contract — the exact JSONB shape
 *     live_portfolio_runner publishes and the migration COMMENT documents:
 *     max_symbol_position_contracts (map, CONTRACT UNITS), max_gross_leverage,
 *     max_net_leverage, and nothing else.
 */
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <memory>
#include "test_db_utils.hpp"
#include "trade_ngin/data/postgres_database.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

// Mirror of the envelope construction in live_portfolio_runner.cpp. If the
// runner's shape changes, change this factory and the contract tests below in
// the same commit — that is the point of them.
nlohmann::json make_published_envelope() {
    nlohmann::json limits;
    nlohmann::json symbol_contracts;
    symbol_contracts["ES"] = 500.0;
    symbol_contracts["NQ"] = 250.0;
    symbol_contracts["GC"] = 100.0;
    limits["max_symbol_position_contracts"] = symbol_contracts;
    limits["max_gross_leverage"] = 2.0;
    limits["max_net_leverage"] = 1.5;
    return limits;
}

}  // namespace

class RiskLimitsTest : public ::testing::Test {
protected:
    void SetUp() override {
        db = std::make_unique<MockPostgresDatabase>("mock://testdb");
        db->connect();
    }

    void TearDown() override {
        if (db && db->is_connected()) {
            db->disconnect();
        }
    }

    std::unique_ptr<MockPostgresDatabase> db;
};

// ---------------------------------------------------------------------------
// store_risk_limits(): validation and error paths
// ---------------------------------------------------------------------------

TEST_F(RiskLimitsTest, StoreRejectsInvalidTableName) {
    auto result = db->store_risk_limits("trend_following", "base",
                                        make_published_envelope(),
                                        "trading.risk_limits; DROP TABLE x--");
    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_ARGUMENT);
}

TEST_F(RiskLimitsTest, StoreRejectsInvalidStrategyId) {
    auto result = db->store_risk_limits("bad id; --", "base",
                                        make_published_envelope(),
                                        "trading.risk_limits");
    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_ARGUMENT);
}

TEST_F(RiskLimitsTest, StoreRejectsEmptyStrategyId) {
    auto result = db->store_risk_limits("", "base", make_published_envelope(),
                                        "trading.risk_limits");
    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_ARGUMENT);
}

TEST_F(RiskLimitsTest, StoreWithValidInputsStopsAtConnectionCheck) {
    // The mock database reports connected but holds no live pqxx connection,
    // so a fully valid call must be stopped by validate_connection() — proving
    // input validation passed and the failure is the connection, not a crash.
    auto result = db->store_risk_limits("trend_following", "base",
                                        make_published_envelope(),
                                        "trading.risk_limits");
    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::CONNECTION_ERROR);
}

TEST_F(RiskLimitsTest, StoreFailsCleanlyWhenDisconnected) {
    db->disconnect();
    auto result = db->store_risk_limits("trend_following", "base",
                                        make_published_envelope(),
                                        "trading.risk_limits");
    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::CONNECTION_ERROR);
}

// ---------------------------------------------------------------------------
// Envelope contract: what the runner publishes and the table COMMENT documents
// ---------------------------------------------------------------------------

TEST_F(RiskLimitsTest, EnvelopeContainsExactlyTheDocumentedKeys) {
    auto limits = make_published_envelope();

    ASSERT_TRUE(limits.contains("max_symbol_position_contracts"));
    ASSERT_TRUE(limits.contains("max_gross_leverage"));
    ASSERT_TRUE(limits.contains("max_net_leverage"));

    // Honest publication: limits the engine does not enforce must be absent —
    // and the misleading dollars-sounding key must never come back.
    EXPECT_FALSE(limits.contains("max_symbol_notional"));
    EXPECT_FALSE(limits.contains("max_gross_notional"));
    EXPECT_FALSE(limits.contains("max_position_count"));
    EXPECT_EQ(limits.size(), 3u);
}

TEST_F(RiskLimitsTest, SymbolCapsAreAContractUnitMap) {
    auto limits = make_published_envelope();
    const auto& caps = limits["max_symbol_position_contracts"];

    ASSERT_TRUE(caps.is_object());
    EXPECT_EQ(caps["ES"].get<double>(), 500.0);
    EXPECT_EQ(caps["NQ"].get<double>(), 250.0);
    EXPECT_EQ(caps["GC"].get<double>(), 100.0);
}

TEST_F(RiskLimitsTest, EnvelopeSurvivesJsonbRoundtrip) {
    // store_risk_limits binds limits.dump() as the $3::jsonb parameter; the
    // consumer parses it back. dump -> parse must be lossless for this shape.
    auto original = make_published_envelope();
    auto restored = nlohmann::json::parse(original.dump());

    EXPECT_EQ(restored, original);
    EXPECT_EQ(restored["max_symbol_position_contracts"]["ES"].get<double>(), 500.0);
    EXPECT_EQ(restored["max_gross_leverage"].get<double>(), 2.0);
}

TEST_F(RiskLimitsTest, EmptySymbolMapIsStillAValidEnvelope) {
    nlohmann::json limits;
    limits["max_symbol_position_contracts"] = nlohmann::json::object();
    limits["max_gross_leverage"] = 2.0;
    limits["max_net_leverage"] = 1.5;

    auto restored = nlohmann::json::parse(limits.dump());
    EXPECT_TRUE(restored["max_symbol_position_contracts"].is_object());
    EXPECT_TRUE(restored["max_symbol_position_contracts"].empty());
}
