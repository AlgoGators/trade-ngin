#pragma once

#include <array>
#include <string>
#include <chrono>
#include <vector>
#include <cmath>

namespace trade_ngin {

// ═══════════════════════════════════════════════════════════════════
// Macro Ontology — the 6 macro regime states from the PDF spec
// ═══════════════════════════════════════════════════════════════════
enum class MacroOntology : size_t {
    EXPANSION_DISINFLATION = 0,   // Goldilocks: growth up, inflation down
    EXPANSION_INFLATIONARY = 1,   // Late cycle: growth up, inflation up
    SLOWDOWN_DISINFLATION  = 2,   // Early slowdown: growth fading, inflation falling
    SLOWDOWN_INFLATIONARY  = 3,   // Stagflation risk: growth fading, inflation sticky
    RECESSION_DEFLATIONARY = 4,   // Deflation: growth contracting, prices falling
    RECESSION_INFLATIONARY = 5    // Worst case: growth contracting, inflation persistent
};

constexpr size_t MACRO_ONTOLOGY_COUNT = 6;

using MacroProbs = std::array<double, MACRO_ONTOLOGY_COUNT>;

inline std::string macro_ontology_name(MacroOntology o) {
    switch (o) {
        case MacroOntology::EXPANSION_DISINFLATION: return "EXPANSION_DISINFLATION";
        case MacroOntology::EXPANSION_INFLATIONARY: return "EXPANSION_INFLATIONARY";
        case MacroOntology::SLOWDOWN_DISINFLATION:  return "SLOWDOWN_DISINFLATION";
        case MacroOntology::SLOWDOWN_INFLATIONARY:  return "SLOWDOWN_INFLATIONARY";
        case MacroOntology::RECESSION_DEFLATIONARY: return "RECESSION_DEFLATIONARY";
        case MacroOntology::RECESSION_INFLATIONARY: return "RECESSION_INFLATIONARY";
    }
    return "UNKNOWN";
}

// ═══════════════════════════════════════════════════════════════════
// MacroDataPoint — a single timestamped macro observation
// ═══════════════════════════════════════════════════════════════════
struct MacroDataPoint {
    std::chrono::system_clock::time_point time;
    double value;
    std::string region;
    std::string index_name;
};

// ═══════════════════════════════════════════════════════════════════
// MacroPanel — all macro series aligned at a point in time
// ═══════════════════════════════════════════════════════════════════
struct MacroPanel {
    double cpi_yoy          = 0.0;   // CPI year-over-year % change
    double gdp_qoq_ann     = 0.0;   // GDP quarter-over-quarter annualized % change
    double ip_yoy           = 0.0;   // Industrial production YoY % change
    double consumer_conf    = 0.0;   // Consumer confidence index level
    double yield_10y        = 0.0;   // 10-year Treasury yield
    double yield_curve_slope = 0.0;  // 10Y - 2Y (or 10Y - 3M) spread

    // Derived z-scores (computed by the quadrant)
    double growth_zscore    = 0.0;
    double inflation_zscore = 0.0;
};

// ═══════════════════════════════════════════════════════════════════
// QuadrantResult — output of the Growth-Inflation Quadrant
// ═══════════════════════════════════════════════════════════════════
struct QuadrantResult {
    MacroProbs probabilities{};      // probability over 6 macro ontology states
    MacroOntology dominant_regime = MacroOntology::EXPANSION_DISINFLATION;
    double confidence       = 0.0;   // 0-1, how concentrated the belief is
    double growth_zscore    = 0.0;   // standardized growth composite
    double inflation_zscore = 0.0;   // standardized inflation composite
    int regime_age_days     = 0;     // how long we've been in the dominant regime

    // Provenance: which indicators drove the classification
    std::string growth_driver;       // e.g. "GDP +2.1% QoQ, IP +1.3% YoY"
    std::string inflation_driver;    // e.g. "CPI YoY +3.2%"
};

// ═══════════════════════════════════════════════════════════════════
// Utility: compute entropy of a probability distribution
// ═══════════════════════════════════════════════════════════════════
inline double entropy(const MacroProbs& p) {
    double h = 0.0;
    for (auto v : p)
        if (v > 1e-12) h -= v * std::log(v);
    return h;
}

inline double max_entropy() {
    return std::log(static_cast<double>(MACRO_ONTOLOGY_COUNT));
}

} // namespace trade_ngin
