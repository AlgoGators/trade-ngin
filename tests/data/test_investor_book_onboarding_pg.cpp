#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/core/time_utils.hpp"

using namespace trade_ngin;

namespace {

std::string owned_dsn() {
    const char* raw = std::getenv("INVESTOR_BOOK_TEST_DSN");
    if (raw == nullptr) throw std::runtime_error("INVESTOR_BOOK_TEST_DSN is required");
    const std::string dsn(raw);
    if (!dsn.starts_with("host=/tmp/algolens-repair-pg-") ||
        dsn.find(" dbname=investor_book_test") == std::string::npos ||
        dsn.find("hostaddr") != std::string::npos || dsn.find("service=") != std::string::npos) {
        throw std::runtime_error("INVESTOR_BOOK_TEST_DSN is not an owned disposable database");
    }
    return dsn;
}

class InvestorBookOnboardingPg : public ::testing::Test {
protected:
    void SetUp() override {
        dsn_ = owned_dsn();
        db_ = std::make_unique<PostgresDatabase>(dsn_);
        ASSERT_TRUE(db_->connect().is_ok());
    }

    long scalar(const std::string& query) const {
        pqxx::connection connection(dsn_);
        pqxx::read_transaction transaction(connection);
        return transaction.exec(query)[0][0].as<long>();
    }

    std::string dsn_;
    std::unique_ptr<PostgresDatabase> db_;
};

TEST_F(InvestorBookOnboardingPg, IdenticalReplayIsStableAndConflictIsAtomic) {
    const InvestorBookOnboarding request{
        "investor_gamma", "INVESTOR_GAMMA", 750'000.0, "2026-11-01",
        {"LIVE_EQUITY_ALPHA_BETA", "LIVE_TREND_FOLLOWING"}, "cpp-pg-test"};
    auto first = db_->onboard_investor_book(request);
    auto replay = db_->onboard_investor_book(request);
    ASSERT_TRUE(first.is_ok()) << (first.is_error() ? first.error()->what() : "");
    ASSERT_TRUE(replay.is_ok()) << (replay.is_error() ? replay.error()->what() : "");
    EXPECT_EQ(first.value(), replay.value());

    auto conflict_request = request;
    conflict_request.initial_capital += 1.0;
    auto conflict = db_->onboard_investor_book(conflict_request);
    ASSERT_TRUE(conflict.is_error());
    EXPECT_EQ(conflict.error()->code(), ErrorCode::INVALID_ARGUMENT);

    EXPECT_EQ(scalar("SELECT count(*) FROM trading.investor_books "
                     "WHERE portfolio_id='INVESTOR_GAMMA' AND initial_capital=750000"), 1);
    EXPECT_EQ(scalar("SELECT count(*) FROM trading.investor_book_strategies "
                     "WHERE portfolio_id='INVESTOR_GAMMA'"), 2);
    EXPECT_EQ(scalar("SELECT count(*) FROM trading.strategy_trading_days_metadata "
                     "WHERE portfolio_id='INVESTOR_GAMMA' AND live_start_date='2026-11-01'"), 2);
    EXPECT_EQ(scalar("SELECT count(*) FROM trading.investor_books "
                     "WHERE portfolio_id='INVESTOR_GAMMA' AND model_stream='system'"), 1);
}

TEST_F(InvestorBookOnboardingPg, RegisteredBookAutomaticallyUsesSystemOnlyMode) {
    Timestamp source_day;
    ASSERT_TRUE(core::parse_utc_date("2026-11-02", source_day));
    const nlohmann::json snapshot = {
        {"strategies", {{"Trend", {{"enabled_live", true}}}}}
    };
    auto begun = db_->begin_live_publication(
        "LIVE_TREND_FOLLOWING", "INVESTOR_GAMMA", source_day,
        snapshot, true, "cpp-pg-test-version");
    ASSERT_TRUE(begun.is_ok()) << (begun.is_error() ? begun.error()->what() : "");
    EXPECT_FALSE(begun.value());
    ASSERT_TRUE(db_->live_publication_mode().has_value());
    EXPECT_EQ(*db_->live_publication_mode(), LivePublicationMode::SystemInvestor);

    auto qt_seed = db_->seed_qt_positions_from_system(
        "LIVE_TREND_FOLLOWING", "Trend", "INVESTOR_GAMMA", "2026-11-02");
    ASSERT_TRUE(qt_seed.is_error());
    EXPECT_EQ(qt_seed.error()->code(), ErrorCode::INVALID_ARGUMENT);
    db_->abandon_live_publication("test_complete");
}

}  // namespace
