// tests/live/corp_actions/test_broker_frame_db.cpp
//
// F-8 -- `AppliedRowCarriesTheRatioNeededToInvertIt`.
//
// The pure tests in test_broker_frame.cpp prove the arithmetic. This one proves the LEDGER
// can feed it: that trading.corp_action_applied actually carries `basis_ratio` (migration
// 006), that the write path stores it, that the read path distinguishes a stored ratio from
// a NULL one, and that the value round-trips well enough to invert a basis to 1e-6 relative.
//
// Why it needs the database. The gap this closes is that the adjusted->raw chain could only
// be recomputed by joining every applied event back to equities_data.ohlcv_1d for its raw
// ex-date close -- and NOT AT ALL once the position closes, because a closed row carries no
// basis (AVERAGE_PRICE_LIFECYCLE.md rule 5) and the dedup row is the last surviving record
// of the chain. A mock cannot show the column exists.
//
// Writes: this test inserts and then deletes rows under a synthetic portfolio_id that no
// runner uses (`F8_BROKER_FRAME_TEST`). It touches no production key and no other table.
//
// Reachability gate matches tests/live/corp_actions/test_corp_action_query_bounds_db.cpp:
//   * TRADE_NGIN_REQUIRE_DB=1 -- unreachable database FAILS rather than skips.
//   * unset -- skip (local dev without a server).

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/live/broker_frame.hpp"

using namespace trade_ngin;

namespace {

constexpr const char* kTestPortfolio = "F8_BROKER_FRAME_TEST";
constexpr const char* kTestStrategyId = "F8_TEST_STRATEGY";
constexpr const char* kTestStrategyName = "F8_TEST_NAME";

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

}  // namespace

class BrokerFrameDbTest : public ::testing::Test {
protected:
    void SetUp() override {
        const bool require_db = [] {
            const char* v = std::getenv("TRADE_NGIN_REQUIRE_DB");
            return v && std::string(v) == "1";
        }();

        conn_string_ = discover_connection_string();
        if (conn_string_.empty()) {
            if (require_db) {
                FAIL() << "TRADE_NGIN_REQUIRE_DB=1 but config/defaults.json is not reachable, "
                          "so the basis_ratio ledger column goes unverified";
            }
            GTEST_SKIP() << "config/defaults.json not reachable; no database to exercise";
        }
        db_ = std::make_shared<PostgresDatabase>(conn_string_);
        auto connected = db_->connect();
        if (connected.is_error() || !db_->is_connected()) {
            if (require_db) {
                FAIL() << "TRADE_NGIN_REQUIRE_DB=1 but the database is unreachable, so the "
                          "basis_ratio ledger column goes unverified";
            }
            GTEST_SKIP() << "database unreachable; this needs the real table";
        }
        purge();
    }

    void TearDown() override {
        if (!conn_string_.empty()) purge();
        if (db_ && db_->is_connected()) db_->disconnect();
    }

    // Scoped to the synthetic portfolio only. Runs before AND after, so a crashed run
    // cannot leave rows behind for the next one to read.
    void purge() {
        try {
            pqxx::connection c(conn_string_);
            pqxx::work w(c);
            w.exec_params("DELETE FROM trading.corp_action_applied WHERE portfolio_id = $1",
                          kTestPortfolio);
            w.commit();
        } catch (const std::exception&) {
            // Nothing to clean, or no table: the tests below report that themselves.
        }
    }

    std::string conn_string_;
    std::shared_ptr<PostgresDatabase> db_;
};

TEST_F(BrokerFrameDbTest, AppliedRowCarriesTheRatioNeededToInvertIt) {
    // 1. The column exists. Without it the chain is not in the ledger at all, and every
    //    reconciliation has to rebuild it from ohlcv_1d -- impossible for a closed row.
    {
        pqxx::connection c(conn_string_);
        pqxx::work w(c);
        auto r = w.exec(
            "SELECT data_type, is_nullable FROM information_schema.columns "
            "WHERE table_schema='trading' AND table_name='corp_action_applied' "
            "  AND column_name='basis_ratio'");
        w.commit();
        ASSERT_EQ(r.size(), 1u)
            << "trading.corp_action_applied.basis_ratio is absent -- apply "
               "migrations/006_corp_action_applied_basis_ratio.sql. Without it the adjusted "
               "cost basis cannot be inverted to the broker frame from the ledger (F-8).";
        EXPECT_EQ(std::string(r[0][0].c_str()), "double precision");
        EXPECT_EQ(std::string(r[0][1].c_str()), "YES")
            << "it must stay nullable: NULL is how a pre-006 row says UNKNOWN";
    }

    // 2. The write path stores it and the read path returns it, for both a dividend and a
    //    split, alongside a row that deliberately carries none.
    std::vector<PostgresDatabase::AppliedCorpActionRow> rows;
    {
        PostgresDatabase::AppliedCorpActionRow div;
        div.symbol = "F8DIV";
        div.action_type = "DIVIDEND";
        div.ex_date = "2026-04-15";
        div.qty_held = 100.0;
        div.dividend_per_share = 0.63;
        div.total_cash = 63.0;
        div.run_date = "2026-04-16";
        div.basis_ratio = broker_frame::dividend_basis_ratio(0.63, 100.0);  // 1.0063
        div.basis_ratio_known = true;
        rows.push_back(div);

        PostgresDatabase::AppliedCorpActionRow spl;
        spl.symbol = "F8DIV";
        spl.action_type = "SPLIT";
        spl.ex_date = "2026-08-11";
        spl.run_date = "2026-08-12";
        spl.basis_ratio = 4.0;
        spl.basis_ratio_known = true;
        rows.push_back(spl);

        // A TERMINATION restates no basis, so it records no ratio. The read must report it
        // as UNKNOWN rather than as a 1.0 a chain could be walked through.
        PostgresDatabase::AppliedCorpActionRow term;
        term.symbol = "F8DIV";
        term.action_type = "TERMINATION";
        term.ex_date = "2026-09-01";
        term.run_date = "2026-09-02";
        term.basis_ratio_known = false;
        rows.push_back(term);
    }

    auto stored = db_->store_applied_corp_actions(kTestPortfolio, kTestStrategyId,
                                                  kTestStrategyName, rows);
    ASSERT_TRUE(stored.is_ok()) << (stored.is_error() ? stored.error()->what() : "");

    auto loaded =
        db_->load_applied_corp_actions(kTestPortfolio, kTestStrategyId, kTestStrategyName);
    ASSERT_TRUE(loaded.is_ok()) << (loaded.is_error() ? loaded.error()->what() : "");
    ASSERT_EQ(loaded.value().size(), 3u);

    std::vector<broker_frame::AppliedEvent> chain;
    bool saw_termination = false;
    for (const auto& r : loaded.value()) {
        if (r.action_type == "DIVIDEND") {
            EXPECT_TRUE(r.basis_ratio_known);
            EXPECT_NEAR(r.basis_ratio, 1.0063, 1e-12);
        } else if (r.action_type == "SPLIT") {
            EXPECT_TRUE(r.basis_ratio_known);
            EXPECT_DOUBLE_EQ(r.basis_ratio, 4.0);
        } else if (r.action_type == "TERMINATION") {
            saw_termination = true;
            EXPECT_FALSE(r.basis_ratio_known)
                << "a NULL basis_ratio must read back as UNKNOWN, not as 1.0";
        }
        broker_frame::AppliedEvent ev;
        ev.symbol = r.symbol;
        ev.ex_date = r.ex_date;
        ev.action_type = r.action_type;
        ev.ratio = r.basis_ratio;
        ev.ratio_known = r.basis_ratio_known;
        ev.dividend_per_share = r.dividend_per_share;
        chain.push_back(std::move(ev));
    }
    EXPECT_TRUE(saw_termination);

    // 3. The stored chain inverts the basis. The book holds 100.00 after the dividend
    //    restatement; the broker's basis is 100.63. The split contributes nothing (both
    //    frames divide by 4) and the ratio-less TERMINATION is not a dividend, so it does
    //    not poison the chain either.
    const double b_book = 100.0;
    const double raw = broker_frame::raw_basis(b_book, chain);
    ASSERT_TRUE(broker_frame::basis_is_known(raw))
        << "the chain read back from the ledger must be invertible";
    EXPECT_NEAR(raw, 100.63, 1e-6 * 100.63);

    // 4. A dividend row with a NULL ratio -- what every row written before migration 006
    //    looks like -- makes the chain unanswerable, which is the whole point of storing it.
    auto legacy = chain;
    for (auto& ev : legacy) {
        if (broker_frame::is_dividend(ev)) ev.ratio_known = false;
    }
    EXPECT_FALSE(broker_frame::basis_is_known(broker_frame::raw_basis(b_book, legacy)));
}
