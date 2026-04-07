#include "trade_ngin/regime_engine/growth_inflation_quadrant.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <sstream>
#include <iomanip>

namespace trade_ngin {

// ═══════════════════════════════════════════════════════════════════
// Each macro ontology state has an expected "center" in
// (growth_z, inflation_z) space. We use Gaussian kernels centered
// on these positions to compute soft probabilities.
//
// Layout in the quadrant:
//
//   inflation_z ↑
//               |  RECESSION_INFLATIONARY   SLOWDOWN_INFLATIONARY   EXPANSION_INFLATIONARY
//               |       (-2.0, 1.5)              (-0.5, 1.0)             (1.0, 1.0)
//               |
//        0 ─────|──────────────────────────────────────────────────────
//               |
//               |  RECESSION_DEFLATIONARY   SLOWDOWN_DISINFLATION   EXPANSION_DISINFLATION
//               |       (-2.0, -1.0)             (-0.5, -0.5)            (1.0, -0.5)
//               +──────────────────────────────────────────────────→ growth_z
//
// ═══════════════════════════════════════════════════════════════════

struct StateCenter {
    MacroOntology state;
    double growth_center;
    double inflation_center;
};

static const StateCenter STATE_CENTERS[] = {
    { MacroOntology::EXPANSION_DISINFLATION,  1.0, -0.5 },
    { MacroOntology::EXPANSION_INFLATIONARY,   1.0,  1.0 },
    { MacroOntology::SLOWDOWN_DISINFLATION,  -0.5, -0.5 },
    { MacroOntology::SLOWDOWN_INFLATIONARY,  -0.5,  1.0 },
    { MacroOntology::RECESSION_DEFLATIONARY, -2.0, -1.0 },
    { MacroOntology::RECESSION_INFLATIONARY, -2.0,  1.5 },
};

// ═══════════════════════════════════════════════════════════════════
// Constructor
// ═══════════════════════════════════════════════════════════════════
GrowthInflationQuadrant::GrowthInflationQuadrant(QuadrantConfig config)
    : config_(std::move(config))
{
    previous_probs_.fill(1.0 / MACRO_ONTOLOGY_COUNT);
}

// ═══════════════════════════════════════════════════════════════════
// update() — main entry point
// ═══════════════════════════════════════════════════════════════════
Result<QuadrantResult> GrowthInflationQuadrant::update(const MacroPanel& panel) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 1. Append to rolling histories
    cpi_yoy_history_.push_back(panel.cpi_yoy);
    gdp_qoq_history_.push_back(panel.gdp_qoq_ann);
    ip_yoy_history_.push_back(panel.ip_yoy);
    conf_history_.push_back(panel.consumer_conf);

    // Trim to lookback window
    while (cpi_yoy_history_.size() > config_.zscore_lookback)
        cpi_yoy_history_.pop_front();
    while (gdp_qoq_history_.size() > config_.zscore_lookback)
        gdp_qoq_history_.pop_front();
    while (ip_yoy_history_.size() > config_.zscore_lookback)
        ip_yoy_history_.pop_front();
    while (conf_history_.size() > config_.zscore_lookback)
        conf_history_.pop_front();

    // Need at least 3 data points for meaningful z-scores
    if (cpi_yoy_history_.size() < 3) {
        QuadrantResult r;
        r.probabilities.fill(1.0 / MACRO_ONTOLOGY_COUNT);
        r.confidence = 0.0;
        r.growth_driver = "Insufficient data";
        r.inflation_driver = "Insufficient data";
        current_result_ = r;
        return r;
    }

    // 2. Compute individual z-scores
    double cpi_z  = compute_zscore(panel.cpi_yoy, cpi_yoy_history_);
    double gdp_z  = compute_zscore(panel.gdp_qoq_ann, gdp_qoq_history_);
    double ip_z   = compute_zscore(panel.ip_yoy, ip_yoy_history_);
    double conf_z = compute_zscore(panel.consumer_conf, conf_history_);

    // 3. Compute growth composite
    double growth_z = compute_growth_composite(gdp_z, ip_z, conf_z);
    double inflation_z = cpi_z;

    // 4. Map to probabilities
    MacroProbs raw_probs = map_to_probabilities(growth_z, inflation_z);

    // 5. Stabilize with EMA
    MacroProbs final_probs = stabilize(raw_probs);
    previous_probs_ = final_probs;
    has_previous_ = true;

    // 6. Find dominant and confidence
    auto [dominant, confidence] = find_dominant(final_probs);

    // 7. Track regime age
    if (dominant == previous_dominant_) {
        regime_age_++;
    } else {
        regime_age_ = 1;
        previous_dominant_ = dominant;
    }

    // 8. Build result
    QuadrantResult result;
    result.probabilities = final_probs;
    result.dominant_regime = dominant;
    result.confidence = confidence;
    result.growth_zscore = growth_z;
    result.inflation_zscore = inflation_z;
    result.regime_age_days = regime_age_ * 30;  // approximate monthly cadence
    result.growth_driver = build_growth_driver(panel, gdp_z, ip_z, conf_z);
    result.inflation_driver = build_inflation_driver(panel, cpi_z);

    current_result_ = result;
    history_.push_back(result);

    return result;
}

// ═══════════════════════════════════════════════════════════════════
// compute_zscore — standardize against rolling window
// ═══════════════════════════════════════════════════════════════════
double GrowthInflationQuadrant::compute_zscore(
    double value, const std::deque<double>& history) const
{
    if (history.size() < 2) return 0.0;

    double sum = std::accumulate(history.begin(), history.end(), 0.0);
    double mean = sum / static_cast<double>(history.size());

    double sq_sum = 0.0;
    for (auto v : history)
        sq_sum += (v - mean) * (v - mean);
    double stddev = std::sqrt(sq_sum / static_cast<double>(history.size() - 1));

    if (stddev < 1e-10) return 0.0;
    return (value - mean) / stddev;
}

// ═══════════════════════════════════════════════════════════════════
// compute_growth_composite — weighted average of growth z-scores
// ═══════════════════════════════════════════════════════════════════
double GrowthInflationQuadrant::compute_growth_composite(
    double gdp_z, double ip_z, double conf_z) const
{
    return config_.weight_gdp  * gdp_z
         + config_.weight_ip   * ip_z
         + config_.weight_conf * conf_z;
}

// ═══════════════════════════════════════════════════════════════════
// map_to_probabilities — Gaussian kernel mapping
//
// For each macro ontology state, compute the probability that the
// current (growth_z, inflation_z) belongs to that state using a
// 2D Gaussian kernel centered on the state's expected position.
// ═══════════════════════════════════════════════════════════════════
MacroProbs GrowthInflationQuadrant::map_to_probabilities(
    double growth_z, double inflation_z) const
{
    MacroProbs probs{};
    double total = 0.0;
    double bw2 = config_.kernel_bandwidth * config_.kernel_bandwidth;

    for (size_t i = 0; i < MACRO_ONTOLOGY_COUNT; ++i) {
        double dg = growth_z - STATE_CENTERS[i].growth_center;
        double di = inflation_z - STATE_CENTERS[i].inflation_center;
        double dist_sq = dg * dg + di * di;

        // Gaussian kernel: exp(-dist^2 / (2 * bandwidth^2))
        probs[i] = std::exp(-dist_sq / (2.0 * bw2));
        total += probs[i];
    }

    // Normalize to sum to 1
    if (total > 1e-12) {
        for (auto& p : probs) p /= total;
    } else {
        probs.fill(1.0 / MACRO_ONTOLOGY_COUNT);
    }

    return probs;
}

// ═══════════════════════════════════════════════════════════════════
// stabilize — EMA smoothing to prevent whipsaw
// ═══════════════════════════════════════════════════════════════════
MacroProbs GrowthInflationQuadrant::stabilize(const MacroProbs& raw) const {
    if (!has_previous_) return raw;

    MacroProbs smoothed{};
    double lambda = config_.ema_lambda;
    double total = 0.0;

    for (size_t i = 0; i < MACRO_ONTOLOGY_COUNT; ++i) {
        smoothed[i] = lambda * raw[i] + (1.0 - lambda) * previous_probs_[i];
        total += smoothed[i];
    }

    // Re-normalize (EMA can drift slightly)
    if (total > 1e-12) {
        for (auto& p : smoothed) p /= total;
    }

    return smoothed;
}

// ═══════════════════════════════════════════════════════════════════
// find_dominant — argmax + confidence (1 - normalized entropy)
// ═══════════════════════════════════════════════════════════════════
std::pair<MacroOntology, double> GrowthInflationQuadrant::find_dominant(
    const MacroProbs& probs) const
{
    size_t best_idx = 0;
    for (size_t i = 1; i < MACRO_ONTOLOGY_COUNT; ++i) {
        if (probs[i] > probs[best_idx]) best_idx = i;
    }

    double h = entropy(probs);
    double h_max = max_entropy();
    double confidence = (h_max > 1e-12) ? 1.0 - (h / h_max) : 0.0;

    return { static_cast<MacroOntology>(best_idx), confidence };
}

// ═══════════════════════════════════════════════════════════════════
// Provenance builders — explain what drove the classification
// ═══════════════════════════════════════════════════════════════════
std::string GrowthInflationQuadrant::build_growth_driver(
    const MacroPanel& panel, double gdp_z, double ip_z, double conf_z) const
{
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2);
    ss << "GDP QoQ " << (panel.gdp_qoq_ann >= 0 ? "+" : "") << panel.gdp_qoq_ann
       << "% (z=" << gdp_z << "), ";
    ss << "IP YoY " << (panel.ip_yoy >= 0 ? "+" : "") << panel.ip_yoy
       << "% (z=" << ip_z << "), ";
    ss << "Conf " << panel.consumer_conf << " (z=" << conf_z << ")";
    return ss.str();
}

std::string GrowthInflationQuadrant::build_inflation_driver(
    const MacroPanel& panel, double cpi_z) const
{
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2);
    ss << "CPI YoY " << (panel.cpi_yoy >= 0 ? "+" : "") << panel.cpi_yoy
       << "% (z=" << cpi_z << ")";
    return ss.str();
}

// ═══════════════════════════════════════════════════════════════════
// Getters
// ═══════════════════════════════════════════════════════════════════
const QuadrantResult& GrowthInflationQuadrant::get_result() const {
    return current_result_;
}

const std::vector<QuadrantResult>& GrowthInflationQuadrant::get_history() const {
    return history_;
}

void GrowthInflationQuadrant::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    cpi_yoy_history_.clear();
    gdp_qoq_history_.clear();
    ip_yoy_history_.clear();
    conf_history_.clear();
    previous_probs_.fill(1.0 / MACRO_ONTOLOGY_COUNT);
    has_previous_ = false;
    regime_age_ = 0;
    history_.clear();
    current_result_ = {};
}

} // namespace trade_ngin
