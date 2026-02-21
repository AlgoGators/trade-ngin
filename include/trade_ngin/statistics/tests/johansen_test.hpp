#pragma once

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/statistics/statistics_common.hpp"
#include <Eigen/Dense>

namespace trade_ngin {
namespace statistics {

/**
 * @brief Johansen cointegration test
 */
class JohansenTest {
public:
    explicit JohansenTest(JohansenTestConfig config);

    /**
     * @brief Test for cointegration among multiple time series
     * @param data Matrix where each column is a time series
     * @return Cointegration test result
     */
    Result<CointegrationResult> test(const Eigen::MatrixXd& data) const;

private:
    JohansenTestConfig config_;

    std::vector<double> get_critical_values(int n_series, int rank) const;
};

} // namespace statistics
} // namespace trade_ngin
