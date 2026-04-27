#include <gtest/gtest.h>
#include <chrono>
#include <memory>
#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "trade_ngin/instruments/equity.hpp"
#include "trade_ngin/instruments/futures.hpp"
#include "trade_ngin/instruments/option.hpp"

// Reach into private state to populate the singleton without DB access.
#define private public
#include "trade_ngin/instruments/instrument_registry.hpp"
#undef private

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

std::shared_ptr<FuturesInstrument> make_futures(const std::string& symbol) {
    FuturesSpec s;
    s.root_symbol = symbol;
    s.exchange = "CME";
    s.currency = "USD";
    s.multiplier = 50.0;
    s.tick_size = 0.25;
    s.commission_per_contract = 2.0;
    s.initial_margin = 12000.0;
    s.maintenance_margin = 9000.0;
    s.weight = 1.0;
    s.trading_hours = "09:30-16:00";
    return std::make_shared<FuturesInstrument>(symbol, s);
}

std::shared_ptr<EquityInstrument> make_equity(const std::string& symbol) {
    EquitySpec s;
    s.exchange = "NASDAQ";
    s.currency = "USD";
    s.lot_size = 100.0;
    s.tick_size = 0.01;
    s.commission_per_share = 0.005;
    s.trading_hours = "09:30-16:00";
    return std::make_shared<EquityInstrument>(symbol, s);
}

std::shared_ptr<OptionInstrument> make_option(const std::string& symbol) {
    OptionSpec s;
    s.underlying_symbol = "AAPL";
    s.type = OptionType::CALL;
    s.style = ExerciseStyle::AMERICAN;
    s.strike = 100.0;
    s.expiry = std::chrono::system_clock::now() + std::chrono::hours(24 * 30);
    s.exchange = "OPRA";
    s.currency = "USD";
    return std::make_shared<OptionInstrument>(symbol, s);
}

}  // namespace

class InstrumentRegistryTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        auto& r = InstrumentRegistry::instance();
        r.instruments_.clear();
        r.initialized_ = false;
        r.db_.reset();
    }
    void TearDown() override {
        auto& r = InstrumentRegistry::instance();
        r.instruments_.clear();
        r.initialized_ = false;
        r.db_.reset();
        TestBase::TearDown();
    }
};

TEST_F(InstrumentRegistryTest, InitializeRequiresNonNullDatabase) {
    auto& r = InstrumentRegistry::instance();
    auto result = r.initialize(nullptr);
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_ARGUMENT);
    EXPECT_FALSE(r.initialized_);
}

TEST_F(InstrumentRegistryTest, InitializeSucceedsWithMockDatabase) {
    auto& r = InstrumentRegistry::instance();
    auto db = std::make_shared<MockPostgresDatabase>("mock://testdb");
    EXPECT_TRUE(r.initialize(db).is_ok());
    EXPECT_TRUE(r.initialized_);
}

TEST_F(InstrumentRegistryTest, InitializeIsIdempotent) {
    auto& r = InstrumentRegistry::instance();
    auto db = std::make_shared<MockPostgresDatabase>("mock://testdb");
    EXPECT_TRUE(r.initialize(db).is_ok());
    EXPECT_TRUE(r.initialize(nullptr).is_ok());  // Second call short-circuits.
}

TEST_F(InstrumentRegistryTest, GetInstrumentReturnsNullForUnknownSymbol) {
    auto& r = InstrumentRegistry::instance();
    EXPECT_EQ(r.get_instrument("ZZZ"), nullptr);
}

TEST_F(InstrumentRegistryTest, GetInstrumentMapsESToMES) {
    auto& r = InstrumentRegistry::instance();
    auto mes = make_futures("MES");
    r.instruments_["MES"] = mes;
    EXPECT_EQ(r.get_instrument("ES"), mes);
    EXPECT_EQ(r.get_instrument("MES"), mes);
}

TEST_F(InstrumentRegistryTest, GetInstrumentMapsNQToMNQAndYMToMYM) {
    auto& r = InstrumentRegistry::instance();
    auto mnq = make_futures("MNQ");
    auto mym = make_futures("MYM");
    r.instruments_["MNQ"] = mnq;
    r.instruments_["MYM"] = mym;
    EXPECT_EQ(r.get_instrument("NQ"), mnq);
    EXPECT_EQ(r.get_instrument("YM"), mym);
}

TEST_F(InstrumentRegistryTest, HasInstrumentRespectsMicroSymbolMapping) {
    auto& r = InstrumentRegistry::instance();
    r.instruments_["MES"] = make_futures("MES");
    EXPECT_TRUE(r.has_instrument("ES"));
    EXPECT_TRUE(r.has_instrument("MES"));
    EXPECT_FALSE(r.has_instrument("NQ"));
    EXPECT_FALSE(r.has_instrument("UNKNOWN"));
}

TEST_F(InstrumentRegistryTest, GetFuturesInstrumentReturnsNullForNonFutures) {
    auto& r = InstrumentRegistry::instance();
    r.instruments_["AAPL"] = make_equity("AAPL");
    r.instruments_["MES"] = make_futures("MES");
    EXPECT_EQ(r.get_futures_instrument("AAPL"), nullptr);
    EXPECT_NE(r.get_futures_instrument("MES"), nullptr);
}

TEST_F(InstrumentRegistryTest, GetEquityInstrumentReturnsNullForNonEquity) {
    auto& r = InstrumentRegistry::instance();
    r.instruments_["MES"] = make_futures("MES");
    r.instruments_["AAPL"] = make_equity("AAPL");
    EXPECT_EQ(r.get_equity_instrument("MES"), nullptr);
    EXPECT_NE(r.get_equity_instrument("AAPL"), nullptr);
}

TEST_F(InstrumentRegistryTest, GetOptionInstrumentReturnsNullForNonOption) {
    auto& r = InstrumentRegistry::instance();
    r.instruments_["MES"] = make_futures("MES");
    r.instruments_["AAPL_OPT"] = make_option("AAPL_OPT");
    EXPECT_EQ(r.get_option_instrument("MES"), nullptr);
    EXPECT_NE(r.get_option_instrument("AAPL_OPT"), nullptr);
}

TEST_F(InstrumentRegistryTest, GetInstrumentsByAssetClassPartitionsCorrectly) {
    auto& r = InstrumentRegistry::instance();
    r.instruments_["MES"] = make_futures("MES");
    r.instruments_["MNQ"] = make_futures("MNQ");
    r.instruments_["AAPL"] = make_equity("AAPL");
    r.instruments_["MSFT"] = make_equity("MSFT");
    r.instruments_["AAPL_OPT"] = make_option("AAPL_OPT");

    EXPECT_EQ(r.get_instruments_by_asset_class(AssetClass::FUTURES).size(), 2u);
    EXPECT_EQ(r.get_instruments_by_asset_class(AssetClass::EQUITIES).size(), 2u);
    EXPECT_EQ(r.get_instruments_by_asset_class(AssetClass::OPTIONS).size(), 1u);
    EXPECT_TRUE(r.get_instruments_by_asset_class(AssetClass::CURRENCIES).empty());
    EXPECT_TRUE(r.get_instruments_by_asset_class(AssetClass::CRYPTO).empty());
}

TEST_F(InstrumentRegistryTest, GetAllInstrumentsReturnsCopyOfMap) {
    auto& r = InstrumentRegistry::instance();
    r.instruments_["MES"] = make_futures("MES");
    r.instruments_["AAPL"] = make_equity("AAPL");
    auto all = r.get_all_instruments();
    EXPECT_EQ(all.size(), 2u);
    EXPECT_TRUE(all.count("MES"));
    EXPECT_TRUE(all.count("AAPL"));
}

TEST_F(InstrumentRegistryTest, LoadInstrumentsErrorsWhenNotInitialized) {
    auto& r = InstrumentRegistry::instance();
    auto result = r.load_instruments();
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::NOT_INITIALIZED);
}
