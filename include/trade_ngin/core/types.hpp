// include/trade_ngin/core/types.hpp

#pragma once

#include <string>
#include <chrono>
#include <vector>
#include <variant>
#include <memory>
#include <optional>
#include <unordered_map>

namespace trade_ngin {

/**
 * @brief Timestamp type for consistent time representation
 * Uses std::chrono for type-safe time handling
 */
using Timestamp = std::chrono::system_clock::time_point;

/**
 * @brief Price type with double precision
 * Used for all price-related calculations
 */
using Price = double;

/**
 * @brief Quantity type for order and position sizes
 * Double to support fractional quantities
 */
using Quantity = double;

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
enum class OrderType {
    MARKET,
    LIMIT,
    STOP,
    STOP_LIMIT,
    NONE
};

/**
 * @brief Time in force enumeration
 */
enum class TimeInForce {
    DAY,
    GTC,     // Good Till Cancel
    IOC,     // Immediate or Cancel
    FOK,     // Fill or Kill
    GTD,     // Good Till Date
    MOC,     // Market on Close
    MOO,     // Market on Open
    NONE
};

/**
 * @brief Asset type enumeration
 */
enum class AssetType {
    FUTURE,
    EQUITY,
    OPTION,
    FOREX,
    CRYPTO,
    NONE
};

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
    double volume;
    std::string symbol;

    Bar() = default;
    Bar(Timestamp ts, Price o, Price h, Price l, Price c, double v, std::string s)
        : timestamp(ts), open(o), high(h), low(l), close(c), volume(v), symbol(std::move(s)) {}
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
    double multiplier;
    double tick_size;
    double commission_per_contract;
    
    // Futures-specific fields
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
        : symbol(std::move(symbol)), side(side), type(type), 
          quantity(qty), price(price), time_in_force(TimeInForce::DAY) {}
};

/**
 * @brief Position structure
 * Represents a current position in an instrument
 */
struct Position {
    std::string symbol;
    Quantity quantity;
    Price average_price;
    double unrealized_pnl;
    double realized_pnl;
    Timestamp last_update;

    // Constructors
    Position(std::string sym, Quantity qty, Price avg_price, double unreal_pnl, double real_pnl, Timestamp ts)
        : symbol(std::move(sym)), 
        quantity(qty), 
        average_price(avg_price), 
        unrealized_pnl(unreal_pnl), 
        realized_pnl(real_pnl), 
        last_update(ts) {}
    
    Position() : quantity(0), average_price(0), unrealized_pnl(0), realized_pnl(0) {}
    
    // Helper method to check if position exists
    bool has_position() const { return quantity != 0; }
    
    // Helper method to get position side
    Side get_side() const {
        if (quantity > 0) return Side::BUY;
        if (quantity < 0) return Side::SELL;
        return Side::NONE;
    }
};

/**
 * @brief Execution report structure
 * Represents a fill or partial fill of an order
 */
struct ExecutionReport {
    std::string order_id;
    std::string exec_id;
    std::string symbol;
    Side side;
    Quantity filled_quantity;
    Price fill_price;
    Timestamp fill_time;
    double commission;
    bool is_partial;
};

/**
 * @brief Market state enumeration for regime detection
 */
enum class MarketRegime {
    TRENDING_UP,
    TRENDING_DOWN,
    MEAN_REVERTING,
    VOLATILE,
    UNDEFINED
};

/**
 * @brief Risk limits structure
 * Holds all risk-related limits for a strategy or portfolio
 */
struct RiskLimits {
    double max_position_size;
    double max_notional_value;
    double max_drawdown;
    double max_leverage;
    double var_limit;
    double max_correlation;
};

/**
 * @brief Asset class enumeration
 * Represents different types of financial instruments
 */
enum class AssetClass {
    FUTURES,
    EQUITIES,
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
inline std::string build_table_name(
    AssetClass asset_class, 
    const std::string& data_type,
    DataFrequency freq) {
    return get_schema_name(asset_class) + "." + 
           data_type + "_" + 
           get_table_suffix(freq);
}

} 


namespace std {
    template <>
    struct less<trade_ngin::Timestamp> {
        bool operator()(const trade_ngin::Timestamp& lhs, 
                        const trade_ngin::Timestamp& rhs) const {
            return lhs < rhs;
        }
    };
}  // namespace std