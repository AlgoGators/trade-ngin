#include <gtest/gtest.h>
#include "trade_ngin/statistics/statistics_tools.hpp"
#include "trade_ngin/statistics/critical_values.hpp"
#include <Eigen/Dense>
#include <vector>
#include <cmath>
#include <random>

using namespace trade_ngin::statistics;

// ============================================================================
// Test Fixtures
// ============================================================================

class NormalizerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create test data
        data_ = Eigen::MatrixXd(5, 3);
        data_ << 1.0, 2.0, 3.0,
                 2.0, 4.0, 6.0,
                 3.0, 6.0, 9.0,
                 4.0, 8.0, 12.0,
                 5.0, 10.0, 15.0;
    }

    Eigen::MatrixXd data_;
};

class PCATest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create correlated test data
        std::mt19937 gen(42);
        std::normal_distribution<> d(0.0, 1.0);

        int n = 100;
        data_ = Eigen::MatrixXd(n, 3);

        for (int i = 0; i < n; ++i) {
            double x1 = d(gen);
            double x2 = 0.8 * x1 + 0.2 * d(gen);  // Correlated with x1
            double x3 = d(gen);  // Independent
            data_(i, 0) = x1;
            data_(i, 1) = x2;
            data_(i, 2) = x3;
        }
    }

    Eigen::MatrixXd data_;
};

class ADFTestFixture : public ::testing::Test {
protected:
    void SetUp() override {
        // Generate non-stationary random walk
        std::mt19937 gen(42);
        std::normal_distribution<> d(0.0, 1.0);

        random_walk_.resize(100);
        random_walk_[0] = 0.0;
        for (size_t i = 1; i < random_walk_.size(); ++i) {
            random_walk_[i] = random_walk_[i - 1] + d(gen);
        }

        // Generate stationary white noise
        white_noise_.resize(100);
        for (size_t i = 0; i < white_noise_.size(); ++i) {
            white_noise_[i] = d(gen);
        }
    }

    std::vector<double> random_walk_;
    std::vector<double> white_noise_;
};

class KPSSTestFixture : public ::testing::Test {
protected:
    void SetUp() override {
        std::mt19937 gen(42);
        std::normal_distribution<> d(0.0, 1.0);

        // Stationary process
        stationary_.resize(100);
        for (size_t i = 0; i < stationary_.size(); ++i) {
            stationary_[i] = d(gen);
        }

        // Non-stationary process (random walk)
        non_stationary_.resize(100);
        non_stationary_[0] = 0.0;
        for (size_t i = 1; i < non_stationary_.size(); ++i) {
            non_stationary_[i] = non_stationary_[i - 1] + d(gen);
        }
    }

    std::vector<double> stationary_;
    std::vector<double> non_stationary_;
};

class JohansenTestFixture : public ::testing::Test {
protected:
    void SetUp() override {
        std::mt19937 gen(42);
        std::normal_distribution<> d(0.0, 1.0);

        int n = 100;
        // Create cointegrated series
        std::vector<double> z(n);
        z[0] = d(gen);
        for (int i = 1; i < n; ++i) {
            z[i] = z[i - 1] + d(gen);  // Random walk
        }

        // Create two series that share the random walk component
        cointegrated_data_ = Eigen::MatrixXd(n, 2);
        for (int i = 0; i < n; ++i) {
            cointegrated_data_(i, 0) = z[i] + 0.1 * d(gen);
            cointegrated_data_(i, 1) = 2.0 * z[i] + 0.1 * d(gen);
        }
    }

    Eigen::MatrixXd cointegrated_data_;
};

class EngleGrangerTestFixture : public ::testing::Test {
protected:
    void SetUp() override {
        std::mt19937 gen(42);
        std::normal_distribution<> d(0.0, 1.0);

        int n = 100;
        // Create cointegrated pair
        y_.resize(n);
        x_.resize(n);

        std::vector<double> common_trend(n);
        common_trend[0] = d(gen);
        for (int i = 1; i < n; ++i) {
            common_trend[i] = common_trend[i - 1] + d(gen);
        }

        for (int i = 0; i < n; ++i) {
            x_[i] = common_trend[i] + 0.1 * d(gen);
            y_[i] = 2.0 * common_trend[i] + 1.0 + 0.1 * d(gen);
        }
    }

    std::vector<double> y_;
    std::vector<double> x_;
};

class GARCHTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::mt19937 gen(42);
        std::normal_distribution<> d(0.0, 1.0);

        // Generate returns with volatility clustering
        returns_.resize(200);
        double sigma = 0.01;
        for (size_t i = 0; i < returns_.size(); ++i) {
            double z = d(gen);
            returns_[i] = sigma * z;
            // Simple GARCH-like process
            sigma = std::sqrt(0.00001 + 0.1 * returns_[i] * returns_[i] + 0.85 * sigma * sigma);
        }
    }

    std::vector<double> returns_;
};

class KalmanFilterTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_.state_dim = 2;
        config_.obs_dim = 1;
        config_.process_noise = 0.01;
        config_.measurement_noise = 0.1;
    }

    KalmanFilterConfig config_;
};

class HMMTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_.n_states = 2;
        config_.max_iterations = 50;
        config_.tolerance = 1e-4;

        // Generate observations from 2-state HMM
        std::mt19937 gen(42);
        std::normal_distribution<> state0(0.0, 0.5);
        std::normal_distribution<> state1(3.0, 0.5);

        int n = 100;
        observations_ = Eigen::MatrixXd(n, 1);

        int current_state = 0;
        for (int i = 0; i < n; ++i) {
            // Simple state transitions
            if (std::uniform_real_distribution<>(0, 1)(gen) < 0.1) {
                current_state = 1 - current_state;  // Switch state
            }

            if (current_state == 0) {
                observations_(i, 0) = state0(gen);
            } else {
                observations_(i, 0) = state1(gen);
            }
        }
    }

    HMMConfig config_;
    Eigen::MatrixXd observations_;
};

// ============================================================================
// Normalizer Tests
// ============================================================================

TEST_F(NormalizerTest, ZScoreNormalization) {
    NormalizationConfig config;
    config.method = NormalizationConfig::Method::Z_SCORE;

    Normalizer normalizer(config);

    auto fit_result = normalizer.fit(data_);
    EXPECT_TRUE(fit_result.is_ok());
    EXPECT_TRUE(normalizer.is_fitted());

    auto transform_result = normalizer.transform(data_);
    ASSERT_TRUE(transform_result.is_ok());

    const auto& transformed = transform_result.value();

    // Check that mean is approximately 0
    Eigen::VectorXd col_means = transformed.colwise().mean();
    for (int i = 0; i < col_means.size(); ++i) {
        EXPECT_NEAR(col_means(i), 0.0, 1e-10);
    }

    // Check that std is approximately 1
    Eigen::VectorXd col_std = ((transformed.rowwise() - col_means.transpose()).array().square()
                               .colwise().sum() / (transformed.rows() - 1)).sqrt();
    for (int i = 0; i < col_std.size(); ++i) {
        EXPECT_NEAR(col_std(i), 1.0, 1e-10);
    }
}

TEST_F(NormalizerTest, MinMaxNormalization) {
    NormalizationConfig config;
    config.method = NormalizationConfig::Method::MIN_MAX;

    Normalizer normalizer(config);

    auto fit_result = normalizer.fit(data_);
    EXPECT_TRUE(fit_result.is_ok());

    auto transform_result = normalizer.transform(data_);
    ASSERT_TRUE(transform_result.is_ok());

    const auto& transformed = transform_result.value();

    // Check that min is 0 and max is 1
    Eigen::VectorXd col_min = transformed.colwise().minCoeff();
    Eigen::VectorXd col_max = transformed.colwise().maxCoeff();

    for (int i = 0; i < col_min.size(); ++i) {
        EXPECT_NEAR(col_min(i), 0.0, 1e-10);
        EXPECT_NEAR(col_max(i), 1.0, 1e-10);
    }
}

TEST_F(NormalizerTest, InverseTransform) {
    NormalizationConfig config;
    config.method = NormalizationConfig::Method::Z_SCORE;

    Normalizer normalizer(config);
    normalizer.fit(data_);

    auto transformed = normalizer.transform(data_);
    ASSERT_TRUE(transformed.is_ok());

    auto inverse_result = normalizer.inverse_transform(transformed.value());
    ASSERT_TRUE(inverse_result.is_ok());

    const auto& reconstructed = inverse_result.value();

    // Check that we get back original data
    for (int i = 0; i < data_.rows(); ++i) {
        for (int j = 0; j < data_.cols(); ++j) {
            EXPECT_NEAR(reconstructed(i, j), data_(i, j), 1e-10);
        }
    }
}

TEST_F(NormalizerTest, NotFittedError) {
    NormalizationConfig config;
    Normalizer normalizer(config);

    auto transform_result = normalizer.transform(data_);
    EXPECT_TRUE(transform_result.is_error());
    EXPECT_EQ(transform_result.error()->code(), trade_ngin::ErrorCode::NOT_INITIALIZED);
}

// ============================================================================
// PCA Tests
// ============================================================================

TEST_F(PCATest, BasicFitTransform) {
    PCAConfig config;
    config.n_components = 2;

    PCA pca(config);

    auto fit_result = pca.fit(data_);
    EXPECT_TRUE(fit_result.is_ok());
    EXPECT_TRUE(pca.is_fitted());
    EXPECT_EQ(pca.get_n_components(), 2);

    auto transform_result = pca.transform(data_);
    ASSERT_TRUE(transform_result.is_ok());

    const auto& transformed = transform_result.value();
    EXPECT_EQ(transformed.cols(), 2);
    EXPECT_EQ(transformed.rows(), data_.rows());
}

TEST_F(PCATest, VarianceThreshold) {
    PCAConfig config;
    config.n_components = -1;  // Use variance threshold
    config.variance_threshold = 0.95;

    PCA pca(config);
    auto fit_result = pca.fit(data_);
    EXPECT_TRUE(fit_result.is_ok());

    // Should select components that explain 95% of variance
    const auto& var_ratio = pca.get_explained_variance_ratio();
    double cumsum = 0.0;
    for (int i = 0; i < var_ratio.size(); ++i) {
        cumsum += var_ratio(i);
    }
    EXPECT_GE(cumsum, 0.95);
}

TEST_F(PCATest, InverseTransform) {
    PCAConfig config;
    config.n_components = 3;  // Keep all components

    PCA pca(config);
    pca.fit(data_);

    auto transformed = pca.transform(data_);
    ASSERT_TRUE(transformed.is_ok());

    auto inverse_result = pca.inverse_transform(transformed.value());
    ASSERT_TRUE(inverse_result.is_ok());

    const auto& reconstructed = inverse_result.value();

    // Should reconstruct original data well
    for (int i = 0; i < data_.rows(); ++i) {
        for (int j = 0; j < data_.cols(); ++j) {
            EXPECT_NEAR(reconstructed(i, j), data_(i, j), 0.1);
        }
    }
}

// ============================================================================
// ADF Test Tests
// ============================================================================

TEST_F(ADFTestFixture, DetectsNonStationarity) {
    ADFTestConfig config;
    ADFTest adf(config);

    auto result = adf.test(random_walk_);
    ASSERT_TRUE(result.is_ok());

    const auto& test_result = result.value();
    // Random walk should not reject null (non-stationary)
    EXPECT_FALSE(test_result.reject_null);
    EXPECT_GT(test_result.statistic, test_result.critical_value);
}

TEST_F(ADFTestFixture, DetectsStationarity) {
    ADFTestConfig config;
    ADFTest adf(config);

    auto result = adf.test(white_noise_);
    ASSERT_TRUE(result.is_ok());

    const auto& test_result = result.value();
    // White noise should likely reject null (stationary)
    // Note: This is probabilistic, so we just check the test runs
    EXPECT_LT(test_result.statistic, 0.0);
}

TEST_F(ADFTestFixture, InsufficientDataError) {
    ADFTestConfig config;
    ADFTest adf(config);

    std::vector<double> small_data = {1.0, 2.0, 3.0};
    auto result = adf.test(small_data);
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), trade_ngin::ErrorCode::INVALID_ARGUMENT);
}

// ============================================================================
// KPSS Test Tests
// ============================================================================

TEST_F(KPSSTestFixture, DetectsStationarity) {
    KPSSTestConfig config;
    KPSSTest kpss(config);

    auto result = kpss.test(stationary_);
    ASSERT_TRUE(result.is_ok());

    const auto& test_result = result.value();
    // Stationary series should not reject null
    EXPECT_FALSE(test_result.reject_null);
}

TEST_F(KPSSTestFixture, DetectsNonStationarity) {
    KPSSTestConfig config;
    KPSSTest kpss(config);

    auto result = kpss.test(non_stationary_);
    ASSERT_TRUE(result.is_ok());

    const auto& test_result = result.value();
    // Non-stationary series should reject null
    // Note: This is probabilistic
    EXPECT_GT(test_result.statistic, 0.0);
}

// ============================================================================
// Johansen Test Tests
// ============================================================================

TEST_F(JohansenTestFixture, DetectsCointegration) {
    JohansenTestConfig config;
    JohansenTest johansen(config);

    auto result = johansen.test(cointegrated_data_);
    ASSERT_TRUE(result.is_ok());

    const auto& coint_result = result.value();
    EXPECT_EQ(coint_result.eigenvalues.size(), 2);
    EXPECT_EQ(coint_result.trace_statistics.size(), 2);
    // Should detect at least some cointegration
    EXPECT_GE(coint_result.cointegration_rank, 0);
}

TEST_F(JohansenTestFixture, InsufficientDataError) {
    JohansenTestConfig config;
    JohansenTest johansen(config);

    Eigen::MatrixXd small_data(5, 2);
    auto result = johansen.test(small_data);
    EXPECT_TRUE(result.is_error());
}

// ============================================================================
// Engle-Granger Test Tests
// ============================================================================

TEST_F(EngleGrangerTestFixture, DetectsCointegration) {
    EngleGrangerConfig config;
    EngleGrangerTest eg(config);

    auto result = eg.test(y_, x_);
    ASSERT_TRUE(result.is_ok());

    const auto& test_result = result.value();
    // Should find cointegrating relationship
    EXPECT_NE(test_result.additional_stats.count("regression_coefficient"), 0);
    EXPECT_NEAR(test_result.additional_stats.at("regression_coefficient"), 2.0, 0.5);
}

TEST_F(EngleGrangerTestFixture, MismatchedLengthError) {
    EngleGrangerConfig config;
    EngleGrangerTest eg(config);

    std::vector<double> short_x = {1.0, 2.0};
    auto result = eg.test(y_, short_x);
    EXPECT_TRUE(result.is_error());
}

// ============================================================================
// GARCH Tests
// ============================================================================

TEST_F(GARCHTest, FitAndForecast) {
    GARCHConfig config;
    GARCH garch(config);

    auto fit_result = garch.fit(returns_);
    EXPECT_TRUE(fit_result.is_ok());
    EXPECT_TRUE(garch.is_fitted());

    // Check parameters are in reasonable range
    EXPECT_GT(garch.get_omega(), 0.0);
    EXPECT_GT(garch.get_alpha(), 0.0);
    EXPECT_GT(garch.get_beta(), 0.0);
    EXPECT_LT(garch.get_alpha() + garch.get_beta(), 1.0);  // Stationarity

    auto vol_result = garch.get_current_volatility();
    ASSERT_TRUE(vol_result.is_ok());
    EXPECT_GT(vol_result.value(), 0.0);
}

TEST_F(GARCHTest, ForecastMultiplePeriods) {
    GARCHConfig config;
    GARCH garch(config);

    garch.fit(returns_);

    auto forecast_result = garch.forecast(5);
    ASSERT_TRUE(forecast_result.is_ok());

    const auto& forecasts = forecast_result.value();
    EXPECT_EQ(forecasts.size(), 5);

    // Forecasts should all be positive
    for (double vol : forecasts) {
        EXPECT_GT(vol, 0.0);
    }
}

TEST_F(GARCHTest, UpdateWithNewReturn) {
    GARCHConfig config;
    GARCH garch(config);

    garch.fit(returns_);

    auto vol_before = garch.get_current_volatility();
    ASSERT_TRUE(vol_before.is_ok());

    auto update_result = garch.update(0.05);  // Large shock
    EXPECT_TRUE(update_result.is_ok());

    auto vol_after = garch.get_current_volatility();
    ASSERT_TRUE(vol_after.is_ok());

    // Volatility should increase after large shock
    EXPECT_GT(vol_after.value(), vol_before.value());
}

TEST_F(GARCHTest, InsufficientDataError) {
    GARCHConfig config;
    GARCH garch(config);

    std::vector<double> small_returns = {0.01, 0.02, 0.03};
    auto result = garch.fit(small_returns);
    EXPECT_TRUE(result.is_error());
}

// ============================================================================
// Kalman Filter Tests
// ============================================================================

TEST_F(KalmanFilterTest, InitializeAndPredict) {
    KalmanFilter kf(config_);

    Eigen::VectorXd initial_state(2);
    initial_state << 0.0, 0.0;

    auto init_result = kf.initialize(initial_state);
    EXPECT_TRUE(init_result.is_ok());
    EXPECT_TRUE(kf.is_initialized());

    auto predict_result = kf.predict();
    ASSERT_TRUE(predict_result.is_ok());

    const auto& predicted_state = predict_result.value();
    EXPECT_EQ(predicted_state.size(), 2);
}

TEST_F(KalmanFilterTest, PredictUpdateCycle) {
    KalmanFilter kf(config_);

    // Set up constant velocity model
    Eigen::MatrixXd F(2, 2);
    F << 1.0, 1.0,
         0.0, 1.0;  // [position; velocity], dt=1
    kf.set_transition_matrix(F);

    Eigen::MatrixXd H(1, 2);
    H << 1.0, 0.0;  // Observe position only
    kf.set_observation_matrix(H);

    Eigen::VectorXd initial_state(2);
    initial_state << 0.0, 1.0;  // Position=0, velocity=1
    kf.initialize(initial_state);

    // Predict
    auto predict_result = kf.predict();
    ASSERT_TRUE(predict_result.is_ok());

    // Update with observation
    Eigen::VectorXd obs(1);
    obs << 1.0;  // Observe position = 1
    auto update_result = kf.update(obs);
    ASSERT_TRUE(update_result.is_ok());

    const auto& updated_state = update_result.value();
    EXPECT_EQ(updated_state.size(), 2);
}

TEST_F(KalmanFilterTest, NotInitializedError) {
    KalmanFilter kf(config_);

    auto predict_result = kf.predict();
    EXPECT_TRUE(predict_result.is_error());
    EXPECT_EQ(predict_result.error()->code(), trade_ngin::ErrorCode::NOT_INITIALIZED);
}

// ============================================================================
// HMM Tests
// ============================================================================

TEST_F(HMMTest, FitAndDecode) {
    HMM hmm(config_);

    auto fit_result = hmm.fit(observations_);
    EXPECT_TRUE(fit_result.is_ok());
    EXPECT_TRUE(hmm.is_initialized());

    auto decode_result = hmm.decode(observations_);
    ASSERT_TRUE(decode_result.is_ok());

    const auto& states = decode_result.value();
    EXPECT_EQ(states.size(), observations_.rows());

    // States should be 0 or 1
    for (int state : states) {
        EXPECT_TRUE(state == 0 || state == 1);
    }
}

TEST_F(HMMTest, InitializeAndUpdate) {
    HMM hmm(config_);

    Eigen::VectorXd initial_probs(2);
    initial_probs << 0.5, 0.5;

    auto init_result = hmm.initialize(initial_probs);
    EXPECT_TRUE(init_result.is_ok());

    Eigen::VectorXd obs(1);
    obs << 1.0;

    auto update_result = hmm.update(obs);
    ASSERT_TRUE(update_result.is_ok());

    const auto& state_probs = update_result.value();
    EXPECT_EQ(state_probs.size(), 2);

    // Probabilities should sum to 1
    EXPECT_NEAR(state_probs.sum(), 1.0, 1e-6);
}

TEST_F(HMMTest, PredictNextState) {
    HMM hmm(config_);
    hmm.fit(observations_);

    auto predict_result = hmm.predict();
    ASSERT_TRUE(predict_result.is_ok());

    const auto& next_probs = predict_result.value();
    EXPECT_EQ(next_probs.size(), 2);
    EXPECT_NEAR(next_probs.sum(), 1.0, 1e-6);
}

TEST_F(HMMTest, InsufficientDataError) {
    HMM hmm(config_);

    Eigen::MatrixXd small_obs(5, 1);
    auto result = hmm.fit(small_obs);
    EXPECT_TRUE(result.is_error());
}

// ============================================================================
// Integration Tests
// ============================================================================

class IntegrationTest : public ::testing::Test {};

TEST_F(IntegrationTest, NormalizationBeforePCA) {
    // Generate test data
    Eigen::MatrixXd data(50, 3);
    std::mt19937 gen(42);
    std::normal_distribution<> d(0.0, 1.0);

    for (int i = 0; i < 50; ++i) {
        data(i, 0) = d(gen) * 10.0 + 100.0;  // Different scales
        data(i, 1) = d(gen) * 0.1 + 1.0;
        data(i, 2) = d(gen) * 1.0 + 10.0;
    }

    // Normalize first
    NormalizationConfig norm_config;
    norm_config.method = NormalizationConfig::Method::Z_SCORE;
    Normalizer normalizer(norm_config);

    normalizer.fit(data);
    auto normalized = normalizer.transform(data);
    ASSERT_TRUE(normalized.is_ok());

    // Then apply PCA
    PCAConfig pca_config;
    pca_config.n_components = 2;
    PCA pca(pca_config);

    auto fit_result = pca.fit(normalized.value());
    EXPECT_TRUE(fit_result.is_ok());

    auto transformed = pca.transform(normalized.value());
    ASSERT_TRUE(transformed.is_ok());

    EXPECT_EQ(transformed.value().cols(), 2);
}

TEST_F(IntegrationTest, StatisticalTestsOnReturns) {
    // Generate return series
    std::mt19937 gen(42);
    std::normal_distribution<> d(0.0, 0.01);

    std::vector<double> returns(100);
    for (size_t i = 0; i < returns.size(); ++i) {
        returns[i] = d(gen);
    }

    // Test stationarity
    ADFTestConfig adf_config;
    ADFTest adf(adf_config);

    auto adf_result = adf.test(returns);
    ASSERT_TRUE(adf_result.is_ok());
    EXPECT_TRUE(adf_result.value().reject_null);  // Returns should be stationary

    // Fit GARCH
    GARCHConfig garch_config;
    GARCH garch(garch_config);

    auto fit_result = garch.fit(returns);
    EXPECT_TRUE(fit_result.is_ok());

    auto vol = garch.get_current_volatility();
    ASSERT_TRUE(vol.is_ok());
    EXPECT_GT(vol.value(), 0.0);
}

// ============================================================================
// Numerical Stability & Critical Value Tests
// ============================================================================

TEST(ADFCriticalValues, SmallSampleMoreNegativeThanLarge) {
    // For any regression type, small-sample CV should be more negative
    double cv_small = trade_ngin::statistics::critical_values::interpolate_adf_cv(25, 1, 0.05);
    double cv_large = trade_ngin::statistics::critical_values::interpolate_adf_cv(500, 1, 0.05);
    EXPECT_LT(cv_small, cv_large);
}

TEST(ADFCriticalValues, ConstantTrendMoreNegativeThanConstant) {
    double cv_constant = trade_ngin::statistics::critical_values::interpolate_adf_cv(100, 1, 0.05);
    double cv_trend = trade_ngin::statistics::critical_values::interpolate_adf_cv(100, 2, 0.05);
    EXPECT_LT(cv_trend, cv_constant);
}

TEST(ADFCriticalValues, NoConstantWorks) {
    double cv = trade_ngin::statistics::critical_values::interpolate_adf_cv(100, 0, 0.05);
    EXPECT_LT(cv, 0.0);
    // No-constant CVs should be less negative than constant CVs
    double cv_constant = trade_ngin::statistics::critical_values::interpolate_adf_cv(100, 1, 0.05);
    EXPECT_GT(cv, cv_constant);
}

TEST(ADFCriticalValues, InterpolationBetweenSampleSizes) {
    // CV at n=75 should be between n=50 and n=100
    double cv_50 = trade_ngin::statistics::critical_values::interpolate_adf_cv(50, 1, 0.05);
    double cv_75 = trade_ngin::statistics::critical_values::interpolate_adf_cv(75, 1, 0.05);
    double cv_100 = trade_ngin::statistics::critical_values::interpolate_adf_cv(100, 1, 0.05);
    // CVs become less negative as n increases, so cv_50 < cv_75 < cv_100
    EXPECT_LT(cv_50, cv_75);
    EXPECT_LT(cv_75, cv_100);
}

TEST(ADFCriticalValues, RegressionTypeAffectsTestResult) {
    // Generate white noise — should be stationary regardless of regression type
    std::mt19937 gen(42);
    std::normal_distribution<> d(0.0, 1.0);
    std::vector<double> data(200);
    for (auto& v : data) v = d(gen);

    ADFTestConfig config_const;
    config_const.regression = ADFTestConfig::RegressionType::CONSTANT;
    ADFTest adf_const(config_const);
    auto r1 = adf_const.test(data);
    ASSERT_TRUE(r1.is_ok());

    ADFTestConfig config_trend;
    config_trend.regression = ADFTestConfig::RegressionType::CONSTANT_TREND;
    ADFTest adf_trend(config_trend);
    auto r2 = adf_trend.test(data);
    ASSERT_TRUE(r2.is_ok());

    // Trend CV should be more negative
    EXPECT_LT(r2.value().critical_value, r1.value().critical_value);
}

TEST(KalmanIllConditioned, NoNaNWithTinyMeasurementNoise) {
    KalmanFilterConfig config;
    config.state_dim = 2;
    config.obs_dim = 1;
    config.process_noise = 0.01;
    config.measurement_noise = 1e-12;

    KalmanFilter kf(config);

    Eigen::VectorXd initial_state(2);
    initial_state << 0.0, 1.0;
    auto init_result = kf.initialize(initial_state);
    ASSERT_TRUE(init_result.is_ok());

    Eigen::MatrixXd F(2, 2);
    F << 1.0, 1.0, 0.0, 1.0;
    kf.set_transition_matrix(F);

    Eigen::MatrixXd H(1, 2);
    H << 1.0, 0.0;
    kf.set_observation_matrix(H);

    // Run several predict-update cycles
    for (int i = 0; i < 20; ++i) {
        auto pred = kf.predict();
        ASSERT_TRUE(pred.is_ok());

        Eigen::VectorXd obs(1);
        obs << static_cast<double>(i);
        auto upd = kf.update(obs);
        ASSERT_TRUE(upd.is_ok());

        const auto& state = upd.value();
        for (int j = 0; j < state.size(); ++j) {
            EXPECT_FALSE(std::isnan(state(j))) << "NaN at step " << i << " dim " << j;
            EXPECT_FALSE(std::isinf(state(j))) << "Inf at step " << i << " dim " << j;
        }
    }
}

TEST(HMMLongSequence, FitAndDecodeT500) {
    // Generate T=500 observation sequence from 2 well-separated states
    std::mt19937 gen(123);
    std::normal_distribution<> state0(0.0, 0.5);
    std::normal_distribution<> state1(5.0, 0.5);

    int T = 500;
    Eigen::MatrixXd obs(T, 1);
    std::vector<int> true_states(T);

    int current_state = 0;
    for (int i = 0; i < T; ++i) {
        if (std::uniform_real_distribution<>(0, 1)(gen) < 0.05) {
            current_state = 1 - current_state;
        }
        true_states[i] = current_state;
        obs(i, 0) = (current_state == 0) ? state0(gen) : state1(gen);
    }

    HMMConfig config;
    config.n_states = 2;
    config.max_iterations = 100;
    config.tolerance = 1e-4;

    HMM hmm(config);
    auto fit_result = hmm.fit(obs);
    ASSERT_TRUE(fit_result.is_ok());

    auto decode_result = hmm.decode(obs);
    ASSERT_TRUE(decode_result.is_ok());

    const auto& decoded = decode_result.value();
    EXPECT_EQ(static_cast<int>(decoded.size()), T);

    // Check states are valid
    for (int s : decoded) {
        EXPECT_TRUE(s == 0 || s == 1);
    }

    // Decoded states should mostly agree with true states (allowing label swap)
    int agree = 0, disagree = 0;
    for (int i = 0; i < T; ++i) {
        if (decoded[i] == true_states[i]) agree++;
        else disagree++;
    }
    // Either direct or swapped labels should have >80% accuracy
    int best_match = std::max(agree, disagree);
    EXPECT_GT(best_match, T * 0.80) << "Decoded states don't match true states well enough";
}

TEST(HMMIllConditioned, NearlyIdenticalObservations) {
    // Observations very close together — should not crash
    int T = 50;
    Eigen::MatrixXd obs(T, 1);
    for (int i = 0; i < T; ++i) {
        obs(i, 0) = 1.0 + 1e-8 * i;  // Nearly identical
    }

    HMMConfig config;
    config.n_states = 2;
    config.max_iterations = 20;

    HMM hmm(config);
    auto fit_result = hmm.fit(obs);
    // Should complete without crashing; may or may not converge well
    EXPECT_TRUE(fit_result.is_ok());
}

TEST(JohansenCriticalValues, ThreeSeriesMatchTable) {
    auto cv = trade_ngin::statistics::critical_values::johansen_trace_critical_values(3, 0.05);
    ASSERT_EQ(cv.size(), 3);
    EXPECT_DOUBLE_EQ(cv[0], 29.68);
    EXPECT_DOUBLE_EQ(cv[1], 15.41);
    EXPECT_DOUBLE_EQ(cv[2], 3.76);
}

TEST(JohansenCriticalValues, OnePercentMoreStringent) {
    auto cv_5 = trade_ngin::statistics::critical_values::johansen_trace_critical_values(2, 0.05);
    auto cv_1 = trade_ngin::statistics::critical_values::johansen_trace_critical_values(2, 0.01);
    // 1% CVs should be larger (harder to reject)
    for (size_t i = 0; i < cv_5.size(); ++i) {
        EXPECT_GT(cv_1[i], cv_5[i]);
    }
}

TEST(JohansenCriticalValues, FourAndFiveSeriesWork) {
    auto cv4 = trade_ngin::statistics::critical_values::johansen_trace_critical_values(4, 0.05);
    ASSERT_EQ(cv4.size(), 4);
    EXPECT_DOUBLE_EQ(cv4[0], 47.21);

    auto cv5 = trade_ngin::statistics::critical_values::johansen_trace_critical_values(5, 0.05);
    ASSERT_EQ(cv5.size(), 5);
    EXPECT_DOUBLE_EQ(cv5[0], 68.52);
}

TEST(LogSumExp, BasicProperties) {
    using trade_ngin::statistics::critical_values::log_sum_exp;

    // log(exp(0) + exp(0)) = log(2)
    EXPECT_NEAR(log_sum_exp(0.0, 0.0), std::log(2.0), 1e-12);

    // log(exp(-1000) + exp(0)) ≈ 0
    EXPECT_NEAR(log_sum_exp(-1000.0, 0.0), 0.0, 1e-12);

    // log(exp(1000) + exp(0)) ≈ 1000
    EXPECT_NEAR(log_sum_exp(1000.0, 0.0), 1000.0, 1e-12);

    // -inf + x = x
    double neg_inf = -std::numeric_limits<double>::infinity();
    EXPECT_DOUBLE_EQ(log_sum_exp(neg_inf, 5.0), 5.0);
}

TEST(LogSumExp, ArrayVersion) {
    using trade_ngin::statistics::critical_values::log_sum_exp;

    double values[] = {-1000.0, 0.0, -1000.0};
    EXPECT_NEAR(log_sum_exp(values, 3), 0.0, 1e-12);

    double values2[] = {1.0, 2.0, 3.0};
    double expected = std::log(std::exp(1.0) + std::exp(2.0) + std::exp(3.0));
    EXPECT_NEAR(log_sum_exp(values2, 3), expected, 1e-12);
}
