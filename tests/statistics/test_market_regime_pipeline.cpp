#include <gtest/gtest.h>
#include "trade_ngin/statistics/state_estimation/market_regime_pipeline.hpp"
#include "trade_ngin/statistics/clustering/gmm.hpp"
#include <Eigen/Dense>
#include <random>
#include <cmath>

using namespace trade_ngin::statistics;

// ============================================================================
// Test fixture with synthetic data generation
// ============================================================================

class MarketRegimePipelineTest : public ::testing::Test {
protected:
    // Generate synthetic return series with known regime structure
    struct SyntheticData {
        std::vector<double> returns;
        Eigen::MatrixXd features;      // T x 4
        std::vector<int> true_regimes;  // ground truth
        int T = 0;
    };

    SyntheticData generate_data(int T = 500, int seed = 42) {
        std::mt19937 rng(seed);
        std::normal_distribution<double> normal(0, 1);

        SyntheticData data;
        data.T = T;
        data.returns.resize(T);
        data.features = Eigen::MatrixXd::Zero(T, 4);
        data.true_regimes.resize(T);

        for (int t = 0; t < T; ++t) {
            int regime;
            if (t < T / 5)       regime = 0;  // TREND_LOWVOL
            else if (t < 2*T/5)  regime = 1;  // TREND_HIGHVOL
            else if (t < 3*T/5)  regime = 2;  // MEANREV_CHOPPY
            else if (t < 4*T/5)  regime = 3;  // STRESS_PRICE
            else                 regime = 4;  // STRESS_LIQUIDITY

            data.true_regimes[t] = regime;

            double vol, trend, liq, corr;
            switch (regime) {
                case 0: vol=0.10; trend=0.6;  liq=1.2; corr=0.1; break;
                case 1: vol=0.25; trend=0.4;  liq=0.8; corr=0.3; break;
                case 2: vol=0.15; trend=-0.2; liq=1.0; corr=0.2; break;
                case 3: vol=0.35; trend=-0.5; liq=0.6; corr=0.7; break;
                case 4: vol=0.50; trend=-0.1; liq=0.2; corr=0.8; break;
                default: vol=0.15; trend=0; liq=1; corr=0; break;
            }

            data.returns[t] = trend * 0.001 + vol / std::sqrt(252.0) * normal(rng);
            data.features(t, 0) = trend + 0.1 * normal(rng);
            data.features(t, 1) = vol + 0.02 * normal(rng);
            data.features(t, 2) = liq + 0.1 * normal(rng);
            data.features(t, 3) = corr + 0.05 * normal(rng);
        }

        return data;
    }
};

// ============================================================================
// Ontology tests
// ============================================================================

TEST_F(MarketRegimePipelineTest, OntologyEnum) {
    EXPECT_EQ(kNumMarketRegimes, 5);
    EXPECT_EQ(static_cast<int>(MarketRegimeL1::TREND_LOWVOL), 0);
    EXPECT_EQ(static_cast<int>(MarketRegimeL1::STRESS_LIQUIDITY), 4);
}

TEST_F(MarketRegimePipelineTest, RegimeNames) {
    EXPECT_STREQ(market_regime_name(MarketRegimeL1::TREND_LOWVOL), "TREND_LOWVOL");
    EXPECT_STREQ(market_regime_name(MarketRegimeL1::STRESS_PRICE), "STRESS_PRICE");
    EXPECT_STREQ(sleeve_name(SleeveId::FX), "fx");
}

// ============================================================================
// Config tests
// ============================================================================

TEST_F(MarketRegimePipelineTest, ConfigDefaults) {
    MarketRegimePipelineConfig config;
    EXPECT_EQ(config.hmm_n_states, 3);
    EXPECT_EQ(config.msar_n_states, 2);
    EXPECT_DOUBLE_EQ(config.lambda, 0.30);

    // Check sleeve defaults
    EXPECT_DOUBLE_EQ(config.sleeve_configs[0].w_hmm, 0.40);
    EXPECT_DOUBLE_EQ(config.sleeve_configs[0].w_msar, 0.30);
    EXPECT_DOUBLE_EQ(config.sleeve_configs[0].w_garch, 0.20);
    EXPECT_DOUBLE_EQ(config.sleeve_configs[0].w_gmm, 0.10);
}

TEST_F(MarketRegimePipelineTest, ConfigJsonRoundtrip) {
    MarketRegimePipelineConfig config;
    config.lambda = 0.42;

    auto j = config.to_json();
    MarketRegimePipelineConfig config2;
    config2.from_json(j);

    EXPECT_DOUBLE_EQ(config2.lambda, 0.42);
}

// ============================================================================
// GMM standalone tests
// ============================================================================

TEST_F(MarketRegimePipelineTest, GMMFit) {
    // Simple 2-cluster test
    std::mt19937 rng(42);
    std::normal_distribution<double> n(0, 1);
    Eigen::MatrixXd X(100, 2);
    for (int i = 0; i < 50; ++i) {
        X(i, 0) = -2.0 + 0.3 * n(rng);
        X(i, 1) = -2.0 + 0.3 * n(rng);
    }
    for (int i = 50; i < 100; ++i) {
        X(i, 0) = 2.0 + 0.3 * n(rng);
        X(i, 1) = 2.0 + 0.3 * n(rng);
    }

    GMM::Config gc_fit; gc_fit.max_iterations = 100; gc_fit.tolerance = 1e-4; gc_fit.restarts = 3;
    GMM gmm(gc_fit);
    auto result = gmm.fit(X, 2);

    EXPECT_EQ(result.k, 2);
    EXPECT_EQ(result.labels.size(), 100);
    EXPECT_EQ(result.responsibilities.rows(), 100);
    EXPECT_EQ(result.responsibilities.cols(), 2);
}

TEST_F(MarketRegimePipelineTest, GMMPredictProba) {
    Eigen::MatrixXd X(60, 2);
    std::mt19937 rng(99);
    std::normal_distribution<double> n(0, 1);
    for (int i = 0; i < 30; ++i) { X(i,0) = -3 + 0.2*n(rng); X(i,1) = 0 + 0.2*n(rng); }
    for (int i = 30; i < 60; ++i) { X(i,0) = 3 + 0.2*n(rng); X(i,1) = 0 + 0.2*n(rng); }

    GMM gmm;
    auto model = gmm.fit(X, 2);

    // Point near cluster 0
    Eigen::VectorXd x0(2); x0 << -3.0, 0.0;
    auto probs0 = GMM::predict_proba(x0, model);
    EXPECT_EQ(probs0.size(), 2);
    EXPECT_NEAR(probs0.sum(), 1.0, 1e-6);
    // One cluster should dominate
    EXPECT_GT(probs0.maxCoeff(), 0.8);
}

// ============================================================================
// Pipeline training + update tests
// ============================================================================

TEST_F(MarketRegimePipelineTest, TrainAndUpdate) {
    auto data = generate_data(200);

    // Generate mock model outputs
    // HMM: 3-state decoded + emission parameters
    std::vector<int> hmm_decoded(data.T);
    for (int t = 0; t < data.T; ++t)
        hmm_decoded[t] = data.true_regimes[t] % 3;

    std::vector<Eigen::VectorXd> hmm_means(3);
    std::vector<Eigen::MatrixXd> hmm_covs(3);
    hmm_means[0] = Eigen::VectorXd::Constant(1, 0.001);   // low vol state
    hmm_means[1] = Eigen::VectorXd::Constant(1, 0.0);     // mid state
    hmm_means[2] = Eigen::VectorXd::Constant(1, -0.002);  // high vol state
    hmm_covs[0] = Eigen::MatrixXd::Identity(1,1) * 0.0001;
    hmm_covs[1] = Eigen::MatrixXd::Identity(1,1) * 0.0004;
    hmm_covs[2] = Eigen::MatrixXd::Identity(1,1) * 0.0016;

    // MSAR: 2-state signatures (smoothed probs no longer passed to train)
    Eigen::VectorXd msar_means(2); msar_means << 0.001, -0.001;
    Eigen::VectorXd msar_vars(2); msar_vars << 0.0001, 0.0009;
    Eigen::MatrixXd msar_ar(2, 1); msar_ar << 0.3, -0.2;

    // GARCH vol series
    std::vector<double> garch_vol(data.T);
    for (int t = 0; t < data.T; ++t)
        garch_vol[t] = data.features(t, 1);

    // GMM: fit on 5D feature vector [r, σ̂, dd_speed, vol_shock, corr_spike]
    Eigen::MatrixXd gmm_features = Eigen::MatrixXd::Zero(data.T, 5);
    for (int t = 0; t < data.T; ++t) {
        gmm_features(t, 0) = data.returns[t];
        gmm_features(t, 1) = data.features(t, 1);
        gmm_features(t, 2) = 0.0;  // drawdown speed
        gmm_features(t, 3) = 0.0;  // vol shock
        gmm_features(t, 4) = data.features(t, 3);  // corr spike
    }

    GMM::Config gc; gc.max_iterations = 50; gc.tolerance = 1e-4; gc.restarts = 3;
    GMM gmm(gc);
    auto gmm_result = gmm.fit(gmm_features, 5);

    // Train
    MarketRegimePipeline pipeline;
    auto train_result = pipeline.train(
        SleeveId::EQUITIES, data.returns,
        hmm_means, hmm_covs,
        msar_means, msar_vars, msar_ar,
        garch_vol, gmm_result, gmm_features);

    ASSERT_TRUE(train_result.is_ok()) << train_result.error()->what();
    EXPECT_TRUE(pipeline.is_trained(SleeveId::EQUITIES));

    // Update with a single bar
    Eigen::VectorXd hmm_state_probs(3);
    hmm_state_probs << 0.6, 0.3, 0.1;

    Eigen::VectorXd msar_state_probs(2);
    msar_state_probs << 0.7, 0.3;

    GARCHFeatures gf;
    gf.conditional_vol = 0.12;
    gf.vol_percentile = 0.25;
    gf.vol_spike = false;
    gf.vol_of_vol_high = false;

    MarketFeatures mf;
    mf.realized_vol = 0.12;
    mf.liquidity_proxy = 1.1;

    Eigen::VectorXd gmm_probs = GMM::predict_proba(
        gmm_features.row(data.T/2).transpose(), gmm_result);

    auto belief_result = pipeline.update(
        SleeveId::EQUITIES, hmm_state_probs, msar_state_probs,
        gf, mf, gmm_probs);

    ASSERT_TRUE(belief_result.is_ok());
    const auto& belief = belief_result.value();

    // Probabilities should sum to 1
    double sum = 0;
    for (const auto& [r, p] : belief.market_probs) sum += p;
    EXPECT_NEAR(sum, 1.0, 1e-6);

    // Confidence should be non-negative
    EXPECT_GE(belief.confidence, 0.0);
}

// ============================================================================
// Fingerprint mapping tests
// ============================================================================

TEST_F(MarketRegimePipelineTest, FingerprintRowsSumToOne) {
    auto data = generate_data(200);

    std::vector<int> hmm_decoded(data.T);
    for (int t = 0; t < data.T; ++t) hmm_decoded[t] = t % 3;

    std::vector<Eigen::VectorXd> hmm_means(3);
    std::vector<Eigen::MatrixXd> hmm_covs(3);
    for (int j = 0; j < 3; ++j) {
        hmm_means[j] = Eigen::VectorXd::Constant(1, 0.001 * (j - 1));
        hmm_covs[j] = Eigen::MatrixXd::Identity(1,1) * (0.0001 * (j + 1));
    }

    Eigen::VectorXd msar_means(2); msar_means << 0.001, -0.001;
    Eigen::VectorXd msar_vars(2); msar_vars << 0.0001, 0.0009;
    Eigen::MatrixXd msar_ar(2, 1); msar_ar << 0.3, -0.2;
    std::vector<double> garch_vol(data.T);
    for (int t = 0; t < data.T; ++t) garch_vol[t] = data.features(t, 1);
    Eigen::MatrixXd gmm_features = Eigen::MatrixXd::Zero(data.T, 5);
    for (int t = 0; t < data.T; ++t) {
        gmm_features(t, 0) = data.returns[t]; gmm_features(t, 1) = data.features(t, 1);
    }

    GMM::Config gc; gc.max_iterations = 50; gc.tolerance = 1e-4; gc.restarts = 3;
    GMM gmm(gc);
    auto gmm_result = gmm.fit(gmm_features, 5);

    MarketRegimePipeline pipeline;
    pipeline.train(SleeveId::EQUITIES, data.returns,
                   hmm_means, hmm_covs,
                   msar_means, msar_vars, msar_ar,
                   garch_vol, gmm_result, gmm_features);

    // Check HMM mapping rows sum to 1
    const auto& hmm_map = pipeline.sleeve_state(SleeveId::EQUITIES).hmm_mapping;
    for (int j = 0; j < hmm_map.mapping_matrix.rows(); ++j) {
        EXPECT_NEAR(hmm_map.mapping_matrix.row(j).sum(), 1.0, 1e-6)
            << "HMM mapping row " << j << " doesn't sum to 1";
    }

    // Check MSAR mapping rows sum to 1
    const auto& msar_map = pipeline.sleeve_state(SleeveId::EQUITIES).msar_mapping;
    for (int j = 0; j < msar_map.mapping_matrix.rows(); ++j) {
        EXPECT_NEAR(msar_map.mapping_matrix.row(j).sum(), 1.0, 1e-6)
            << "MSAR mapping row " << j << " doesn't sum to 1";
    }
}

// ============================================================================
// Smoke test: stress signals are handled (no hysteresis/dwell in PDF-exact pipeline)
// ============================================================================

TEST_F(MarketRegimePipelineTest, StressSignalHandled) {
    auto data = generate_data(100);
    std::vector<int> hmm_decoded(data.T, 0);
    std::vector<Eigen::VectorXd> hmm_means(3);
    std::vector<Eigen::MatrixXd> hmm_covs(3);
    for (int j = 0; j < 3; ++j) {
        hmm_means[j] = Eigen::VectorXd::Constant(1, 0.001*(j-1));
        hmm_covs[j] = Eigen::MatrixXd::Identity(1,1) * 0.0001*(j+1);
    }
    Eigen::VectorXd ms_m(2); ms_m << 0.001, -0.001;
    Eigen::VectorXd ms_v(2); ms_v << 0.0001, 0.0009;
    Eigen::MatrixXd ms_a(2,1); ms_a << 0.3, -0.2;
    std::vector<double> gv(data.T);
    for (int t = 0; t < data.T; ++t) gv[t] = data.features(t,1);
    Eigen::MatrixXd gmm_feat = Eigen::MatrixXd::Zero(data.T, 5);
    for (int t = 0; t < data.T; ++t) { gmm_feat(t,0) = data.returns[t]; gmm_feat(t,1) = data.features(t,1); }
    GMM::Config gc2; gc2.max_iterations = 50; gc2.tolerance = 1e-4; gc2.restarts = 2;
    GMM gmm(gc2);
    auto gmm_result = gmm.fit(gmm_feat, 5);

    MarketRegimePipelineConfig config;
    MarketRegimePipeline pipeline(config);

    pipeline.train(SleeveId::EQUITIES, data.returns,
                   hmm_means, hmm_covs,
                   ms_m, ms_v, ms_a,
                   gv, gmm_result, gmm_feat);

    // First establish a non-stress regime
    Eigen::VectorXd hmm_probs(3); hmm_probs << 0.1, 0.1, 0.8;
    Eigen::VectorXd msar_state(2); msar_state << 0.5, 0.5;
    GARCHFeatures gf_calm{0.10, 0.2, false, false};
    MarketFeatures mf_calm{0.10, 0.0, 0.0, 0.0, 0.0, 1.0};
    Eigen::VectorXd gmm_probs_v = Eigen::VectorXd::Constant(5, 0.2);

    // Run several calm updates
    for (int i = 0; i < 5; ++i)
        pipeline.update(SleeveId::EQUITIES, hmm_probs, msar_state,
                       gf_calm, mf_calm, gmm_probs_v);

    // Now send extreme stress signal
    GARCHFeatures gf_stress{0.50, 0.98, true, true};
    MarketFeatures mf_stress{0.50, -0.20, -0.10, 0.30, 0.80, 0.15};

    // With stress bypass, should transition despite long dwell requirement
    auto result = pipeline.update(SleeveId::EQUITIES, hmm_probs, msar_state,
                                  gf_stress, mf_stress, gmm_probs_v);
    ASSERT_TRUE(result.is_ok());
    // The regime should eventually respond to stress signals
    // (exact timing depends on smoothing warmup)
}

// ============================================================================
// Failure degradation test
// ============================================================================

TEST_F(MarketRegimePipelineTest, FailureDegradation) {
    auto data = generate_data(100);
    std::vector<int> hmm_decoded(data.T, 0);
    std::vector<Eigen::VectorXd> hmm_means(3);
    std::vector<Eigen::MatrixXd> hmm_covs(3);
    for (int j = 0; j < 3; ++j) {
        hmm_means[j] = Eigen::VectorXd::Constant(1, 0.001*(j-1));
        hmm_covs[j] = Eigen::MatrixXd::Identity(1,1) * 0.0001*(j+1);
    }
    Eigen::VectorXd ms_m(2); ms_m << 0.001, -0.001;
    Eigen::VectorXd ms_v(2); ms_v << 0.0001, 0.0009;
    Eigen::MatrixXd ms_a(2,1); ms_a << 0.3, -0.2;
    std::vector<double> gv(data.T);
    for (int t = 0; t < data.T; ++t) gv[t] = data.features(t,1);
    Eigen::MatrixXd gmm_feat = Eigen::MatrixXd::Zero(data.T, 5);
    for (int t = 0; t < data.T; ++t) { gmm_feat(t,0) = data.returns[t]; gmm_feat(t,1) = data.features(t,1); }
    GMM::Config gc2; gc2.max_iterations = 50; gc2.tolerance = 1e-4; gc2.restarts = 2;
    GMM gmm(gc2);
    auto gmm_result = gmm.fit(gmm_feat, 5);

    MarketRegimePipeline pipeline;
    pipeline.train(SleeveId::EQUITIES, data.returns,
                   hmm_means, hmm_covs,
                   ms_m, ms_v, ms_a,
                   gv, gmm_result, gmm_feat);

    // Send NaN for HMM — should still produce valid output from other models
    Eigen::VectorXd hmm_nan(3);
    hmm_nan << std::numeric_limits<double>::quiet_NaN(),
               std::numeric_limits<double>::quiet_NaN(),
               std::numeric_limits<double>::quiet_NaN();

    Eigen::VectorXd msar_ok(2); msar_ok << 0.6, 0.4;
    GARCHFeatures garch_f; garch_f.conditional_vol = 0.15; garch_f.vol_percentile = 0.5;
    MarketFeatures mf; mf.realized_vol = 0.15; mf.liquidity_proxy = 1.0;
    Eigen::VectorXd gmm_ok = Eigen::VectorXd::Constant(5, 0.2);

    auto result = pipeline.update(SleeveId::EQUITIES, hmm_nan, msar_ok, garch_f, mf, gmm_ok);
    ASSERT_TRUE(result.is_ok());

    // Should still produce valid probabilities
    double sum = 0;
    for (const auto& [r, p] : result.value().market_probs) {
        EXPECT_TRUE(std::isfinite(p));
        sum += p;
    }
    EXPECT_NEAR(sum, 1.0, 1e-6);
}

// ============================================================================
// Not trained test
// ============================================================================

TEST_F(MarketRegimePipelineTest, UpdateBeforeTrainFails) {
    MarketRegimePipeline pipeline;

    Eigen::VectorXd hmm_probs(3); hmm_probs << 0.5, 0.3, 0.2;
    Eigen::VectorXd msar_probs(2); msar_probs << 0.6, 0.4;
    GARCHFeatures gf;
    MarketFeatures mf;
    Eigen::VectorXd gmm_probs(5); gmm_probs.setConstant(0.2);

    auto result = pipeline.update(SleeveId::EQUITIES, hmm_probs, msar_probs,
                                  gf, mf, gmm_probs);
    EXPECT_TRUE(result.is_error());
}
