#pragma once

#include <vector>

namespace trade_ngin::detail {

// Preconditions: rectangular nonempty matrix; rows >= 20; each row is one
// aligned daily observation and columns are in caller order.
std::vector<std::vector<double>> annualized_sample_covariance(
    const std::vector<std::vector<double>>& aligned_returns);

}  // namespace trade_ngin::detail
