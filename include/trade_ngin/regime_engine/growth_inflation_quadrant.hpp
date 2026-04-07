#pragma once

#include "trade_ngin/regime_engine/macro_types.hpp"
#include "trade_ngin/core/error.hpp"

#include <vector>
#include <deque>
#include <mutex>
#include <optional>
#include <string>

namespace trade_ngin {

// ═══════════════════════════════════════════════════════════════════
// GrowthInflationQuadrant — Section B3 of the Regime Engine spec
//
// Purpose: Convert raw macro indicators (CPI, GDP, IP, consumer
// confidence) into interpretable quadrant probabilities over the
// 6-state MacroOntology. This is the "anchor" that prevents the
// macro system from becoming black-box drift.
//
// The quadrant works by:
//   1. Computing a growth composite z-score from GDP, IP, and
//      consumer confidence (each z-scored against its own history)
//   2. Computing an inflation z-score from CPI YoY changes
//   3. Mapping the (growth_z, inflation_z) pair into a soft
//      probability distribution over the 6 macro states using
//      a bivariate Gaussian kernel centered on each state's
//      expected position in the quadrant
//
// Design principle: This is NOT a model — it's a deterministic
// mapping. It should be interpretable, stable, and fast.
// ═══════════════════════════════════════════════════════════════════

struct QuadrantConfig {
    // Lookback window for z-score computation (in data points)
    size_t zscore_lookback = 60;     // ~5 years of monthly data

    // Smoothing factor for EMA stabilization (0 = no smoothing)
    double ema_lambda = 0.3;

    // How spread out the Gaussian kernels are (larger = softer boundaries)
    double kernel_bandwidth = 1.0;

    // Thresholds for growth classification
    double growth_expansion_threshold  =  0.5;   // z > 0.5 → expansion
    double growth_slowdown_threshold   = -0.5;    // z < -0.5 → slowdown
    double growth_recession_threshold  = -1.5;    // z < -1.5 → recession

    // Thresholds for inflation classification
    double inflation_high_threshold    =  0.5;    // z > 0.5 → inflationary
    double inflation_low_threshold     = -0.5;    // z < -0.5 → disinflationary

    // Weights for growth composite
    double weight_gdp     = 0.40;
    double weight_ip      = 0.35;
    double weight_conf    = 0.25;
};

class GrowthInflationQuadrant {
public:
    explicit GrowthInflationQuadrant(QuadrantConfig config = {});

    /// Update with a new MacroPanel observation.
    /// Call this each time new macro data arrives (typically monthly).
    Result<QuadrantResult> update(const MacroPanel& panel);

    /// Get the current quadrant result without updating.
    const QuadrantResult& get_result() const;

    /// Get the full history of quadrant results.
    const std::vector<QuadrantResult>& get_history() const;

    /// Reset all state.
    void reset();

    /// Get config (for diagnostics).
    const QuadrantConfig& get_config() const { return config_; }

private:
    QuadrantConfig config_;

    // Rolling history for z-score computation
    std::deque<double> cpi_yoy_history_;
    std::deque<double> gdp_qoq_history_;
    std::deque<double> ip_yoy_history_;
    std::deque<double> conf_history_;

    // Previous stabilized probabilities (for EMA smoothing)
    MacroProbs previous_probs_{};
    bool has_previous_ = false;

    // Regime age tracking
    MacroOntology previous_dominant_ = MacroOntology::EXPANSION_DISINFLATION;
    int regime_age_ = 0;

    // Full result history
    std::vector<QuadrantResult> history_;

    // Current result
    QuadrantResult current_result_;

    mutable std::mutex mutex_;

    // ── Internal methods ──

    /// Compute z-score of value against a rolling window.
    double compute_zscore(double value, const std::deque<double>& history) const;

    /// Compute the growth composite z-score from individual z-scores.
    double compute_growth_composite(double gdp_z, double ip_z, double conf_z) const;

    /// Map (growth_z, inflation_z) → soft probabilities over MacroOntology.
    /// Uses Gaussian kernels centered on each state's expected position.
    MacroProbs map_to_probabilities(double growth_z, double inflation_z) const;

    /// Apply EMA stabilization to avoid whipsaw.
    MacroProbs stabilize(const MacroProbs& raw) const;

    /// Find the dominant regime and compute confidence.
    std::pair<MacroOntology, double> find_dominant(const MacroProbs& probs) const;

    /// Build provenance strings for diagnostics.
    std::string build_growth_driver(const MacroPanel& panel, double gdp_z,
                                     double ip_z, double conf_z) const;
    std::string build_inflation_driver(const MacroPanel& panel, double cpi_z) const;
};

} // namespace trade_ngin
