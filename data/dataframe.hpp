#pragma once
#include <unordered_map>
#include <string>
#include <vector>
#include <memory>
#include <cmath>
#include <limits>
#include <arrow/api.h>
#include <arrow/table.h>

/**
 * @class DataFrame
 * @brief A simple container that holds in-memory columnar data as double vectors and
 *        can import from an Apache Arrow Table.
 */
class DataFrame {
public:
    /**
     * @brief Default constructor
     */
    DataFrame() = default;

    /**
     * @brief Construct from a map of column names to value vectors
     */
    DataFrame(const std::unordered_map<std::string, std::vector<double>>& data) {
        for (const auto& [name, values] : data) {
            add_column(name, values);
        }
    }

    /**
     * @brief Returns true if the DataFrame has zero rows.
     */
    bool empty() const {
        return (rows_ == 0);
    }

    /**
     * @brief Add a column to the DataFrame.
     * @param name The name of the column.
     * @param values The vector of values for the column.
     */
    void add_column(const std::string& name, const std::vector<double>& values) {
        if (rows_ == 0) {
            rows_ = values.size();
        } else if (values.size() != rows_) {
            throw std::runtime_error("Column size mismatch");
        }
        data_[name] = values;
    }

    /**
     * @brief Get a copy of the specified column by name as a vector of double.
     * @param name The column name.
     * @return A vector of double containing the column's data (NaN if missing).
     */
    std::vector<double> get_column(const std::string &name) const {
        auto it = data_.find(name);
        if (it == data_.end()) {
            return {};
        }
        return it->second;
    }

    /**
     * @brief Number of rows in the DataFrame.
     */
    size_t rows() const {
        return rows_;
    }

    /**
     * @brief Return a list of all column names stored in the DataFrame.
     */
    std::vector<std::string> columns() const {
        std::vector<std::string> cols;
        cols.reserve(data_.size());
        for (const auto &kv : data_) {
            cols.push_back(kv.first);
        }
        return cols;
    }

private:
    std::unordered_map<std::string, std::vector<double>> data_;
    size_t rows_ = 0;
}; 