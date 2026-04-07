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
