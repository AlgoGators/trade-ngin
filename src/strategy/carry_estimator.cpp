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

double rule_count_fdm(int rules) {
    // Carver, Advanced Futures Trading Strategies, table 52 as printed.
    static const double table[] = {0.0,  1.00, 1.02, 1.03, 1.23, 1.25, 1.27, 1.29, 1.32, 1.34, 1.35, 1.36,
                                   1.38, 1.39, 1.41, 1.42, 1.44, 1.46, 1.48, 1.50, 1.53, 1.54, 1.55};
    if (rules >= 1 && rules <= 22) return table[rules];
    if (rules == 25) return 1.69;
    if (rules == 30) return 1.81;
    if (rules == 35) return 1.93;
    if (rules >= 40) return 2.00;
    return 0.0;
}

Blend blend(double trend_mean, int trend_rules, double carry_mean, int carry_rules,
            double carry_weight) {
    Blend out;
    const double trend_style = 1.0 - carry_weight;
    out.trend_rules = trend_style > 0.0 ? trend_rules : 0;
    out.carry_rules = carry_weight > 0.0 ? carry_rules : 0;
    out.rules = out.trend_rules + out.carry_rules;
    out.trend_weight = out.trend_rules > 0 ? trend_style / out.trend_rules : 0.0;
    out.carry_weight = out.carry_rules > 0 ? carry_weight / out.carry_rules : 0.0;
    out.fdm = rule_count_fdm(out.rules);
    out.trend_part = out.trend_rules > 0 ? trend_style * trend_mean : 0.0;
    out.carry_part = out.carry_rules > 0 ? carry_weight * carry_mean : 0.0;
    double forecast = out.fdm * (out.trend_part + out.carry_part);
    if (!std::isfinite(forecast)) forecast = 0.0;
    out.forecast = std::clamp(forecast, -kCarryCap, kCarryCap);
    return out;
}

}  // namespace carry_estimator
}  // namespace trade_ngin
