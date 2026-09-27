#include "trade_ngin/optimization/annualized_sample_covariance.hpp"

namespace trade_ngin::detail {

std::vector<std::vector<double>> annualized_sample_covariance(
    const std::vector<std::vector<double>>& aligned_returns) {
    const size_t min_periods = aligned_returns.size();
    const size_t num_assets = aligned_returns.front().size();

    // Preserve PortfolioManager's original loop and reduction order exactly.
    std::vector<double> means(num_assets, 0.0);
    for (size_t i = 0; i < num_assets; ++i) {
        for (size_t t = 0; t < min_periods; ++t) {
            means[i] += aligned_returns[t][i];
        }
        means[i] /= min_periods;
    }

    std::vector<std::vector<double>> covariance(num_assets, std::vector<double>(num_assets, 0.0));
    double divisor = (min_periods > 1) ? (min_periods - 1) : 1.0;

    for (size_t i = 0; i < num_assets; ++i) {
        for (size_t j = 0; j < num_assets; ++j) {
            double cov_sum = 0.0;
            for (size_t t = 0; t < min_periods; ++t) {
                cov_sum += (aligned_returns[t][i] - means[i]) * (aligned_returns[t][j] - means[j]);
            }

            covariance[i][j] = cov_sum / divisor;
            covariance[i][j] *= 252.0;
        }
    }

    return covariance;
}

}  // namespace trade_ngin::detail
