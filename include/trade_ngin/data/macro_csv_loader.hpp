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
    double bpgv{0.0};                // Building Permit Growth Volatility
    double bpgv_ewma{0.0};           // EWMA(6)-smoothed BPGV
    double bpgv_percentile{0.0};     // Rolling 60-month percentile [0, 100]
    double yield_curve_spread{0.0};  // 10Y-2Y Treasury spread
    double ewma_slope{0.0};          // BPGV EWMA slope (rising/falling)
    double regime_score{0.0};        // Composite score [-1, +1]
    double permit_growth{0.0};       // Month-over-month permit growth rate
    bool strong_risk_on{false};      // Strong risk-on flag
};

/**
 * @brief Loads pre-computed monthly macro regime data from CSV files.
 *
 * CSV format (header + data rows):
 *   year,month,bpgv,bpgv_ewma,bpgv_percentile,yield_curve_spread,ewma_slope,regime_score,permit_growth,strong_risk_on
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
