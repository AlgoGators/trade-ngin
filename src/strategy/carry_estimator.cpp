// src/strategy/carry_estimator.cpp
#include "trade_ngin/strategy/carry_estimator.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "trade_ngin/strategy/trend_estimator.hpp"

namespace trade_ngin {
namespace carry_estimator {

double raw_carry(const CarryQuote& quote) {
    if (quote.months < 0) return quote.other_close - quote.held_close;
    if (quote.months > 0) return quote.held_close - quote.other_close;
    return 0.0;
}

double expiry_difference(const CarryQuote& quote) {
    return static_cast<double>(std::abs(quote.months)) / 12.0;
}

double annualised_raw_carry(const CarryQuote& quote) {
    if (quote.months == 0) return 0.0;
    return raw_carry(quote) / expiry_difference(quote);
}

Estimate estimate(const Window& window, const std::vector<int>& spans, double scalar) {
    Estimate out;
    const std::size_t total = window.close.size();
    out.smoothed.assign(spans.size(), 0.0);
    out.scaled.assign(spans.size(), 0.0);
    if (total == 0 || window.quote.size() != total || window.forecast_sigma.size() != total) {
        return out;
    }
    // The trend estimators' window: the last kWindowBars bars, indexed from the window's first.
    const std::size_t first =
        total > trend_estimator::kWindowBars ? total - trend_estimator::kWindowBars : 0;
    const std::size_t n = total - first;
    std::vector<double> lambda(spans.size(), 0.0);
    for (std::size_t s = 0; s < spans.size(); ++s) {
        lambda[s] = 2.0 / (static_cast<double>(spans[s]) + 1.0);
    }
    std::optional<CarryQuote> last_quote;
    double last_annualised = 0.0;
    double forecast = 0.0;
    double divisor = 0.0;
    bool seeded = false;
    std::vector<double> smoothed(spans.size(), 0.0);
    for (std::size_t k = 0; k < n; ++k) {
        const auto& quote = window.quote[first + k];
        if (quote) {
            last_quote = quote;
            last_annualised = annualised_raw_carry(*quote);
        }
        if (k == 0 || !last_quote) continue;  // bar 0 has no volatility; no raw carry known yet
        divisor = window.close[first + k] * window.forecast_sigma[first + k];
        forecast = last_annualised / divisor;
        if (!std::isfinite(forecast)) forecast = 0.0;
        for (std::size_t s = 0; s < spans.size(); ++s) {
            smoothed[s] = seeded ? lambda[s] * forecast + (1.0 - lambda[s]) * smoothed[s] : forecast;
        }
        seeded = true;
        ++out.forecast_bars;
    }
    out.quoted = window.quote[total - 1].has_value();
    out.active = seeded;
    if (!out.active) return out;
    out.quote = last_quote;
    out.raw_carry = raw_carry(*last_quote);
    out.annualised = last_annualised;
    out.divisor = divisor;
    out.forecast = forecast;
    double sum = 0.0;
    for (std::size_t s = 0; s < spans.size(); ++s) {
        out.smoothed[s] = smoothed[s];
        double scaled = smoothed[s] * scalar;
        if (!std::isfinite(scaled)) scaled = 0.0;
        out.scaled[s] = std::clamp(scaled, -kCarryCap, kCarryCap);
        sum += out.scaled[s];
    }
    out.mean_scaled = spans.empty() ? 0.0 : sum / static_cast<double>(spans.size());
    return out;
}

}  // namespace carry_estimator
}  // namespace trade_ngin
