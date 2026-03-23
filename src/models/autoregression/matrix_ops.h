#pragma once
#include <vector>
#include <stdexcept>
#include <iostream>

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
        std::cerr << "Warning: Attempting to transpose an empty matrix." << std::endl;
        std::abort();
        return matrix();
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

matrix to_column_matrix(const std::vector<double>& data) {
    matrix m(data.size(), std::vector<double>(1, 0.0));
    for (size_t i = 0; i < data.size(); ++i) {
        m[i][0] = data[i];
    }
    return m;
}