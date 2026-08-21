#include <gtest/gtest.h>
#include "trade_ngin/regime_engine/growth_inflation_quadrant.hpp"
#include "trade_ngin/regime_engine/macro_types.hpp"

using namespace trade_ngin;

// ═══════════════════════════════════════════════════════════════════
// Test: basic construction and default state
// ═══════════════════════════════════════════════════════════════════
TEST(GrowthInflationQuadrant, DefaultConstruction) {
    GrowthInflationQuadrant quad;
    auto result = quad.get_result();
    // Before any update, probabilities should be zero-initialized
    EXPECT_EQ(result.confidence, 0.0);
}

// ═══════════════════════════════════════════════════════════════════
// Test: insufficient data returns uniform probabilities
// ═══════════════════════════════════════════════════════════════════
TEST(GrowthInflationQuadrant, InsufficientData) {
    GrowthInflationQuadrant quad;

    MacroPanel panel;
    panel.cpi_yoy = 3.0;
    panel.gdp_qoq_ann = 2.5;
    panel.ip_yoy = 1.0;
    panel.consumer_conf = 70.0;

    auto result = quad.update(panel);
    ASSERT_TRUE(result.is_ok());

    // With only 1 data point, should get uniform probs
    auto probs = result.value().probabilities;
    for (size_t i = 0; i < MACRO_ONTOLOGY_COUNT; ++i) {
        EXPECT_NEAR(probs[i], 1.0 / MACRO_ONTOLOGY_COUNT, 0.01);
    }
}

// ═══════════════════════════════════════════════════════════════════
// Test: expansion + disinflation quadrant
// Strong growth, falling inflation → EXPANSION_DISINFLATION
// ═══════════════════════════════════════════════════════════════════
TEST(GrowthInflationQuadrant, ExpansionDisinflation) {
    GrowthInflationQuadrant quad;

    // Feed ~10 data points with moderate baseline
    for (int i = 0; i < 10; ++i) {
        MacroPanel panel;
        panel.cpi_yoy = 2.5;
        panel.gdp_qoq_ann = 2.0;
        panel.ip_yoy = 1.5;
        panel.consumer_conf = 70.0;
        quad.update(panel);
    }

    // Now feed a strong growth / low inflation observation
    MacroPanel panel;
    panel.cpi_yoy = 1.5;        // inflation dropping below avg
    panel.gdp_qoq_ann = 4.0;    // growth well above avg
    panel.ip_yoy = 3.5;          // IP strong
    panel.consumer_conf = 80.0;  // confidence high

    auto result = quad.update(panel);
    ASSERT_TRUE(result.is_ok());

    auto& r = result.value();
    // EXPANSION_DISINFLATION should be dominant
    EXPECT_EQ(r.dominant_regime, MacroOntology::EXPANSION_DISINFLATION);
    EXPECT_GT(r.probabilities[static_cast<size_t>(MacroOntology::EXPANSION_DISINFLATION)], 0.3);
    EXPECT_GT(r.confidence, 0.0);
    EXPECT_GT(r.growth_zscore, 0.0);
    EXPECT_LT(r.inflation_zscore, 0.0);
}

// ═══════════════════════════════════════════════════════════════════
// Test: stagflation — slowdown + inflationary
// Weak growth, rising inflation → SLOWDOWN_INFLATIONARY
// ═══════════════════════════════════════════════════════════════════
TEST(GrowthInflationQuadrant, StagflationRisk) {
    GrowthInflationQuadrant quad;

    // Feed baseline
    for (int i = 0; i < 10; ++i) {
        MacroPanel panel;
        panel.cpi_yoy = 2.5;
        panel.gdp_qoq_ann = 2.0;
        panel.ip_yoy = 1.5;
        panel.consumer_conf = 70.0;
        quad.update(panel);
    }

    // Stagflation: growth falling, inflation rising
    MacroPanel panel;
    panel.cpi_yoy = 5.0;        // inflation spiking
    panel.gdp_qoq_ann = 0.5;    // growth collapsing
    panel.ip_yoy = -0.5;        // IP contracting
    panel.consumer_conf = 58.0;  // confidence falling

    auto result = quad.update(panel);
    ASSERT_TRUE(result.is_ok());

    auto& r = result.value();
    // Should be in SLOWDOWN_INFLATIONARY or RECESSION_INFLATIONARY territory
    size_t slowdown_inf = static_cast<size_t>(MacroOntology::SLOWDOWN_INFLATIONARY);
    size_t recession_inf = static_cast<size_t>(MacroOntology::RECESSION_INFLATIONARY);
    double stag_prob = r.probabilities[slowdown_inf] + r.probabilities[recession_inf];
    EXPECT_GT(stag_prob, 0.4);
    EXPECT_LT(r.growth_zscore, 0.0);
    EXPECT_GT(r.inflation_zscore, 0.0);
}

// ═══════════════════════════════════════════════════════════════════
// Test: probabilities always sum to 1
// ═══════════════════════════════════════════════════════════════════
TEST(GrowthInflationQuadrant, ProbabilitiesSumToOne) {
    GrowthInflationQuadrant quad;

    for (int i = 0; i < 20; ++i) {
        MacroPanel panel;
        panel.cpi_yoy = 2.0 + 0.3 * i;
        panel.gdp_qoq_ann = 3.0 - 0.2 * i;
        panel.ip_yoy = 1.5 - 0.1 * i;
        panel.consumer_conf = 75.0 - i;

        auto result = quad.update(panel);
        ASSERT_TRUE(result.is_ok());

        double sum = 0.0;
        for (auto p : result.value().probabilities) sum += p;
        EXPECT_NEAR(sum, 1.0, 1e-10);
    }
}

// ═══════════════════════════════════════════════════════════════════
// Test: EMA stabilization prevents sudden jumps
// ═══════════════════════════════════════════════════════════════════
TEST(GrowthInflationQuadrant, EMAStabilization) {
    QuadrantConfig config;
    config.ema_lambda = 0.3;  // strong smoothing
    GrowthInflationQuadrant quad(config);

    // Build baseline
    for (int i = 0; i < 10; ++i) {
        MacroPanel panel;
        panel.cpi_yoy = 2.0;
        panel.gdp_qoq_ann = 2.5;
        panel.ip_yoy = 1.5;
        panel.consumer_conf = 70.0;
        quad.update(panel);
    }

    auto before = quad.get_result();

    // Sudden shock
    MacroPanel shock;
    shock.cpi_yoy = 8.0;
    shock.gdp_qoq_ann = -3.0;
    shock.ip_yoy = -5.0;
    shock.consumer_conf = 45.0;
    quad.update(shock);

    auto after = quad.get_result();

    // With EMA lambda=0.3, the change should be gradual, not instant
    // The dominant regime may or may not have changed, but the probabilities
    // should not have jumped entirely to the new state
    size_t dom_before = static_cast<size_t>(before.dominant_regime);
    double prob_before_state_after = after.probabilities[dom_before];
    // The old state should still have some residual probability
    EXPECT_GT(prob_before_state_after, 0.05);
}

// ═══════════════════════════════════════════════════════════════════
// Test: regime age tracking
// ═══════════════════════════════════════════════════════════════════
TEST(GrowthInflationQuadrant, RegimeAgeTracking) {
    GrowthInflationQuadrant quad;

    // Feed consistent data → same regime should persist
    for (int i = 0; i < 15; ++i) {
        MacroPanel panel;
        panel.cpi_yoy = 2.0;
        panel.gdp_qoq_ann = 3.0;
        panel.ip_yoy = 2.0;
        panel.consumer_conf = 75.0;
        quad.update(panel);
    }

    auto result = quad.get_result();
    // After 15 updates in same regime, age should be > 0
    EXPECT_GT(result.regime_age_days, 0);
}

// ═══════════════════════════════════════════════════════════════════
// Test: provenance strings are populated
// ═══════════════════════════════════════════════════════════════════
TEST(GrowthInflationQuadrant, ProvenanceStrings) {
    GrowthInflationQuadrant quad;

    for (int i = 0; i < 5; ++i) {
        MacroPanel panel;
        panel.cpi_yoy = 3.0;
        panel.gdp_qoq_ann = 2.0;
        panel.ip_yoy = 1.0;
        panel.consumer_conf = 68.0;
        quad.update(panel);
    }

    auto result = quad.get_result();
    EXPECT_FALSE(result.growth_driver.empty());
    EXPECT_FALSE(result.inflation_driver.empty());
    EXPECT_TRUE(result.growth_driver.find("GDP") != std::string::npos);
    EXPECT_TRUE(result.inflation_driver.find("CPI") != std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════
// Test: reset clears all state
// ═══════════════════════════════════════════════════════════════════
TEST(GrowthInflationQuadrant, ResetClearsState) {
    GrowthInflationQuadrant quad;

    for (int i = 0; i < 10; ++i) {
        MacroPanel panel;
        panel.cpi_yoy = 3.0;
        panel.gdp_qoq_ann = 2.0;
        panel.ip_yoy = 1.0;
        panel.consumer_conf = 68.0;
        quad.update(panel);
    }

    EXPECT_GT(quad.get_history().size(), 0u);

    quad.reset();

    EXPECT_EQ(quad.get_history().size(), 0u);
    EXPECT_EQ(quad.get_result().confidence, 0.0);
}

// ═══════════════════════════════════════════════════════════════════
// Test: get_config() returns correct configuration
// Covers the inline getter in the .hpp header (line 80)
// ═══════════════════════════════════════════════════════════════════
TEST(GrowthInflationQuadrant, GetConfig) {
    QuadrantConfig custom_config;
    custom_config.ema_lambda = 0.5;
    custom_config.zscore_lookback = 120;
    custom_config.kernel_bandwidth = 1.5;

    GrowthInflationQuadrant quad(custom_config);

    const QuadrantConfig& retrieved = quad.get_config();
    EXPECT_EQ(retrieved.ema_lambda, 0.5);
    EXPECT_EQ(retrieved.zscore_lookback, 120u);
    EXPECT_EQ(retrieved.kernel_bandwidth, 1.5);
}

// ═══════════════════════════════════════════════════════════════════
// Test: z-score computation with zero variance
// When all historical values are identical, stddev = 0 and zscore = 0.
// This covers line 159 in growth_inflation_quadrant.cpp
// ═══════════════════════════════════════════════════════════════════
TEST(GrowthInflationQuadrant, ZscoreZeroVariance) {
    GrowthInflationQuadrant quad;

    // Feed 10 identical CPI values (zero variance)
    for (int i = 0; i < 10; ++i) {
        MacroPanel panel;
        panel.cpi_yoy = 2.5;      // constant
        panel.gdp_qoq_ann = 2.5;  // constant
        panel.ip_yoy = 1.5;       // constant
        panel.consumer_conf = 70.0; // constant
        quad.update(panel);
    }

    // With perfect constant data, z-scores should be 0
    // and probabilities should be uniform across all states
    auto result = quad.get_result();
    EXPECT_EQ(result.growth_zscore, 0.0);
    EXPECT_EQ(result.inflation_zscore, 0.0);

    // With all z-scores at 0, probabilities should reflect
    // the center state preference (EXPANSION_DISINFLATION is at +1, +1)
    // but all states should have non-zero probability
    for (size_t i = 0; i < MACRO_ONTOLOGY_COUNT; ++i) {
        EXPECT_GT(result.probabilities[i], 0.0);
    }
}

// ═══════════════════════════════════════════════════════════════════
// Test: stabilize() on first update (no previous state)
// When has_previous_ = false, stabilize returns raw probabilities.
// This covers line 212 in growth_inflation_quadrant.cpp
// ═══════════════════════════════════════════════════════════════════
TEST(GrowthInflationQuadrant, StabilizeFirstCall) {
    GrowthInflationQuadrant quad;

    // Feed exactly 3 data points (minimum for z-score)
    for (int i = 0; i < 3; ++i) {
        MacroPanel panel;
        panel.cpi_yoy = 2.0 + i * 0.5;
        panel.gdp_qoq_ann = 3.0 - i * 0.3;
        panel.ip_yoy = 1.5 + i * 0.1;
        panel.consumer_conf = 75.0 - i * 2;
        auto result = quad.update(panel);
        ASSERT_TRUE(result.is_ok());

        if (i == 2) {
            // On third update, has_previous should be true after first real computation
            auto current = quad.get_result();
            double sum = 0.0;
            for (auto p : current.probabilities) sum += p;
            EXPECT_NEAR(sum, 1.0, 1e-10);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════
// Test: invalid ema_lambda (≤0 or >1)
// This covers the early return error on line 63-67
// ═══════════════════════════════════════════════════════════════════
TEST(GrowthInflationQuadrant, InvalidEmaLambda) {
    QuadrantConfig config;

    // Test ema_lambda = 0 (invalid)
    config.ema_lambda = 0.0;
    GrowthInflationQuadrant quad1(config);

    MacroPanel panel;
    panel.cpi_yoy = 2.5;
    panel.gdp_qoq_ann = 2.0;
    panel.ip_yoy = 1.5;
    panel.consumer_conf = 70.0;

    auto result1 = quad1.update(panel);
    EXPECT_FALSE(result1.is_ok());
    EXPECT_EQ(result1.error()->code(), ErrorCode::INVALID_ARGUMENT);

    // Test ema_lambda > 1 (invalid)
    config.ema_lambda = 1.5;
    GrowthInflationQuadrant quad2(config);
    auto result2 = quad2.update(panel);
    EXPECT_FALSE(result2.is_ok());
    EXPECT_EQ(result2.error()->code(), ErrorCode::INVALID_ARGUMENT);

    // Test ema_lambda = 1.0 (valid, disables smoothing)
    config.ema_lambda = 1.0;
    GrowthInflationQuadrant quad3(config);
    auto result3 = quad3.update(panel);
    EXPECT_TRUE(result3.is_ok());
}

// ═══════════════════════════════════════════════════════════════════
// Test: integration with global macro schema shape
// Simulates loading macro data as if from global_macro_data schema
// and verifies end-to-end regime detection works correctly.
// ═══════════════════════════════════════════════════════════════════
TEST(GrowthInflationQuadrant, IntegrationGlobalMacroData) {
    // Simulate a sequence of realistic macro regimes
    GrowthInflationQuadrant quad;

    // Phase 1: Early expansion (months 0-5)
    // Characteristic: rising growth, falling/stable inflation
    for (int m = 0; m < 6; ++m) {
        MacroPanel panel;
        panel.cpi_yoy = 2.5 - 0.1 * m;  // deflating
        panel.gdp_qoq_ann = 2.0 + 0.3 * m;  // accelerating
        panel.ip_yoy = 1.2 + 0.2 * m;  // improving
        panel.consumer_conf = 70.0 + 2 * m;  // gaining confidence
        auto result = quad.update(panel);
        ASSERT_TRUE(result.is_ok());

        if (m == 5) {
            auto r = result.value();
            // Should be approaching expansion + disinflation
            size_t exp_disinf = static_cast<size_t>(MacroOntology::EXPANSION_DISINFLATION);
            EXPECT_GT(r.probabilities[exp_disinf], 0.15);
            EXPECT_GT(r.confidence, 0.0);
        }
    }

    // Phase 2: Late cycle / inflation spike (months 6-11)
    // Characteristic: growth holding but inflation rising
    for (int m = 6; m < 12; ++m) {
        MacroPanel panel;
        panel.cpi_yoy = 1.8 + 0.4 * (m - 6);  // inflationary
        panel.gdp_qoq_ann = 3.5 - 0.1 * (m - 6);  // still solid
        panel.ip_yoy = 2.5 + 0.1 * (m - 6);  // still growing
        panel.consumer_conf = 82.0 - (m - 6);  // slightly cooling
        auto result = quad.update(panel);
        ASSERT_TRUE(result.is_ok());

        if (m == 11) {
            auto r = result.value();
            // Should be in expansion + inflationary territory
            size_t exp_inf = static_cast<size_t>(MacroOntology::EXPANSION_INFLATIONARY);
            EXPECT_GT(r.probabilities[exp_inf], 0.15);
            EXPECT_GT(r.inflation_zscore, 0.0);
        }
    }

    // Phase 3: Slowdown (months 12-17)
    // Characteristic: growth weakening, inflation still elevated
    for (int m = 12; m < 18; ++m) {
        MacroPanel panel;
        panel.cpi_yoy = 4.2 - 0.15 * (m - 12);  // gradually cooling
        panel.gdp_qoq_ann = 3.0 - 0.3 * (m - 12);  // slowing
        panel.ip_yoy = 2.0 - 0.25 * (m - 12);  // weakening
        panel.consumer_conf = 76.0 - 0.5 * (m - 12);  // declining confidence
        auto result = quad.update(panel);
        ASSERT_TRUE(result.is_ok());

        if (m == 17) {
            auto r = result.value();
            // Should be in slowdown territory (either disinflation or inflationary)
            size_t slowdown_disinf = static_cast<size_t>(MacroOntology::SLOWDOWN_DISINFLATION);
            size_t slowdown_inf = static_cast<size_t>(MacroOntology::SLOWDOWN_INFLATIONARY);
            double slowdown_prob = r.probabilities[slowdown_disinf] + r.probabilities[slowdown_inf];
            EXPECT_GT(slowdown_prob, 0.20);
            EXPECT_LT(r.growth_zscore, 0.5);  // growth z-score should be lower
        }
    }

    // Verify history was accumulated
    auto history = quad.get_history();
    EXPECT_EQ(history.size(), 18u);

    // Verify regime age is being tracked (should be at least a few "months")
    auto final_result = quad.get_result();
    EXPECT_GT(final_result.regime_age_days, 0);

    // Verify provenance is populated for diagnostics
    EXPECT_FALSE(final_result.growth_driver.empty());
    EXPECT_FALSE(final_result.inflation_driver.empty());
}
