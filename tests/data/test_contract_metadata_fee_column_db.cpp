// CM1 / migration 014: the contract metadata loader carries metadata.contract_metadata's
// "Fee Per Contract" column into the table the instrument registry reads, by name, when the
// database has it, and loads exactly as before when it does not.
//
// Read-only: one SELECT on information_schema and the loader's own SELECT on
// metadata.contract_metadata. Nothing is written.
//
// Which branch runs depends on the database behind TRADE_NGIN_TEST_DSN: with migration 014 applied
// the loaded table must carry the column with the stored value on every row; without it the
// loaded table must not carry it. The RED/GREEN evidence runs against a throwaway database
// holding a copy of the metadata table with 014 applied.
//
// Reachability gate as tests/storage/test_live_results_key_roundtrip_db.cpp:
//   * TRADE_NGIN_REQUIRE_DB=1 -- unreachable database FAILS rather than skips.
//   * unset -- skip (local dev without a server).

#include <gtest/gtest.h>
#include <arrow/api.h>
#include <pqxx/pqxx>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>

#include "trade_ngin/data/postgres_database.hpp"

using namespace trade_ngin;

namespace {

std::string test_dsn() {
    const char* dsn = std::getenv("TRADE_NGIN_TEST_DSN");
    return (dsn && *dsn) ? std::string(dsn) : std::string();
}

}  // namespace

class ContractMetadataFeeColumnDbTest : public ::testing::Test {
protected:
    void SetUp() override {
        const char* v = std::getenv("TRADE_NGIN_REQUIRE_DB");
        const bool require_db = v && std::string(v) == "1";
        conn_ = test_dsn();
        if (conn_.empty()) {
            if (require_db) FAIL() << "TRADE_NGIN_REQUIRE_DB=1 but TRADE_NGIN_TEST_DSN is not set";
            GTEST_SKIP() << "TRADE_NGIN_TEST_DSN not set; no database to exercise";
        }
        db_ = std::make_shared<PostgresDatabase>(conn_);
        auto connected = db_->connect();
        if (connected.is_error() || !db_->is_connected()) {
            if (require_db) FAIL() << "TRADE_NGIN_REQUIRE_DB=1 but the database is unreachable";
            GTEST_SKIP() << "database unreachable";
        }
    }
    void TearDown() override {
        if (db_ && db_->is_connected()) db_->disconnect();
    }
    std::string conn_;
    std::shared_ptr<PostgresDatabase> db_;
};

TEST_F(ContractMetadataFeeColumnDbTest, LoaderCarriesTheFeeColumnExactlyWhenTheTableHasIt) {
    // The database's own truth, through a separate connection.
    bool has_column = false;
    std::map<std::string, double> stored;  // Databento Symbol -> fee
    {
        pqxx::connection c(conn_);
        pqxx::work w(c);
        auto n = w.exec("SELECT count(*) FROM information_schema.columns "
                        "WHERE table_schema = 'metadata' AND table_name = 'contract_metadata' "
                        "AND column_name = 'Fee Per Contract'");
        has_column = n[0][0].as<int>() == 1;
        if (has_column) {
            for (const auto& row : w.exec("SELECT \"Databento Symbol\", \"Fee Per Contract\" "
                                          "FROM metadata.contract_metadata")) {
                stored[row[0].as<std::string>()] = std::stod(row[1].as<std::string>());
            }
        }
        w.commit();
    }

    auto loaded = db_->get_contract_metadata();
    ASSERT_TRUE(loaded.is_ok()) << loaded.error()->what();
    auto table = loaded.value();
    ASSERT_GT(table->num_rows(), 0);
    auto fee = table->GetColumnByName("Fee Per Contract");
    auto symbols = table->GetColumnByName("Databento Symbol");
    ASSERT_NE(symbols, nullptr);

    if (!has_column) {
        EXPECT_EQ(fee, nullptr) << "a table without the column must load as before";
        return;
    }
    ASSERT_NE(fee, nullptr) << "the database has \"Fee Per Contract\" but the loader dropped it";
    ASSERT_EQ(fee->type()->id(), arrow::Type::DOUBLE);
    ASSERT_EQ(fee->num_chunks(), 1);
    auto fee_values = std::static_pointer_cast<arrow::DoubleArray>(fee->chunk(0));
    auto sym_values = std::static_pointer_cast<arrow::StringArray>(symbols->chunk(0));
    ASSERT_EQ(static_cast<size_t>(table->num_rows()), stored.size());
    for (int64_t i = 0; i < table->num_rows(); ++i) {
        const std::string sym = sym_values->GetString(i);
        ASSERT_FALSE(fee_values->IsNull(i)) << sym;
        EXPECT_DOUBLE_EQ(fee_values->Value(i), stored.at(sym)) << sym;
    }
}
