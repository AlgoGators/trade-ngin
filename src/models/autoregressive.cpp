#include "autoregressive.h"
#include <cstdio>
#include <cmath>
#include <iostream>

using std::vector;

matrix operator*(const matrix& a, const matrix& b) {
    if (a.empty() || b.empty() || a[0].size() != b.size()) {
        throw std::invalid_argument("Incompatible matrix dimensions for multiplication.");
    }

    size_t a_num_rows = a.size();
    size_t b_num_columns = b[0].size();
    size_t shared_dim = b.size();
    matrix result(a_num_rows, std::vector<double>(b_num_columns, 0.0));
    for (size_t i {0}; i < a_num_rows; ++i) {
        const auto& a_row = a[i];
        for (size_t j {0}; j < b_num_columns; ++j) {
            double sum = 0.0;
            for (size_t k {0}; k < shared_dim; ++k) {
                sum += a_row[k] * b[k][j];
            }
            result[i][j] = sum;
        }
    }
    return result;
}

matrix transpose(const matrix& mat) {
    if (mat.empty()) {
        return {};
    }
    size_t num_rows = mat.size();
    size_t num_cols = mat[0].size();
    matrix transposed(num_cols, std::vector<double>(num_rows, 0.0));
    for (size_t i {0}; i < num_rows; ++i) {
        for (size_t j {0}; j < num_cols; ++j) {
            transposed[j][i] = mat[i][j];
        }
    }
    return transposed;
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

    // Only want to work with vectors
    if (data[0].size() != 1) {
        throw std::invalid_argument("Input data must be a vector (matrix with one column).");
    }

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

    double prediction = omega;
    for (size_t i {0}; i < coefficients.size(); ++i) {
        double noise = sqrt(var) * 2.0 * ((double)rand() / RAND_MAX - 0.5);
        prediction += coefficients[i] * data[data.size() - 1 - i][0] + noise;
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


/*************************************

 Test functions for matrix operations

*************************************/


void test_transpose(const matrix& mat) {
    std::printf("Original matrix:\n");
    for (const auto& row : mat) {
        for (const auto& val : row) {
            std::printf("%f ", val);
        }
        std::printf("\n");
    }

    matrix transposed {transpose(mat)};
    std::printf("Transposed matrix:\n");
    for (const auto& row : transposed) {
        for (const auto& val : row) {
            std::printf("%f ", val);
        }
        std::printf("\n");
    }
}

void test_matrix_multiplication(const matrix& a, const matrix& b) {
    std::printf("Matrix A:\n");
    for (const auto& row : a) {
        for (const auto& val : row) {
            std::printf("%f ", val);
        }
        std::printf("\n");
    }

    std::printf("Matrix B:\n");
    for (const auto& row : b) {
        for (const auto& val : row) {
            std::printf("%f ", val);
        }
        std::printf("\n");
    }

    try {
        matrix result = a * b;
        std::printf("Result of A * B:\n");
        for (const auto& row : result) {
            for (const auto& val : row) {
                std::printf("%f ", val);
            }
            std::printf("\n");
        }
    } catch (const std::invalid_argument& e) {
        std::printf("Error: %s\n", e.what());
    }
}

void test_autoregression_model() {
    matrix data 
    {
        {1.0}, {2.1}, {2.9}, {4.2}, {4.8}, {6.3}, {6.9}, {8.1}
    };
    std::cout << data.size() << " " << data[0].size() << std::endl;
    AutoRegressiveModel ar_model(2);
    ar_model.fit_model(data);
    std::vector<double> coefficients = ar_model.get_coefficients();
    std::printf("Fitted coefficients:\n");
    for (const auto& coeff : coefficients) {
        std::printf("%f ", coeff);
    }
    std::printf("\n");

    // predict_next expects exactly `lag` (2) most recent points
    matrix window {{6.9}, {8.1}};
    double prediction = ar_model.predict_next(window);
    std::printf("Predicted next value: %f\n", prediction);
}

int main() {
    /*
    matrix mat = 
    {
        {1, 2, 3}, 
        {4, 5, 6}
    };
    test_transpose(mat);

    matrix a {
        {1, 2},
        {3, 4}
    };
    matrix b {
        {5, 6},
        {7, 8}
    };
    test_matrix_multiplication(a, b);
    */

    std::cout << "Testing AutoRegressiveModel" << std::endl;
    test_autoregression_model();
    return 0;
}