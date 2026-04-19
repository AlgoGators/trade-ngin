// test_macro_regime_pipeline.cpp
// GTest suite for the Synthesized Macro Regime Pipeline

#include <gtest/gtest.h>
#include "trade_ngin/statistics/state_estimation/macro_regime_pipeline.hpp"

#include <Eigen/Dense>
#include <cmath>
#include <random>
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
