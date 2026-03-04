#pragma once
#include <vector>
#include <stdexcept>
using matrix = std::vector<std::vector<double>>;

class AutoRegressiveModel {
    private:
        double omega;
        std::vector<double> coefficients;
        double var;

        int lag;

    public:
        AutoRegressiveModel(int lag) 
        : lag(lag) {};

        // Fit the AR coefficients and variance to the data
        void fit_model(const matrix& data);
        void fit_model(const std::vector<double>& data);

        // predict next day price based on the fitted model
        double predict_next(const matrix& data);
        double predict_next(const std::vector<double>& data);

        std::vector<double> get_coefficients() const {
            return coefficients;
        }

};