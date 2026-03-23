#include "autoregressive.h"
#include "matrix_ops.h"
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <random>
#include <pqxx/pqxx>
#include <map>
#include <string>

using std::vector;

namespace {

double sample_gaussian_noise(double variance) {
    if (variance <= 0.0) {
        return 0.0;
    }

    static thread_local std::mt19937 generator(std::random_device{}());
    std::normal_distribution<double> distribution(0.0, std::sqrt(variance));
    return distribution(generator);
}

}


// Gaussian elimination to solve Ax = b for x, where A is a square matrix and b is a column vector
std::vector<double> gaussian_elimination(const matrix& A, const matrix& b) {
    size_t n = A.size();
    matrix augmented(n, std::vector<double>(n + 1, 0.0));
    for (size_t i {0}; i < n; ++i) {
        for (size_t j {0}; j < n; ++j) {
            augmented[i][j] = A[i][j];
        }
        augmented[i][n] = b[i][0];
    }

    for (size_t i {0}; i < n; ++i) {
        size_t max_row = i;
        for (size_t k {i + 1}; k < n; ++k) {
            if (std::abs(augmented[k][i]) > std::abs(augmented[max_row][i])) {
                max_row = k;
            }
        }
        std::swap(augmented[i], augmented[max_row]);

        for (size_t k {i + 1}; k < n; ++k) {
            double factor = augmented[k][i] / augmented[i][i];
            for (size_t j {i}; j <= n; ++j) {
                augmented[k][j] -= factor * augmented[i][j];
            }
        }
    }

    std::vector<double> x(n, 0.0);
    for (int i {static_cast<int>(n) - 1}; i >= 0; --i) {
        x[i] = augmented[i][n] / augmented[i][i];
        for (int k {i - 1}; k >= 0; --k) {
            augmented[k][n] -= augmented[k][i] * x[i];
        }
    }
    return x;
}

void AutoRegressiveModel::fit_model(const matrix& data) {

    if (data.empty()) {
        throw std::invalid_argument("Input data must not be empty.");
    }

    // Only want to work with vectors
    if (data[0].size() != 1) {
        throw std::invalid_argument("Input data must be a vector (matrix with one column).");
    }
    if (data.size() <= static_cast<size_t>(lag)) {
        throw std::invalid_argument("Input data must contain more rows than the configured lag.");
    }
    // print shape of data
    // std::cout << data.size() << " " << data[0].size() << std::endl;


    matrix y{data.begin() + lag, data.end()};
    matrix x{data.begin(), data.end() - lag};
    // add column of 1s to x for omega
    for (auto& row : x) {
        row.insert(row.begin(), 1.0);
    }
    matrix x_T = transpose(x);

    matrix numerator = x_T * y;
    matrix denominator = x_T * x;
    coefficients = gaussian_elimination(denominator, numerator);

    // set omega to first value of result and coefficients to the rest
    omega = coefficients[0];
    coefficients.erase(coefficients.begin());

    // approx error distribution
    double forecast {0.0};
    std::vector<double> residuals(y.size(), 0.0);
    for (size_t t {0}; t < y.size(); ++t) {
        forecast = omega;
        for (size_t j {0}; j < coefficients.size(); ++j) {
            forecast += coefficients[j] * x[t][j + 1];  // j+1 skips the leading 1.0 bias column
        }
        residuals[t] = y[t][0] - forecast;
    }

    var = 0.0;
    double N = residuals.size();
    for (double& residual : residuals) {
        var += (residual * residual);
    }
    var /= N;
}

void AutoRegressiveModel::fit_model(const vector<double>& data) {
    matrix mat_data(data.size(), vector<double>(1, 0.0));
    for (size_t i {0}; i < data.size(); ++i) {
        mat_data[i][0] = data[i];
    }
    fit_model(mat_data);
}

double AutoRegressiveModel::predict_next(const matrix& data) {
    
    if (data[0].size() != 1) {
        throw std::invalid_argument("Input data must be a vector (matrix with one column).");
    }
    if (data.size() != static_cast<size_t>(lag)) {
        throw std::invalid_argument("Input data should match length of lag window");
    }

    double prediction = omega + sample_gaussian_noise(var);
    for (size_t i {0}; i < coefficients.size(); ++i) {
        prediction += coefficients[i] * data[data.size() - 1 - i][0];
    }
    return prediction;
}

double AutoRegressiveModel::predict_next(const vector<double>& data) {
    matrix mat_data(data.size(), vector<double>(1, 0.0));
    for (size_t i {0}; i < data.size(); ++i) {
        mat_data[i][0] = data[i];
    }
    return predict_next(mat_data);
}

double AutoRegressiveModel::backtest(const matrix& data) {
    if (data.empty()) {
        throw std::invalid_argument("Input data must not be empty.");
    }
    if (data[0].size() != 1) {
        throw std::invalid_argument("Input data must be a vector (matrix with one column).");
    }
    if (data.size() <= static_cast<size_t>(lag)) {
        throw std::invalid_argument("Input data must contain more rows than the configured lag.");
    }

    double total_error = 0.0;
    for (size_t t {lag}; t < data.size(); ++t) {
        matrix window(data.begin() + t - lag, data.begin() + t);
        double prediction = predict_next(window);
        double actual = data[t][0];
        total_error += std::abs(prediction - actual);
    }
    return total_error / (data.size() - lag);
}


/*************************************

 Test functions for matrix operations

*************************************/



void test_autoregression_model(matrix data, matrix window, std::vector<double> eval_data, int lag) {
    std::cout << data.size() << " " << data[0].size() << std::endl;

    AutoRegressiveModel ar_model(lag);
    ar_model.fit_model(data);

    std::vector<double> coefficients = ar_model.get_coefficients();
    std::printf("Fitted coefficients:\n");
    for (const auto& coeff : coefficients) {
        std::printf("%f ", coeff);
    }
    std::printf("\n");

    std::printf("Window for prediction:\n");
    for (const auto& row : window) {
        std::printf("%f ", row[0]);
    }
    std::printf("\n");

    double prediction = ar_model.predict_next(window);
    std::printf("Predicted next value: %f\n", prediction);
    std::printf("Actual next value: %f\n", eval_data[0]);
}

void historical_backtest_autoregression_model(const matrix& full_data, size_t lag, size_t min_train_size) {
    if (full_data.size() <= static_cast<size_t>(min_train_size)) {
        throw std::invalid_argument("Not enough data for historical backtest.");
    }

    if (min_train_size < lag) {
        throw std::invalid_argument("min_train_size must be at least as large as lag.");
    }

    double abs_error_sum = 0.0;
    size_t num_predictions = 0;

    // t is the index of the value we are trying to predict
    // train on [0, t), predict full_data[t]
    for (size_t t {min_train_size}; t < full_data.size(); ++t) {
        matrix train_data(full_data.begin(), full_data.begin() + t);
        matrix prediction_window(full_data.begin() + (t - lag), full_data.begin() + t);

        AutoRegressiveModel ar_model(lag);
        ar_model.fit_model(train_data);

        double prediction = ar_model.predict_next(prediction_window);
        double actual = full_data[t][0];
        double abs_error = std::abs(prediction - actual);

        abs_error_sum += abs_error;
        ++num_predictions;

        std::printf(
            "Step %zu | train_size=%zu | prediction=%f | actual=%f | abs_error=%f\n",
            t - min_train_size + 1,
            train_data.size(),
            prediction,
            actual,
            abs_error
        );
    }

    double mae = (num_predictions > 0) ? abs_error_sum / num_predictions : 0.0;
    std::printf("Historical backtest MAE over %zu forecasts: %f\n", num_predictions, mae);
}

int main() {
    std::map<std::string, std::string> database = {
        {"host", "13.58.153.216"},
        {"port", "5432"},
        {"user", "postgres"},
        {"password", "algogators"},
        {"dbname", "new_algo_data"}
    };

    try {
        pqxx::connection c(
            "dbname=" + database["dbname"] +
            " user=" + database["user"] +
            " password=" + database["password"] +
            " hostaddr=" + database["host"] +
            " port=" + database["port"]
        );

        pqxx::work txn(c);

        size_t lag = 5;


        size_t total_points = 5000;

        // minimum amount of data required before first forecast
        size_t min_train_size = lag * 3;

        std::vector<double> historical_data;

        pqxx::result r = txn.exec(
            "SELECT close "
            "FROM futures_data.new_data_ohlcv_1d "
            "WHERE symbol = 'NG' "
            "ORDER BY \"time\" ASC "
            "LIMIT " + std::to_string(total_points) + ";"
        );

        for (const pqxx::row &row : r) {
            double price = row["close"].as<double>();
            historical_data.push_back(price);
        }

        txn.commit();

        if (historical_data.size() <= static_cast<size_t>(min_train_size)) {
            throw std::invalid_argument("Not enough historical data for requested backtest.");
        }

        matrix historical_data_matrix = to_column_matrix(historical_data);
        historical_backtest_autoregression_model(historical_data_matrix, lag, min_train_size);

        return 0;
    }
    catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}