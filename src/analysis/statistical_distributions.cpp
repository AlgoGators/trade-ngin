#include "trade_ngin/analysis/statistical_distributions.hpp"
#include <cmath>
#include <stdexcept>

namespace trade_ngin {
namespace analysis {

// Normal distribution CDF using rational approximation
double StatisticalDistributions::normal_cdf(double x) {
    // Constants for rational approximation
    const double a1 =  0.254829592;
    const double a2 = -0.284496736;
    const double a3 =  1.421413741;
    const double a4 = -1.453152027;
    const double a5 =  1.061405429;
    const double p  =  0.3275911;

    int sign = 1;
    if (x < 0) {
        sign = -1;
    }
    x = std::abs(x) / SQRT_2;

    // A&S formula 7.1.26
    double t = 1.0 / (1.0 + p * x);
    double y = 1.0 - (((((a5 * t + a4) * t) + a3) * t + a2) * t + a1) * t * std::exp(-x * x);

    return 0.5 * (1.0 + sign * y);
}

// Normal distribution PDF
double StatisticalDistributions::normal_pdf(double x) {
    return std::exp(-0.5 * x * x) / std::sqrt(2.0 * PI);
}

// Normal quantile using Beasley-Springer-Moro algorithm
double StatisticalDistributions::normal_quantile(double p) {
    if (p <= 0.0 || p >= 1.0) {
        throw std::invalid_argument("Probability must be in (0, 1)");
    }

    // Coefficients for the rational approximation
    const double a0 = 2.50662823884;
    const double a1 = -18.61500062529;
    const double a2 = 41.39119773534;
    const double a3 = -25.44106049637;

    const double b0 = -8.47351093090;
    const double b1 = 23.08336743743;
    const double b2 = -21.06224101826;
    const double b3 = 3.13082909833;

    const double c0 = 0.3374754822726147;
    const double c1 = 0.9761690190917186;
    const double c2 = 0.1607979714918209;
    const double c3 = 0.0276438810333863;
    const double c4 = 0.0038405729373609;
    const double c5 = 0.0003951896511919;
    const double c6 = 0.0000321767881768;
    const double c7 = 0.0000002888167364;
    const double c8 = 0.0000003960315187;

    double x = p - 0.5;
    double r;

    if (std::abs(x) < 0.42) {
        // Central region
        r = x * x;
        r = x * (((a3 * r + a2) * r + a1) * r + a0) /
            ((((b3 * r + b2) * r + b1) * r + b0) * r + 1.0);
        return r;
    }

    // Tail region
    r = p;
    if (x > 0.0) {
        r = 1.0 - p;
    }
    r = std::log(-std::log(r));
    r = c0 + r * (c1 + r * (c2 + r * (c3 + r * (c4 + r * (c5 + r * (c6 + r * (c7 + r * c8)))))));
    if (x < 0.0) {
        r = -r;
    }
    return r;
}

// Log-gamma function using Lanczos approximation
double StatisticalDistributions::log_gamma(double x) {
    const double coef[8] = {
        676.5203681218851,
        -1259.1392167224028,
        771.32342877765313,
        -176.61502916214059,
        12.507343278686905,
        -0.13857109526572012,
        9.9843695780195716e-6,
        1.5056327351493116e-7
    };

    if (x < 0.5) {
        return std::log(PI / std::sin(PI * x)) - log_gamma(1.0 - x);
    }

    x -= 1.0;
    double base = x + 7.5;
    double sum = 0.99999999999980993;
    for (int i = 0; i < 8; ++i) {
        sum += coef[i] / (x + i + 1.0);
    }

    return std::log(2.506628274631000502) + std::log(sum) - base + std::log(base) * (x + 0.5);
}

// Incomplete gamma function using series expansion
double StatisticalDistributions::incomplete_gamma(double x, double a) {
    if (x < 0.0 || a <= 0.0) {
        throw std::invalid_argument("Invalid parameters for incomplete gamma");
    }

    if (x == 0.0) {
        return 0.0;
    }

    // Series expansion
    const int max_iter = 200;
    const double epsilon = 1e-10;

    double sum = 1.0 / a;
    double term = 1.0 / a;

    for (int n = 1; n < max_iter; ++n) {
        term *= x / (a + n);
        sum += term;
        if (std::abs(term) < epsilon * std::abs(sum)) {
            break;
        }
    }

    return sum * std::exp(-x + a * std::log(x) - log_gamma(a));
}

// Incomplete beta function using continued fraction
double StatisticalDistributions::incomplete_beta(double x, double a, double b) {
    if (x < 0.0 || x > 1.0) {
        throw std::invalid_argument("x must be in [0, 1]");
    }
    if (a <= 0.0 || b <= 0.0) {
        throw std::invalid_argument("a and b must be positive");
    }

    if (x == 0.0 || x == 1.0) {
        return x;
    }

    // Use symmetry relation if needed
    bool flip = false;
    if (x > (a + 1.0) / (a + b + 2.0)) {
        flip = true;
        std::swap(a, b);
        x = 1.0 - x;
    }

    // Continued fraction evaluation using modified Lentz's method
    const int max_iter = 200;
    const double epsilon = 1e-10;
    const double tiny = 1e-30;

    double qab = a + b;
    double qap = a + 1.0;
    double qam = a - 1.0;
    double c = 1.0;
    double d = 1.0 - qab * x / qap;

    if (std::abs(d) < tiny) d = tiny;
    d = 1.0 / d;
    double h = d;

    for (int m = 1; m <= max_iter; ++m) {
        int m2 = 2 * m;
        double aa = m * (b - m) * x / ((qam + m2) * (a + m2));
        d = 1.0 + aa * d;
        if (std::abs(d) < tiny) d = tiny;
        c = 1.0 + aa / c;
        if (std::abs(c) < tiny) c = tiny;
        d = 1.0 / d;
        h *= d * c;

        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2));
        d = 1.0 + aa * d;
        if (std::abs(d) < tiny) d = tiny;
        c = 1.0 + aa / c;
        if (std::abs(c) < tiny) c = tiny;
        d = 1.0 / d;
        double delta = d * c;
        h *= delta;

        if (std::abs(delta - 1.0) < epsilon) {
            break;
        }
    }

    double front = std::exp(log_gamma(a + b) - log_gamma(a) - log_gamma(b) +
                           a * std::log(x) + b * std::log(1.0 - x)) / a;
    double result = front * h;

    return flip ? 1.0 - result : result;
}

// Student's t-distribution CDF
double StatisticalDistributions::t_cdf(double x, double df) {
    if (df <= 0.0) {
        throw std::invalid_argument("Degrees of freedom must be positive");
    }

    double a = 0.5 * df;
    double b = 0.5;
    double t = df / (df + x * x);

    double p = 0.5 * incomplete_beta(t, a, b);

    return x >= 0.0 ? 1.0 - p : p;
}

// Student's t-distribution quantile using Newton-Raphson
double StatisticalDistributions::t_quantile(double p, double df) {
    if (p <= 0.0 || p >= 1.0) {
        throw std::invalid_argument("Probability must be in (0, 1)");
    }
    if (df <= 0.0) {
        throw std::invalid_argument("Degrees of freedom must be positive");
    }

    // For large df, use normal approximation
    if (df > 100) {
        return normal_quantile(p);
    }

    // Initial guess using normal approximation
    double x = normal_quantile(p);

    // Newton-Raphson refinement
    const int max_iter = 20;
    const double epsilon = 1e-10;

    for (int i = 0; i < max_iter; ++i) {
        double cdf_val = t_cdf(x, df);
        double error = cdf_val - p;

        if (std::abs(error) < epsilon) {
            break;
        }

        // PDF of t-distribution
        double pdf_val = std::exp(log_gamma((df + 1) / 2) - log_gamma(df / 2)) /
                        std::sqrt(df * PI) * std::pow(1 + x * x / df, -(df + 1) / 2);

        x -= error / pdf_val;
    }

    return x;
}

// Chi-square distribution CDF
double StatisticalDistributions::chi_square_cdf(double x, double df) {
    if (x < 0.0) {
        return 0.0;
    }
    if (df <= 0.0) {
        throw std::invalid_argument("Degrees of freedom must be positive");
    }

    return incomplete_gamma(0.5 * x, 0.5 * df);
}

// Chi-square distribution quantile using Newton-Raphson
double StatisticalDistributions::chi_square_quantile(double p, double df) {
    if (p <= 0.0 || p >= 1.0) {
        throw std::invalid_argument("Probability must be in (0, 1)");
    }
    if (df <= 0.0) {
        throw std::invalid_argument("Degrees of freedom must be positive");
    }

    // Initial guess
    double x = df * std::pow(1.0 - 2.0 / (9.0 * df) + normal_quantile(p) *
               std::sqrt(2.0 / (9.0 * df)), 3.0);
    x = std::max(0.001, x);

    // Newton-Raphson refinement
    const int max_iter = 20;
    const double epsilon = 1e-10;

    for (int i = 0; i < max_iter; ++i) {
        double cdf_val = chi_square_cdf(x, df);
        double error = cdf_val - p;

        if (std::abs(error) < epsilon) {
            break;
        }

        // PDF of chi-square distribution
        double pdf_val = std::exp((df / 2 - 1) * std::log(x) - x / 2 -
                         log_gamma(df / 2) - (df / 2) * std::log(2.0));

        x -= error / pdf_val;
        x = std::max(0.001, x);
    }

    return x;
}

} // namespace analysis
} // namespace trade_ngin
