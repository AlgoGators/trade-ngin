// include/trade_ngin/core/run_id_generator.hpp
// Utility for generating portfolio and strategy run IDs
#pragma once

#include "trade_ngin/core/types.hpp"
#include <string>
#include <vector>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <chrono>

namespace trade_ngin {

/**
 * @brief Utility class for generating run IDs for portfolio and individual strategies
 * 
 * Portfolio run IDs combine multiple strategy names: "TREND&MOMENTUM_20251217_195130_366"
 * Individual strategy run IDs: "TREND_FOLLOWING_20251217_195130_366"
 */
class RunIdGenerator {
public:
    /**
     * @brief Generate portfolio run ID (combined strategy names)
     * @param strategy_names Vector of strategy names (e.g., ["TREND_FOLLOWING", "MOMENTUM"])
     * @param timestamp Timestamp for the run
     * @return Combined run ID: "MOMENTUM&TREND_FOLLOWING_20251217_195130_366" (sorted)
     */
    static std::string generate_portfolio_run_id(
        const std::vector<std::string>& strategy_names,
        const Timestamp& timestamp
    );

    /**
     * @brief Generate portfolio run ID with explicit timestamp string
     * @param strategy_names Vector of strategy names
     * @param timestamp_str Timestamp string in format "YYYYMMDD_HHMMSS_MMM"
     * @return Combined run ID
     */
    static std::string generate_portfolio_run_id(
        const std::vector<std::string>& strategy_names,
        const std::string& timestamp_str
    );

    /**
     * @brief Generate individual strategy run ID (for metadata)
     * @param strategy_name Individual strategy name (e.g., "TREND_FOLLOWING")
     * @param timestamp Timestamp for the run
     * @return Individual run ID: "TREND_FOLLOWING_20251217_195130_366"
     */
    static std::string generate_strategy_run_id(
        const std::string& strategy_name,
        const Timestamp& timestamp
    );

    /**
     * @brief Generate individual strategy run ID with explicit timestamp string
     * @param strategy_name Individual strategy name
     * @param timestamp_str Timestamp string in format "YYYYMMDD_HHMMSS_MMM"
     * @return Individual run ID
     */
    static std::string generate_strategy_run_id(
        const std::string& strategy_name,
        const std::string& timestamp_str
    );

    /**
     * @brief Generate live portfolio run ID (combined strategy names + date + sequence)
     * @param strategy_names Vector of strategy names
     * @param date Date for the run
     * @param sequence Sequence number (default: 1)
     * @return Combined run ID: "MOMENTUM&TREND_FOLLOWING_20251217_001" (sorted)
     */
    static std::string generate_live_portfolio_run_id(
        const std::vector<std::string>& strategy_names,
        const Timestamp& date,
        int sequence = 1
    );

    /**
     * @brief Generate timestamp string from Timestamp
     * @param timestamp Timestamp to convert
     * @return Timestamp string: "YYYYMMDD_HHMMSS_MMM"
     */
    static std::string generate_timestamp_string(const Timestamp& timestamp);

    /**
     * @brief Generate date string from Timestamp
     * @param date Timestamp to convert
     * @return Date string: "YYYYMMDD"
     */
    static std::string generate_date_string(const Timestamp& date);

private:
    /**
     * @brief Combine strategy names with '&' separator (sorted for consistency)
     * @param strategy_names Vector of strategy names
     * @return Combined string: "MOMENTUM&TREND_FOLLOWING" (sorted)
     */
    static std::string combine_strategy_names(const std::vector<std::string>& strategy_names);
};

} // namespace trade_ngin

