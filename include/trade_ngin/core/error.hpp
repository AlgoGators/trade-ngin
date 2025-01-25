// include/trade_ngin/core/error.hpp

#pragma once

#include <string>
#include <exception>
#include <memory>
#include <system_error>
#include <unordered_map>

namespace trade_ngin {

/**
 * @brief Custom error codes for the trading system
 * Defines all possible error conditions that can occur
 */
enum class ErrorCode {
    // General errors
    NONE = 0,
    UNKNOWN_ERROR,
    INVALID_ARGUMENT,
    NOT_INITIALIZED,
    
    // Data errors
    DATABASE_ERROR,
    DATA_NOT_FOUND,
    INVALID_DATA,
    CONVERSION_ERROR,
    
    // Trading errors
    ORDER_REJECTED,
    INSUFFICIENT_FUNDS,
    POSITION_LIMIT_EXCEEDED,
    INVALID_ORDER,
    
    // Strategy errors
    STRATEGY_ERROR,
    INVALID_SIGNAL,
    
    // Risk errors
    RISK_LIMIT_EXCEEDED,
    INVALID_RISK_CALCULATION,
    
    // System errors
    CONNECTION_ERROR,
    TIMEOUT_ERROR,
    API_ERROR,
    
    // Custom error range
    CUSTOM_ERROR_START = 1000
};

/**
 * @brief Custom error category for trade_ngin errors
 */
class TradeError : public std::runtime_error {
public:
    /**
     * @brief Constructor for TradeError
     * @param code The error code
     * @param message Detailed error message
     * @param component Component where error occurred
     */
    TradeError(ErrorCode code, 
               const std::string& message,
               const std::string& component = "")
        : std::runtime_error(message),
          code_(code),
          component_(component) {}

    /**
     * @brief Get the error code
     * @return ErrorCode representing the type of error
     */
    ErrorCode code() const noexcept { return code_; }

    /**
     * @brief Get the component where error occurred
     * @return String identifying the component
     */
    const std::string& component() const noexcept { return component_; }

    /**
     * @brief Convert error to string representation
     * @return Formatted error string
     */
    std::string to_string() const {
        return "Error in " + component_ + ": " + what() + 
               " (Code: " + std::to_string(static_cast<int>(code_)) + ")";
    }

private:
    ErrorCode code_;
    std::string component_;
};

/**
 * @brief Result type for operations that can fail
 * @tparam T The type of the successful result
 */
template<typename T>
class Result {
public:
    /**
     * @brief Constructor for success case
     * @param value The successful result value
     */
    Result(const T& value) : value_(value), error_(nullptr) {}

    /**
     * @brief Constructor for error case
     * @param error The error that occurred
     */
    Result(std::unique_ptr<TradeError> error) 
        : error_(std::move(error)) {}

    /**
     * @brief Check if result represents success
     * @return true if operation was successful
     */
    bool is_ok() const { return error_ == nullptr; }

    /**
     * @brief Check if result represents error
     * @return true if operation failed
     */
    bool is_error() const { return error_ != nullptr; }

    /**
     * @brief Get the success value
     * @return Reference to the contained value
     * @throws TradeError if result represents an error
     */
    const T& value() const {
        if (error_) throw *error_;
        return value_;
    }

    /**
     * @brief Get the error if present
     * @return Pointer to the error, or nullptr if success
     */
    const TradeError* error() const { return error_.get(); }

private:
    T value_;
    std::unique_ptr<TradeError> error_;
};

// Specialization for void
template<>
class Result<void> {
public:
    Result() : error_(nullptr) {}
    Result(std::unique_ptr<TradeError> error) 
        : error_(std::move(error)) {}

    bool is_ok() const { return error_ == nullptr; }
    bool is_error() const { return error_ != nullptr; }

    void value() const {
        if (error_) throw *error_;
    }

    const TradeError* error() const { return error_.get(); }

private:
    std::unique_ptr<TradeError> error_;
};

/**
 * @brief Helper for creating error results
 * @tparam T The type of the successful result
 * @param code The error code
 * @param message The error message
 * @param component The component where error occurred
 * @return Result representing the error
 */
template<typename T>
Result<T> make_error(ErrorCode code, 
                    const std::string& message, 
                    const std::string& component = "") {
    return Result<T>(std::make_unique<TradeError>(code, message, component));
}

} // namespace trade_ngin