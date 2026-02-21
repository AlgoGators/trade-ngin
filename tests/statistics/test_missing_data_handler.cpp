#include <gtest/gtest.h>
#include "trade_ngin/statistics/preprocessing/missing_data_handler.hpp"
#include <cmath>
#include <limits>

using namespace trade_ngin::statistics;

static const double NaN = std::numeric_limits<double>::quiet_NaN();

class MissingDataHandlerTest : public ::testing::Test {
protected:
    void SetUp() override {
        clean_data_ = {1.0, 2.0, 3.0, 4.0, 5.0};
        data_with_nan_ = {1.0, NaN, 3.0, NaN, 5.0};
    }

    std::vector<double> clean_data_;
    std::vector<double> data_with_nan_;
};

TEST_F(MissingDataHandlerTest, ErrorStrategyReturnsErrorOnNaN) {
    MissingDataHandlerConfig config;
    config.strategy = MissingDataHandlerConfig::Strategy::ERROR;
    MissingDataHandler handler(config);

    auto result = handler.handle(data_with_nan_);
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), trade_ngin::ErrorCode::INVALID_DATA);
}

TEST_F(MissingDataHandlerTest, ErrorStrategySucceedsOnCleanData) {
    MissingDataHandlerConfig config;
    config.strategy = MissingDataHandlerConfig::Strategy::ERROR;
    MissingDataHandler handler(config);

    auto result = handler.handle(clean_data_);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().size(), clean_data_.size());
}

TEST_F(MissingDataHandlerTest, DropRemovesNaNEntries) {
    MissingDataHandlerConfig config;
    config.strategy = MissingDataHandlerConfig::Strategy::DROP;
    MissingDataHandler handler(config);

    auto result = handler.handle(data_with_nan_);
    ASSERT_TRUE(result.is_ok());

    const auto& cleaned = result.value();
    EXPECT_EQ(cleaned.size(), 3u);  // 1.0, 3.0, 5.0
    EXPECT_DOUBLE_EQ(cleaned[0], 1.0);
    EXPECT_DOUBLE_EQ(cleaned[1], 3.0);
    EXPECT_DOUBLE_EQ(cleaned[2], 5.0);
}

TEST_F(MissingDataHandlerTest, ForwardFillCarriesLastValue) {
    MissingDataHandlerConfig config;
    config.strategy = MissingDataHandlerConfig::Strategy::FORWARD_FILL;
    MissingDataHandler handler(config);

    auto result = handler.handle(data_with_nan_);
    ASSERT_TRUE(result.is_ok());

    const auto& filled = result.value();
    EXPECT_EQ(filled.size(), 5u);
    EXPECT_DOUBLE_EQ(filled[0], 1.0);
    EXPECT_DOUBLE_EQ(filled[1], 1.0);  // forward filled from index 0
    EXPECT_DOUBLE_EQ(filled[2], 3.0);
    EXPECT_DOUBLE_EQ(filled[3], 3.0);  // forward filled from index 2
    EXPECT_DOUBLE_EQ(filled[4], 5.0);
}

TEST_F(MissingDataHandlerTest, InterpolateFillsGapsLinearly) {
    MissingDataHandlerConfig config;
    config.strategy = MissingDataHandlerConfig::Strategy::INTERPOLATE;
    MissingDataHandler handler(config);

    auto result = handler.handle(data_with_nan_);
    ASSERT_TRUE(result.is_ok());

    const auto& filled = result.value();
    EXPECT_EQ(filled.size(), 5u);
    EXPECT_DOUBLE_EQ(filled[0], 1.0);
    EXPECT_DOUBLE_EQ(filled[1], 2.0);  // linearly interpolated between 1 and 3
    EXPECT_DOUBLE_EQ(filled[2], 3.0);
    EXPECT_DOUBLE_EQ(filled[3], 4.0);  // linearly interpolated between 3 and 5
    EXPECT_DOUBLE_EQ(filled[4], 5.0);
}

TEST_F(MissingDataHandlerTest, MeanFillReplacesWithMean) {
    MissingDataHandlerConfig config;
    config.strategy = MissingDataHandlerConfig::Strategy::MEAN_FILL;
    MissingDataHandler handler(config);

    auto result = handler.handle(data_with_nan_);
    ASSERT_TRUE(result.is_ok());

    const auto& filled = result.value();
    EXPECT_EQ(filled.size(), 5u);

    // Mean of non-NaN: (1+3+5)/3 = 3.0
    EXPECT_DOUBLE_EQ(filled[1], 3.0);
    EXPECT_DOUBLE_EQ(filled[3], 3.0);
}

TEST_F(MissingDataHandlerTest, MultivariateDropRemovesNaNRows) {
    MissingDataHandlerConfig config;
    config.strategy = MissingDataHandlerConfig::Strategy::DROP;
    MissingDataHandler handler(config);

    Eigen::MatrixXd mat(4, 2);
    mat << 1.0, 2.0,
           NaN, 4.0,
           5.0, 6.0,
           7.0, NaN;

    auto result = handler.handle(mat);
    ASSERT_TRUE(result.is_ok());

    const auto& cleaned = result.value();
    EXPECT_EQ(cleaned.rows(), 2);  // Only rows 0 and 2 are clean
    EXPECT_DOUBLE_EQ(cleaned(0, 0), 1.0);
    EXPECT_DOUBLE_EQ(cleaned(1, 0), 5.0);
}

TEST_F(MissingDataHandlerTest, CountMissingAccuracy) {
    EXPECT_EQ(MissingDataHandler::count_missing(clean_data_), 0u);
    EXPECT_EQ(MissingDataHandler::count_missing(data_with_nan_), 2u);

    Eigen::MatrixXd mat(2, 2);
    mat << 1.0, NaN,
           NaN, 4.0;
    EXPECT_EQ(MissingDataHandler::count_missing(mat), 2u);
}

TEST_F(MissingDataHandlerTest, AllNaNDataError) {
    MissingDataHandlerConfig config;
    config.strategy = MissingDataHandlerConfig::Strategy::MEAN_FILL;
    MissingDataHandler handler(config);

    std::vector<double> all_nan = {NaN, NaN, NaN};
    auto result = handler.handle(all_nan);
    EXPECT_TRUE(result.is_error());
}

TEST_F(MissingDataHandlerTest, EdgeNaNHandling) {
    // Forward fill with first value NaN should error
    MissingDataHandlerConfig config;
    config.strategy = MissingDataHandlerConfig::Strategy::FORWARD_FILL;
    MissingDataHandler handler(config);

    std::vector<double> first_nan = {NaN, 2.0, 3.0};
    auto result = handler.handle(first_nan);
    EXPECT_TRUE(result.is_error());

    // Interpolate with edge NaN should extrapolate with nearest
    MissingDataHandlerConfig interp_config;
    interp_config.strategy = MissingDataHandlerConfig::Strategy::INTERPOLATE;
    MissingDataHandler interp_handler(interp_config);

    std::vector<double> edge_nan = {NaN, 2.0, 3.0, NaN};
    auto interp_result = interp_handler.handle(edge_nan);
    ASSERT_TRUE(interp_result.is_ok());

    const auto& filled = interp_result.value();
    EXPECT_DOUBLE_EQ(filled[0], 2.0);  // Extrapolated with nearest valid
    EXPECT_DOUBLE_EQ(filled[3], 3.0);  // Extrapolated with nearest valid
}

TEST_F(MissingDataHandlerTest, EmptyDataError) {
    MissingDataHandlerConfig config;
    MissingDataHandler handler(config);

    std::vector<double> empty;
    auto result = handler.handle(empty);
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), trade_ngin::ErrorCode::INVALID_ARGUMENT);
}
