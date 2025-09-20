// src/instruments/option.cpp
#include "trade_ngin/instruments/option.hpp"
#include <cmath>
#include <regex>
#include "trade_ngin/core/time_utils.hpp"

namespace trade_ngin {

namespace {
// Constants for numerical calculations
constexpr double SQRT_2PI = 2.506628274631000502415765284811045253006;
constexpr double MAX_ITERATIONS = 100;
constexpr double EPSILON = 1e-8;

// Standard normal cumulative distribution function
double norm_cdf(double x) {
    return 0.5 * (1.0 + std::erf(x / std::sqrt(2.0)));
}

// Standard normal probability density function
double norm_pdf(double x) {
    return std::exp(-0.5 * x * x) / SQRT_2PI;
}

}  // anonymous namespace

OptionInstrument::OptionInstrument(std::string symbol, OptionSpec spec)
    : symbol_(std::move(symbol)), spec_(std::move(spec)) {}

bool OptionInstrument::is_tradeable() const {
    auto now = std::chrono::system_clock::now();
    return !symbol_.empty() && !spec_.exchange.empty() && spec_.tick_size > 0.0 &&
           spec_.multiplier > 0.0 && now < spec_.expiry;
}

bool OptionInstrument::is_market_open(const Timestamp& timestamp) const {
    try {
        // Convert timestamp to local time
        std::time_t time = std::chrono::system_clock::to_time_t(timestamp);
        std::tm local_time_buffer;
        std::tm* local_time = trade_ngin::core::safe_localtime(&time, &local_time_buffer);

        // Check for weekdays only
        if (local_time->tm_wday == 0 || local_time->tm_wday == 6) {
            return false;
        }

        // Parse trading hours
        std::regex time_pattern("(\\d{2}):(\\d{2})-(\\d{2}):(\\d{2})");
        std::smatch matches;
        if (!std::regex_match(spec_.trading_hours, matches, time_pattern)) {
            return false;
        }

        int start_hour = std::stoi(matches[1]);
        int start_min = std::stoi(matches[2]);
        int end_hour = std::stoi(matches[3]);
        int end_min = std::stoi(matches[4]);

        int current_minutes = local_time->tm_hour * 60 + local_time->tm_min;
        int start_minutes = start_hour * 60 + start_min;
        int end_minutes = end_hour * 60 + end_min;

        return current_minutes >= start_minutes && current_minutes <= end_minutes;

    } catch (const std::exception&) {
        return false;
    }
}

double OptionInstrument::round_price(double price) const {
    return std::round(price / spec_.tick_size) * spec_.tick_size;
}

double OptionInstrument::get_notional_value(double quantity, double price) const {
    return std::abs(quantity) * price * spec_.multiplier;
}

double OptionInstrument::calculate_commission(double quantity) const {
    return std::abs(quantity) * spec_.commission_per_contract;
}

double OptionInstrument::get_margin_requirement() const {
    // For simplicity, using basic margin requirements
    // In practice, this would involve complex calculations based on
    // underlying price, volatility, and option moneyness
    return spec_.margin_requirement;
}

int OptionInstrument::days_to_expiry(const Timestamp& from) const {
    auto duration = spec_.expiry - from;
    return std::chrono::duration_cast<std::chrono::hours>(duration).count() / 24;
}

bool OptionInstrument::is_in_the_money(double underlying_price) const {
    if (spec_.type == OptionType::CALL) {
        return underlying_price > spec_.strike;
    } else {
        return underlying_price < spec_.strike;
    }
}

double OptionInstrument::get_moneyness(double underlying_price) const {
    return underlying_price / spec_.strike;
}

Greeks OptionInstrument::calculate_greeks(double underlying_price, double volatility,
                                          double risk_free_rate) const {
    Greeks greeks;
    auto now = std::chrono::system_clock::now();
    double t = days_to_expiry(now) / 365.0;  // Time to expiry in years

    if (t <= 0.0) {
        return greeks;  // Return zero Greeks if expired
    }

    double sqrt_t = std::sqrt(t);
    double d1 = (std::log(underlying_price / spec_.strike) +
                 (risk_free_rate + 0.5 * volatility * volatility) * t) /
                (volatility * sqrt_t);
    double d2 = d1 - volatility * sqrt_t;

    // Calculate Greeks based on option type
    if (spec_.type == OptionType::CALL) {
        greeks.delta = norm_cdf(d1);
        greeks.theta = (-underlying_price * volatility * norm_pdf(d1)) / (2 * sqrt_t) -
                       risk_free_rate * spec_.strike * std::exp(-risk_free_rate * t) * norm_cdf(d2);
    } else {
        greeks.delta = norm_cdf(d1) - 1.0;
        greeks.theta =
            (-underlying_price * volatility * norm_pdf(d1)) / (2 * sqrt_t) +
            risk_free_rate * spec_.strike * std::exp(-risk_free_rate * t) * norm_cdf(-d2);
    }

    // Common Greeks for both calls and puts
    greeks.gamma = norm_pdf(d1) / (underlying_price * volatility * sqrt_t);
    greeks.vega = underlying_price * sqrt_t * norm_pdf(d1) / 100.0;  // Per 1% vol change
    greeks.rho = (spec_.type == OptionType::CALL ? 1.0 : -1.0) * spec_.strike * t *
                 std::exp(-risk_free_rate * t) *
                 norm_cdf(spec_.type == OptionType::CALL ? d2 : -d2) / 100.0;  // Per 1% rate change

    return greeks;
}

std::optional<double> OptionInstrument::calculate_implied_volatility(double option_price,
                                                                     double underlying_price,
                                                                     double risk_free_rate) const {
    // Newton-Raphson method for implied volatility calculation
    double vol_guess = 0.3;  // Initial guess of 30%

    for (int i = 0; i < MAX_ITERATIONS; ++i) {
        double price = calculate_theoretical_price(underlying_price, vol_guess, risk_free_rate);

        if (std::abs(price - option_price) < EPSILON) {
            return vol_guess;
        }

        Greeks greeks = calculate_greeks(underlying_price, vol_guess, risk_free_rate);
        double vega = greeks.vega * 100.0;  // Convert back from per 1% to raw

        if (std::abs(vega) < EPSILON) {
            break;  // Avoid division by zero
        }

        vol_guess -= (price - option_price) / vega;

        if (vol_guess <= 0.0) {
            vol_guess = 0.0001;  // Keep volatility positive
        }
    }

    return std::nullopt;  // Failed to converge
}

double OptionInstrument::calculate_theoretical_price(double underlying_price, double volatility,
                                                     double risk_free_rate) const {
    auto now = std::chrono::system_clock::now();
    double t = days_to_expiry(now) / 365.0;  // Time to expiry in years

    if (t <= 0.0) {
        // Return intrinsic value if expired
        if (spec_.type == OptionType::CALL) {
            return std::max(0.0, underlying_price - spec_.strike);
        } else {
            return std::max(0.0, spec_.strike - underlying_price);
        }
    }

    double sqrt_t = std::sqrt(t);
    double d1 = (std::log(underlying_price / spec_.strike) +
                 (risk_free_rate + 0.5 * volatility * volatility) * t) /
                (volatility * sqrt_t);
    double d2 = d1 - volatility * sqrt_t;

    if (spec_.type == OptionType::CALL) {
        return underlying_price * norm_cdf(d1) -
               spec_.strike * std::exp(-risk_free_rate * t) * norm_cdf(d2);
    } else {
        return spec_.strike * std::exp(-risk_free_rate * t) * norm_cdf(-d2) -
               underlying_price * norm_cdf(-d1);
    }
}

}  // namespace trade_ngin
