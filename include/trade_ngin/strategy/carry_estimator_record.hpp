// include/trade_ngin/strategy/carry_estimator_record.hpp
#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "trade_ngin/core/record_file.hpp"
#include "trade_ngin/strategy/carry_estimator.hpp"

namespace trade_ngin {

/**
 * @brief The trend sleeve's record of its carry rules and the blend (trade-ngin #185), one row per
 *        symbol per signal bar with carry on, for the hand checks against the reference.
 *
 * Written only when the environment names a directory (TRADE_NGIN_SERIES_DUMP_DIR, as
 * trend_estimator_record.hpp); a production run never sets it, and without it nothing is opened.
 * The file is <dir>/carry_<strategy id>.csv, one header line when it is created, every value with
 * seventeen significant digits.
 *
 * Columns: the signal bar's date; the symbol; 1 when a raw carry is known (else the trend alone was
 * traded and the carry columns are 0); 1 when the bar has its own quote (else the last known stands);
 * the window bars with a carry forecast; the quote the raw carry comes from (held and other symbol,
 * months, the two closes); raw carry; annualised raw carry; the divisor (sigma_p x 16 = close x the
 * forecast's volatility); the bar's carry forecast; each span's smoothed and capped value in the
 * configured order; the carry average; the trend speeds' average before and after the equity slow
 * rule; the rules of each style and their sum; the FDM; the blended forecast.
 */
inline void append_carry_record(const std::string& strategy_id, const std::string& date,
                                const std::string& symbol, const std::vector<int>& spans,
                                const carry_estimator::Estimate& carry,
                                const carry_estimator::Blend& blend, double trend_mean,
                                double trend_part, bool used) {
    const char* dir = std::getenv("TRADE_NGIN_SERIES_DUMP_DIR");
    if (dir == nullptr || *dir == '\0') return;
    const std::string name = "carry_" + strategy_id + ".csv";
    std::string header =
        "date,symbol,active,quoted,forecast_bars,held_symbol,other_symbol,months,held_close,"
        "other_close,raw_carry,annualised,divisor,forecast";
    for (int span : spans) header += ",smoothed_" + std::to_string(span);
    for (int span : spans) header += ",scaled_" + std::to_string(span);
    header += ",carry_mean,trend_mean,trend_part,trend_rules,carry_rules,rules,fdm,blended";
    std::FILE* out = record_file::open_rows(dir, name, header);
    if (out == nullptr) return;
    const CarryQuote quote = carry.quote ? *carry.quote : CarryQuote{};
    std::fprintf(out, "%s,%s,%d,%d,%zu,%s,%s,%d,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g", date.c_str(),
                 symbol.c_str(), used ? 1 : 0, carry.quoted ? 1 : 0, carry.forecast_bars,
                 quote.held_symbol.c_str(), quote.other_symbol.c_str(), quote.months,
                 quote.held_close, quote.other_close, carry.raw_carry, carry.annualised,
                 carry.divisor, carry.forecast);
    for (std::size_t s = 0; s < spans.size(); ++s) {
        std::fprintf(out, ",%.17g", s < carry.smoothed.size() ? carry.smoothed[s] : 0.0);
    }
    for (std::size_t s = 0; s < spans.size(); ++s) {
        std::fprintf(out, ",%.17g", s < carry.scaled.size() ? carry.scaled[s] : 0.0);
    }
    std::fprintf(out, ",%.17g,%.17g,%.17g,%d,%d,%d,%.17g,%.17g\n", carry.mean_scaled, trend_mean,
                 used ? trend_part : trend_mean, blend.trend_rules, blend.carry_rules, blend.rules,
                 blend.fdm, blend.forecast);
    record_file::close_rows(out, dir, name);
}

}  // namespace trade_ngin
