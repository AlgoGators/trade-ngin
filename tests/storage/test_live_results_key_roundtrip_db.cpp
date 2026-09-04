// FIX-0 end-to-end: does what LiveResultsManager writes come back through the read the
// runner actually performs?
//
// The unit tests pin the key at the store_* call boundary against a mock. That proves the
// threading but not the round trip -- the defect was precisely that write key and read key
// disagreed, and only a real server shows whether a row written one way is found the other
// way. So: write positions through LiveResultsManager under a scratch identity, then read
// them back with db->load_positions_by_date() using the same three columns the equity
// runner reads with, and separately confirm the pre-FIX-0 key finds nothing.
//
// Writes touch trading.positions but only ever under the scratch triple below, which no
// live or backtest book uses; every row is deleted in TearDown. Nothing production-keyed
// is read, written, or removed.
//
// Reachability gate matches tests/data/test_db_transaction_atomicity.cpp:
//   * TRADE_NGIN_REQUIRE_DB=1 -- unreachable database FAILS rather than skips.
//   * unset -- skip (local dev without a server).

#include <gtest/gtest.h>
#include <pqxx/pqxx>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/storage/live_results_manager.hpp"

using namespace trade_ngin;

namespace {

// Scratch identity. Deliberately unlike any configured book so a stray row cannot be
// mistaken for, or collide with, real results.
constexpr const char* kScratchStrategyId = "FIX0_ROUNDTRIP_PROBE_ID";
constexpr const char* kScratchStrategyName = "FIX0_ROUNDTRIP_PROBE_NAME";
constexpr const char* kScratchPortfolio = "FIX0_ROUNDTRIP_PROBE_PORTFOLIO";

std::string discover_connection_string() {
    namespace fs = std::filesystem;
    fs::path dir = fs::current_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        fs::path candidate = dir / "config" / "defaults.json";
        if (fs::exists(candidate)) {
            try {
                std::ifstream in(candidate);
                nlohmann::json j = nlohmann::json::parse(in);
                const auto& d = j.at("database");
                return "postgresql://" + d.at("username").get<std::string>() + ":" +
                       d.at("password").get<std::string>() + "@" +
                       d.at("host").get<std::string>() + ":" + d.at("port").get<std::string>() +
                       "/" + d.at("name").get<std::string>();
            } catch (const std::exception&) {
                return {};
            }
        }
        dir = dir.parent_path();
    }
    return {};
}

Timestamp date_at(int year, int month, int day) {
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = 12;
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}

Position make_position(const std::string& symbol, double qty, double avg_price) {
    Position p;
    p.symbol = symbol;
    p.quantity = qty;
    p.average_price = avg_price;
    p.unrealized_pnl = 0.0;
    p.realized_pnl = 0.0;
    p.last_update = date_at(2026, 3, 16);
    return p;
}

}  // namespace

class LiveResultsKeyRoundTripTest : public ::testing::Test {
protected:
    void SetUp() override {
        const bool require_db = [] {
            const char* v = std::getenv("TRADE_NGIN_REQUIRE_DB");
            return v && std::string(v) == "1";
        }();

        conn_ = discover_connection_string();
        if (conn_.empty()) {
            if (require_db) {
                FAIL() << "TRADE_NGIN_REQUIRE_DB=1 but config/defaults.json is not reachable, "
                          "so the FIX-0 write/read key agreement goes unverified";
            }
            GTEST_SKIP() << "config/defaults.json not reachable; no database to exercise";
        }
        db_ = std::make_shared<PostgresDatabase>(conn_);
        auto connected = db_->connect();
        if (connected.is_error() || !db_->is_connected()) {
            if (require_db) {
                FAIL() << "TRADE_NGIN_REQUIRE_DB=1 but the database is unreachable, so the "
                          "FIX-0 write/read key agreement goes unverified";
            }
            GTEST_SKIP() << "database unreachable; key agreement needs a real server";
        }
        clear_scratch_rows();
    }

    void TearDown() override {
        if (db_ && db_->is_connected()) {
            clear_scratch_rows();
            db_->disconnect();
        }
    }

    // Cleanup goes through its own connection so it never depends on the class under test.
    void clear_scratch_rows() {
        try {
            pqxx::connection c(conn_);
            pqxx::work w(c);
            w.exec(std::string("DELETE FROM trading.positions WHERE strategy_id = ") +
                   w.quote(kScratchStrategyId) + " OR portfolio_id = " +
                   w.quote(kScratchPortfolio));
            w.commit();
        } catch (const std::exception&) {
            // Nothing to clean, or no table -- the assertions below will say so.
        }
    }

    std::string conn_;
    std::shared_ptr<PostgresDatabase> db_;
};

TEST_F(LiveResultsKeyRoundTripTest, PositionsWrittenByManagerAreFoundByTheRunnersRead) {
    const auto date = date_at(2026, 3, 16);

    LiveResultsManager mgr(db_, /*store_enabled=*/true, kScratchStrategyId, kScratchPortfolio,
                           kScratchStrategyName);
    mgr.set_positions({make_position("AAPL", 12.0, 190.5), make_position("MSFT", -4.0, 410.25)});
    ASSERT_TRUE(mgr.save_positions_snapshot(date).is_ok());

    // The read the equity runner performs: all three key columns, explicitly.
    auto loaded = db_->load_positions_by_date(kScratchStrategyId, kScratchStrategyName,
                                              kScratchPortfolio, date, "trading.positions");
    ASSERT_TRUE(loaded.is_ok()) << loaded.error()->what();
    const auto& book = loaded.value();
    ASSERT_EQ(book.size(), 2u) << "write key and read key disagree; the book comes back empty";
    ASSERT_TRUE(book.count("AAPL"));
    ASSERT_TRUE(book.count("MSFT"));
    EXPECT_NEAR(static_cast<double>(book.at("AAPL").quantity), 12.0, 1e-9);
    EXPECT_NEAR(static_cast<double>(book.at("AAPL").average_price), 190.5, 1e-9);
    EXPECT_NEAR(static_cast<double>(book.at("MSFT").quantity), -4.0, 1e-9);
}

TEST_F(LiveResultsKeyRoundTripTest, ThePreFixKeyFindsNothing) {
    const auto date = date_at(2026, 3, 16);

    LiveResultsManager mgr(db_, /*store_enabled=*/true, kScratchStrategyId, kScratchPortfolio,
                           kScratchStrategyName);
    mgr.set_positions({make_position("AAPL", 12.0, 190.5)});
    ASSERT_TRUE(mgr.save_positions_snapshot(date).is_ok());

    // Pre-FIX-0 the row landed under (id, id, BASE_PORTFOLIO). Reading that way must now
    // come back empty -- if it does not, the id is still being reused as the name, or the
    // configured portfolio is still being ignored.
    auto wrong_name = db_->load_positions_by_date(kScratchStrategyId, kScratchStrategyId,
                                                  kScratchPortfolio, date, "trading.positions");
    ASSERT_TRUE(wrong_name.is_ok()) << wrong_name.error()->what();
    EXPECT_TRUE(wrong_name.value().empty()) << "strategy_id is still being written as the name";

    auto wrong_portfolio = db_->load_positions_by_date(
        kScratchStrategyId, kScratchStrategyName, "BASE_PORTFOLIO", date, "trading.positions");
    ASSERT_TRUE(wrong_portfolio.is_ok()) << wrong_portfolio.error()->what();
    EXPECT_TRUE(wrong_portfolio.value().empty())
        << "the row landed on the futures book instead of the configured portfolio";
}
