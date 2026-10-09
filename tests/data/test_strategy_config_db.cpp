// QT plan E2 against a real PostgreSQL (migration 022): the active strategy_config row is read
// for the right portfolio, no row is "no row" and not an error, a failed lookup is an error, and
// the settings used land on the day's live_run_metadata row (credential-free, exactly one row).
// TRADE_NGIN_TEST_DSN only; throwaway portfolio ids; every row deleted at the end.
#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/postgres_database.hpp"

using namespace trade_ngin;

namespace {
std::string dsn() {
    const char* v = std::getenv("TRADE_NGIN_TEST_DSN");
    return v && *v ? std::string(v) : std::string();
}
constexpr const char* kBook = "E2_STRATEGY_CONFIG_TEST";
constexpr const char* kOther = "E2_STRATEGY_CONFIG_OTHER";
const Timestamp kDay = std::chrono::system_clock::from_time_t(1791417600 + 12 * 3600);  // 2026-10-08 12:00 UTC
}  // namespace

class StrategyConfigDbTest : public ::testing::Test {
protected:
    void SetUp() override {
        conn_ = dsn();
        if (conn_.empty()) GTEST_SKIP() << "TRADE_NGIN_TEST_DSN not set";
        db_ = std::make_shared<PostgresDatabase>(conn_);
        if (db_->connect().is_error()) GTEST_SKIP() << "database unreachable";
        pqxx::connection c(conn_);
        pqxx::work w(c);
        if (w.exec("SELECT to_regclass('trading.strategy_config') IS NULL")[0][0].as<bool>() ||
            w.exec("SELECT count(*) FROM information_schema.columns WHERE table_schema='trading' "
                   "AND table_name='live_run_metadata' AND column_name='settings_used'")[0][0]
                    .as<int>() == 0) {
            GTEST_SKIP() << "migration 022 not applied";
        }
        purge(w);
        w.commit();
    }
    void TearDown() override {
        if (!conn_.empty() && db_) {
            pqxx::connection c(conn_);
            pqxx::work w(c);
            purge(w);
            w.commit();
        }
        if (db_) db_->disconnect();
    }
    static void purge(pqxx::work& w) {
        w.exec("DELETE FROM trading.strategy_config WHERE portfolio_id IN ('" + std::string(kBook) +
               "', '" + kOther + "')");
        w.exec("DELETE FROM trading.live_run_metadata WHERE portfolio_id = '" + std::string(kBook) +
               "'");
    }
    void exec(const std::string& sql) {
        pqxx::connection c(conn_);
        pqxx::work w(c);
        w.exec(sql);
        w.commit();
    }
    std::string conn_;
    std::shared_ptr<PostgresDatabase> db_;
};

TEST_F(StrategyConfigDbTest, NoActiveRowIsNotAnError) {
    exec("INSERT INTO trading.strategy_config (portfolio_id, version, overrides, reason, created_by) "
         "VALUES ('" + std::string(kBook) + "', 1, '{\"live\": {\"historical_days\": 800}}', 'draft', 'dom')");
    auto r = db_->get_active_strategy_config(kBook);
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    EXPECT_FALSE(r.value().has_value()) << "an inactive row was read as active";
}

TEST_F(StrategyConfigDbTest, TheActiveRowOfThisPortfolioIsRead) {
    exec("INSERT INTO trading.strategy_config (portfolio_id, version, overrides, reason, created_by, is_active) VALUES "
         "('" + std::string(kBook) + "', 1, '{\"live\": {\"historical_days\": 800}}', 'first', 'dom', false), "
         "('" + std::string(kBook) + "', 2, '{\"risk\": {\"max_drawdown\": 0.25}}', 'tighter drawdown', 'dom', true), "
         "('" + std::string(kOther) + "', 1, '{\"risk\": {\"max_drawdown\": 0.5}}', 'other book', 'x', true)");
    auto r = db_->get_active_strategy_config(kBook);
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    ASSERT_TRUE(r.value().has_value());
    EXPECT_EQ(r.value()->portfolio_id, kBook);
    EXPECT_EQ(r.value()->version, 2);
    EXPECT_EQ(r.value()->overrides, nlohmann::json({{"risk", {{"max_drawdown", 0.25}}}}));
    EXPECT_EQ(r.value()->reason, "tighter drawdown");
    EXPECT_EQ(r.value()->created_by, "dom");
}

TEST_F(StrategyConfigDbTest, AFailedLookupIsAnErrorNeverNoRow) {
    db_->disconnect();
    auto r = db_->get_active_strategy_config(kBook);
    EXPECT_TRUE(r.is_error()) << "a lookup on a closed connection read as 'no active row'";
}

TEST_F(StrategyConfigDbTest, TheSettingsUsedLandOnTheDaysRunRow) {
    ASSERT_TRUE(db_->store_live_run_metadata(kDay, "LIVE_E2_TEST", kBook, {{"T", 1.0}},
                                             {{"total_capital", 1}}, nlohmann::json::object())
                    .is_ok());
    const nlohmann::json used = {{"strategy_config_version", 2},
                                 {"config", {{"portfolio_id", kBook}, {"risk", {{"max_drawdown", 0.25}}}}}};
    auto w = db_->store_settings_used(kDay, "LIVE_E2_TEST", kBook, used);
    ASSERT_TRUE(w.is_ok()) << w.error()->what();
    pqxx::connection c(conn_);
    pqxx::work t(c);
    auto r = t.exec("SELECT settings_used::text, portfolio_config::text FROM trading.live_run_metadata "
                    "WHERE portfolio_id = '" + std::string(kBook) + "'");
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(nlohmann::json::parse(r[0][0].as<std::string>()), used);
    EXPECT_EQ(nlohmann::json::parse(r[0][1].as<std::string>()), nlohmann::json({{"total_capital", 1}}));
    t.commit();
    // A later mark of the same row (store_live_run_metadata's upsert) keeps the settings used.
    ASSERT_TRUE(db_->store_live_run_metadata(kDay, "LIVE_E2_TEST", kBook, {{"T", 1.0}},
                                             {{"total_capital", 1}, {"risk_refusal", {{"m", 1}}}},
                                             nlohmann::json::object())
                    .is_ok());
    pqxx::work t2(c);
    auto r2 = t2.exec("SELECT settings_used::text FROM trading.live_run_metadata WHERE portfolio_id = '" +
                      std::string(kBook) + "'");
    EXPECT_EQ(nlohmann::json::parse(r2[0][0].as<std::string>()), used);
}

TEST_F(StrategyConfigDbTest, NoRunRowOrACredentialRefusesTheWrite) {
    auto none = db_->store_settings_used(kDay, "LIVE_E2_TEST", kBook,
                                         {{"strategy_config_version", nullptr}, {"config", nlohmann::json::object()}});
    EXPECT_TRUE(none.is_error()) << "a write that touched no row reported success";
    ASSERT_TRUE(db_->store_live_run_metadata(kDay, "LIVE_E2_TEST", kBook, {{"T", 1.0}},
                                             nlohmann::json::object(), nlohmann::json::object())
                    .is_ok());
    auto secret = db_->store_settings_used(
        kDay, "LIVE_E2_TEST", kBook,
        {{"strategy_config_version", nullptr}, {"config", {{"database", {{"password", "sk-SENSITIVE"}}}}}});
    ASSERT_TRUE(secret.is_error());
    EXPECT_EQ(std::string(secret.error()->what()).find("sk-SENSITIVE"), std::string::npos);
    pqxx::connection c(conn_);
    pqxx::work t(c);
    EXPECT_TRUE(t.exec("SELECT settings_used IS NULL FROM trading.live_run_metadata WHERE portfolio_id = '" +
                       std::string(kBook) + "'")[0][0]
                    .as<bool>());
}
