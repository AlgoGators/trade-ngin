// include/trade_ngin/instruments/futures.hpp
#pragma once

#include <chrono>
#include <cmath>
#include <optional>
#include <string>
#include "trade_ngin/instruments/instrument.hpp"

namespace trade_ngin {

/**
 * @brief Futures contract specification
 */
struct FuturesSpec {
    std::string root_symbol;                // Root symbol (e.g., "ES" for E-mini S&P)
    std::string exchange;                   // Exchange code
    std::string currency;                   // Trading currency
    double multiplier;                      // Contract multiplier
    double tick_size;                       // One tick in PRICE units (metadata "Tick Size")
    double commission_per_contract;         // Commission per contract
    double initial_margin;                  // Initial margin requirement
    double maintenance_margin;              // Maintenance margin requirement
    double weight;                          // Weight for portfolio allocation
    std::string trading_hours;              // Trading hours specification
    std::optional<Timestamp> expiry;        // Contract expiration
    std::optional<std::string> underlying;  // Underlying instrument
    // One tick in DOLLARS per contract (metadata "Minimum Price Fluctuation", the tick value):
    // tick_size x multiplier on a consistent metadata row. 0 = not given.
    double tick_value{0.0};
    // The metadata's "Fee Per Contract" (dollars per contract per side, all-in), when the table
    // has that column (migration 014); empty when it does not, and the cost model then charges
    // its configured fee.
    std::optional<double> fee_per_contract;
};

/**
 * @brief Futures contract implementation
 */
class FuturesInstrument : public Instrument {
public:
    /**
     * @brief Constructor
     * @param symbol Contract symbol
     * @param spec Contract specification
     */
    FuturesInstrument(std::string symbol, FuturesSpec spec);

    // Implement Instrument interface
    const std::string& get_symbol() const override {
        return symbol_;
    }
    AssetType get_type() const override {
        return AssetType::FUTURE;
    }
    const std::string& get_exchange() const override {
        return spec_.exchange;
    }
    const std::string& get_currency() const override {
        return spec_.currency;
    }
    double get_multiplier() const override {
        return spec_.multiplier;
    }
    double get_tick_size() const override {
        return spec_.tick_size;
    }
    double get_commission_per_contract() const override {
        return spec_.commission_per_contract;
    }
    double get_point_value() const override {
        return spec_.tick_size * spec_.multiplier;
    }
    /**
     * @brief One tick in dollars per contract as the metadata states it ("Minimum Price
     *        Fluctuation"); 0 when not given
     */
    double get_tick_value() const {
        return spec_.tick_value;
    }
    /**
     * @brief The metadata's per-contract fee, when the metadata carries one
     */
    const std::optional<double>& get_fee_per_contract() const {
        return spec_.fee_per_contract;
    }

    bool is_tradeable() const override;
    double get_margin_requirement() const override {
        return spec_.initial_margin;
    }
    // Override the price/qty overload so the base-class default (which forwards
    // to the no-arg version returning per-contract margin) doesn't strip the
    // contract-count multiplication. MarginManager calls this overload and
    // expects total dollars, not per-contract.
    double get_margin_requirement(double price, double quantity) const override {
        (void)price;  // Futures margin is a fixed dollar amount per contract.
        return std::abs(quantity) * spec_.initial_margin;
    }
    std::string get_trading_hours() const override {
        return spec_.trading_hours;
    }
    bool is_market_open(const Timestamp& timestamp) const override;
    double round_price(double price) const override;
    double get_notional_value(double quantity, double price) const override;
    double calculate_commission(double quantity) const override;

    /**
     * @brief Get contract expiration
     */
    const std::optional<Timestamp>& get_expiry() const {
        return spec_.expiry;
    }

    /**
     * @brief Get maintenance margin
     */
    double get_maintenance_margin() const {
        return spec_.maintenance_margin;
    }

    /**
     * @brief Get root symbol
     */
    const std::string& get_root_symbol() const {
        return spec_.root_symbol;
    }

    /**
     * @brief Get underlying symbol if exists
     */
    const std::optional<std::string>& get_underlying() const {
        return spec_.underlying;
    }

    /**
     * @brief Get weight per contract for portfolio allocation
     */
    double get_weight() const {
        return spec_.weight;
    }

    /**
     * @brief Calculate days to expiry
     * @param from Reference timestamp
     * @return Days to expiry or nullopt if no expiry
     */
    std::optional<int> days_to_expiry(const Timestamp& from) const;

    /**
     * @brief Check if contract is expired
     * @param timestamp Reference time
     * @return true if contract is expired
     */
    bool is_expired(const Timestamp& timestamp) const;

private:
    std::string symbol_;
    FuturesSpec spec_;

    /**
     * @brief Parse trading hours string
     * @param timestr Time string to parse
     * @return Pair of hour and minute
     */
    std::pair<int, int> parse_time(const std::string& timestr) const;
};

}  // namespace trade_ngin