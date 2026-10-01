// Coverage for live_data_loader.cpp focusing on:
// - Constructor throws on null DB
// - validate_connection error path when db is constructed but not connected
//
// Full query-result tests require a live PostgreSQL instance and are
// deferred to the postgres_database refactor (see deliverables/unit_testing/).

#include <gtest/gtest.h>
#include <chrono>
#include <memory>
#include <limits>
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/live/live_data_loader.hpp"

using namespace trade_ngin;

namespace {
class QueryResultDb : public PostgresDatabase {
public:
    std::shared_ptr<arrow::Table> table;
    QueryResultDb():PostgresDatabase("mock://query-result") {}
    bool is_connected() const override {return true;}
    Result<std::shared_ptr<arrow::Table>> execute_query(const std::string&) override {return table;}
};
std::shared_ptr<arrow::Table> string_cell(const std::string& value,bool null=false) {
    arrow::StringBuilder builder;
    if(null) EXPECT_TRUE(builder.AppendNull().ok()); else EXPECT_TRUE(builder.Append(value).ok());
    std::shared_ptr<arrow::Array> values; EXPECT_TRUE(builder.Finish(&values).ok());
    return arrow::Table::Make(arrow::schema({arrow::field("value",arrow::utf8())}),{values});
}
}

TEST(LiveDataLoaderConversions, SqlUtf8AndTypedNumericValuesAreReadWithoutBufferReinterpretation) {
    auto db=std::make_shared<QueryResultDb>();LiveDataLoader loader(db);
    db->table=string_cell("1");
    auto count=loader.load_total_trades_count("S","B",Timestamp{});
    ASSERT_TRUE(count.is_ok());EXPECT_EQ(count.value(),1);
    arrow::DoubleBuilder builder;ASSERT_TRUE(builder.Append(1234.5).ok());
    std::shared_ptr<arrow::Array> values;ASSERT_TRUE(builder.Finish(&values).ok());
    db->table=arrow::Table::Make(arrow::schema({arrow::field("value",arrow::float64())}),{values});
    auto equity=loader.load_portfolio_value("S","B",Timestamp{});
    ASSERT_TRUE(equity.is_ok());EXPECT_DOUBLE_EQ(equity.value(),1234.5);
    db->table=string_cell("",true);
    equity=loader.load_portfolio_value("S","B",Timestamp{});
    ASSERT_TRUE(equity.is_ok());EXPECT_DOUBLE_EQ(equity.value(),0);
}

TEST(LiveDataLoaderConversions, MalformedNonfiniteAndInvalidCountsAreErrorsNotValidNumbers) {
    auto db=std::make_shared<QueryResultDb>();LiveDataLoader loader(db);
    for(const auto& value:{"garbage","12oops","NaN","Infinity","-Infinity"}) {
        db->table=string_cell(value);
        EXPECT_TRUE(loader.load_portfolio_value("S","B",Timestamp{}).is_error())<<value;
        EXPECT_TRUE(loader.load_total_trades_count("S","B",Timestamp{}).is_error())<<value;
        EXPECT_TRUE(loader.load_daily_returns_history("S","B",Timestamp{}).is_error())<<value;
        EXPECT_TRUE(loader.load_daily_pnl_history("S","B",Timestamp{}).is_error())<<value;
        EXPECT_TRUE(loader.load_equity_curve_history("S","B",Timestamp{}).is_error())<<value;
    }
    for(const auto& value:{"-1","1.5","2147483648"}) {
        db->table=string_cell(value);
        EXPECT_TRUE(loader.get_live_results_count("S","B").is_error())<<value;
    }
}

class LiveDataLoaderTest : public ::testing::Test {
protected:
    // Construct a PostgresDatabase but DON'T call connect(); is_connected()
    // returns false so validate_connection() will return DATABASE_ERROR.
    std::shared_ptr<PostgresDatabase> make_disconnected_db() const {
        return std::make_shared<PostgresDatabase>("host=invalid port=1 user=u dbname=d");
    }
    Timestamp now() const { return std::chrono::system_clock::now(); }
};

TEST_F(LiveDataLoaderTest, ConstructorRejectsNullDb) {
    EXPECT_THROW(LiveDataLoader(nullptr, "trading"), std::invalid_argument);
}

TEST_F(LiveDataLoaderTest, IsConnectedFalseWhenDbNotConnected) {
    LiveDataLoader loader(make_disconnected_db(), "trading");
    EXPECT_FALSE(loader.is_connected());
}

#define ASSERT_DB_ERROR(expr)                                                  \
    do {                                                                       \
        auto __r = (expr);                                                     \
        ASSERT_TRUE(__r.is_error());                                           \
        EXPECT_EQ(__r.error()->code(), ErrorCode::DATABASE_ERROR);             \
    } while (0)

TEST_F(LiveDataLoaderTest, LoadPreviousPortfolioValueDisconnectedErrors) {
    LiveDataLoader l(make_disconnected_db(), "trading");
    ASSERT_DB_ERROR(l.load_previous_portfolio_value("S", "P", now()));
}

TEST_F(LiveDataLoaderTest, LoadPortfolioValueDisconnectedErrors) {
    LiveDataLoader l(make_disconnected_db(), "trading");
    ASSERT_DB_ERROR(l.load_portfolio_value("S", "P", now()));
}

TEST_F(LiveDataLoaderTest, LoadLiveResultsDisconnectedErrors) {
    LiveDataLoader l(make_disconnected_db(), "trading");
    ASSERT_DB_ERROR(l.load_live_results("S", "P", now()));
}

TEST_F(LiveDataLoaderTest, LoadPreviousDayDataDisconnectedErrors) {
    LiveDataLoader l(make_disconnected_db(), "trading");
    ASSERT_DB_ERROR(l.load_previous_day_data("S", "P", now()));
}

TEST_F(LiveDataLoaderTest, HasLiveResultsDisconnectedErrors) {
    LiveDataLoader l(make_disconnected_db(), "trading");
    ASSERT_DB_ERROR(l.has_live_results("S", "P", now()));
}

TEST_F(LiveDataLoaderTest, GetLiveResultsCountDisconnectedErrors) {
    LiveDataLoader l(make_disconnected_db(), "trading");
    ASSERT_DB_ERROR(l.get_live_results_count("S", "P"));
}

TEST_F(LiveDataLoaderTest, LoadDailyReturnsHistoryDisconnectedErrors) {
    LiveDataLoader l(make_disconnected_db(), "trading");
    ASSERT_DB_ERROR(l.load_daily_returns_history("S", "P", now()));
}

TEST_F(LiveDataLoaderTest, LoadDailyPnLHistoryDisconnectedErrors) {
    LiveDataLoader l(make_disconnected_db(), "trading");
    ASSERT_DB_ERROR(l.load_daily_pnl_history("S", "P", now()));
}

#define ASSERT_EMPTY_PORTFOLIO_ERROR(expr)                                    \
    do {                                                                       \
        auto __r = (expr);                                                     \
        ASSERT_TRUE(__r.is_error());                                           \
        EXPECT_EQ(__r.error()->code(), ErrorCode::INVALID_ARGUMENT);           \
    } while (0)

TEST_F(LiveDataLoaderTest, EveryPortfolioScopedReadRejectsEmptyIdentity) {
    LiveDataLoader loader(make_disconnected_db(), "trading");
    const auto date = now();
    ASSERT_EMPTY_PORTFOLIO_ERROR(loader.load_previous_portfolio_value("S", "", date));
    ASSERT_EMPTY_PORTFOLIO_ERROR(loader.load_portfolio_value("S", "", date));
    ASSERT_EMPTY_PORTFOLIO_ERROR(loader.load_live_results("S", "", date));
    ASSERT_EMPTY_PORTFOLIO_ERROR(loader.load_previous_day_data("S", "", date));
    ASSERT_EMPTY_PORTFOLIO_ERROR(loader.has_live_results("S", "", date));
    ASSERT_EMPTY_PORTFOLIO_ERROR(loader.get_live_results_count("S", ""));
    ASSERT_EMPTY_PORTFOLIO_ERROR(loader.load_daily_returns_history("S", "", date));
    ASSERT_EMPTY_PORTFOLIO_ERROR(loader.load_daily_pnl_history("S", "", date));
    ASSERT_EMPTY_PORTFOLIO_ERROR(loader.load_equity_curve_history("S", "", date));
    ASSERT_EMPTY_PORTFOLIO_ERROR(loader.load_total_trades_count("S", "", date));
    ASSERT_EMPTY_PORTFOLIO_ERROR(loader.load_positions("S", "", date));
    ASSERT_EMPTY_PORTFOLIO_ERROR(loader.load_positions_for_export("S", "", date));
    ASSERT_EMPTY_PORTFOLIO_ERROR(loader.load_daily_transaction_costs("S", "", date));
    ASSERT_EMPTY_PORTFOLIO_ERROR(loader.load_margin_metrics("S", "", date));
    ASSERT_EMPTY_PORTFOLIO_ERROR(loader.load_daily_metrics_for_email("S", "", date));
    ASSERT_EMPTY_PORTFOLIO_ERROR(
        loader.load_commissions_by_symbol("S", "OWNER", "", date));
}
