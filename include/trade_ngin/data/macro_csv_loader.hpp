// include/trade_ngin/data/macro_csv_loader.hpp
#pragma once

#include <optional>
#include <string>
#include <vector>
#include "trade_ngin/core/error.hpp"

namespace trade_ngin {

/**
 * @brief Monthly macro regime record loaded from pre-computed CSV.
 *
 * Produced by scripts/generate_bpgv_macro.py from FRED/ALFRED data.
 * Consumed by BPGVRotationStrategy at initialization.
 */
struct MonthlyMacroRecord {
    int year{0};
    int month{0};
    double bpgv{0.0};                // Building Permit Growth Volatility (legacy: PERMIT total, 12m rolling stdev)
    double bpgv_ewma{0.0};           // EWMA(6)-smoothed BPGV
    double bpgv_percentile{0.0};     // Rolling 60-month percentile [0, 100]
    double yield_curve_spread{0.0};  // 10Y-2Y Treasury spread
    double ewma_slope{0.0};          // BPGV EWMA slope (rising/falling)
    double regime_score{0.0};        // Composite score [-1, +1]
    double permit_growth{0.0};       // Month-over-month permit growth rate
    bool strong_risk_on{false};      // Strong risk-on flag

    // T1.1+T1.3 paper-faithful signals (Cortes & LaPoint 2025) — appended at
    // end of CSV row; legacy CSVs without these columns are still loadable.
    // See reports/a1_data_sufficiency_for_garch.md.
    double bpg_sfh{0.0};             // log(PERMIT1_t / PERMIT1_{t-1}); single-family permit growth
    double bpgv_sfh{0.0};            // Rolling 12m stdev of bpg_sfh
    double bpgv_garch{0.0};          // σ_{t+1|t}^BPG from expanding-window GARCH(1,1) QMLE on bpg_sfh
    double bpgv_garch_percentile{0.0};  // Rolling 60m percentile of bpgv_garch [0, 100]
    bool garch_converged{false};     // 1 if QMLE converged that month
    double garch_alpha{0.0};         // ARCH coefficient α (sanity: α+β ≈ 0.95-0.99 per paper)
    double garch_beta{0.0};          // GARCH coefficient β
};

/**
 * @brief Loads pre-computed monthly macro regime data from CSV files.
 *
 * CSV format (header + data rows):
 *   year,month,bpgv,bpgv_ewma,bpgv_percentile,yield_curve_spread,ewma_slope,regime_score,permit_growth,strong_risk_on
 *   [,bpg_sfh,bpgv_sfh,bpgv_garch,bpgv_garch_percentile,garch_converged,garch_alpha,garch_beta]
 *
 * Columns from bpg_sfh onward are optional — pre-T1.1 CSVs that stop at
 * strong_risk_on still load, with the new fields defaulting to 0 / false.
 */
class MacroCSVLoader {
public:
    /**
     * @brief Load macro regime data from a CSV file.
     * @param filepath Path to the CSV file.
     * @return Sorted vector of MonthlyMacroRecord, or error.
     */
    static Result<std::vector<MonthlyMacroRecord>> load(const std::string& filepath);

    /**
     * @brief Find the record for an exact year/month.
     * @return The record if found, std::nullopt otherwise.
     */
    static std::optional<MonthlyMacroRecord> find_record(
        const std::vector<MonthlyMacroRecord>& records, int year, int month);

    /**
     * @brief Find the most recent record at or before the given year/month.
     * @return The record if found, std::nullopt if no records exist before the date.
     */
    static std::optional<MonthlyMacroRecord> find_record_before(
        const std::vector<MonthlyMacroRecord>& records, int year, int month);
};

}  // namespace trade_ngin
