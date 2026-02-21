#pragma once

#include "trade_ngin/statistics/statistics_common.hpp"
#include <vector>

namespace trade_ngin {
namespace statistics {

class OutlierHandler {
public:
    explicit OutlierHandler(OutlierHandlerConfig config = {});

    Result<std::vector<double>> handle(const std::vector<double>& data) const;
    Result<std::vector<size_t>> detect(const std::vector<double>& data) const;

private:
    OutlierHandlerConfig config_;

    Result<void> validate(const std::vector<double>& data) const;

    std::vector<double> winsorize(const std::vector<double>& data) const;
    std::vector<double> trim(const std::vector<double>& data, const std::vector<size_t>& outlier_indices) const;
    std::vector<double> mad_filter(const std::vector<double>& data, const std::vector<size_t>& outlier_indices) const;

    std::vector<size_t> detect_percentile(const std::vector<double>& data) const;
    std::vector<size_t> detect_mad(const std::vector<double>& data) const;

    static double percentile(const std::vector<double>& sorted_data, double p);
    static double median(const std::vector<double>& data);
};

} // namespace statistics
} // namespace trade_ngin
