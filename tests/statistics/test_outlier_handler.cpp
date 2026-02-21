#include <gtest/gtest.h>
#include "trade_ngin/statistics/preprocessing/outlier_handler.hpp"
#include <cmath>
#include <limits>
#include <algorithm>

using namespace trade_ngin::statistics;

class OutlierHandlerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Normal data with a few outliers
        data_ = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0,
                 11.0, 12.0, 13.0, 14.0, 15.0, 16.0, 17.0, 18.0, 19.0, 20.0};
        // Add extreme outliers
        data_with_outliers_ = data_;
        data_with_outliers_.push_back(1000.0);
        data_with_outliers_.push_back(-500.0);
    }

    std::vector<double> data_;
    std::vector<double> data_with_outliers_;
};

TEST_F(OutlierHandlerTest, WinsorizeCapsExtremes) {
    OutlierHandlerConfig config;
    config.method = OutlierHandlerConfig::Method::WINSORIZE;
    config.lower_percentile = 0.1;
    config.upper_percentile = 0.9;
    OutlierHandler handler(config);

    auto result = handler.handle(data_with_outliers_);
    ASSERT_TRUE(result.is_ok());

    const auto& cleaned = result.value();
    EXPECT_EQ(cleaned.size(), data_with_outliers_.size());

    // Find the min and max — they should be capped
    double min_val = *std::min_element(cleaned.begin(), cleaned.end());
    double max_val = *std::max_element(cleaned.begin(), cleaned.end());

    // Outliers should have been capped
    EXPECT_GT(min_val, -500.0);
    EXPECT_LT(max_val, 1000.0);
}

TEST_F(OutlierHandlerTest, TrimRemovesOutliers) {
    OutlierHandlerConfig config;
    config.method = OutlierHandlerConfig::Method::TRIM;
    config.lower_percentile = 0.1;
    config.upper_percentile = 0.9;
    OutlierHandler handler(config);

    auto result = handler.handle(data_with_outliers_);
    ASSERT_TRUE(result.is_ok());

    const auto& cleaned = result.value();
    // Trimmed vector should be shorter
    EXPECT_LT(cleaned.size(), data_with_outliers_.size());

    // No values outside the percentile bounds should remain
    for (double v : cleaned) {
        EXPECT_GT(v, -500.0);
        EXPECT_LT(v, 1000.0);
    }
}

TEST_F(OutlierHandlerTest, MADFilterDetectsOutliers) {
    OutlierHandlerConfig config;
    config.method = OutlierHandlerConfig::Method::MAD_FILTER;
    config.mad_threshold = 3.0;
    OutlierHandler handler(config);

    auto result = handler.handle(data_with_outliers_);
    ASSERT_TRUE(result.is_ok());

    const auto& cleaned = result.value();
    EXPECT_EQ(cleaned.size(), data_with_outliers_.size());

    // The extreme outliers (1000, -500) should have been replaced with median
    // All values should be within a reasonable range now
    for (double v : cleaned) {
        EXPECT_GT(v, -100.0);
        EXPECT_LT(v, 100.0);
    }
}

TEST_F(OutlierHandlerTest, DetectReturnsCorrectIndices) {
    OutlierHandlerConfig config;
    config.method = OutlierHandlerConfig::Method::MAD_FILTER;
    config.mad_threshold = 3.0;
    OutlierHandler handler(config);

    auto result = handler.detect(data_with_outliers_);
    ASSERT_TRUE(result.is_ok());

    const auto& indices = result.value();
    EXPECT_GE(indices.size(), 2u);  // At least the two extreme outliers

    // The last two elements (1000 and -500) should be detected
    bool found_1000 = false, found_neg500 = false;
    for (size_t idx : indices) {
        if (data_with_outliers_[idx] == 1000.0) found_1000 = true;
        if (data_with_outliers_[idx] == -500.0) found_neg500 = true;
    }
    EXPECT_TRUE(found_1000);
    EXPECT_TRUE(found_neg500);
}

TEST_F(OutlierHandlerTest, NoModificationWhenNoOutliers) {
    OutlierHandlerConfig config;
    config.method = OutlierHandlerConfig::Method::MAD_FILTER;
    config.mad_threshold = 10.0;  // Very wide threshold — no outliers
    OutlierHandler handler(config);

    // Use data without outliers
    auto result = handler.handle(data_);
    ASSERT_TRUE(result.is_ok());

    const auto& cleaned = result.value();
    EXPECT_EQ(cleaned.size(), data_.size());

    // Values should be unchanged since nothing is an outlier
    for (size_t i = 0; i < data_.size(); ++i) {
        EXPECT_DOUBLE_EQ(cleaned[i], data_[i]);
    }
}

TEST_F(OutlierHandlerTest, InsufficientDataError) {
    OutlierHandlerConfig config;
    OutlierHandler handler(config);

    std::vector<double> tiny = {1.0, 2.0};
    auto result = handler.handle(tiny);
    EXPECT_TRUE(result.is_error());
}

TEST_F(OutlierHandlerTest, NaNRejected) {
    OutlierHandlerConfig config;
    OutlierHandler handler(config);

    std::vector<double> data = {1.0, 2.0, std::numeric_limits<double>::quiet_NaN(), 4.0};
    auto result = handler.handle(data);
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), trade_ngin::ErrorCode::INVALID_DATA);
}

TEST_F(OutlierHandlerTest, InvalidConfigError) {
    OutlierHandlerConfig config;
    config.method = OutlierHandlerConfig::Method::WINSORIZE;
    config.lower_percentile = 0.9;
    config.upper_percentile = 0.1;  // lower >= upper
    OutlierHandler handler(config);

    auto result = handler.handle(data_);
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), trade_ngin::ErrorCode::INVALID_ARGUMENT);
}

TEST_F(OutlierHandlerTest, EmptyDataError) {
    OutlierHandlerConfig config;
    OutlierHandler handler(config);

    std::vector<double> empty;
    auto result = handler.handle(empty);
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), trade_ngin::ErrorCode::INVALID_ARGUMENT);
}
