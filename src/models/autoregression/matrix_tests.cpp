#include <cstdio>
#include <cmath>
#include <iostream>
#include "autoregressive.h"
#include "matrix_ops.h"

using std::vector;  


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

int main() {
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
    return 0;
}