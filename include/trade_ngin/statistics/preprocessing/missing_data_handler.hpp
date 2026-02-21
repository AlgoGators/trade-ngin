#pragma once

#include "trade_ngin/statistics/statistics_common.hpp"
#include <Eigen/Dense>
#include <vector>

namespace trade_ngin {
namespace statistics {

class MissingDataHandler {
public:
    explicit MissingDataHandler(MissingDataHandlerConfig config = {});

    Result<std::vector<double>> handle(const std::vector<double>& data) const;
    Result<Eigen::MatrixXd> handle(const Eigen::MatrixXd& data) const;

    static size_t count_missing(const std::vector<double>& data);
    static size_t count_missing(const Eigen::MatrixXd& data);

private:
    MissingDataHandlerConfig config_;

    Result<void> validate(const std::vector<double>& data) const;
    Result<void> validate(const Eigen::MatrixXd& data) const;
};

} // namespace statistics
} // namespace trade_ngin
