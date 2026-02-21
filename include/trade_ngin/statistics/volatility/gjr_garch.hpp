#pragma once

#include "trade_ngin/statistics/base/volatility_model.hpp"
#include "trade_ngin/statistics/statistics_common.hpp"
#include <mutex>

namespace trade_ngin {
namespace statistics {

class GJRGARCH : public VolatilityModel {
public:
    explicit GJRGARCH(GJRGARCHConfig config);

    Result<void> fit(const std::vector<double>& returns) override;
    Result<std::vector<double>> forecast(int n_periods = 1) const override;
    Result<double> get_current_volatility() const override;
    Result<void> update(double new_return) override;
    bool is_fitted() const override { return fitted_; }

    Result<ConvergenceInfo> fit_with_diagnostics(const std::vector<double>& returns);
    const ConvergenceInfo& get_convergence_info() const { return last_convergence_info_; }

    double get_omega() const { return omega_; }
    double get_alpha() const { return alpha_; }
    double get_gamma() const { return gamma_; }
    double get_beta() const { return beta_; }

private:
    GJRGARCHConfig config_;
    double omega_;
    double alpha_;
    double gamma_;
    double beta_;
    std::vector<double> residuals_;
    std::vector<double> conditional_variances_;
    double current_volatility_{0.0};
    bool fitted_{false};
    ConvergenceInfo last_convergence_info_;
    mutable std::mutex mutex_;

    Result<void> estimate_parameters(const std::vector<double>& returns);
    double log_likelihood(const std::vector<double>& returns,
                         double omega, double alpha, double gamma, double beta) const;
};

} // namespace statistics
} // namespace trade_ngin
