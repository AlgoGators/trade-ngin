//include/trade_ngin/data/conversion_utils.hpp
#pragma once

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/core/error.hpp"
#include <arrow/api.h>
#include <arrow/type_traits.h>
#include <memory>
#include <vector>

namespace trade_ngin {

class DataConversionUtils {
public:
    /**
     * @brief Convert Arrow Table to vector of Bars
     * @param table Arrow table containing OHLCV data
     * @return Result containing vector of Bars
     */
    static Result<std::vector<Bar>> arrow_table_to_bars(
        const std::shared_ptr<arrow::Table>& table);

private:
    /**
     * @brief Extract timestamp from Arrow array
     * @param array Arrow array containing timestamps
     * @param index Row index
     * @return Result containing timestamp
     */
    static Result<Timestamp> extract_timestamp(
        const std::shared_ptr<arrow::Array>& array,
        int64_t index);

    /**
     * @brief Extract double value from Arrow array
     * @param array Arrow array containing doubles
     * @param index Row index
     * @return Result containing double value
     */
    static Result<double> extract_double(
        const std::shared_ptr<arrow::Array>& array,
        int64_t index);

    /**
     * @brief Extract string value from Arrow array
     * @param array Arrow array containing strings
     * @param index Row index
     * @return Result containing string value
     */
    static Result<std::string> extract_string(
        const std::shared_ptr<arrow::Array>& array,
        int64_t index);
};

} // namespace trade_ngin