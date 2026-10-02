// tests/live/test_live_results_roll_cost_columns.cpp
//
// T-ROLLX commit 3 (migration 017): LiveDataLoader::load_live_results appends daily_roll_costs and
// total_roll_costs to its SELECT (33 columns) and reads them as the last two fields. A result
// narrower than the SELECT (the 31-column shape the sizing-read tests' double serves) must read them
// as 0, not read a column past the table's end: the first cut did, and three LiveSizingReads tests
// crashed on one build of it (SegFault) while surviving on another.

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "../data/test_db_utils.hpp"
#include "trade_ngin/live/live_data_loader.hpp"

using namespace trade_ngin;
using trade_ngin::testing::MockPostgresDatabase;

namespace {

// Serves one live_results row of `n` double columns, column 31 and 32 (when present) the roll pair.
class RollCostColumnsDb : public MockPostgresDatabase {
public:
    explicit RollCostColumnsDb(int n) : MockPostgresDatabase("mock://roll_cost_columns"), n_(n) {
        (void)connect();
    }

    Result<std::shared_ptr<arrow::Table>> execute_query(const std::string& query) override {
        if (query.find(".live_results") == std::string::npos) {
            return MockPostgresDatabase::execute_query(query);
        }
        std::vector<std::shared_ptr<arrow::Field>> fields;
        std::vector<std::shared_ptr<arrow::Array>> arrays;
        for (int c = 0; c < n_; ++c) {
            arrow::DoubleBuilder b;
            double v = 0.0;
            if (c == 13) v = 12.5;   // daily_transaction_costs
            if (c == 31) v = 3.25;   // daily_roll_costs
            if (c == 32) v = 40.75;  // total_roll_costs
            ARROW_CHECK_OK(b.Append(v));
            std::shared_ptr<arrow::Array> a;
            ARROW_CHECK_OK(b.Finish(&a));
            fields.push_back(arrow::field("c" + std::to_string(c), arrow::float64()));
            arrays.push_back(a);
        }
        return Result<std::shared_ptr<arrow::Table>>(
            arrow::Table::Make(arrow::schema(fields), arrays));
    }

private:
    int n_;
};

Timestamp some_day() { return Timestamp(std::chrono::seconds(1'777'000'000)); }

}  // namespace

TEST(LiveResultsRollCostColumns, TheThirtyThreeColumnRowReadsTheRollPair) {
    auto db = std::make_shared<RollCostColumnsDb>(33);
    LiveDataLoader loader(db, "trading");
    auto r = loader.load_live_results("S", "P", some_day());
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    EXPECT_DOUBLE_EQ(r.value().daily_transaction_costs, 12.5);
    EXPECT_DOUBLE_EQ(r.value().daily_roll_costs, 3.25);
    EXPECT_DOUBLE_EQ(r.value().total_roll_costs, 40.75);
}

TEST(LiveResultsRollCostColumns, AThirtyOneColumnRowReadsTheRollPairAsZero) {
    auto db = std::make_shared<RollCostColumnsDb>(31);
    LiveDataLoader loader(db, "trading");
    auto r = loader.load_live_results("S", "P", some_day());
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    EXPECT_DOUBLE_EQ(r.value().daily_transaction_costs, 12.5);
    EXPECT_DOUBLE_EQ(r.value().daily_roll_costs, 0.0);
    EXPECT_DOUBLE_EQ(r.value().total_roll_costs, 0.0);
}
