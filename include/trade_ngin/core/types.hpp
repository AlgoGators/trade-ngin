// include/trade_ngin/core/types.hpp

#pragma once

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <istream>
#include <memory>
#include <optional>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace trade_ngin {

/**
 * @brief Fixed-point decimal type for financial calculations
 * Uses 8 decimal places to avoid floating-point precision issues
 */
class Decimal {
private:
    static constexpr int64_t SCALE = 100000000LL;  // 10^8 for 8 decimal places
    int64_t value_;

public:
    // Constructors
    constexpr Decimal() : value_(0) {}
    constexpr explicit Decimal(int64_t raw_value) : value_(raw_value) {}

    Decimal(double d) {
        if (std::isnan(d) || std::isinf(d)) {
            throw std::invalid_argument("Cannot create Decimal from NaN or infinity");
        }
        if (d > (static_cast<double>(INT64_MAX) / SCALE) ||
            d < (static_cast<double>(INT64_MIN) / SCALE)) {
            throw std::overflow_error("Double value too large for Decimal");
        }
        value_ = static_cast<int64_t>(d * SCALE + (d >= 0 ? 0.5 : -0.5));
    }

    Decimal(int i) : value_(static_cast<int64_t>(i) * SCALE) {}
    
    // Only define long constructor if long is different from int64_t
    template<typename T = long>
    Decimal(T l, typename std::enable_if<!std::is_same<T, int64_t>::value>::type* = nullptr) 
        : value_(static_cast<int64_t>(l) * SCALE) {}

    // Conversion operators
    explicit operator double() const {
        return static_cast<double>(value_) / SCALE;
    }

    explicit operator float() const {
        return static_cast<float>(value_) / SCALE;
    }

    // Arithmetic operators
    Decimal operator+(const Decimal& other) const {
        int64_t result = value_ + other.value_;
        // Check for overflow
        if ((value_ > 0 && other.value_ > 0 && result < 0) ||
            (value_ < 0 && other.value_ < 0 && result > 0)) {
            throw std::overflow_error("Decimal addition overflow");
        }
        return Decimal(result);
    }

    Decimal operator-(const Decimal& other) const {
        int64_t result = value_ - other.value_;
        // Check for overflow
        if ((value_ > 0 && other.value_ < 0 && result < 0) ||
            (value_ < 0 && other.value_ > 0 && result > 0)) {
            throw std::overflow_error("Decimal subtraction overflow");
        }
        return Decimal(result);
    }

    Decimal operator*(const Decimal& other) const {
        // Check for overflow using double precision
        double temp = static_cast<double>(value_) * static_cast<double>(other.value_);
        temp /= SCALE;
        if (temp > INT64_MAX || temp < INT64_MIN) {
            throw std::overflow_error("Decimal multiplication overflow");
        }
        return Decimal(static_cast<int64_t>(temp));
    }

    Decimal operator/(const Decimal& other) const {
        if (other.value_ == 0) {
            throw std::domain_error("Division by zero");
        }
        // Use double precision for division
        double temp = static_cast<double>(value_) * SCALE;
        temp /= other.value_;
        if (temp > INT64_MAX || temp < INT64_MIN) {
            throw std::overflow_error("Decimal division overflow");
        }
        return Decimal(static_cast<int64_t>(temp));
    }

    // Compound assignment operators
    Decimal& operator+=(const Decimal& other) {
        *this = *this + other;
        return *this;
    }

    Decimal& operator-=(const Decimal& other) {
        *this = *this - other;
        return *this;
    }

    Decimal& operator*=(const Decimal& other) {
        *this = *this * other;
        return *this;
    }

    Decimal& operator/=(const Decimal& other) {
        *this = *this / other;
        return *this;
    }

    // Comparison operators
    bool operator==(const Decimal& other) const {
        return value_ == other.value_;
    }
    bool operator!=(const Decimal& other) const {
        return value_ != other.value_;
    }
    bool operator<(const Decimal& other) const {
        return value_ < other.value_;
    }
    bool operator<=(const Decimal& other) const {
        return value_ <= other.value_;
    }
    bool operator>(const Decimal& other) const {
        return value_ > other.value_;
    }
    bool operator>=(const Decimal& other) const {
        return value_ >= other.value_;
    }

    // Unary operators
    Decimal operator-() const {
        if (value_ == INT64_MIN) {
            throw std::overflow_error("Decimal negation overflow");
        }
        return Decimal(-value_);
    }

    Decimal operator+() const {
        return *this;
    }

    // Utility functions
    Decimal abs() const {
        return value_ >= 0 ? *this : -*this;
    }

    bool is_zero() const {
        return value_ == 0;
    }
    bool is_positive() const {
        return value_ > 0;
    }
    bool is_negative() const {
        return value_ < 0;
    }

    // Raw value access (for serialization)
    int64_t raw_value() const {
        return value_;
    }
    static Decimal from_raw(int64_t raw) {
        return Decimal(raw);
    }

    // String conversion
    std::string to_string() const {
        int64_t integer_part = value_ / SCALE;
        int64_t fractional_part = std::abs(value_ % SCALE);

        std::string result = std::to_string(integer_part);
        if (fractional_part > 0) {
            std::string frac_str = std::to_string(fractional_part);
            // Pad with leading zeros
            while (frac_str.length() < 8) {
                frac_str = "0" + frac_str;
            }
            // Remove trailing zeros
            while (frac_str.back() == '0') {
                frac_str.pop_back();
            }
            if (!frac_str.empty()) {
                result += "." + frac_str;
            }
        }
        return result;
    }

    // Stream operators
    friend std::ostream& operator<<(std::ostream& os, const Decimal& d) {
        return os << d.to_string();
    }

    // Helper functions for common operations
    static Decimal from_double(double d) {
        return Decimal(d);
    }

    double to_double() const {
        return static_cast<double>(*this);
    }

    // Helper for metrics and other systems that expect double
    double as_double() const {
        return static_cast<double>(*this);
    }
};

/**
 * @brief Timestamp type for consistent time representation
 * Uses std::chrono for type-safe time handling
 */
using Timestamp = std::chrono::system_clock::time_point;

/**
 * @brief Price type using fixed-point decimal
 * Used for all price-related calculations
 */
using Price = Decimal;

/**
 * @brief Quantity type for order and position sizes
 * Using Decimal to support precise fractional quantities
 */
using Quantity = Decimal;

/**
 * @brief Trading side enumeration
 */
enum class Side {
    BUY,
    SELL,
    NONE  // Used for invalid/undefined states
};

/**
 * @brief Order type enumeration
 */
enum class OrderType { MARKET, LIMIT, STOP, STOP_LIMIT, NONE };

/**
 * @brief Time in force enumeration
 */
enum class TimeInForce {
    DAY,
    GTC,  // Good Till Cancel
    IOC,  // Immediate or Cancel
    FOK,  // Fill or Kill
    GTD,  // Good Till Date
    MOC,  // Market on Close
    MOO,  // Market on Open
    NONE
};

/**
 * @brief Asset type enumeration
 */
enum class AssetType { FUTURE, EQUITY, OPTION, FOREX, CRYPTO, NONE };

/**
 * @brief Market data bar structure
 * Represents OHLCV data for any timeframe
 */
struct Bar {
    Timestamp timestamp;
    Price open;
    Price high;
    Price low;
    Price close;
    double volume;  // Keep as double for now since volume is typically not a financial calculation
    std::string symbol;

    Bar() = default;
    Bar(Timestamp ts, Price o, Price h, Price l, Price c, double v, std::string s)
        : timestamp(ts), open(o), high(h), low(l), close(c), volume(v), symbol(std::move(s)) {}

    // Helper constructor that takes doubles for compatibility
    Bar(Timestamp ts, double o, double h, double l, double c, double v, std::string s)
        : timestamp(ts),
          open(Decimal(o)),
          high(Decimal(h)),
          low(Decimal(l)),
          close(Decimal(c)),
          volume(v),
          symbol(std::move(s)) {}
};

/**
 * @brief Contract specification structure
 * Holds all relevant information about a tradeable instrument
 */
struct ContractSpec {
    std::string symbol;
    AssetType type;
    std::string exchange;
    std::string currency;
    Decimal multiplier;
    Decimal tick_size;
    Decimal commission_per_contract;
    std::optional<Timestamp> expiry;
    std::optional<std::string> underlying;
};

/**
 * @brief Order structure
 * Represents a trading order with all necessary fields
 */
struct Order {
    std::string order_id;
    std::string symbol;
    Side side;
    OrderType type;
    Quantity quantity;
    Price price;
    TimeInForce time_in_force;
    Timestamp timestamp;
    std::string strategy_id;  // ID of the strategy that generated this order

    // Optional fields
    std::optional<Price> stop_price;
    std::optional<Timestamp> good_till_date;

    Order() = default;
    Order(std::string symbol, Side side, OrderType type, Quantity qty, Price price)
        : symbol(std::move(symbol)),
          side(side),
          type(type),
          quantity(qty),
          price(price),
          time_in_force(TimeInForce::DAY) {}
};

/**
 * @brief Position structure
 * Represents a current position in an instrument
 */
struct Position {
    std::string symbol;
    Quantity quantity;
    Price average_price;
    Decimal unrealized_pnl;
    Decimal realized_pnl;
    Timestamp last_update;
    // Note: previous_price and contract_size fields removed

    // Constructors
    Position(std::string sym, Quantity qty, Price avg_price, Decimal unreal_pnl, Decimal real_pnl,
             Timestamp ts, Decimal prev_price = Decimal(0.0), Decimal contract_sz = Decimal(1.0))
        : symbol(std::move(sym)),
          quantity(qty),
          average_price(avg_price),
          unrealized_pnl(unreal_pnl),
          realized_pnl(real_pnl),
          last_update(ts) {
        // prev_price and contract_sz parameters are ignored since fields were removed
        (void)prev_price;
        (void)contract_sz;
    }

    Position() : quantity(0), average_price(0), unrealized_pnl(0), realized_pnl(0) {}

    // Helper method to check if position exists
    bool has_position() const {
        return !quantity.is_zero();
    }

    // Helper method to get position side
    Side get_side() const {
        if (quantity.is_positive())
            return Side::BUY;
        if (quantity.is_negative())
            return Side::SELL;
        return Side::NONE;
    }
};

/**
 * @brief Execution report structure
 * Represents a fill or partial fill of an order
 *
 * Transaction cost breakdown (backtest only):
 * - commissions_fees: Explicit fees (|qty| × fee_per_contract)
 * - implicit_price_impact: Spread + market impact in price units per contract
 * - slippage_market_impact: Implicit costs in dollars
 * - total_transaction_costs: commissions_fees + slippage_market_impact
 */
struct ExecutionReport {
    std::string order_id;
    std::string exec_id;
    std::string symbol;
    Side side;
    Quantity filled_quantity;
    Price fill_price;  // Reference fill price (no costs embedded)
    Timestamp fill_time;

    // Transaction cost breakdown
    Decimal commissions_fees;         // Explicit: |qty| × fee_per_contract
    Decimal implicit_price_impact;    // Spread + impact in price units
    Decimal slippage_market_impact;   // Implicit costs in dollars
    Decimal total_transaction_costs;  // commissions_fees + slippage_market_impact

    bool is_partial;
};

/**
 * @brief Market state enumeration for regime detection
 */
enum class MarketRegime { TRENDING_UP, TRENDING_DOWN, MEAN_REVERTING, VOLATILE, UNDEFINED };

/**
 * @brief Risk limits structure
 * Holds all risk-related limits for a strategy or portfolio
 */
struct RiskLimits {
    Decimal max_position_size;
    Decimal max_notional_value;
    Decimal max_drawdown;
    Decimal max_leverage;
    Decimal var_limit;
    Decimal max_correlation;
};

/**
 * @brief Asset class enumeration
 * Represents different types of financial instruments
 */
enum class AssetClass {
    FUTURES,
    EQUITIES,
    OPTIONS,
    FIXED_INCOME,
    CURRENCIES,
    COMMODITIES,
    CRYPTO,
    UNKNOWN
};

/**
 * @brief Data frequency enumeration
 * Represents different timeframes for market data
 */
enum class DataFrequency {
    DAILY,      // 1d
    HOURLY,     // 1h
    MINUTE_15,  // 15m
    MINUTE_5,   // 5m
    MINUTE_1    // 1m
};

/**
 * @brief Convert DataFrequency to table suffix
 * @param freq The data frequency
 * @return String representation for table name
 */
inline std::string get_table_suffix(DataFrequency freq) {
    switch (freq) {
        case DataFrequency::DAILY:
            return "1d";
        case DataFrequency::HOURLY:
            return "1h";
        case DataFrequency::MINUTE_15:
            return "15m";
        case DataFrequency::MINUTE_5:
            return "5m";
        case DataFrequency::MINUTE_1:
            return "1m";
        default:
            return "1d";
    }
}

/**
 * @brief Convert AssetClass to schema name
 * @param asset_class The asset class
 * @return String representation of schema name
 */
inline std::string get_schema_name(AssetClass asset_class) {
    switch (asset_class) {
        case AssetClass::FUTURES:
            return "futures_data";
        case AssetClass::EQUITIES:
            return "equities_data";
        case AssetClass::FIXED_INCOME:
            return "fixed_income_data";
        case AssetClass::CURRENCIES:
            return "currencies_data";
        case AssetClass::COMMODITIES:
            return "commodities_data";
        case AssetClass::CRYPTO:
            return "crypto_data";
        default:
            return "unknown_data";
    }
}

/**
 * @brief Build full table name from components
 * @param asset_class Asset class for schema
 * @param data_type Base table name (e.g., "ohlcv")
 * @param freq Data frequency
 * @return Full table name (e.g., "futures_data.ohlcv_1d")
 */
inline std::string build_table_name(AssetClass asset_class, const std::string& data_type,
                                    DataFrequency freq) {
    return get_schema_name(asset_class) + "." + data_type + "_" + get_table_suffix(freq);
}

}  // namespace trade_ngin

// Specialization for std::to_string to work with Decimal
namespace std {
inline string to_string(const trade_ngin::Decimal& d) {
    return d.to_string();
}
}  // namespace std

namespace std {
template <>
struct less<trade_ngin::Timestamp> {
    bool operator()(const trade_ngin::Timestamp& lhs, const trade_ngin::Timestamp& rhs) const {
        return lhs < rhs;
    }
};
}  // namespace std