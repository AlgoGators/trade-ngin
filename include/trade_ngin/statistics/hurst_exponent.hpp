#pragma once

#include "trade_ngin/statistics/statistics_common.hpp"
#include "trade_ngin/core/error.hpp"
#include <vector>
#include <mutex>

namespace trade_ngin {
namespace statistics {

class HurstExponent {
public:
    explicit HurstExponent(HurstExponentConfig config = {});

    Result<HurstResult> compute(const std::vector<double>& data);

private:
    HurstExponentConfig config_;
    mutable std::mutex mutex_;

    HurstResult rs_analysis(const std::vector<double>& data) const;
    HurstResult dfa(const std::vector<double>& data) const;
    HurstResult periodogram(const std::vector<double>& data) const;

    // Log-log OLS regression: returns slope and R²
    static std::pair<double, double> log_log_ols(const std::vector<double>& log_x,
                                                  const std::vector<double>& log_y);
    static std::string interpret(double h);
};

} // namespace statistics
} // namespace trade_ngin
