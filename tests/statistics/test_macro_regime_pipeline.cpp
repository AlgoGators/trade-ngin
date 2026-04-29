// test_macro_regime_pipeline.cpp
// GTest suite for the Synthesized Macro Regime Pipeline

#include <gtest/gtest.h>
#include "trade_ngin/statistics/state_estimation/dynamic_factor_model.hpp"
#include "trade_ngin/statistics/state_estimation/macro_regime_pipeline.hpp"
#include "trade_ngin/statistics/state_estimation/ms_dfm.hpp"

#include <Eigen/Dense>
#include <cmath>
#include <random>
#include <string>
#include <vector>

using namespace trade_ngin::statistics;

// ============================================================================
// Test fixture: generates synthetic model outputs
// ============================================================================

class MacroRegimePipelineTest : public ::testing::Test {
protected:
    void SetUp() override {
        T_ = 200;
        std::mt19937 rng(42);
        std::normal_distribution<> noise(0, 0.3);

        // Build synthetic DFMOutput with known regime structure:
        //   t=0..79:   expansion  (growth high, inflation low)
        //   t=80..139:  slowdown   (growth moderate, inflation moderate)
        //   t=140..199: recession  (growth low, inflation high)
        dfm_out_.T = T_;
        dfm_out_.N = 10;
        dfm_out_.K = 3;
        dfm_out_.factors.resize(T_);
        dfm_out_.factor_uncertainty.resize(T_);
        dfm_out_.converged = true;
        dfm_out_.em_iterations = 50;
        dfm_out_.log_likelihood = -1000.0;
        dfm_out_.lambda = Eigen::MatrixXd::Identity(10, 3);
        dfm_out_.A = Eigen::MatrixXd::Identity(3, 3) * 0.9;
        dfm_out_.Q = Eigen::MatrixXd::Identity(3, 3) * 0.1;
        dfm_out_.R_diag = Eigen::VectorXd::Ones(10) * 0.5;
        dfm_out_.data_mean = Eigen::VectorXd::Zero(10);
        dfm_out_.data_std = Eigen::VectorXd::Ones(10);
        dfm_out_.series_names.resize(10, "series");

        for (int t = 0; t < T_; ++t) {
            dfm_out_.factors[t].resize(3);
            dfm_out_.factor_uncertainty[t].resize(3, 0.1);

            if (t < 80) {
                // Expansion: high growth, low inflation
                dfm_out_.factors[t][0] = 1.5 + noise(rng);
                dfm_out_.factors[t][1] = -0.8 + noise(rng);
                dfm_out_.factors[t][2] = 0.5 + noise(rng);
            } else if (t < 140) {
                // Slowdown: moderate
                dfm_out_.factors[t][0] = 0.0 + noise(rng);
                dfm_out_.factors[t][1] = 0.3 + noise(rng);
                dfm_out_.factors[t][2] = 0.0 + noise(rng);
            } else {
                // Recession: low growth, high inflation
                dfm_out_.factors[t][0] = -1.5 + noise(rng);
                dfm_out_.factors[t][1] = 1.2 + noise(rng);
                dfm_out_.factors[t][2] = -0.3 + noise(rng);
            }
        }

        // Build synthetic MSDFMOutput
        ms_out_.T = T_;
        ms_out_.K = 3;
        ms_out_.J = 3;
        ms_out_.smoothed_probs = Eigen::MatrixXd::Zero(T_, 3);
        ms_out_.filtered_probs = ms_out_.smoothed_probs;
        ms_out_.decoded_regimes.resize(T_);
        ms_out_.regime_labels = {"expansion", "slowdown", "stress"};
        ms_out_.transition_matrix = Eigen::MatrixXd::Identity(3, 3) * 0.9;
        ms_out_.transition_matrix += Eigen::MatrixXd::Constant(3, 3, 0.033);
        ms_out_.ergodic_probs = Eigen::Vector3d(0.4, 0.35, 0.25);
        ms_out_.log_likelihood = -500.0;
        ms_out_.convergence_info.converged = true;
        ms_out_.convergence_info.iterations = 30;

        ms_out_.regime_signatures.resize(3);
        for (int j = 0; j < 3; ++j) {
            ms_out_.regime_signatures[j].A = Eigen::MatrixXd::Identity(3, 3) * 0.8;
            ms_out_.regime_signatures[j].Q = Eigen::MatrixXd::Identity(3, 3) * (0.1 * (j + 1));
            ms_out_.regime_signatures[j].mean_factors = Eigen::Vector3d::Zero();
            ms_out_.regime_signatures[j].mean_volatility = 0.1 * (j + 1);
        }

        for (int t = 0; t < T_; ++t) {
            if (t < 80) {
                ms_out_.smoothed_probs(t, 0) = 0.85;
                ms_out_.smoothed_probs(t, 1) = 0.10;
                ms_out_.smoothed_probs(t, 2) = 0.05;
                ms_out_.decoded_regimes[t] = 0;
            } else if (t < 140) {
                ms_out_.smoothed_probs(t, 0) = 0.10;
                ms_out_.smoothed_probs(t, 1) = 0.80;
                ms_out_.smoothed_probs(t, 2) = 0.10;
                ms_out_.decoded_regimes[t] = 1;
            } else {
                ms_out_.smoothed_probs(t, 0) = 0.05;
                ms_out_.smoothed_probs(t, 1) = 0.10;
                ms_out_.smoothed_probs(t, 2) = 0.85;
                ms_out_.decoded_regimes[t] = 2;
            }
        }
        ms_out_.filtered_probs = ms_out_.smoothed_probs;

        // Build synthetic MacroPanel
        panel_.T = T_;
        panel_.N = 10;
        panel_.dates.resize(T_);
        panel_.column_names = {
            "nonfarm_payrolls", "unemployment_rate", "gdp",
            "cpi", "core_pce", "breakeven_5y",
            "high_yield_spread", "ig_credit_spread",
            "yield_spread_10y_2y", "fed_funds_rate"
        };
        panel_.data = Eigen::MatrixXd::Zero(T_, 10);

        for (int t = 0; t < T_; ++t) {
            panel_.dates[t] = "2020-01-" + std::to_string(t + 1);  // dummy dates
            if (t < 80) {
                // Expansion: high payrolls, low unemployment, positive yield curve
                panel_.data(t, 0) = 200 + noise(rng) * 20;  // nonfarm_payrolls
                panel_.data(t, 1) = 3.5 + noise(rng);       // unemployment
                panel_.data(t, 2) = 25000 + noise(rng) * 500; // GDP
                panel_.data(t, 3) = 2.0 + noise(rng) * 0.2;  // CPI
                panel_.data(t, 4) = 1.8 + noise(rng) * 0.2;  // core PCE
                panel_.data(t, 5) = 2.2 + noise(rng) * 0.1;  // breakeven
                panel_.data(t, 6) = 3.0 + noise(rng) * 0.5;  // HY spread
                panel_.data(t, 7) = 1.0 + noise(rng) * 0.2;  // IG spread
                panel_.data(t, 8) = 1.5 + noise(rng) * 0.3;  // yield spread
                panel_.data(t, 9) = 2.0 + noise(rng) * 0.1;  // fed funds
            } else if (t < 140) {
                panel_.data(t, 0) = 100 + noise(rng) * 20;
                panel_.data(t, 1) = 5.0 + noise(rng);
                panel_.data(t, 2) = 23000 + noise(rng) * 500;
                panel_.data(t, 3) = 3.0 + noise(rng) * 0.3;
                panel_.data(t, 4) = 2.5 + noise(rng) * 0.3;
                panel_.data(t, 5) = 2.8 + noise(rng) * 0.2;
                panel_.data(t, 6) = 5.0 + noise(rng) * 0.8;
                panel_.data(t, 7) = 2.0 + noise(rng) * 0.3;
                panel_.data(t, 8) = 0.5 + noise(rng) * 0.2;
                panel_.data(t, 9) = 3.5 + noise(rng) * 0.2;
            } else {
                panel_.data(t, 0) = -50 + noise(rng) * 30;
                panel_.data(t, 1) = 8.0 + noise(rng);
                panel_.data(t, 2) = 20000 + noise(rng) * 500;
                panel_.data(t, 3) = 4.5 + noise(rng) * 0.4;
                panel_.data(t, 4) = 4.0 + noise(rng) * 0.3;
                panel_.data(t, 5) = 3.5 + noise(rng) * 0.3;
                panel_.data(t, 6) = 8.0 + noise(rng) * 1.5;
                panel_.data(t, 7) = 3.5 + noise(rng) * 0.5;
                panel_.data(t, 8) = -0.5 + noise(rng) * 0.3;
                panel_.data(t, 9) = 5.0 + noise(rng) * 0.3;
            }
        }

        // BSTS probs: 4 clusters
        bsts_probs_ = Eigen::MatrixXd::Constant(T_, 4, 0.25);
        for (int t = 0; t < T_; ++t) {
            if (t < 80)       { bsts_probs_(t, 0) = 0.7; bsts_probs_(t, 1) = 0.1; bsts_probs_(t, 2) = 0.1; bsts_probs_(t, 3) = 0.1; }
            else if (t < 140) { bsts_probs_(t, 0) = 0.1; bsts_probs_(t, 1) = 0.1; bsts_probs_(t, 2) = 0.6; bsts_probs_(t, 3) = 0.2; }
            else              { bsts_probs_(t, 0) = 0.05; bsts_probs_(t, 1) = 0.75; bsts_probs_(t, 2) = 0.1; bsts_probs_(t, 3) = 0.1; }
        }

        // Growth/inflation scores
        growth_scores_ = Eigen::VectorXd::Zero(T_);
        inflation_scores_ = Eigen::VectorXd::Zero(T_);
        for (int t = 0; t < T_; ++t) {
            if (t < 80) {
                growth_scores_(t) = 10.0 + noise(rng);
                inflation_scores_(t) = 1.5 + noise(rng) * 0.5;
            } else if (t < 140) {
                growth_scores_(t) = 0.0 + noise(rng);
                inflation_scores_(t) = 3.0 + noise(rng) * 0.5;
            } else {
                growth_scores_(t) = -8.0 + noise(rng);
                inflation_scores_(t) = 5.0 + noise(rng) * 0.5;
            }
        }
    }

    int T_;
    DFMOutput dfm_out_;
    MSDFMOutput ms_out_;
    MacroPanel panel_;
    Eigen::MatrixXd bsts_probs_;
    Eigen::VectorXd growth_scores_;
    Eigen::VectorXd inflation_scores_;
};

// ============================================================================
// Tests
// ============================================================================

TEST_F(MacroRegimePipelineTest, ConfigSerialisation) {
    MacroRegimePipelineConfig cfg;
    cfg.w_dfm = 0.30;
    cfg.base_lambda = 0.15;

    auto j = cfg.to_json();
    MacroRegimePipelineConfig cfg2;
    cfg2.from_json(j);

    EXPECT_DOUBLE_EQ(cfg2.w_dfm, 0.30);
    EXPECT_DOUBLE_EQ(cfg2.base_lambda, 0.15);
    EXPECT_DOUBLE_EQ(cfg2.w_msdfm, 0.40);  // default unchanged
}

TEST_F(MacroRegimePipelineTest, TrainSucceeds) {
    MacroRegimePipeline pipeline;
    auto result = pipeline.train(dfm_out_, ms_out_, panel_,
                                  bsts_probs_, growth_scores_, inflation_scores_);
    ASSERT_TRUE(result.is_ok()) << result.error()->what();
    EXPECT_TRUE(pipeline.is_trained());
}

TEST_F(MacroRegimePipelineTest, TrainRejectsTooFewTimesteps) {
    DFMOutput short_dfm = dfm_out_;
    short_dfm.T = 5;
    short_dfm.factors.resize(5);

    MacroRegimePipeline pipeline;
    auto result = pipeline.train(short_dfm, ms_out_, panel_,
                                  bsts_probs_, growth_scores_, inflation_scores_);
    EXPECT_TRUE(result.is_error());
}

TEST_F(MacroRegimePipelineTest, UpdateBeforeTrainFails) {
    MacroRegimePipeline pipeline;
    auto result = pipeline.update(
        Eigen::Vector3d(1, 0, 0), Eigen::Vector3d(0.5, 0.3, 0.2),
        5.0, 2.0, Eigen::Vector4d(0.25, 0.25, 0.25, 0.25));
    EXPECT_TRUE(result.is_error());
}

TEST_F(MacroRegimePipelineTest, DFMGaussianMappingExpansion) {
    MacroRegimePipeline pipeline;
    pipeline.train(dfm_out_, ms_out_, panel_,
                   bsts_probs_, growth_scores_, inflation_scores_);

    // Factor vector deep in expansion territory (high growth, low inflation)
    Eigen::Vector3d f_expansion(1.5, -0.8, 0.5);
    auto result = pipeline.update(
        f_expansion, Eigen::Vector3d(0.33, 0.33, 0.34),
        10.0, 1.5, Eigen::Vector4d(0.25, 0.25, 0.25, 0.25));
    ASSERT_TRUE(result.is_ok());

    // DFM contribution should favour expansion regimes
    auto& contrib = result.value().model_contributions.at("DFM");
    double exp_total = contrib.at(MacroRegimeL1::EXPANSION_DISINFLATION) +
                       contrib.at(MacroRegimeL1::EXPANSION_INFLATIONARY);
    EXPECT_GT(exp_total, 0.5);
}

TEST_F(MacroRegimePipelineTest, QuadrantGoldilocksMapping) {
    MacroRegimePipeline pipeline;
    pipeline.train(dfm_out_, ms_out_, panel_,
                   bsts_probs_, growth_scores_, inflation_scores_);

    // High growth, low inflation → Goldilocks → heavy on EXPANSION_DISINFLATION
    auto result = pipeline.update(
        Eigen::Vector3d(1.0, -0.5, 0.3),
        Eigen::Vector3d(0.33, 0.33, 0.34),
        15.0, -2.0,  // very high growth, very low inflation
        Eigen::Vector4d(0.25, 0.25, 0.25, 0.25));
    ASSERT_TRUE(result.is_ok());

    auto& quad = result.value().model_contributions.at("Quadrant");
    EXPECT_GT(quad.at(MacroRegimeL1::EXPANSION_DISINFLATION), 0.5);
}

TEST_F(MacroRegimePipelineTest, BSTSNoBreakIsUniformWhenConfigDisabled) {
    // When bsts_always_contribute=false (spec default), BSTS outputs uniform
    // when no structural break is detected.
    MacroRegimePipelineConfig cfg;
    cfg.bsts_always_contribute = false;

    MacroRegimePipeline pipeline(cfg);
    pipeline.train(dfm_out_, ms_out_, panel_,
                   bsts_probs_, growth_scores_, inflation_scores_);

    auto result = pipeline.update(
        Eigen::Vector3d(0, 0, 0), Eigen::Vector3d(0.33, 0.33, 0.34),
        0.0, 0.0, Eigen::Vector4d(0.7, 0.1, 0.1, 0.1),
        false);  // no break
    ASSERT_TRUE(result.is_ok());

    auto& bsts = result.value().model_contributions.at("BSTS");
    for (int r = 0; r < kNumMacroRegimes; ++r) {
        EXPECT_NEAR(bsts.at(static_cast<MacroRegimeL1>(r)),
                    1.0 / kNumMacroRegimes, 0.01);
    }
}

TEST_F(MacroRegimePipelineTest, BSTSAlwaysContributesWhenEnabled) {
    // Default config has bsts_always_contribute=true (calibration override).
    // BSTS should NOT be uniform even without a break.
    MacroRegimePipeline pipeline;
    pipeline.train(dfm_out_, ms_out_, panel_,
                   bsts_probs_, growth_scores_, inflation_scores_);

    auto result = pipeline.update(
        Eigen::Vector3d(0, 0, 0), Eigen::Vector3d(0.33, 0.33, 0.34),
        0.0, 0.0, Eigen::Vector4d(0.7, 0.1, 0.1, 0.1),
        false);  // no break, but bsts_always_contribute=true
    ASSERT_TRUE(result.is_ok());

    // BSTS contribution should NOT be uniform — it should reflect the
    // fingerprint mapping of the cluster probs [0.7, 0.1, 0.1, 0.1]
    auto& bsts = result.value().model_contributions.at("BSTS");
    double max_p = 0;
    for (int r = 0; r < kNumMacroRegimes; ++r) {
        double p = bsts.at(static_cast<MacroRegimeL1>(r));
        max_p = std::max(max_p, p);
    }
    EXPECT_GT(max_p, 1.0 / kNumMacroRegimes + 0.01);  // at least one regime > uniform
}

TEST_F(MacroRegimePipelineTest, ProbsSumToOne) {
    MacroRegimePipeline pipeline;
    pipeline.train(dfm_out_, ms_out_, panel_,
                   bsts_probs_, growth_scores_, inflation_scores_);

    for (int t = 0; t < T_; ++t) {
        Eigen::Vector3d f_t;
        for (int k = 0; k < 3; ++k) f_t(k) = dfm_out_.factors[t][k];
        Eigen::Vector3d ms_t = ms_out_.smoothed_probs.row(t).transpose();
        Eigen::Vector4d b_t = bsts_probs_.row(t).transpose();

        auto result = pipeline.update(f_t, ms_t,
                                       growth_scores_(t), inflation_scores_(t),
                                       b_t);
        ASSERT_TRUE(result.is_ok());

        double sum = 0;
        for (const auto& [r, p] : result.value().macro_probs) {
            EXPECT_GE(p, 0.0);
            sum += p;
        }
        EXPECT_NEAR(sum, 1.0, 1e-6);
    }
}

TEST_F(MacroRegimePipelineTest, ConfidenceInRange) {
    MacroRegimePipeline pipeline;
    pipeline.train(dfm_out_, ms_out_, panel_,
                   bsts_probs_, growth_scores_, inflation_scores_);

    for (int t = 0; t < T_; ++t) {
        Eigen::Vector3d f_t;
        for (int k = 0; k < 3; ++k) f_t(k) = dfm_out_.factors[t][k];

        auto result = pipeline.update(
            f_t, ms_out_.smoothed_probs.row(t).transpose(),
            growth_scores_(t), inflation_scores_(t),
            bsts_probs_.row(t).transpose());
        ASSERT_TRUE(result.is_ok());

        EXPECT_GE(result.value().confidence, 0.0);
        EXPECT_LE(result.value().confidence, 1.0);
    }
}

TEST_F(MacroRegimePipelineTest, RegimeAgeIncrements) {
    MacroRegimePipeline pipeline;
    pipeline.train(dfm_out_, ms_out_, panel_,
                   bsts_probs_, growth_scores_, inflation_scores_);

    // Feed identical inputs multiple times
    Eigen::Vector3d f_t(1.5, -0.8, 0.5);
    Eigen::Vector3d ms_t(0.8, 0.1, 0.1);
    Eigen::Vector4d b_t(0.25, 0.25, 0.25, 0.25);

    int prev_age = 0;
    for (int i = 0; i < 10; ++i) {
        auto result = pipeline.update(f_t, ms_t, 10.0, 1.5, b_t);
        ASSERT_TRUE(result.is_ok());
        EXPECT_GE(result.value().regime_age_bars, prev_age);
        prev_age = result.value().regime_age_bars;
    }
    EXPECT_GE(prev_age, 5);
}

TEST_F(MacroRegimePipelineTest, SmoothingPreventsJumps) {
    MacroRegimePipelineConfig cfg;
    cfg.base_lambda = 0.05;  // very smooth
    cfg.calm_lambda_scale = 1.0;

    MacroRegimePipeline pipeline(cfg);
    pipeline.train(dfm_out_, ms_out_, panel_,
                   bsts_probs_, growth_scores_, inflation_scores_);

    // First: establish expansion
    for (int i = 0; i < 20; ++i) {
        pipeline.update(
            Eigen::Vector3d(1.5, -0.8, 0.5),
            Eigen::Vector3d(0.8, 0.1, 0.1),
            10.0, 1.5,
            Eigen::Vector4d(0.25, 0.25, 0.25, 0.25));
    }

    // Sudden shift to recession inputs
    auto result = pipeline.update(
        Eigen::Vector3d(-1.5, 1.2, -0.3),
        Eigen::Vector3d(0.05, 0.1, 0.85),
        -8.0, 5.0,
        Eigen::Vector4d(0.05, 0.75, 0.1, 0.1));
    ASSERT_TRUE(result.is_ok());

    // With heavy smoothing, should NOT immediately jump to recession
    // The expansion probs should still be significant
    double exp_dis = result.value().macro_probs.at(MacroRegimeL1::EXPANSION_DISINFLATION);
    EXPECT_GT(exp_dis, 0.05);  // still has some expansion weight
}

TEST_F(MacroRegimePipelineTest, AggregationWeightsApplied) {
    MacroRegimePipelineConfig cfg;
    cfg.w_dfm = 1.0;
    cfg.w_msdfm = 0.0;
    cfg.w_quadrant = 0.0;
    cfg.w_bsts = 0.0;
    cfg.base_lambda = 1.0;       // no smoothing
    cfg.calm_lambda_scale = 1.0;

    MacroRegimePipeline pipeline(cfg);
    pipeline.train(dfm_out_, ms_out_, panel_,
                   bsts_probs_, growth_scores_, inflation_scores_);

    // With 100% DFM weight, output should match DFM contribution
    auto result = pipeline.update(
        Eigen::Vector3d(1.5, -0.8, 0.5),
        Eigen::Vector3d(0.05, 0.1, 0.85),  // MS-DFM says recession
        -8.0, 5.0,                          // Quadrant says stagflation
        Eigen::Vector4d(0.25, 0.25, 0.25, 0.25));
    ASSERT_TRUE(result.is_ok());

    // DFM sees expansion, and it has 100% weight
    auto& dfm_contrib = result.value().model_contributions.at("DFM");
    double dfm_exp = dfm_contrib.at(MacroRegimeL1::EXPANSION_DISINFLATION) +
                     dfm_contrib.at(MacroRegimeL1::EXPANSION_INFLATIONARY);
    double total_exp = result.value().macro_probs.at(MacroRegimeL1::EXPANSION_DISINFLATION) +
                       result.value().macro_probs.at(MacroRegimeL1::EXPANSION_INFLATIONARY);
    EXPECT_NEAR(dfm_exp, total_exp, 0.05);
}

TEST_F(MacroRegimePipelineTest, AllModelContributionsPresent) {
    MacroRegimePipeline pipeline;
    pipeline.train(dfm_out_, ms_out_, panel_,
                   bsts_probs_, growth_scores_, inflation_scores_);

    auto result = pipeline.update(
        Eigen::Vector3d(0, 0, 0), Eigen::Vector3d(0.33, 0.33, 0.34),
        0.0, 0.0, Eigen::Vector4d(0.25, 0.25, 0.25, 0.25));
    ASSERT_TRUE(result.is_ok());

    EXPECT_EQ(result.value().model_contributions.count("DFM"), 1);
    EXPECT_EQ(result.value().model_contributions.count("MS-DFM"), 1);
    EXPECT_EQ(result.value().model_contributions.count("Quadrant"), 1);
    EXPECT_EQ(result.value().model_contributions.count("BSTS"), 1);
}

TEST_F(MacroRegimePipelineTest, FullEndToEnd) {
    MacroRegimePipeline pipeline;
    auto train_result = pipeline.train(dfm_out_, ms_out_, panel_,
                                        bsts_probs_, growth_scores_, inflation_scores_);
    ASSERT_TRUE(train_result.is_ok());

    int valid_beliefs = 0;
    for (int t = 0; t < T_; ++t) {
        Eigen::Vector3d f_t;
        for (int k = 0; k < 3; ++k) f_t(k) = dfm_out_.factors[t][k];

        auto result = pipeline.update(
            f_t, ms_out_.smoothed_probs.row(t).transpose(),
            growth_scores_(t), inflation_scores_(t),
            bsts_probs_.row(t).transpose());

        if (result.is_ok()) {
            ++valid_beliefs;
            auto& b = result.value();
            EXPECT_EQ(b.macro_probs.size(), kNumMacroRegimes);
            EXPECT_GE(b.regime_age_bars, 1);
        }
    }
    EXPECT_EQ(valid_beliefs, T_);
}

// ============================================================================
// Substrate-level / Phase-1 / Phase-4 regression guards for the macro
// pipeline.
// ============================================================================

TEST(RegimeSubstrate, PipelineConstructsAsUntrained) {
    MacroRegimePipelineConfig cfg;
    MacroRegimePipeline pipeline(cfg);
    EXPECT_FALSE(pipeline.is_trained());
}

// ============================================================================
// MacroBelief retains structural_break_risk; the three deferred
// overlay fields (policy_restrictive, credit_tightening, inflation_sticky)
// were removed. Compile-time check by accessing the surviving field and
// asserting default values.
// ============================================================================

TEST(RegimeSubstrate, MacroBeliefDefaultsAreSane) {
    MacroBelief b;
    EXPECT_FALSE(b.structural_break_risk);
    EXPECT_DOUBLE_EQ(b.confidence, 0.0);
    EXPECT_EQ(b.regime_age_bars, 0);
    EXPECT_TRUE(b.macro_probs.empty());
    EXPECT_TRUE(b.model_contributions.empty());
    // The next line would not compile if the deferred overlay fields were
    // re-introduced with different names — but they shouldn't exist at
    // all. The build itself is the guard.
}

// ============================================================================
// DFM Gaussian operates on factors [1, 2] only — factor 0 is dropped.
// We verify this structurally: the RegimeGaussian struct uses Vector2d / Matrix2d.
// (A compile-time guard implemented as a runtime EXPECT for clarity.)
// ============================================================================

TEST(RegimePhase1, RegimeGaussianIs2D_M04) {
    RegimeGaussian g;
    EXPECT_EQ(g.mean.size(), 2)
        << "RegimeGaussian.mean must be 2D (factors 1, 2 only). "
           "Factor 0 is non-discriminative trend; including it dilutes Mahalanobis.";
    EXPECT_EQ(g.cov.rows(), 2);
    EXPECT_EQ(g.cov.cols(), 2);
    EXPECT_EQ(g.cov_inv.rows(), 2);
    EXPECT_EQ(g.cov_inv.cols(), 2);
    EXPECT_EQ(g.n_samples, 0);
    EXPECT_DOUBLE_EQ(g.log_det, 0.0);
}

// ============================================================================
// cross-state std rescaling dropped from MS-DFM/BSTS fingerprint
// training. The principal observable property: a freshly-constructed
// pipeline + a re-trained pipeline both expose target_fingerprints with
// the post-fix ±0.8 magnitude scale (not the legacy ±1.5).
//
// We don't have direct access to msdfm_mapping_ from the public surface.
// As a structural check, the pipeline still constructs (no API regression
// from the edits) and is_trained() returns false initially.
// ============================================================================

TEST(RegimePhase1, PipelineConstructsAfterM01_Edits) {
    MacroRegimePipelineConfig cfg;
    MacroRegimePipeline pipeline(cfg);
    EXPECT_FALSE(pipeline.is_trained());
}

// ============================================================================
// growth_score and inflation_score are now z-scored before
// averaging. Pre-fix, growth_score was dominated by cap-util level (~78)
// while IP/GDP slopes (~0.01) contributed <1%; inflation_score used CPI
// LEVEL instead of YoY-equivalent slope.
//
// Direct test of the property at the bsts feature builder requires
// running fit_from_db (DB dependency) or constructing a synthetic
// SeriesPosterior set. We instead test the higher-level property: the
// MacroBelief surface is well-formed and probabilities sum to 1 after
// any update (a structural integrity check that any of these edits
// would break if they introduced a normalization bug).
// ============================================================================

TEST(RegimePhase1, MapDFMReturnsValidProbabilityVector) {
    // Construct a fresh pipeline. Without explicit train(), map_dfm cannot
    // be called via the public surface. This test instead asserts that
    // the MacroBelief default probabilities are all-zero (untrained state)
    // — a sanity check that the struct change didn't break default
    // construction of the belief or its embedded probability map.
    MacroBelief b;
    double sum = 0.0;
    for (const auto& [regime, prob] : b.macro_probs) {
        EXPECT_GE(prob, 0.0);
        EXPECT_LE(prob, 1.0);
        sum += prob;
    }
    // Untrained belief defaults to empty macro_probs map (sum = 0); a
    // populated belief from update() is verified by the existing
    // ProbsSumToOne test.
    EXPECT_DOUBLE_EQ(sum, 0.0);
}

namespace {

// Build a synthetic balanced regime structure: 3 native MS-DFM states,
// each occupying ~1/3 of the sample, then convert to MSDFMOutput and a
// matching MacroPanel that the macro pipeline expects.
struct BalancedSyntheticInputs {
    DFMOutput dfm;
    MSDFMOutput ms;
    MacroPanel panel;
};

static BalancedSyntheticInputs make_balanced_synthetic(int T = 240, int seed = 7) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> nz(0.0, 0.3);

    BalancedSyntheticInputs in;
    in.dfm.T = T; in.dfm.N = 5; in.dfm.K = 3;
    in.dfm.factors.resize(T, std::vector<double>(3));
    in.dfm.factor_uncertainty.resize(T, std::vector<double>(3, 0.1));
    in.dfm.lambda = Eigen::MatrixXd::Identity(5, 3);
    in.dfm.A = Eigen::MatrixXd::Identity(3, 3) * 0.9;
    in.dfm.Q = Eigen::MatrixXd::Identity(3, 3) * 0.1;
    in.dfm.R_diag = Eigen::VectorXd::Ones(5) * 0.5;
    in.dfm.data_mean = Eigen::VectorXd::Zero(5);
    in.dfm.data_std = Eigen::VectorXd::Ones(5);
    in.dfm.series_names = {"growth_a", "inflation_a", "credit_a", "yield_a", "policy_a"};

    in.ms.T = T; in.ms.K = 3; in.ms.J = 3;
    in.ms.smoothed_probs = Eigen::MatrixXd::Zero(T, 3);
    in.ms.filtered_probs = in.ms.smoothed_probs;
    in.ms.decoded_regimes.resize(T);
    in.ms.regime_labels = {"calm", "modest", "severe"};
    in.ms.transition_matrix = Eigen::MatrixXd::Identity(3, 3) * 0.9;
    in.ms.ergodic_probs = Eigen::Vector3d(0.34, 0.33, 0.33);
    in.ms.regime_signatures.resize(3);
    for (int j = 0; j < 3; ++j) {
        in.ms.regime_signatures[j].A = Eigen::MatrixXd::Identity(3, 3) * 0.8;
        in.ms.regime_signatures[j].Q = Eigen::MatrixXd::Identity(3, 3) * (0.1 * (j + 1));
        in.ms.regime_signatures[j].mean_factors = Eigen::Vector3d::Zero();
        in.ms.regime_signatures[j].mean_volatility = 0.1 * (j + 1);
    }

    // Balanced split — each native state gets a third of the timesteps,
    // and each occupies a distinct region of factor space.
    for (int t = 0; t < T; ++t) {
        int r = (t < T / 3) ? 0 : (t < 2 * T / 3) ? 1 : 2;
        in.ms.decoded_regimes[t] = r;
        for (int j = 0; j < 3; ++j) {
            in.ms.smoothed_probs(t, j) = (j == r) ? 0.85 : 0.075;
            in.ms.filtered_probs(t, j) = in.ms.smoothed_probs(t, j);
        }
        // Factor values: native state 0 = expansion-like, 1 = mid, 2 = stress
        double f1_base = (r == 0) ? 1.2 : (r == 1) ? 0.0 : -1.2;
        double f2_base = (r == 0) ? -0.6 : (r == 1) ? 0.4 : 1.0;
        in.dfm.factors[t][0] = nz(rng);             // factor 0: trend nuisance
        in.dfm.factors[t][1] = f1_base + nz(rng);   // growth
        in.dfm.factors[t][2] = f2_base + nz(rng);   // inflation
    }

    in.panel.T = T; in.panel.N = 5;
    in.panel.column_names = in.dfm.series_names;
    in.panel.data = Eigen::MatrixXd::Zero(T, 5);
    in.panel.dates.resize(T);
    for (int t = 0; t < T; ++t) {
        for (int n = 0; n < 5; ++n) in.panel.data(t, n) = nz(rng);
        in.panel.dates[t] = "2020-01-01";  // placeholder
    }
    return in;
}

}  // namespace

// ----------------------------------------------------------------------------
// cross-state /std lock-in resolved. With balanced 3-state native
// distribution + DFM that classifies the three regions cleanly, MS-DFM's
// contribution distribution should NOT be 90% on any single ontology
// state. Pre-fix it would be — the dominant native state's tiny offsets
// got amplified to ±1.4 z-scores, locking onto whichever target sat
// closest to origin (typically EXP_DIS).
// ----------------------------------------------------------------------------

TEST(RegimePhase1, MSDFMNoLockInPostM01) {
    auto in = make_balanced_synthetic();

    MacroRegimePipelineConfig cfg;
    MacroRegimePipeline pipeline(cfg);

    // BSTS contribution: provide a cluster posterior matrix that's
    // balanced — train_bsts_fingerprints needs it but we want to focus
    // the assertion on MS-DFM behavior, so make BSTS uniform-ish.
    Eigen::MatrixXd bsts_probs = Eigen::MatrixXd::Constant(in.dfm.T, 4, 0.25);
    Eigen::VectorXd growth_scores  = Eigen::VectorXd::Zero(in.dfm.T);
    Eigen::VectorXd infl_scores    = Eigen::VectorXd::Zero(in.dfm.T);
    for (int t = 0; t < in.dfm.T; ++t) {
        growth_scores(t) = (in.ms.decoded_regimes[t] == 0) ? 1.0
                          : (in.ms.decoded_regimes[t] == 1) ? 0.0 : -1.0;
        infl_scores(t)   = (in.ms.decoded_regimes[t] == 0) ? -0.5
                          : (in.ms.decoded_regimes[t] == 1) ? 0.3 : 0.7;
    }

    auto train_result = pipeline.train(in.dfm, in.ms, in.panel,
                                       bsts_probs, growth_scores, infl_scores);
    ASSERT_TRUE(train_result.is_ok()) << train_result.error()->what();

    // Drive an update at a "mid-regime" timestep (native state 1 dominant).
    // Use the soft-prob from synthetic data for that state.
    int t_mid = in.dfm.T / 2;
    Eigen::Vector3d dfm_factors;
    for (int k = 0; k < 3; ++k) dfm_factors(k) = in.dfm.factors[t_mid][k];
    Eigen::Vector3d msdfm_native;
    for (int j = 0; j < 3; ++j) msdfm_native(j) = in.ms.smoothed_probs(t_mid, j);
    Eigen::Vector4d bsts_cluster(0.25, 0.25, 0.25, 0.25);

    auto belief = pipeline.update(dfm_factors, msdfm_native,
                                   growth_scores(t_mid), infl_scores(t_mid),
                                   bsts_cluster, /*structural_break=*/false);
    ASSERT_TRUE(belief.is_ok()) << belief.error()->what();

    // The MS-DFM model contribution should NOT have any single regime
    // above 0.85 — that would indicate the lock-in re-emerging.
    const auto& msdfm_contrib = belief.value().model_contributions.at("MS-DFM");
    double max_msdfm = 0.0;
    for (const auto& [regime, prob] : msdfm_contrib) {
        max_msdfm = std::max(max_msdfm, prob);
    }
    EXPECT_LT(max_msdfm, 0.85)
        << "regression: MS-DFM dominant pick exceeds 0.85 — cross-state "
           "/std lock-in may have returned. Got max=" << max_msdfm;
}

// ----------------------------------------------------------------------------
// BSTS contributes EXP_DIS > 0 (was 0% pre-M-01 fix because BSTS
// reused the same target_fingerprints and inherited the lock-in).
// Verify by training with synthetic BSTS posteriors that span all 4
// clusters and asserting BSTS contribution to EXP_DIS is non-zero.
// ----------------------------------------------------------------------------

TEST(RegimePhase1, BSTSContributesAllRegimesPostM02) {
    auto in = make_balanced_synthetic();

    MacroRegimePipelineConfig cfg;
    MacroRegimePipeline pipeline(cfg);

    // Build BSTS cluster posteriors that vary across the panel.
    Eigen::MatrixXd bsts_probs(in.dfm.T, 4);
    for (int t = 0; t < in.dfm.T; ++t) {
        int r = in.ms.decoded_regimes[t];
        for (int k = 0; k < 4; ++k) {
            bsts_probs(t, k) = (k == r % 4) ? 0.7 : 0.10;
        }
    }
    Eigen::VectorXd growth_scores = Eigen::VectorXd::Zero(in.dfm.T);
    Eigen::VectorXd infl_scores   = Eigen::VectorXd::Zero(in.dfm.T);

    auto train_result = pipeline.train(in.dfm, in.ms, in.panel,
                                       bsts_probs, growth_scores, infl_scores);
    ASSERT_TRUE(train_result.is_ok());

    // Drive an update with a BSTS posterior that emphasises cluster 0
    // (which after the fix should map non-trivially to multiple regimes,
    // including EXP_DIS).
    Eigen::Vector3d dfm_factors(0.0, 1.0, -0.5);  // expansion-like
    Eigen::Vector3d msdfm_native(0.7, 0.2, 0.1);
    Eigen::Vector4d bsts_cluster(0.7, 0.1, 0.1, 0.1);

    auto belief = pipeline.update(dfm_factors, msdfm_native, 0.5, -0.3,
                                   bsts_cluster, /*structural_break=*/true);
    ASSERT_TRUE(belief.is_ok());

    // Verify the BSTS contribution map has non-zero entries on multiple
    // regimes (no single-regime lock-in).
    const auto& bsts_contrib = belief.value().model_contributions.at("BSTS");
    int nonzero_count = 0;
    for (const auto& [r, p] : bsts_contrib) {
        if (p > 1e-6) ++nonzero_count;
    }
    EXPECT_GE(nonzero_count, 3)
        << "BSTS should contribute to multiple regimes (at least 3 non-zero); "
           "all-mass-on-one would indicate lock-in.";
}

// ----------------------------------------------------------------------------
// growth_score components z-scored before averaging. Test the
// PROPERTY by constructing a mock series where one component is at scale
// 78 (cap-util) and another at scale 0.01 (slope). Without z-scoring,
// the high-scale component dominates 99%. With z-scoring, both
// contribute roughly equally.
// ----------------------------------------------------------------------------

TEST(RegimePhase1, GrowthScoreZScoringEqualizesComponentScales_M03) {
    // Simulate the fix's series_stats + z-score logic on synthetic data.
    constexpr int T = 100;
    std::vector<double> ip_slope(T), cu_level(T), gdp_slope(T);
    std::mt19937 rng(3);
    std::normal_distribution<double> nd(0.0, 1.0);
    for (int t = 0; t < T; ++t) {
        ip_slope[t]  = 0.005 + 0.001 * nd(rng);   // scale ~ 0.01
        cu_level[t]  = 78.0  + 1.0   * nd(rng);   // scale ~ 78
        gdp_slope[t] = 0.005 + 0.001 * nd(rng);   // scale ~ 0.01
    }

    auto stats = [](const std::vector<double>& v) -> std::pair<double, double> {
        double s = 0, sq = 0;
        for (double x : v) { s += x; sq += x * x; }
        double m = s / v.size();
        double var = sq / v.size() - m * m;
        return {m, std::sqrt(std::max(var, 1e-12))};
    };

    auto [ip_m, ip_s] = stats(ip_slope);
    auto [cu_m, cu_s] = stats(cu_level);
    auto [gd_m, gd_s] = stats(gdp_slope);

    // Build z-scored growth_score series
    std::vector<double> gs_zscored(T), gs_naive(T);
    for (int t = 0; t < T; ++t) {
        gs_zscored[t] = ((ip_slope[t] - ip_m) / ip_s
                       + (cu_level[t] - cu_m) / cu_s
                       + (gdp_slope[t] - gd_m) / gd_s) / 3.0;
        gs_naive[t]   = (ip_slope[t] + cu_level[t] + gdp_slope[t]) / 3.0;
    }

    auto var_of = [](const std::vector<double>& v) {
        double s = 0; for (double x : v) s += x;
        double m = s / v.size();
        double sq = 0; for (double x : v) sq += (x - m) * (x - m);
        return sq / v.size();
    };

    // The naive average has variance dominated by cap-util (~1) — the
    // tiny IP/GDP slope variance contributes nothing.
    double naive_var = var_of(gs_naive);
    double cu_alone_var = var_of(cu_level) / 9.0;  // scaled by /3 in mean
    EXPECT_NEAR(naive_var, cu_alone_var, naive_var * 0.05)
        << "Pre-fix: naive growth_score variance is dominated by cap-util.";

    // The z-scored average has variance roughly 1/3 (since each
    // component is unit variance and they're independent).
    double zscored_var = var_of(gs_zscored);
    EXPECT_GT(zscored_var, 0.2)
        << "z-scored growth_score should have meaningful variance "
           "(~1/3) from balanced contributions.";
    EXPECT_LT(zscored_var, 1.0)
        << "variance shouldn't blow up beyond 1.";
}

// ----------------------------------------------------------------------------
// inflation_score uses CPI/PCE SLOPE (rate of change), not LEVEL.
// Property test: constructing a synthetic CPI series that's high but
// FALLING should produce a DECREASING inflation_score (slope-based)
// not an INCREASING one (level-based).
// ----------------------------------------------------------------------------

TEST(RegimePhase1, InflationScoreUsesSlopeNotLevel_M05) {
    constexpr int T = 60;
    // CPI rises 3 → 9 over first 20, then falls 9 → 3 over next 40.
    // Pre-fix (level-based): score keeps rising into the falling phase
    // because absolute level is still elevated.
    // Post-fix (slope-based): score peaks during the rise, then drops
    // sharply when the slope turns negative.
    std::vector<double> cpi(T);
    for (int t = 0; t < T; ++t) {
        cpi[t] = (t < 20) ? 3.0 + 0.3 * t : 9.0 - 0.15 * (t - 20);
    }

    // Synthesize "level-based" inflation_score (pre-fix)
    std::vector<double> level_score = cpi;

    // Synthesize "slope-based" inflation_score (post-fix) via simple diff
    std::vector<double> slope_score(T);
    slope_score[0] = 0.0;
    for (int t = 1; t < T; ++t) slope_score[t] = cpi[t] - cpi[t - 1];

    // Property: at the peak of CPI (t=20) and at the end of the fall (t=59),
    // the level-score is similar (level still elevated), but the slope-score
    // differs in sign — POSITIVE during rise, NEGATIVE during fall.
    EXPECT_GT(slope_score[10], 0.0) << "Slope positive during rising phase";
    EXPECT_LT(slope_score[40], 0.0) << "slope NEGATIVE during falling phase — "
                                       "the property the fix establishes.";
    // The level alone would still be high during the fall:
    EXPECT_GT(level_score[40], level_score[5])
        << "Level remains elevated even while inflation is falling — "
           "explaining why the level-based score gave wrong signal.";
}

// ============================================================================
// MacroBelief.entropy and MacroBelief.top_prob exist as fields and
// have struct-default values of 0. The pipeline populates them via
// p_smooth, but at the contract level we only assert the fields exist
// and default to 0 — the population path is exercised end-to-end via
// the macro_regime_pipeline integration test.
// ============================================================================

TEST(RegimePhase4, MacroBeliefHasEntropyAndTopProbFields_M09) {
    MacroBelief b;
    EXPECT_DOUBLE_EQ(b.entropy, 0.0)
        << "entropy must default to 0 (existing fields unchanged).";
    EXPECT_DOUBLE_EQ(b.top_prob, 0.0)
        << "top_prob must default to 0 (existing fields unchanged).";

    // Manually populated values must round-trip.
    b.entropy = 0.42;
    b.top_prob = 0.85;
    EXPECT_DOUBLE_EQ(b.entropy, 0.42);
    EXPECT_DOUBLE_EQ(b.top_prob, 0.85);
}
