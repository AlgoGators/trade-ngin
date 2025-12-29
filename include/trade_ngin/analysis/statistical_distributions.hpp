#pragma once

#include <cmath>
#include <algorithm>

namespace trade_ngin {
namespace analysis {

/**
 * @brief Statistical distribution functions for hypothesis testing
 *
 * This module provides cumulative distribution functions (CDF),
 * probability density functions (PDF), and quantile functions for
 * commonly used statistical distributions.
 */
class StatisticalDistributions {
public:
    /**
     * @brief Standard normal distribution CDF (Φ(x))
     * @param x Input value
     * @return Probability that X ≤ x for X ~ N(0,1)
     */
    static double normal_cdf(double x);

    /**
     * @brief Standard normal distribution PDF (φ(x))
     * @param x Input value
     * @return Probability density at x for X ~ N(0,1)
     */
    static double normal_pdf(double x);

    /**
     * @brief Inverse of standard normal CDF (quantile function)
     * @param p Probability (0 < p < 1)
     * @return Value x such that Φ(x) = p
     */
    static double normal_quantile(double p);

    /**
     * @brief Student's t-distribution CDF
     * @param x Input value
     * @param df Degrees of freedom
     * @return Probability that T ≤ x for T ~ t(df)
     */
    static double t_cdf(double x, double df);

    /**
     * @brief Student's t-distribution quantile function
     * @param p Probability (0 < p < 1)
     * @param df Degrees of freedom
     * @return Value x such that P(T ≤ x) = p
     */
    static double t_quantile(double p, double df);

    /**
     * @brief Chi-square distribution CDF
     * @param x Input value
     * @param df Degrees of freedom
     * @return Probability that χ² ≤ x for χ² ~ χ²(df)
     */
    static double chi_square_cdf(double x, double df);

    /**
     * @brief Chi-square distribution quantile function
     * @param p Probability (0 < p < 1)
     * @param df Degrees of freedom
     * @return Value x such that P(χ² ≤ x) = p
     */
    static double chi_square_quantile(double p, double df);

private:
    // Helper functions for incomplete beta and gamma functions
    static double incomplete_beta(double x, double a, double b);
    static double incomplete_gamma(double x, double a);
    static double log_gamma(double x);
    static constexpr double PI = 3.14159265358979323846;
    static constexpr double SQRT_2 = 1.41421356237309504880;
};

} // namespace analysis
} // namespace trade_ngin
