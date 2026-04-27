#include <gtest/gtest.h>
#include <arrow/api.h>
#include <arrow/array/builder_binary.h>
#include <arrow/array/builder_primitive.h>
#include <arrow/util/logging.h>
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

// ===== string_to_asset_type (private; reached via #define private public) =====

TEST_F(InstrumentRegistryTest, StringToAssetTypeFutureAliases) {
    auto& r = InstrumentRegistry::instance();
    EXPECT_EQ(r.string_to_asset_type("FUTURE"), AssetType::FUTURE);
    EXPECT_EQ(r.string_to_asset_type("FUT"), AssetType::FUTURE);
    EXPECT_EQ(r.string_to_asset_type("Futures"), AssetType::FUTURE);
}

TEST_F(InstrumentRegistryTest, StringToAssetTypeEquityAliases) {
    auto& r = InstrumentRegistry::instance();
    EXPECT_EQ(r.string_to_asset_type("EQUITY"), AssetType::EQUITY);
    EXPECT_EQ(r.string_to_asset_type("STK"), AssetType::EQUITY);
}

TEST_F(InstrumentRegistryTest, StringToAssetTypeOptionAliases) {
    auto& r = InstrumentRegistry::instance();
    EXPECT_EQ(r.string_to_asset_type("OPTION"), AssetType::OPTION);
    EXPECT_EQ(r.string_to_asset_type("OPT"), AssetType::OPTION);
}

TEST_F(InstrumentRegistryTest, StringToAssetTypeForexAliases) {
    auto& r = InstrumentRegistry::instance();
    EXPECT_EQ(r.string_to_asset_type("FOREX"), AssetType::FOREX);
    EXPECT_EQ(r.string_to_asset_type("FX"), AssetType::FOREX);
}

TEST_F(InstrumentRegistryTest, StringToAssetTypeCryptoAndUnknown) {
    auto& r = InstrumentRegistry::instance();
    EXPECT_EQ(r.string_to_asset_type("CRYPTO"), AssetType::CRYPTO);
    EXPECT_EQ(r.string_to_asset_type("UNKNOWN"), AssetType::NONE);
    EXPECT_EQ(r.string_to_asset_type(""), AssetType::NONE);
}

// ===== create_instrument_from_db (private; reached via #define private public) =====
//
// Construct minimal Arrow tables matching the schema the production code reads.

namespace {

struct ContractRow {
    std::string databento_symbol;
    std::string ib_symbol;
    std::string asset_type;
    std::string exchange;
    double contract_size{0.0};
    double min_tick{0.0};
    std::string tick_size;
    double initial_margin{0.0};
    double maintenance_margin{0.0};
    std::string trading_hours;
    std::string sector;
};

std::shared_ptr<arrow::Table> build_contract_table(const std::vector<ContractRow>& rows) {
    arrow::MemoryPool* pool = arrow::default_memory_pool();
    arrow::StringBuilder dbento_b(pool), ib_b(pool), at_b(pool), ex_b(pool),
                          ts_b(pool), th_b(pool), sec_b(pool);
    arrow::DoubleBuilder cs_b(pool), mt_b(pool), im_b(pool), mm_b(pool);
    for (const auto& row : rows) {
        ARROW_CHECK_OK(dbento_b.Append(row.databento_symbol));
        ARROW_CHECK_OK(ib_b.Append(row.ib_symbol));
        ARROW_CHECK_OK(at_b.Append(row.asset_type));
        ARROW_CHECK_OK(ex_b.Append(row.exchange));
        ARROW_CHECK_OK(cs_b.Append(row.contract_size));
        ARROW_CHECK_OK(mt_b.Append(row.min_tick));
        ARROW_CHECK_OK(ts_b.Append(row.tick_size));
        ARROW_CHECK_OK(im_b.Append(row.initial_margin));
        ARROW_CHECK_OK(mm_b.Append(row.maintenance_margin));
        ARROW_CHECK_OK(th_b.Append(row.trading_hours));
        ARROW_CHECK_OK(sec_b.Append(row.sector));
    }
    std::shared_ptr<arrow::Array> dbento, ib, at, ex, cs, mt, ts, im, mm, th, sec;
    ARROW_CHECK_OK(dbento_b.Finish(&dbento));
    ARROW_CHECK_OK(ib_b.Finish(&ib));
    ARROW_CHECK_OK(at_b.Finish(&at));
    ARROW_CHECK_OK(ex_b.Finish(&ex));
    ARROW_CHECK_OK(cs_b.Finish(&cs));
    ARROW_CHECK_OK(mt_b.Finish(&mt));
    ARROW_CHECK_OK(ts_b.Finish(&ts));
    ARROW_CHECK_OK(im_b.Finish(&im));
    ARROW_CHECK_OK(mm_b.Finish(&mm));
    ARROW_CHECK_OK(th_b.Finish(&th));
    ARROW_CHECK_OK(sec_b.Finish(&sec));
    auto schema = arrow::schema({
        arrow::field("Databento Symbol", arrow::utf8()),
        arrow::field("IB Symbol", arrow::utf8()),
        arrow::field("Asset Type", arrow::utf8()),
        arrow::field("Exchange", arrow::utf8()),
        arrow::field("Contract Size", arrow::float64()),
        arrow::field("Minimum Price Fluctuation", arrow::float64()),
        arrow::field("Tick Size", arrow::utf8()),
        arrow::field("Overnight Initial Margin", arrow::float64()),
        arrow::field("Overnight Maintenance Margin", arrow::float64()),
        arrow::field("Trading Hours (EST)", arrow::utf8()),
        arrow::field("Sector", arrow::utf8()),
    });
    return arrow::Table::Make(schema, {dbento, ib, at, ex, cs, mt, ts, im, mm, th, sec});
}

}  // namespace

TEST_F(InstrumentRegistryTest, CreateInstrumentFromDbBuildsFutures) {
    auto& r = InstrumentRegistry::instance();
    auto table = build_contract_table({
        {"ES.v.0", "", "FUTURE", "CME", 50.0, 0.25, "0.25", 12000.0, 9000.0,
         "09:30-16:00", ""},
    });
    auto instr = r.create_instrument_from_db(table, 0);
    ASSERT_NE(instr, nullptr);
    EXPECT_EQ(instr->get_type(), AssetType::FUTURE);
    EXPECT_EQ(instr->get_symbol(), "ES.v.0");
    EXPECT_DOUBLE_EQ(instr->get_multiplier(), 50.0);
}

TEST_F(InstrumentRegistryTest, CreateInstrumentFromDbBuildsEquity) {
    auto& r = InstrumentRegistry::instance();
    auto table = build_contract_table({
        {"AAPL", "", "EQUITY", "NASDAQ", 1.0, 0.01, "0.01", 0.0, 0.0,
         "09:30-16:00", "Technology"},
    });
    auto instr = r.create_instrument_from_db(table, 0);
    ASSERT_NE(instr, nullptr);
    EXPECT_EQ(instr->get_type(), AssetType::EQUITY);
    EXPECT_EQ(instr->get_symbol(), "AAPL");
}

TEST_F(InstrumentRegistryTest, CreateInstrumentFromDbReturnsNullForOption) {
    auto& r = InstrumentRegistry::instance();
    auto table = build_contract_table({
        {"AAPL_OPT", "", "OPTION", "OPRA", 100.0, 0.01, "0.01", 0.0, 0.0, "09:30-16:00", ""},
    });
    EXPECT_EQ(r.create_instrument_from_db(table, 0), nullptr);
}

TEST_F(InstrumentRegistryTest, CreateInstrumentFromDbReturnsNullForUnsupportedType) {
    auto& r = InstrumentRegistry::instance();
    auto table = build_contract_table({
        {"BTC", "", "CRYPTO", "Coinbase", 1.0, 0.01, "0.01", 0.0, 0.0, "00:00-23:59", ""},
    });
    EXPECT_EQ(r.create_instrument_from_db(table, 0), nullptr);
}

TEST_F(InstrumentRegistryTest, CreateInstrumentFromDbFallsBackToIBSymbolWhenDatabentoEmpty) {
    auto& r = InstrumentRegistry::instance();
    auto table = build_contract_table({
        {"", "ES_IB", "FUTURE", "CME", 50.0, 0.25, "0.25", 12000.0, 9000.0,
         "09:30-16:00", ""},
    });
    auto instr = r.create_instrument_from_db(table, 0);
    ASSERT_NE(instr, nullptr);
    EXPECT_EQ(instr->get_symbol(), "ES_IB");
}

TEST_F(InstrumentRegistryTest, CreateInstrumentFromDbReturnsNullWhenBothSymbolsEmpty) {
    auto& r = InstrumentRegistry::instance();
    auto table = build_contract_table({
        {"", "", "FUTURE", "CME", 50.0, 0.25, "0.25", 0.0, 0.0, "09:30-16:00", ""},
    });
    EXPECT_EQ(r.create_instrument_from_db(table, 0), nullptr);
}

TEST_F(InstrumentRegistryTest, CreateInstrumentFromDbDefaultsContractSizeWhenZero) {
    auto& r = InstrumentRegistry::instance();
    auto table = build_contract_table({
        {"ES.v.0", "", "FUTURE", "CME", 0.0, 0.25, "0.25", 12000.0, 9000.0,
         "09:30-16:00", ""},
    });
    auto instr = r.create_instrument_from_db(table, 0);
    ASSERT_NE(instr, nullptr);
    EXPECT_DOUBLE_EQ(instr->get_multiplier(), 1.0);  // defaulted from 0.0
}
