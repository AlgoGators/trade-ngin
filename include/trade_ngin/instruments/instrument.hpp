// include/trade_ngin/instruments/instrument.hpp
#pragma once

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/core/error.hpp"
#include <string>
#include <memory>
#include <optional>

namespace trade_ngin {

/**
 * @brief Base class for all tradeable instruments
 */
class Instrument {
public:
    virtual ~Instrument() = default;

    /**
     * @brief Get instrument symbol
     */
    virtual const std::string& get_symbol() const = 0;

    /**
     * @brief Get instrument type
     */
    virtual AssetType get_type() const = 0;

    /**
     * @brief Get exchange where instrument is traded
     */
    virtual const std::string& get_exchange() const = 0;

    /**
     * @brief Get trading currency
     */
    virtual const std::string& get_currency() const = 0;

    /**
     * @brief Get contract multiplier
     */
    virtual double get_multiplier() const = 0;

    /**
     * @brief Get minimum price increment (tick size)
     */
    virtual double get_tick_size() const = 0;

    /**
     * @brief Get commission per contract
     */
    virtual double get_commission_per_contract() const = 0;

    /**
     * @brief Get point value (tick size * multiplier)
     */
    virtual double get_point_value() const = 0;

    /**
     * @brief Check if instrument is tradeable
     */
    virtual bool is_tradeable() const = 0;

    /**
     * @brief Get margin requirement
     */
    virtual double get_margin_requirement() const = 0;

    /**
     * @brief Get trading hours
     * @return String representation of trading hours
     */
    virtual std::string get_trading_hours() const = 0;

    /**
     * @brief Check if market is open at given time
     * @param timestamp Time to check
     * @return true if market is open
     */
    virtual bool is_market_open(const Timestamp& timestamp) const = 0;

    /**
     * @brief Round price to valid tick size
     * @param price Price to round
     * @return Rounded price
     */
    virtual double round_price(double price) const = 0;

    /**
     * @brief Get notional value of position
     * @param quantity Position size
     * @param price Current price
     * @return Notional value
     */
    virtual double get_notional_value(double quantity, double price) const = 0;

    /**
     * @brief Calculate commission for trade
     * @param quantity Trade size
     * @return Total commission
     */
    virtual double calculate_commission(double quantity) const = 0;

protected:
    Instrument() = default;
};

} // namespace trade_ngin