#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <stdexcept>
#include <algorithm>

// A minimal DataFrame class with basic operations
// basic impl until real data
class DataFrame {
public:
    DataFrame() = default;

    // Construct from a map of column_name -> data
    DataFrame(const std::unordered_map<std::string, std::vector<double>>& data) {
        for (const auto& kv : data) {
            columns_.push_back(kv.first);
            data_.push_back(kv.second);
            if (!data_.back().empty()) {
                if (rows_ == 0) rows_ = data_.back().size();
                else if (data_.back().size() != rows_)
                    throw std::runtime_error("All columns must have the same length");
            }
        }
    }

    bool empty() const { return columns_.empty() || rows_ == 0; }

    size_t rows() const { return rows_; }
    size_t cols() const { return columns_.size(); }

    const std::vector<std::string>& columns() const { return columns_; }

    // Returns a column by name
    std::vector<double> get_column(const std::string& name) const {
        auto it = std::find(columns_.begin(), columns_.end(), name);
        if (it == columns_.end()) throw std::runtime_error("Column not found: " + name);
        size_t idx = std::distance(columns_.begin(), it);
        return data_[idx];
    }

    // Add a new column
    void add_column(const std::string& name, const std::vector<double>& col) {
        if (!empty() && col.size() != rows_) {
            throw std::runtime_error("New column length doesn't match current DataFrame rows");
        }
        if (empty()) rows_ = col.size();
        columns_.push_back(name);
        data_.push_back(col);
    }

    // Join: adds columns from another df (outer join by index)
    // For simplicity, we assume both have the same number of rows.
    DataFrame join(const DataFrame& other) const {
        if (empty()) return other;
        if (other.empty()) return *this;
        if (rows_ != other.rows_) throw std::runtime_error("Row count mismatch in join");
        DataFrame df = *this;
        for (size_t i = 0; i < other.cols(); ++i) {
            auto col_name = other.columns_[i];
            // Check for duplicates
            if (std::find(df.columns_.begin(), df.columns_.end(), col_name) != df.columns_.end())
                throw std::runtime_error("Duplicate column name in join: " + col_name);
            df.columns_.push_back(col_name);
            df.data_.push_back(other.data_[i]);
        }
        return df;
    }

    // Element-wise addition
    DataFrame add(const DataFrame& other) const {
        if (rows_ != other.rows_) throw std::runtime_error("Row count mismatch in add");
        // Add only if columns match by name and in order
        if (cols() != other.cols()) throw std::runtime_error("Column count mismatch in add");
        DataFrame result;
        for (size_t i = 0; i < cols(); ++i) {
            if (columns_[i] != other.columns_[i]) 
                throw std::runtime_error("Column name mismatch in add");
            std::vector<double> col(rows_);
            for (size_t r = 0; r < rows_; ++r) {
                col[r] = data_[i][r] + other.data_[i][r];
            }
            result.add_column(columns_[i], col);
        }
        return result;
    }

    // Element-wise division by a single-row DataFrame
    DataFrame div_row(const DataFrame& row_df) const {
        if (row_df.rows_ != 1) throw std::runtime_error("div_row expects single row DataFrame");
        // Matches columns by name
        DataFrame result;
        for (size_t i = 0; i < cols(); ++i) {
            const auto& col_name = columns_[i];
            auto it = std::find(row_df.columns_.begin(), row_df.columns_.end(), col_name);
            if (it == row_df.columns_.end()) throw std::runtime_error("Column not found in div_row: " + col_name);
            size_t idx = std::distance(row_df.columns_.begin(), it);
            double divisor = row_df.data_[idx][0];
            if (divisor == 0.0) throw std::runtime_error("Division by zero in div_row");
            std::vector<double> col(rows_);
            for (size_t r = 0; r < rows_; ++r) {
                col[r] = data_[i][r] / divisor;
            }
            result.add_column(col_name, col);
        }
        return result;
    }

    // Element-wise multiplication with single-row DataFrame
    DataFrame mul_row(const DataFrame& row_df) const {
        if (row_df.rows_ != 1) throw std::runtime_error("mul_row expects single row DataFrame");
        DataFrame result;
        for (size_t i = 0; i < cols(); ++i) {
            const auto& col_name = columns_[i];
            auto it = std::find(row_df.columns_.begin(), row_df.columns_.end(), col_name);
            if (it == row_df.columns_.end()) throw std::runtime_error("Column not found in mul_row: " + col_name);
            size_t idx = std::distance(row_df.columns_.begin(), it);
            double factor = row_df.data_[idx][0];
            std::vector<double> col(rows_);
            for (size_t r = 0; r < rows_; ++r) {
                col[r] = data_[i][r] * factor;
            }
            result.add_column(col_name, col);
        }
        return result;
    }

private:
    std::vector<std::string> columns_;
    std::vector<std::vector<double>> data_;
    size_t rows_ = 0;
};

