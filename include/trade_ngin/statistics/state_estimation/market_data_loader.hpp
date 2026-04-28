#pragma once

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/postgres_database.hpp"

#include <Eigen/Dense>
#include <string>
#include <vector>

namespace trade_ngin {
namespace statistics {

// ============================================================================
// MarketPanel — OHLCV time series for a single symbol
// ============================================================================

struct MarketPanel {
    std::vector<Bar>    bars;       // T bars, sorted ascending by date
    std::vector<double> returns;    // T-1 log returns (from close prices)
    std::vector<double> volumes;    // T volume values
    std::vector<std::string> dates; // T dates, ISO "YYYY-MM-DD"
    std::string symbol;
    int T = 0;
};

// ============================================================================
// SleevePanel — aggregated data for all symbols in one sleeve
// ============================================================================

struct SleevePanel {
    std::string sleeve_name;                        // "equities", "rates", "fx", "commodities"
    std::vector<MarketPanel> symbol_panels;          // one per symbol
    Eigen::MatrixXd          composite_returns;      // T × N_symbols
    std::vector<std::string>  dates;                 // T dates (aligned across symbols)
    int T = 0;
    int N = 0;                                       // number of symbols
};

// ============================================================================
// MarketDataLoader
//
// Loads market OHLCV data from PostgreSQL for use in the market regime
// pipeline. Mirrors MacroDataLoader but queries market_data / futures_data
// schemas instead of macro_data.
//
// Usage:
//   MarketDataLoader loader(db);
//   auto panel = loader.load_sleeve("fx", {"6E","6J","6B"}, AssetClass::FUTURES,
//                                   "2020-01-01", "2024-12-31");
// ============================================================================

class MarketDataLoader {
public:
    explicit MarketDataLoader(PostgresDatabase& db);

    // Load data for a single symbol
    Result<MarketPanel> load_symbol(
        const std::string& symbol,
        AssetClass asset_class,
        const std::string& start_date = "",
        const std::string& end_date   = "",
        DataFrequency freq = DataFrequency::DAILY) const;

    // Load data for an entire sleeve (multiple symbols, aligned dates)
    Result<SleevePanel> load_sleeve(
        const std::string& sleeve_name,
        const std::vector<std::string>& symbols,
        AssetClass asset_class,
        const std::string& start_date = "",
        const std::string& end_date   = "",
        DataFrequency freq = DataFrequency::DAILY) const;

    // Compute log returns from close prices.
    // L-26: emits NaN (not silent 0.0) for missing/non-positive prices.
    // Public for testing — pure utility, no class state.
    static std::vector<double> compute_log_returns(const std::vector<Bar>& bars);

private:
    PostgresDatabase& db_;

    // Convert Arrow Table from get_market_data() to MarketPanel
    static Result<MarketPanel> arrow_to_panel(
        const std::shared_ptr<arrow::Table>& table,
        const std::string& symbol);

    // Align multiple symbol panels to common dates
    static Result<SleevePanel> align_panels(
        const std::string& sleeve_name,
        std::vector<MarketPanel>& panels);
};

} // namespace statistics
} // namespace trade_ngin
