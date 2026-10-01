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

TEST_F(InvestorBookOnboardingPg, CombinedEquityBookAcceptsEveryConfiguredOwnerBatch) {
    Timestamp source_day;
    ASSERT_TRUE(core::parse_utc_date("2026-11-03", source_day));
    const nlohmann::json snapshot = {
        {"strategies",
         {{"ALPHA", {{"enabled_live", true}}},
          {"BETA", {{"enabled_live", true}}}}}
    };
    auto begun = db_->begin_live_publication(
        "LIVE_EQUITY_ALPHA_BETA", "INVESTOR_GAMMA", source_day,
        snapshot, true, "cpp-pg-test-version");
    ASSERT_TRUE(begun.is_ok()) << (begun.is_error() ? begun.error()->what() : "");
    ASSERT_FALSE(begun.value());
    ASSERT_EQ(db_->live_publication_mode(), LivePublicationMode::SystemInvestor);

    for (const std::string owner : {"ALPHA", "BETA"}) {
        const QtModelPositionBatch empty{
            "INVESTOR_GAMMA", "LIVE_EQUITY_ALPHA_BETA", owner,
            "2026-11-03", {}};
        auto stored = db_->store_model_position_batch(empty);
        EXPECT_TRUE(stored.is_ok())
            << owner << ": " << (stored.is_error() ? stored.error()->what() : "");
    }
    db_->abandon_live_publication("test_complete");
}

TEST_F(InvestorBookOnboardingPg,
       LiveAccountingContextLoadsEveryCumulativeFieldAndScopedDayCount) {
    const InvestorBookOnboarding request{
        "investor_accounting", "INVESTOR_ACCOUNTING", 1000.0, "2026-11-01",
        {"LIVE_EQUITY_ACCOUNTING"}, "cpp-accounting-test"};
    ASSERT_TRUE(db_->onboard_investor_book(request).is_ok());

    pqxx::connection connection(dsn_);
    pqxx::work transaction(connection);
    transaction.exec(
        "DELETE FROM trading.live_results WHERE portfolio_id=$1 AND strategy_id=$2",
        pqxx::params{request.portfolio_id, request.strategy_ids.front()});
    transaction.exec(
        "INSERT INTO trading.live_results "
        "(portfolio_id,strategy_id,date,portfolio_type,current_portfolio_value,"
        " total_pnl,total_realized_pnl,total_unrealized_pnl,total_transaction_costs,"
        " daily_realized_pnl,daily_transaction_costs,gross_notional,net_notional,"
        " margin_posted) VALUES "
        "($1,$2,'2026-11-01'::date,'system',1000,0,0,0,0,0,0,0,0,0),"
        "($1,$2,'2026-11-02'::date,'system',1020,20,50,-20,10,50,10,400,250,100)",
        pqxx::params{request.portfolio_id, request.strategy_ids.front()});
    transaction.commit();

    Timestamp target;
    ASSERT_TRUE(core::parse_utc_date("2026-11-03", target));
    auto loaded = db_->get_live_accounting_context(
        request.strategy_ids.front(), request.portfolio_id, target, "system");

    ASSERT_TRUE(loaded.is_ok())
        << (loaded.is_error() ? loaded.error()->what() : "");
    EXPECT_TRUE(loaded.value().previous_found);
    EXPECT_EQ(loaded.value().previous_source_day, "2026-11-02");
    EXPECT_DOUBLE_EQ(loaded.value().previous_current_portfolio_value, 1020.0);
    EXPECT_DOUBLE_EQ(loaded.value().previous_total_pnl, 20.0);
    EXPECT_DOUBLE_EQ(loaded.value().previous_total_realized_pnl, 50.0);
    EXPECT_DOUBLE_EQ(loaded.value().previous_total_unrealized_pnl, -20.0);
    EXPECT_DOUBLE_EQ(loaded.value().previous_total_transaction_costs, 10.0);
    EXPECT_DOUBLE_EQ(loaded.value().previous_daily_realized_pnl, 50.0);
    EXPECT_DOUBLE_EQ(loaded.value().previous_daily_transaction_costs, 10.0);
    EXPECT_DOUBLE_EQ(loaded.value().previous_gross_notional, 400.0);
    EXPECT_DOUBLE_EQ(loaded.value().previous_net_notional, 250.0);
    EXPECT_DOUBLE_EQ(loaded.value().previous_margin_posted, 100.0);
    EXPECT_TRUE(loaded.value().preceding_found);
    EXPECT_DOUBLE_EQ(loaded.value().preceding_current_portfolio_value, 1000.0);
    EXPECT_DOUBLE_EQ(loaded.value().preceding_total_pnl, 0.0);
    EXPECT_EQ(loaded.value().previous_trading_days, 2);
    EXPECT_EQ(loaded.value().trading_days, 3);
}

TEST_F(InvestorBookOnboardingPg,
       CorporateActionDedupIsRolledBackWithAnAbandonedInvestorPublication) {
    {
        pqxx::connection cleanup_connection(dsn_);
        pqxx::work cleanup(cleanup_connection);
        cleanup.exec(
            "DELETE FROM trading.corp_action_applied "
            "WHERE portfolio_id=$1 AND strategy_id=$2 AND strategy_name=$3 "
            "AND symbol=$4 AND action_type=$5 AND ex_date=$6::date",
            pqxx::params{"INVESTOR_GAMMA", "LIVE_EQUITY_ALPHA_BETA", "ALPHA",
                         "SYN", "SPLIT", "2026-11-04"});
        cleanup.commit();
    }
    Timestamp source_day;
    ASSERT_TRUE(core::parse_utc_date("2026-11-04", source_day));
    const nlohmann::json snapshot = {
        {"strategies",
         {{"ALPHA", {{"enabled_live", true}}},
          {"BETA", {{"enabled_live", true}}}}}
    };
    auto begun = db_->begin_live_publication(
        "LIVE_EQUITY_ALPHA_BETA", "INVESTOR_GAMMA", source_day,
        snapshot, true, "cpp-corp-action-atomicity");
    ASSERT_TRUE(begun.is_ok())
        << (begun.is_error() ? begun.error()->what() : "");
    ASSERT_FALSE(begun.value());
    ASSERT_EQ(db_->live_publication_mode(), LivePublicationMode::SystemInvestor);

    const PostgresDatabase::AppliedCorpActionRow row{
        "SYN", "SPLIT", "2026-11-04", 10.0, 0.0, 0.0,
        "2026-11-04", 2.0, true};
    auto staged = db_->store_applied_corp_actions(
        "INVESTOR_GAMMA", "LIVE_EQUITY_ALPHA_BETA", "ALPHA", {row});
    ASSERT_TRUE(staged.is_ok())
        << (staged.is_error() ? staged.error()->what() : "");
    EXPECT_EQ(scalar(
        "SELECT count(*) FROM trading.corp_action_applied "
        "WHERE portfolio_id='INVESTOR_GAMMA' "
        "AND strategy_id='LIVE_EQUITY_ALPHA_BETA' "
        "AND strategy_name='ALPHA' AND symbol='SYN' "
        "AND action_type='SPLIT' AND ex_date='2026-11-04'"), 0);

    db_->abandon_live_publication("atomicity_regression");
    EXPECT_EQ(scalar(
        "SELECT count(*) FROM trading.corp_action_applied "
        "WHERE portfolio_id='INVESTOR_GAMMA' "
        "AND strategy_id='LIVE_EQUITY_ALPHA_BETA' "
        "AND strategy_name='ALPHA' AND symbol='SYN' "
        "AND action_type='SPLIT' AND ex_date='2026-11-04'"), 0);
}

}  // namespace
