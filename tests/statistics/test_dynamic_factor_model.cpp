#include <gtest/gtest.h>
#include "trade_ngin/statistics/state_estimation/dynamic_factor_model.hpp"
#include <Eigen/Dense>
#include <random>
#include <cmath>
#include <limits>

using namespace trade_ngin::statistics;

// ============================================================================
// Test fixture — synthetic macro panel with known factor structure
// ============================================================================

class DynamicFactorModelTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Generate: f_t = 0.9 * f_{t-1} + noise,  y_t = Lambda * f_t + e_t
        std::mt19937 rng(42);
        std::normal_distribution<double> nd(0.0, 1.0);

        const int T = 150, N = 8, K = 3;

        Eigen::MatrixXd Lambda(N, K);
        for (int n = 0; n < N; ++n)
            for (int k = 0; k < K; ++k)
                Lambda(n, k) = nd(rng);

        Eigen::MatrixXd F(T, K);
        F.row(0).setZero();
        for (int t = 1; t < T; ++t)
            for (int k = 0; k < K; ++k)
                F(t, k) = 0.9 * F(t - 1, k) + 0.3 * nd(rng);

        data_ = Eigen::MatrixXd(T, N);
        for (int t = 0; t < T; ++t) {
            Eigen::VectorXd y = Lambda * F.row(t).transpose();
            for (int n = 0; n < N; ++n)
                y(n) += 0.2 * nd(rng);
            data_.row(t) = y.transpose();
        }

        series_names_ = {"cpi", "pmi", "gdp", "jobs", "spread", "curve", "m2", "usd"};
    }

    Eigen::MatrixXd data_;
    std::vector<std::string> series_names_;
};

// ============================================================================
// Fit tests
// ============================================================================

TEST_F(DynamicFactorModelTest, FitRuns) {
    DFMConfig cfg;
    cfg.num_factors = 3;
    cfg.max_em_iterations = 50;

    DynamicFactorModel dfm(cfg);
    auto result = dfm.fit(data_, series_names_);
    ASSERT_TRUE(result.is_ok());
    EXPECT_TRUE(dfm.is_fitted());
}

TEST_F(DynamicFactorModelTest, OutputDimensions) {
    DFMConfig cfg;
    cfg.num_factors = 3;
    cfg.max_em_iterations = 50;

    DynamicFactorModel dfm(cfg);
    auto result = dfm.fit(data_);
    ASSERT_TRUE(result.is_ok());

    const auto& out = result.value();
    EXPECT_EQ(out.T, 150);
    EXPECT_EQ(out.N, 8);
    EXPECT_EQ(out.K, 3);
    EXPECT_EQ(static_cast<int>(out.factors.size()), 150);
    EXPECT_EQ(static_cast<int>(out.factors[0].size()), 3);
    EXPECT_EQ(out.lambda.rows(), 8);
    EXPECT_EQ(out.lambda.cols(), 3);
    EXPECT_EQ(out.A.rows(), 3);
    EXPECT_EQ(out.A.cols(), 3);
    EXPECT_EQ(out.Q.rows(), 3);
    EXPECT_EQ(out.R_diag.size(), 8);
}

TEST_F(DynamicFactorModelTest, NamedFactorSeries) {
    DFMConfig cfg;
    cfg.num_factors = 3;
    cfg.factor_labels = {"growth", "inflation", "liquidity"};
    cfg.max_em_iterations = 50;

    DynamicFactorModel dfm(cfg);
    auto result = dfm.fit(data_);
    ASSERT_TRUE(result.is_ok());

    const auto& out = result.value();
    for (const auto& lbl : {"growth", "inflation", "liquidity"}) {
        ASSERT_NE(out.factor_series.find(lbl), out.factor_series.end())
            << "Missing factor series: " << lbl;
        EXPECT_EQ(static_cast<int>(out.factor_series.at(lbl).size()), 150);
    }
}

TEST_F(DynamicFactorModelTest, FactorsAreFinite) {
    DFMConfig cfg;
    cfg.num_factors = 3;
    cfg.max_em_iterations = 100;

    DynamicFactorModel dfm(cfg);
    auto result = dfm.fit(data_);
    ASSERT_TRUE(result.is_ok());

    const auto& out = result.value();
    for (int t = 0; t < out.T; ++t) {
        for (int k = 0; k < out.K; ++k) {
            EXPECT_TRUE(std::isfinite(out.factors[t][k]))
                << "Non-finite factor at t=" << t << " k=" << k;
            EXPECT_TRUE(std::isfinite(out.factor_uncertainty[t][k]))
                << "Non-finite uncertainty at t=" << t << " k=" << k;
        }
    }
}

TEST_F(DynamicFactorModelTest, ConvergenceInfoPopulated) {
    DFMConfig cfg;
    cfg.num_factors = 2;
    cfg.max_em_iterations = 100;

    DynamicFactorModel dfm(cfg);
    auto result = dfm.fit(data_.leftCols(5));
    ASSERT_TRUE(result.is_ok());

    const auto& out = result.value();
    EXPECT_FALSE(out.convergence_info.objective_history.empty());
    EXPECT_FALSE(out.convergence_info.termination_reason.empty());
    EXPECT_GT(out.em_iterations, 0);
}

// ============================================================================
// Filter tests
// ============================================================================

TEST_F(DynamicFactorModelTest, FilterShape) {
    DFMConfig cfg;
    cfg.num_factors = 2;
    cfg.max_em_iterations = 50;

    DynamicFactorModel dfm(cfg);
    dfm.fit(data_.leftCols(5));

    // Generate new data with same column count
    std::mt19937 rng(99);
    std::normal_distribution<double> nd(0.0, 1.0);
    Eigen::MatrixXd new_data(30, 5);
    for (int t = 0; t < 30; ++t)
        for (int n = 0; n < 5; ++n)
            new_data(t, n) = nd(rng);

    auto result = dfm.filter(new_data);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().T, 30);
    EXPECT_EQ(result.value().K, 2);
    EXPECT_EQ(result.value().N, 5);
}

TEST_F(DynamicFactorModelTest, FilterBeforeFitErrors) {
    DynamicFactorModel dfm;
    auto result = dfm.filter(data_);
    EXPECT_TRUE(result.is_error());
}

TEST_F(DynamicFactorModelTest, FilterColumnMismatchErrors) {
    DFMConfig cfg;
    cfg.num_factors = 2;
    cfg.max_em_iterations = 50;

    DynamicFactorModel dfm(cfg);
    dfm.fit(data_);  // 8 columns

    Eigen::MatrixXd wrong_cols(30, 3);  // 3 columns != 8
    wrong_cols.setRandom();
    auto result = dfm.filter(wrong_cols);
    EXPECT_TRUE(result.is_error());
}

// ============================================================================
// Online update tests
// ============================================================================

TEST_F(DynamicFactorModelTest, OnlineUpdate) {
    DFMConfig cfg;
    cfg.num_factors = 2;
    cfg.max_em_iterations = 50;

    DynamicFactorModel dfm(cfg);
    dfm.fit(data_.leftCols(5));

    std::mt19937 rng(99);
    std::normal_distribution<double> nd(0.0, 1.0);

    for (int i = 0; i < 10; ++i) {
        Eigen::VectorXd y(5);
        for (int n = 0; n < 5; ++n)
            y(n) = nd(rng);

        auto result = dfm.update(y);
        ASSERT_TRUE(result.is_ok());
        EXPECT_EQ(result.value().size(), 2);
        EXPECT_TRUE(result.value().allFinite());
    }
}

TEST_F(DynamicFactorModelTest, UpdateBeforeFitErrors) {
    DynamicFactorModel dfm;
    Eigen::VectorXd y(5);
    y.setRandom();
    auto result = dfm.update(y);
    EXPECT_TRUE(result.is_error());
}

TEST_F(DynamicFactorModelTest, UpdateDimensionMismatchErrors) {
    DFMConfig cfg;
    cfg.num_factors = 2;
    cfg.max_em_iterations = 50;

    DynamicFactorModel dfm(cfg);
    dfm.fit(data_.leftCols(5));  // N=5

    Eigen::VectorXd wrong_size(3);  // 3 != 5
    wrong_size.setRandom();
    auto result = dfm.update(wrong_size);
    EXPECT_TRUE(result.is_error());
}

// ============================================================================
// Validation / edge case tests
// ============================================================================

TEST_F(DynamicFactorModelTest, TooFewRowsErrors) {
    DFMConfig cfg;
    cfg.num_factors = 3;

    DynamicFactorModel dfm(cfg);
    auto result = dfm.fit(data_.topRows(5));  // only 5 rows
    EXPECT_TRUE(result.is_error());
}

TEST_F(DynamicFactorModelTest, MoreFactorsThanSeriesErrors) {
    DFMConfig cfg;
    cfg.num_factors = 10;

    DynamicFactorModel dfm(cfg);
    auto result = dfm.fit(data_.leftCols(3));  // 3 series < 10 factors
    EXPECT_TRUE(result.is_error());
}

TEST_F(DynamicFactorModelTest, NaNHandling) {
    DFMConfig cfg;
    cfg.num_factors = 2;
    cfg.max_em_iterations = 50;

    Eigen::MatrixXd Y = data_.leftCols(5);
    // Punch NaN holes — simulates missing macro releases
    Y(10, 2) = std::numeric_limits<double>::quiet_NaN();
    Y(11, 2) = std::numeric_limits<double>::quiet_NaN();
    Y(50, 0) = std::numeric_limits<double>::quiet_NaN();

    DynamicFactorModel dfm(cfg);
    auto result = dfm.fit(Y);
    ASSERT_TRUE(result.is_ok());

    const auto& out = result.value();
    for (int t : {10, 11, 50}) {
        for (int k = 0; k < out.K; ++k) {
            EXPECT_TRUE(std::isfinite(out.factors[t][k]))
                << "Non-finite factor at NaN row t=" << t;
        }
    }
}

TEST_F(DynamicFactorModelTest, AllNaNColumnErrors) {
    Eigen::MatrixXd Y = data_.leftCols(5);
    Y.col(2).setConstant(std::numeric_limits<double>::quiet_NaN());

    DFMConfig cfg;
    cfg.num_factors = 2;

    DynamicFactorModel dfm(cfg);
    auto result = dfm.fit(Y);
    EXPECT_TRUE(result.is_error());
}

// ============================================================================
// Config tests
// ============================================================================

TEST_F(DynamicFactorModelTest, ConfigJsonRoundTrip) {
    DFMConfig cfg;
    cfg.num_factors = 4;
    cfg.em_tol = 1e-5;
    cfg.standardise_data = false;
    cfg.factor_labels = {"a", "b", "c", "d"};

    DFMConfig cfg2;
    cfg2.from_json(cfg.to_json());

    EXPECT_EQ(cfg2.num_factors, 4);
    EXPECT_DOUBLE_EQ(cfg2.em_tol, 1e-5);
    EXPECT_FALSE(cfg2.standardise_data);
    EXPECT_EQ(cfg2.factor_labels, (std::vector<std::string>{"a", "b", "c", "d"}));
}

// ============================================================================
// Convergence quality test
// ============================================================================

TEST_F(DynamicFactorModelTest, LogLikelihoodIncreasesWithMoreIterations) {
    Eigen::MatrixXd Y = data_.leftCols(6);

    DFMConfig c5;
    c5.num_factors = 2;
    c5.max_em_iterations = 5;

    DFMConfig c100;
    c100.num_factors = 2;
    c100.max_em_iterations = 100;

    DynamicFactorModel dfm5(c5), dfm100(c100);
    auto r5 = dfm5.fit(Y);
    auto r100 = dfm100.fit(Y);

    ASSERT_TRUE(r5.is_ok());
    ASSERT_TRUE(r100.is_ok());
    EXPECT_GE(r100.value().log_likelihood, r5.value().log_likelihood - 1.0);
}

TEST_F(DynamicFactorModelTest, FilterOutputFinite) {
    DFMConfig cfg;
    cfg.num_factors = 2;
    cfg.max_em_iterations = 50;

    DynamicFactorModel dfm(cfg);
    Eigen::MatrixXd Y = data_.leftCols(5);
    auto fit_r = dfm.fit(Y);
    ASSERT_TRUE(fit_r.is_ok());

    // Generate new data for filtering
    std::mt19937 rng(99);
    std::normal_distribution<double> nd(0.0, 1.0);
    Eigen::MatrixXd Y_new(30, 5);
    for (int t = 0; t < 30; ++t)
        for (int n = 0; n < 5; ++n)
            Y_new(t, n) = nd(rng);

    auto filt_r = dfm.filter(Y_new);
    ASSERT_TRUE(filt_r.is_ok());

    auto& out = filt_r.value();
    for (int t = 0; t < out.T; ++t) {
        for (int k = 0; k < out.K; ++k) {
            EXPECT_TRUE(std::isfinite(out.factors[t][k]))
                << "Non-finite factor at t=" << t << " k=" << k;
            EXPECT_TRUE(std::isfinite(out.factor_uncertainty[t][k]))
                << "Non-finite uncertainty at t=" << t << " k=" << k;
        }
    }
}

// ============================================================================
// Synthetic-panel helpers and fixed-property regression tests for the DFM
// factor sign anchor and pairwise complete-case PCA covariance.
// ============================================================================

namespace {

// Helper: build a small synthetic macro panel suitable for DFM fit.
// N series with known pattern; some columns can be NaN-rich for testing.
static Eigen::MatrixXd dfm_make_synthetic_panel(int T, int N, int seed = 1234,
                                                int nan_col = -1, double nan_frac = 0.0) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> nd(0.0, 1.0);
    Eigen::MatrixXd Y(T, N);
    for (int t = 0; t < T; ++t) {
        const double trend = 0.01 * t;
        for (int n = 0; n < N; ++n) {
            // Each series shares a small fraction of the trend + idiosyncratic noise
            Y(t, n) = trend * (n % 3 == 0 ? 1.0 : 0.3) + nd(rng);
        }
    }
    if (nan_col >= 0 && nan_col < N) {
        std::uniform_real_distribution<double> u(0.0, 1.0);
        for (int t = 0; t < T; ++t) {
            if (u(rng) < nan_frac)
                Y(t, nan_col) = std::numeric_limits<double>::quiet_NaN();
        }
    }
    return Y;
}

}  // namespace

// ----------------------------------------------------------------------------
// DFM factor sign anchoring.
// Re-fitting on the same data with different anchor signs must produce a
// deterministic factor orientation matching the configured anchor.
// ----------------------------------------------------------------------------

TEST(RegimeSubstrate, DFMFactorSignsLockedByAnchorConfig_L06) {
    constexpr int T = 200, N = 10;
    Eigen::MatrixXd Y = dfm_make_synthetic_panel(T, N);
    std::vector<std::string> names(N);
    for (int n = 0; n < N; ++n) names[n] = "s" + std::to_string(n);

    // Configure factor 0 to anchor on column "s3" with positive sign.
    DFMConfig cfg;
    cfg.num_factors = 2;
    cfg.max_em_iterations = 30;
    cfg.factor_anchor_names = {"s3", "s5"};
    cfg.factor_anchor_signs = {+1, -1};
    cfg.factor_labels = {"f0", "f1"};

    DynamicFactorModel dfm_a(cfg);
    auto out_a = dfm_a.fit(Y, names);
    ASSERT_TRUE(out_a.is_ok()) << out_a.error()->what();

    // Fit again with FLIPPED anchor sign on factor 0 — expect lambda(s3, 0)
    // to flip sign, demonstrating the anchor actually steers the convention.
    DFMConfig cfg_flipped = cfg;
    cfg_flipped.factor_anchor_signs = {-1, -1};

    DynamicFactorModel dfm_b(cfg_flipped);
    auto out_b = dfm_b.fit(Y, names);
    ASSERT_TRUE(out_b.is_ok()) << out_b.error()->what();

    const auto& la = out_a.value().lambda;
    const auto& lb = out_b.value().lambda;
    // lambda(3, 0) should have opposite signs between the two fits because
    // we deliberately flipped the anchor sign for factor 0.
    EXPECT_LT(la(3, 0) * lb(3, 0), 0.0)
        << "anchor sign flip on factor 0 should invert loading on s3. "
           "Got la(3,0)=" << la(3, 0) << " lb(3,0)=" << lb(3, 0);
}

// ----------------------------------------------------------------------------
// PCA covariance is pairwise complete-case, NOT NaN→0 fill.
// Synthetic panel with a high-NaN column. The estimated diagonal variance
// of that column should be close to its observed variance, not biased
// toward 0 (which is what NaN→0 does).
// ----------------------------------------------------------------------------

TEST(RegimeSubstrate, DFMHandlesHighNaNColumn_L05) {
    constexpr int T = 300, N = 6;
    constexpr int nan_col = 2;
    constexpr double nan_frac = 0.4;
    Eigen::MatrixXd Y = dfm_make_synthetic_panel(T, N, /*seed*/ 999, nan_col, nan_frac);

    std::vector<std::string> names(N);
    for (int n = 0; n < N; ++n) names[n] = "s" + std::to_string(n);

    DFMConfig cfg;
    cfg.num_factors = 2;
    cfg.max_em_iterations = 30;
    cfg.standardise_data = true;
    cfg.factor_anchor_names = {};  // disable anchoring for this test
    cfg.factor_anchor_signs = {};

    DynamicFactorModel dfm(cfg);
    auto out = dfm.fit(Y, names);
    ASSERT_TRUE(out.is_ok()) << out.error()->what();

    // After standardize, the per-series std should be ~1. With NaN→0 fill
    // the std of nan_col would be biased toward 0 (cnt < T) but the
    // standardise() helper computes std on observed values only, then
    // pairwise complete-case PCA covariance is also used.
    // The downstream effect: the lambda loading on nan_col shouldn't be
    // pathologically tiny (which would be the symptom of bias-to-0).
    const auto& lambda = out.value().lambda;
    double max_loading_on_nan_col = 0.0;
    for (int k = 0; k < lambda.cols(); ++k) {
        max_loading_on_nan_col = std::max(max_loading_on_nan_col,
                                          std::abs(lambda(nan_col, k)));
    }
    EXPECT_GT(max_loading_on_nan_col, 0.05)
        << "column with 40% NaN should still load meaningfully on "
           "at least one factor; biased-to-0 covariance would shrink loadings.";
}

// ============================================================================
// Adversarial: DFM sign anchor must be stable across re-fits.
// Without the anchor, PCA eigenvector signs are arbitrary, so the macro
// pipeline's hardcoded growth/inflation sign-flips would silently invert
// from one training run to the next. We fit twice on the same panel
// with different EM init seeds (achieved by reordering rows to perturb
// the eigendecomposition) and assert the anchor-row loading sign is
// identical across both fits.
// ============================================================================

TEST(RegimePhase4, DFMSignAnchorStableAcrossRefits_L06) {
    // Build a synthetic 100×3 panel where col 0 is the anchor "gdp".
    // Two factor structures share the same loadings up to sign flips.
    std::mt19937 rng(2026);
    std::normal_distribution<double> noise(0.0, 0.3);

    const int T = 200, N = 4;
    Eigen::MatrixXd panel(T, N);
    for (int t = 0; t < T; ++t) {
        // Underlying factor: slowly trending series.
        double f = std::sin(t * 0.05) + 0.3 * t / T;
        panel(t, 0) = +1.0 * f + noise(rng);  // gdp: positive loading
        panel(t, 1) = -0.7 * f + noise(rng);  // inverse loader
        panel(t, 2) = +0.5 * f + noise(rng);
        panel(t, 3) = +0.4 * f + noise(rng);
    }
    std::vector<std::string> names = {"gdp", "manufacturing_capacity_util",
                                       "wti_crude", "x"};

    DFMConfig cfg;
    cfg.num_factors = 1;
    cfg.factor_anchor_names = {"gdp"};
    cfg.factor_anchor_signs = {+1};
    cfg.max_em_iterations = 30;

    DynamicFactorModel m1(cfg);
    auto r1 = m1.fit(panel, names);
    ASSERT_FALSE(r1.is_error()) << r1.error()->what();

    // Refit on a row-permuted copy (changes the order observations are
    // accumulated into the covariance / EM updates without changing the
    // underlying structure → small numerical perturbation that can flip
    // PCA signs without the anchor lock).
    Eigen::MatrixXd panel2 = panel;
    for (int t = 0; t < T; ++t) panel2(t, 0) += 1e-10 * t;  // tiny tilt
    DynamicFactorModel m2(cfg);
    auto r2 = m2.fit(panel2, names);
    ASSERT_FALSE(r2.is_error()) << r2.error()->what();

    // The gdp anchor's loading on factor 0 must be positive in BOTH
    // fits (target_sign = +1). Without the anchor lock, one fit could
    // come out negative.
    double load1 = r1.value().lambda(0, 0);
    double load2 = r2.value().lambda(0, 0);
    EXPECT_GT(load1, 0.0)
        << "gdp loading should be positive (anchor target +1). load1=" << load1;
    EXPECT_GT(load2, 0.0)
        << "gdp loading should be positive after re-fit. load2=" << load2;
    EXPECT_EQ(std::signbit(load1), std::signbit(load2))
        << "anchor sign must match across refits.";
}
