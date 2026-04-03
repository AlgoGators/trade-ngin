// include/trade_ngin/data/daily_macro_csv_loader.hpp
#pragma once

#include <optional>
#include <string>
#include <vector>
#include "trade_ngin/core/error.hpp"

namespace trade_ngin {

/**
 * @brief Daily macro data record for the Copper-Gold IP strategy.
 *
 * Produced by scripts/generate_copper_gold_macro.py from FRED/OECD data.
 * Consumed by CopperGoldIPStrategy at initialization.
 */
struct DailyMacroRecord {
    int year{0};
    int month{0};
    int day{0};
    double dxy{0.0};               // US Dollar Index (DTWEXBGS)
    double vix{0.0};               // CBOE Volatility Index (VIXCLS)
    double hy_spread{0.0};         // High-yield credit spread bps (BAMLH0A0HYM2)
    double breakeven_10y{0.0};     // 10Y breakeven inflation (T10YIE)
    double yield_10y{0.0};         // 10Y Treasury yield (DGS10)
    double tips_10y{0.0};          // 10Y TIPS yield (DFII10)
    double spx{0.0};              // S&P 500 close (SP500)
    double fed_balance_sheet{0.0}; // Fed balance sheet trillions (WALCL)
    double china_cli{0.0};         // China OECD composite leading indicator
    double cny_usd{0.0};           // CNY/USD exchange rate (DEXCHUS)

    int date_key() const { return year * 10000 + month * 100 + day; }
};

/**
 * @brief Loads daily macro data from CSV files for the Copper-Gold IP strategy.
 *
 * CSV format (header + data rows):
 *   year,month,day,dxy,vix,hy_spread,breakeven_10y,yield_10y,tips_10y,spx,fed_balance_sheet,china_cli,cny_usd
 */
class DailyMacroCSVLoader {
public:
    /**
     * @brief Load daily macro data from a CSV file.
     * @param filepath Path to the CSV file.
     * @return Sorted vector of DailyMacroRecord, or error.
     */
    static Result<std::vector<DailyMacroRecord>> load(const std::string& filepath);

    /**
     * @brief Find the record for an exact year/month/day.
     * @return The record if found, std::nullopt otherwise.
     */
    static std::optional<DailyMacroRecord> find_record(
        const std::vector<DailyMacroRecord>& records, int year, int month, int day);

    /**
     * @brief Find the most recent record at or before the given date.
     * @return The record if found, std::nullopt if no records exist before the date.
     */
    static std::optional<DailyMacroRecord> find_record_before(
        const std::vector<DailyMacroRecord>& records, int year, int month, int day);
};

}  // namespace trade_ngin
