// include/trade_ngin/instruments/instrument.hpp
#pragma once

#include <memory>
#include <optional>
#include <string>
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"

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
     * @brief Get margin requirement (legacy, no-context form).
     *
     * For futures this returns the contract's initial-margin dollar amount.
     * For equities this is unreliable -- equity margin depends on price and
     * signed quantity (cash account: full notional; Reg T long: 50%; Reg T
     * short: 150%). Prefer the price/quantity overload below for equities.
     */
    virtual double get_margin_requirement() const = 0;

    /**
     * @brief Get margin requirement given price and signed quantity.
     *
     * Default body forwards to the no-arg version so futures and other
     * legacy instruments don't need to override. Equities override this to
     * compute account-mode-aware margin (cash vs Reg T, long vs short).
     *
     * @param price Reference price per unit (e.g. close)
     * @param quantity Signed share/contract count (positive long, negative short)
     * @return Margin requirement in dollars
     */
    virtual double get_margin_requirement(double price, double quantity) const {
        (void)price;
        (void)quantity;
        return get_margin_requirement();
    }

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

}  // namespace trade_ngin