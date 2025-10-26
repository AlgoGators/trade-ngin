#pragma once

#include <string>
#include <sstream>
#include <iomanip>

namespace trade_ngin {

/**
 * @brief Helper class for building safe SQL queries
 *
 * Provides consistent query building with proper escaping
 * to prevent SQL injection while maintaining readability.
 */
class QueryBuilder {
public:
    /**
     * @brief Escape a string value for SQL
     * @param value The string to escape
     * @return Escaped and quoted string
     */
    static std::string escape_string(const std::string& value) {
        std::string result = "'";
        for (char c : value) {
            if (c == '\'') {
                result += "''";  // Escape single quotes
            } else if (c == '\\') {
                result += "\\\\";  // Escape backslashes
            } else {
                result += c;
            }
        }
        result += "'";
        return result;
    }

    /**
     * @brief Format a numeric value for SQL
     * @param value The numeric value
     * @return String representation of the number
     */
    template<typename T>
    static std::string format_number(T value) {
        return std::to_string(value);
    }

    /**
     * @brief Build a WHERE clause with strategy_id
     * @param strategy_id The strategy identifier
     * @return WHERE clause string
     */
    static std::string where_strategy(const std::string& strategy_id) {
        return "WHERE strategy_id = " + escape_string(strategy_id);
    }

    /**
     * @brief Build a WHERE clause with strategy_id and date
     * @param strategy_id The strategy identifier
     * @param date The date in YYYY-MM-DD format
     * @return WHERE clause string
     */
    static std::string where_strategy_date(const std::string& strategy_id,
                                          const std::string& date) {
        return "WHERE strategy_id = " + escape_string(strategy_id) +
               " AND date = " + escape_string(date);
    }

    /**
     * @brief Build a WHERE clause for date range
     * @param strategy_id The strategy identifier
     * @param start_date Start date
     * @param end_date End date
     * @return WHERE clause string
     */
    static std::string where_strategy_date_range(const std::string& strategy_id,
                                                const std::string& start_date,
                                                const std::string& end_date) {
        return "WHERE strategy_id = " + escape_string(strategy_id) +
               " AND date >= " + escape_string(start_date) +
               " AND date <= " + escape_string(end_date);
    }
};

}  // namespace trade_ngin