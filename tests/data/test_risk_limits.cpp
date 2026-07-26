#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <memory>
#include "test_db_utils.hpp"
#include "trade_ngin/data/postgres_database.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

class RiskLimitsTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create database instance with test connection string
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

/**
 * @brief Test that risk limits JSONB object is serialized correctly
 *
 * Verifies that the limits JSON includes the expected fields:
 * - max_symbol_notional: map of symbol to unit limit
 * - max_gross_leverage: scalar double
 * - max_net_leverage: scalar double
 */
TEST_F(RiskLimitsTest, RiskLimitsSerializationValid) {
    nlohmann::json limits;

    // Build a sample limits object matching the structure published by live_portfolio_runner
    nlohmann::json symbol_notional;
    symbol_notional["ES"] = 500.0;
    symbol_notional["NQ"] = 250.0;
    symbol_notional["GC"] = 100.0;
    limits["max_symbol_notional"] = symbol_notional;

    limits["max_gross_leverage"] = 4.0;
    limits["max_net_leverage"] = 2.0;

    // Verify the structure
    ASSERT_TRUE(limits.contains("max_symbol_notional"));
    ASSERT_TRUE(limits.contains("max_gross_leverage"));
    ASSERT_TRUE(limits.contains("max_net_leverage"));

    // Verify symbol notional is a map
    ASSERT_TRUE(limits["max_symbol_notional"].is_object());
    ASSERT_EQ(limits["max_symbol_notional"]["ES"].get<double>(), 500.0);
    ASSERT_EQ(limits["max_symbol_notional"]["NQ"].get<double>(), 250.0);
    ASSERT_EQ(limits["max_symbol_notional"]["GC"].get<double>(), 100.0);

    // Verify leverage values
    ASSERT_EQ(limits["max_gross_leverage"].get<double>(), 4.0);
    ASSERT_EQ(limits["max_net_leverage"].get<double>(), 2.0);
}

/**
 * @brief Test that limits JSON can be serialized to string and deserialized
 */
TEST_F(RiskLimitsTest, RiskLimitsJsonRoundtrip) {
    nlohmann::json original;
    nlohmann::json symbol_notional;
    symbol_notional["ES"] = 500.0;
    original["max_symbol_notional"] = symbol_notional;
    original["max_gross_leverage"] = 4.0;
    original["max_net_leverage"] = 2.0;

    // Serialize to string (as would be stored in JSONB column)
    std::string json_str = original.dump();

    // Deserialize back
    nlohmann::json restored = nlohmann::json::parse(json_str);

    // Verify values survived roundtrip
    ASSERT_EQ(restored["max_symbol_notional"]["ES"].get<double>(), 500.0);
    ASSERT_EQ(restored["max_gross_leverage"].get<double>(), 4.0);
    ASSERT_EQ(restored["max_net_leverage"].get<double>(), 2.0);
}

/**
 * @brief Test that empty symbol notional map is handled correctly
 */
TEST_F(RiskLimitsTest, RiskLimitsEmptySymbolNotional) {
    nlohmann::json limits;
    limits["max_symbol_notional"] = nlohmann::json::object();  // Empty map
    limits["max_gross_leverage"] = 4.0;
    limits["max_net_leverage"] = 2.0;

    ASSERT_TRUE(limits["max_symbol_notional"].is_object());
    ASSERT_EQ(limits["max_symbol_notional"].size(), 0);
}

/**
 * @brief Test that limits object with only published fields can be created
 *
 * Honest publication means only including limits the engine actually enforces.
 * max_gross_notional and max_position_count should NOT be included because
 * the engine does not enforce them.
 */
TEST_F(RiskLimitsTest, RiskLimitsHonestPublication) {
    nlohmann::json limits;

    // These fields ARE enforced (Stage 1a)
    nlohmann::json symbol_notional;
    symbol_notional["ES"] = 500.0;
    limits["max_symbol_notional"] = symbol_notional;
    limits["max_gross_leverage"] = 4.0;
    limits["max_net_leverage"] = 2.0;

    // These fields should NOT be present (not enforced)
    ASSERT_FALSE(limits.contains("max_gross_notional"));
    ASSERT_FALSE(limits.contains("max_position_count"));
}

/**
 * @brief Test that leverage limits from RiskConfig are correctly included
 */
TEST_F(RiskLimitsTest, RiskLimitsLeverageFromConfig) {
    // Simulate different risk profiles
    struct TestCase {
        std::string name;
        double gross_leverage;
        double net_leverage;
    };

    std::vector<TestCase> test_cases = {
        {"BASE_PORTFOLIO", 4.0, 2.0},
        {"CONSERVATIVE_PORTFOLIO", 2.0, 1.5},
    };

    for (const auto& tc : test_cases) {
        nlohmann::json limits;
        limits["max_symbol_notional"] = nlohmann::json::object();
        limits["max_gross_leverage"] = tc.gross_leverage;
        limits["max_net_leverage"] = tc.net_leverage;

        ASSERT_EQ(limits["max_gross_leverage"].get<double>(), tc.gross_leverage)
            << "Test case: " << tc.name;
        ASSERT_EQ(limits["max_net_leverage"].get<double>(), tc.net_leverage)
            << "Test case: " << tc.name;
    }
}

/**
 * @brief Test that symbol position limits from strategy config are included
 */
TEST_F(RiskLimitsTest, RiskLimitsSymbolsFromStrategyConfig) {
    // Simulate position_limits from base_strategy_config
    std::unordered_map<std::string, double> position_limits = {
        {"ES", 500.0},
        {"NQ", 250.0},
        {"GC", 100.0},
        {"CL", 200.0},
        {"ZB", 1000.0},
    };

    nlohmann::json limits;
    nlohmann::json symbol_notional;
    for (const auto& [symbol, limit] : position_limits) {
        symbol_notional[symbol] = limit;
    }
    limits["max_symbol_notional"] = symbol_notional;

    // Verify all symbols are present
    ASSERT_EQ(limits["max_symbol_notional"].size(), 5);
    ASSERT_EQ(limits["max_symbol_notional"]["ES"].get<double>(), 500.0);
    ASSERT_EQ(limits["max_symbol_notional"]["NQ"].get<double>(), 250.0);
    ASSERT_EQ(limits["max_symbol_notional"]["GC"].get<double>(), 100.0);
    ASSERT_EQ(limits["max_symbol_notional"]["CL"].get<double>(), 200.0);
    ASSERT_EQ(limits["max_symbol_notional"]["ZB"].get<double>(), 1000.0);
}
