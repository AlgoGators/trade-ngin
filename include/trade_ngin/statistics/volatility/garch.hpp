#pragma once

#include "trade_ngin/statistics/base/volatility_model.hpp"
#include "trade_ngin/statistics/statistics_common.hpp"
#include <mutex>

namespace trade_ngin {
namespace statistics {

/**
 * @brief GARCH(p,q) volatility model
 */
class GARCH : public VolatilityModel {
public:
    explicit GARCH(GARCHConfig config);

    Result<void> fit(const std::vector<double>& returns) override;
    Result<std::vector<double>> forecast(int n_periods = 1) const override;
    Result<double> get_current_volatility() const override;
    Result<void> update(double new_return) override;
    bool is_fitted() const override { return fitted_; }

    double get_omega() const { return omega_; }
    double get_alpha() const { return alpha_; }
    double get_beta() const { return beta_; }

private:
    GARCHConfig config_;
    double omega_;      // Constant term
    double alpha_;      // ARCH coefficient
    double beta_;       // GARCH coefficient
    std::vector<double> residuals_;
    std::vector<double> conditional_variances_;
    double current_volatility_{0.0};
    bool fitted_{false};
    mutable std::mutex mutex_;

    // Estimate parameters using maximum likelihood
    Result<void> estimate_parameters(const std::vector<double>& returns);
    double log_likelihood(const std::vector<double>& returns,
                         double omega, double alpha, double beta) const;
};

} // namespace statistics
} // namespace trade_ngin
