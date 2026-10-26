// include/trade_ngin/strategy/carry_estimator.hpp
#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "trade_ngin/core/types.hpp"

namespace trade_ngin {
namespace carry_estimator {

/**
 * The carry rules (trade-ngin #185; Carver, Advanced Futures Trading Strategies, strategy ten) at one
 * signal bar, as a function of the same fixed trailing window of consumed bars the trend estimators
 * read (trend_estimator.hpp, kWindowBars) and of nothing else.
 *
 *   raw carry            = nearer close - held close   (the quote's months < 0)
 *                        = held close - further close  (months > 0: the held contract is the front)
 *   expiry difference    = |months| / 12
 *   annualised raw carry = raw carry / expiry difference
 *   carry forecast       = annualised raw carry / (sigma_p x 16)
 *   smoothed(span)       = EWMA_span(carry forecast), lambda = 2 / (span + 1)
 *   scaled(span)         = smoothed(span) x 30, capped at +/-20
 *
 * sigma_p x 16 is the trend forecast's own divisor at the bar: close x forecast_sigma, the raw close
 * the trend forecast divides by and the forecast's volatility the trend estimators compute for the
 * same bar (trend_estimator::estimate's forecast_sigma_bars). The two prices are the quote's: real
 * contract closes of the same date (CarryQuote), never the adjusted series.
 *
 * The missing-price rule (HD 2026-10-26, Q4): a bar with no quote keeps the last known annualised
 * raw carry of the window (its own pair and expiry difference) and divides it by its own bar's
 * divisor. Before the window's first quote there is no carry forecast, and every EWMA starts at the
 * first forecast (seeded there, as the trend EMAs are seeded at the window's first level). An
 * instrument with no quote anywhere in its window up to the signal bar has no carry: `active` is
 * false and the strategy trades its trend forecast alone (Q3, applied per bar). Bar 0 of the window
 * has no volatility and gives no forecast, but its quote is known. Nothing is carried from one call
 * to the next.
 */
inline constexpr double kCarryScalar = 30.0;
inline constexpr double kCarryCap = 20.0;

/// Carver's four spans (business days), fastest first.
inline const std::vector<int>& default_spans() {
    static const std::vector<int> spans{5, 20, 60, 120};
    return spans;
}

/// nearer - held (months < 0), held - further (months > 0); 0 for months 0.
double raw_carry(const CarryQuote& quote);
/// |months| / 12.
double expiry_difference(const CarryQuote& quote);
/// raw_carry / expiry_difference; 0 for months 0.
double annualised_raw_carry(const CarryQuote& quote);

/// One symbol's window, oldest first, all three vectors the same length (the trend window's bars).
struct Window {
    std::vector<std::optional<CarryQuote>> quote;
    std::vector<double> close;           // raw close, the trend forecast's price
    std::vector<double> forecast_sigma;  // the trend forecast's volatility per bar (bar 0 unused)
};

struct Estimate {
    bool active = false;        // a raw carry is known at the signal bar (else: trend only)
    bool quoted = false;        // the signal bar has its own quote (else the last known stands)
    std::size_t forecast_bars = 0;  // window bars with a carry forecast, up to the signal bar
    std::optional<CarryQuote> quote;  // the quote the signal bar's raw carry comes from
    double raw_carry = 0.0;
    double annualised = 0.0;
    double divisor = 0.0;       // sigma_p x 16 at the signal bar: close x forecast_sigma
    double forecast = 0.0;      // the signal bar's carry forecast, before any smoothing
    std::vector<double> smoothed;  // per span, at the signal bar
    std::vector<double> scaled;    // per span: x scalar, capped
    double mean_scaled = 0.0;      // the equal-weight mean of the capped spans
};

/**
 * @brief The carry rules at the window's last bar.
 * @param window the trailing consumed bars (at most trend_estimator::kWindowBars are read: the last)
 * @param spans the EWMA spans the contract keeps
 * @param scalar the forecast scalar (30)
 */
Estimate estimate(const Window& window, const std::vector<int>& spans,
                  double scalar = kCarryScalar);

}  // namespace carry_estimator
}  // namespace trade_ngin
