// src/core/run_id_generator.cpp
// Implementation of run ID generation utilities

#include "trade_ngin/core/run_id_generator.hpp"
#include <ctime>

namespace trade_ngin {

std::string RunIdGenerator::combine_strategy_names(const std::vector<std::string>& strategy_names) {
    if (strategy_names.empty()) {
        return "";
    }

    // Sort strategy names for consistency
    auto sorted = strategy_names;
    std::sort(sorted.begin(), sorted.end());

    // Combine with '&' separator
    std::string combined = sorted[0];
    for (size_t i = 1; i < sorted.size(); ++i) {
        combined += "&" + sorted[i];
    }

    return combined;
}

std::string RunIdGenerator::generate_timestamp_string(const Timestamp& timestamp) {
    auto time_t = std::chrono::system_clock::to_time_t(timestamp);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        timestamp.time_since_epoch()).count() % 1000;

    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time_t), "%Y%m%d_%H%M%S");
    ss << "_" << std::setfill('0') << std::setw(3) << ms;

    return ss.str();
}

std::string RunIdGenerator::generate_date_string(const Timestamp& date) {
    auto time_t = std::chrono::system_clock::to_time_t(date);
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time_t), "%Y%m%d");
    return ss.str();
}

std::string RunIdGenerator::generate_portfolio_run_id(
    const std::vector<std::string>& strategy_names,
    const Timestamp& timestamp
) {
    std::string combined = combine_strategy_names(strategy_names);
    std::string timestamp_str = generate_timestamp_string(timestamp);
    return combined + "_" + timestamp_str;
}

std::string RunIdGenerator::generate_portfolio_run_id(
    const std::vector<std::string>& strategy_names,
    const std::string& timestamp_str
) {
    std::string combined = combine_strategy_names(strategy_names);
    return combined + "_" + timestamp_str;
}

std::string RunIdGenerator::generate_strategy_run_id(
    const std::string& strategy_name,
    const Timestamp& timestamp
) {
    std::string timestamp_str = generate_timestamp_string(timestamp);
    return strategy_name + "_" + timestamp_str;
}

std::string RunIdGenerator::generate_strategy_run_id(
    const std::string& strategy_name,
    const std::string& timestamp_str
) {
    return strategy_name + "_" + timestamp_str;
}

std::string RunIdGenerator::generate_live_portfolio_run_id(
    const std::vector<std::string>& strategy_names,
    const Timestamp& date,
    int sequence
) {
    std::string combined = combine_strategy_names(strategy_names);
    std::string date_str = generate_date_string(date);
    
    std::stringstream ss;
    ss << combined << "_" << date_str << "_";
    ss << std::setfill('0') << std::setw(3) << sequence;
    
    return ss.str();
}

} // namespace trade_ngin

