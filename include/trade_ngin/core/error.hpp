// include/trade_ngin/core/error.hpp

#pragma once

#include <exception>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>

namespace trade_ngin {

/**
 * @brief Custom error codes for the trading system
 * Defines all possible error conditions that can occur
 */
enum class ErrorCode {
    NONE = 0,
    UNKNOWN_ERROR = 1,
    INVALID_ARGUMENT = 2,
    NOT_INITIALIZED = 3,

    // Data errors
    DATABASE_ERROR = 4,
    DATA_NOT_FOUND = 5,
    INVALID_DATA = 6,
    CONVERSION_ERROR = 7,

    // Trading errors
    ORDER_REJECTED = 8,
    INSUFFICIENT_FUNDS = 9,
    POSITION_LIMIT_EXCEEDED = 10,
    INVALID_ORDER = 11,

    // Strategy errors
    STRATEGY_ERROR = 12,
    INVALID_SIGNAL = 13,

    // Risk errors
    RISK_LIMIT_EXCEEDED = 14,
    INVALID_RISK_CALCULATION = 15,

    // System errors
    CONNECTION_ERROR = 16,
    TIMEOUT_ERROR = 17,
    API_ERROR = 18,

    // Market data errors
    MARKET_DATA_ERROR = 19,

    // File and I/O errors
    FILE_NOT_FOUND = 20,
    FILE_IO_ERROR = 21,
    PERMISSION_ERROR = 22,

    // JSON and parsing errors
    JSON_PARSE_ERROR = 23,

    // Security and encryption errors
    ENCRYPTION_ERROR = 24,
    DECRYPTION_ERROR = 25,

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
    TradeError(ErrorCode code, const std::string& message, const std::string& component = "")
        : std::runtime_error(message), code_(code), component_(component) {}

    /**
     * @brief Get the error code
     * @return ErrorCode representing the type of error
     */
    ErrorCode code() const noexcept {
        return code_;
    }

    /**
     * @brief Get the component where error occurred
     * @return String identifying the component
     */
    const std::string& component() const noexcept {
        return component_;
    }

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
template <typename T>
class Result {
public:
    /**
     * @brief Constructor for success case
     * @param value The successful result
     * @tparam U The type of the successful result
     */
    template <typename U = T>
    Result(U&& value) : value_(std::forward<U>(value)), error_(nullptr) {}

    /**
     * @brief Constructor for error case
     * @param error The error that occurred
     */
    Result(std::unique_ptr<TradeError> error) : error_(std::move(error)) {}

    /**
     * @brief Move constructor
     * @param other The Result to move
     */
    Result(Result&& other) noexcept
        : value_(std::move(other.value_)), error_(std::move(other.error_)) {}

    /**
     * @brief Move assignment operator
     * @param other The Result to move
     * @return Reference to this Result
     */
    Result& operator=(Result&& other) noexcept {
        if (this != &other) {
            value_ = std::move(other.value_);
            error_ = std::move(other.error_);
        }
        return *this;
    }

    /**
     * @brief Copy constructor (deleted)
     */
    Result(const Result&) = delete;

    /**
     * @brief Copy assignment operator (deleted)
     */
    Result& operator=(const Result&) = delete;

    /**
     * @brief Check if result represents success
     * @return true if operation was successful
     */
    bool is_ok() const {
        return error_ == nullptr;
    }

    /**
     * @brief Check if result represents error
     * @return true if operation failed
     */
    bool is_error() const {
        return error_ != nullptr;
    }

    /**
     * @brief Get the success value
     * @return Reference to the contained value
     * @throws TradeError if result represents an error
     */
    const T& value() const {
        if (error_)
            throw *error_;
        return value_;
    }

    /**
     * @brief Get the error if present
     * @return Pointer to the error, or nullptr if success
     */
    const TradeError* error() const {
        return error_.get();
    }

private:
    T value_;
    std::unique_ptr<TradeError> error_;
};

// Specialization for void
template <>
class Result<void> {
public:
    Result() : error_(nullptr) {}
    Result(std::unique_ptr<TradeError> error) : error_(std::move(error)) {}

    bool is_ok() const {
        return error_ == nullptr;
    }
    bool is_error() const {
        return error_ != nullptr;
    }

    void value() const {
        if (error_)
            throw *error_;
    }

    const TradeError* error() const {
        return error_.get();
    }

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
template <typename T>
Result<T> make_error(ErrorCode code, const std::string& message,
                     const std::string& component = "") {
    return Result<T>(std::make_unique<TradeError>(code, message, component));
}

}  // namespace trade_ngin
