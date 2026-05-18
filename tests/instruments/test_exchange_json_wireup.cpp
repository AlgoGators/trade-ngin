#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "trade_ngin/instruments/equity.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

// Regression test for audit finding §1.2. Phase 1b passes the
// equity_exchanges.json path into load_equity_instruments() from the 3
// callers (live + 2 backtest apps). Previously, every equity defaulted to
// "NYSE" regardless of actual listing because no caller provided the path.

namespace {

// Use synthetic symbols so we don't collide with anything other tests
// may have registered in the singleton registry.
const std::string kNasdaq1 = "PHASE1_T23_NASDAQ_A";
const std::string kNasdaq2 = "PHASE1_T23_NASDAQ_B";
const std::string kNyse1 = "PHASE1_T23_NYSE_A";
const std::string kUnlisted = "PHASE1_T23_UNLISTED";

class ExchangeJsonWireupTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();

        tmp_dir_ = std::filesystem::temp_directory_path() /
                   ("exchange_wireup_" + std::to_string(std::rand()));
        std::filesystem::create_directories(tmp_dir_);
        json_path_ = tmp_dir_ / "exchanges.json";

        std::ofstream out(json_path_);
        out << "{\n"
            << "  \"_comment\": \"test fixture\",\n"
            << "  \"NASDAQ\": [\"" << kNasdaq1 << "\", \"" << kNasdaq2 << "\"],\n"
            << "  \"NYSE\": [\"" << kNyse1 << "\"]\n"
            << "}\n";
        out.close();

        // Initialize the singleton registry with a mock DB if it hasn't been
        // touched yet (no-op if already initialized by another test).
        auto& registry = InstrumentRegistry::instance();
        auto db = std::make_shared<MockPostgresDatabase>("mock://testdb");
        ASSERT_TRUE(db->connect().is_ok());
        (void)registry.initialize(db);  // Idempotent: no-op if already initialized.
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(tmp_dir_, ec);
        TestBase::TearDown();
    }

    std::filesystem::path tmp_dir_;
    std::filesystem::path json_path_;
};

}  // namespace

// With JSON path supplied: NASDAQ entries get "NASDAQ", NYSE entries get
// "NYSE", and symbols not listed in either fall back to "NYSE" (the loader's
// hardcoded default).
TEST_F(ExchangeJsonWireupTest, JsonPathPopulatesExchangesPerSymbol) {
    auto& registry = InstrumentRegistry::instance();

    std::vector<std::string> symbols{kNasdaq1, kNasdaq2, kNyse1, kUnlisted};
    auto load_result = registry.load_equity_instruments(symbols, json_path_.string());
    ASSERT_TRUE(load_result.is_ok())
        << "load_equity_instruments failed: " << load_result.error()->what();

    auto a = registry.get_equity_instrument(kNasdaq1);
    auto b = registry.get_equity_instrument(kNasdaq2);
    auto c = registry.get_equity_instrument(kNyse1);
    auto d = registry.get_equity_instrument(kUnlisted);

    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);
    ASSERT_NE(d, nullptr);

    EXPECT_EQ(a->get_exchange(), "NASDAQ");
    EXPECT_EQ(b->get_exchange(), "NASDAQ");
    EXPECT_EQ(c->get_exchange(), "NYSE");
    EXPECT_EQ(d->get_exchange(), "NYSE")
        << "Symbols absent from the JSON should fall back to the NYSE default.";
}

// Documents the pre-fix behavior: when no JSON path is supplied, every
// symbol gets "NYSE" regardless of actual listing. This is what every
// caller was unintentionally doing pre-Phase-1b.
TEST_F(ExchangeJsonWireupTest, EmptyPathFallsBackToAllNYSE) {
    auto& registry = InstrumentRegistry::instance();

    // Distinct symbol set so we don't collide with the test above's
    // registrations (load_equity_instruments skips already-registered).
    const std::string fallback_a = "PHASE1_T23_FALLBACK_A";
    const std::string fallback_b = "PHASE1_T23_FALLBACK_B";

    std::vector<std::string> symbols{fallback_a, fallback_b};
    auto load_result = registry.load_equity_instruments(symbols);  // no path
    ASSERT_TRUE(load_result.is_ok());

    auto a = registry.get_equity_instrument(fallback_a);
    auto b = registry.get_equity_instrument(fallback_b);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);

    EXPECT_EQ(a->get_exchange(), "NYSE");
    EXPECT_EQ(b->get_exchange(), "NYSE");
}
