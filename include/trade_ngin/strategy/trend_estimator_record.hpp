// include/trade_ngin/strategy/trend_estimator_record.hpp
#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "trade_ngin/strategy/trend_estimator.hpp"

namespace trade_ngin {

/**
 * @brief A trend sleeve's own record of the estimators it computed, one row per symbol per signal
 *        bar, for the acceptance comparison against the reference implementation.
 *
 * Written only when the environment names a directory (TRADE_NGIN_SERIES_DUMP_DIR, the consumed
 * series record's variable); a production run never sets it, and without it nothing is opened,
 * written or logged. The file is <dir>/estimator_<strategy id>.csv, appended to, one header line
 * when it is created. Every value is printed with seventeen significant digits.
 *
 * Columns: the signal bar's date; the symbol; the bars the window held and the values behind the
 * long-run mean; the annualisation factor; sigma_short, sigma_long, sigma (the sizing volatility)
 * and the forecast's volatility; the attenuation; the combined forecast; the capital, weight,
 * multiplier and raw close the position was sized on; the position before any limit; then each
 * pair's scaled forecast, in the configured order.
 */
inline void append_trend_estimator_record(const std::string& strategy_id, const std::string& date,
                                          const std::string& symbol,
                                          const trend_estimator::Estimate& estimate,
                                          const std::vector<std::pair<int, int>>& pairs,
                                          double forecast, double capital, double weight,
                                          double multiplier, double price,
                                          double optimal_position) {
    const char* dir = std::getenv("TRADE_NGIN_SERIES_DUMP_DIR");
    if (dir == nullptr || *dir == '\0') return;
    const std::string path = std::string(dir) + "/estimator_" + strategy_id + ".csv";
    bool fresh = true;
    if (std::FILE* probe = std::fopen(path.c_str(), "r")) {
        fresh = false;
        std::fclose(probe);
    }
    std::FILE* out = std::fopen(path.c_str(), "a");
    if (out == nullptr) return;
    if (fresh) {
        std::fprintf(out,
                     "date,symbol,window_bars,long_values,factor,sigma_short,sigma_long,sigma,"
                     "forecast_sigma,attenuation,forecast,capital,weight,multiplier,price,"
                     "optimal_position");
        for (const auto& [fast, slow] : pairs) std::fprintf(out, ",scaled_%d_%d", fast, slow);
        std::fprintf(out, "\n");
    }
    std::fprintf(out, "%s,%s,%zu,%zu,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g",
                 date.c_str(), symbol.c_str(), estimate.window_bars, estimate.long_values,
                 estimate.factor, estimate.sigma_short, estimate.sigma_long, estimate.sigma,
                 estimate.forecast_sigma, estimate.attenuation, forecast, capital, weight,
                 multiplier, price, optimal_position);
    for (double scaled : estimate.scaled) std::fprintf(out, ",%.17g", scaled);
    std::fprintf(out, "\n");
    std::fclose(out);
}

}  // namespace trade_ngin
