// EGARCH::fit must end at a likelihood maximum, not at the grid-search start.
// On simulated EGARCH series the fitted parameters must score at least as well
// as the true parameters that generated the data (an MLE can only do better on
// its own sample). log_likelihood is private, so it is reached through the
// `#define private public` pattern; every standard header the class pulls in is
// included first.

#include <gtest/gtest.h>
#include <array>
#include <cmath>
#include <mutex>
#include <random>
#include <string>
#include <vector>
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/statistics/base/volatility_model.hpp"
#include "trade_ngin/statistics/statistics_common.hpp"
#define private public
#include "trade_ngin/statistics/volatility/egarch.hpp"
#undef private

using namespace trade_ngin::statistics;

TEST(EGARCHFitQuality, FitReachesAtLeastTheTrueParametersLikelihood) {
    // Bound written before the run: on 20 series of 2,000 bars, every fit's
    // log-likelihood is no more than 0.5 below the true parameters'. Before the
    // optimizer fix, 11 of these 20 seeds stopped at the grid point
    // (beta = 0.85) 13 to 45 nats short.
    const double w = -0.2, a = 0.15, g = -0.10, b = 0.97;
    const double e_abs_z = std::sqrt(2.0 / M_PI);
    int short_fits = 0;
    for (unsigned seed = 1; seed <= 20; ++seed) {
        std::mt19937 gen(seed);
        std::normal_distribution<double> nd(0.0, 1.0);
        double log_h = w / (1.0 - b);
        std::vector<double> r(2000);
        for (auto& x : r) {
            const double z = nd(gen);
            x = std::sqrt(std::exp(log_h)) * z;
            log_h = w + a * (std::abs(z) - e_abs_z) + g * z + b * log_h;
        }
        EGARCH m(EGARCHConfig{});
        ASSERT_TRUE(m.fit(r).is_ok()) << "seed " << seed;
        const double ll_fit = m.log_likelihood(m.residuals_, m.omega_, m.alpha_, m.gamma_, m.beta_);
        const double ll_true = m.log_likelihood(m.residuals_, w, a, g, b);
        if (ll_fit < ll_true - 0.5) {
            ++short_fits;
            ADD_FAILURE() << "seed " << seed << ": fit ll " << ll_fit << " is " << (ll_true - ll_fit)
                          << " below the true parameters' (beta=" << m.beta_
                          << ", gamma=" << m.gamma_ << ")";
        }
    }
    EXPECT_EQ(short_fits, 0);
}
