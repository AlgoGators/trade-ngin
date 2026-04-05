#pragma once

#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/core/error.hpp"

#include <Eigen/Dense>
#include <string>
#include <vector>

namespace trade_ngin {
namespace statistics {

// ============================================================================
// MacroPanel — the output of MacroDataLoader
// Rows = dates (ascending), Cols = macro indicators
// ============================================================================

struct MacroPanel {
    Eigen::MatrixXd   data;          // T x N  (NaN for missing releases)
    std::vector<std::string> dates;  // length T, ISO format "YYYY-MM-DD"
    std::vector<std::string> column_names;  // length N
    int T = 0;
    int N = 0;
};

// ============================================================================
// MacroDataLoader
//
// Queries the macro_data schema (inflation, growth, yield_curve,
// credit_spreads, liquidity, market) from PostgreSQL, full-outer-joins
// on date, and returns a MacroPanel ready for DynamicFactorModel::fit().
//
// Usage:
//   MacroDataLoader loader(db);
//   auto panel = loader.load();
//   dfm.fit(panel->data, panel->column_names);
// ============================================================================

class MacroDataLoader {
public:
    explicit MacroDataLoader(PostgresDatabase& db);

    // Load the full macro panel from macro_data schema.
    // Optionally filter by date range (ISO "YYYY-MM-DD" strings).
    Result<MacroPanel> load(const std::string& start_date = "",
                            const std::string& end_date   = "") const;

    // Load a single observation row for a given date (for online update).
    // Returns a vector of length N with NaN for missing columns.
    Result<std::pair<Eigen::VectorXd, std::vector<std::string>>>
    load_single(const std::string& date) const;

private:
    PostgresDatabase& db_;

    // Build the SQL that full-outer-joins all 6 tables
    std::string build_panel_query(const std::string& start_date,
                                  const std::string& end_date) const;
};

} // namespace statistics
} // namespace trade_ngin
