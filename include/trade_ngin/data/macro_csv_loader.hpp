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
    double bpgv{0.0};                // Building Permit Growth Volatility (12m rolling stdev)
    double bpgv_ewma{0.0};           // EWMA(6)-smoothed BPGV
    double bpgv_percentile{0.0};     // Rolling 60-month percentile [0, 100]
    double yield_curve_spread{0.0};  // 10Y-2Y Treasury spread
    double ewma_slope{0.0};          // BPGV EWMA slope (rising/falling)
    double regime_score{0.0};        // Composite score [-1, +1]
    double permit_growth{0.0};       // Month-over-month permit growth rate
    bool strong_risk_on{false};      // Strong risk-on flag

    double bpg_sfh{0.0};                // log(PERMIT1_t / PERMIT1_{t-1}); single-family permit growth
    double bpgv_sfh{0.0};               // Rolling 12m stdev of bpg_sfh
    double bpgv_garch{0.0};             // sigma_{t+1|t} from expanding-window GARCH(1,1) on bpg_sfh
    double bpgv_garch_percentile{0.0};  // Rolling 60m percentile of bpgv_garch [0, 100]
    bool garch_converged{false};        // 1 if the GARCH fit converged that month
    double garch_alpha{0.0};            // ARCH coefficient
    double garch_beta{0.0};             // GARCH coefficient
};

/**
 * @brief Loads pre-computed monthly macro regime data from CSV files.
 *
 * Columns are read by position after a header line:
 *   year,month,bpgv,bpgv_ewma,bpgv_percentile,yield_curve_spread,ewma_slope,regime_score,permit_growth,strong_risk_on
 *   [,bpg_sfh,bpgv_sfh,bpgv_garch,bpgv_garch_percentile,garch_converged,garch_alpha,garch_beta]
 *
 * The first ten are required. The GARCH columns are optional: absent or empty, a field keeps
 * its default. A value that is present and does not parse is an error. Later columns are
 * ignored.
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
