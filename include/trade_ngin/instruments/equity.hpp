// include/trade_ngin/instruments/equity.hpp
#pragma once

#include "trade_ngin/instruments/instrument.hpp"
#include <string>
#include <vector>

namespace trade_ngin {

struct DividendInfo {
    Timestamp ex_date;
    Timestamp payment_date;
    double amount;
    bool is_special;
};

/**
 * @brief Stock specification
 */
struct EquitySpec {
    std::string exchange;              // Exchange code
    std::string currency;             // Trading currency
    double lot_size{100.0};          // Standard lot size
    double tick_size{0.01};          // Minimum price increment
    double commission_per_share{0.0}; // Commission per share
    bool is_etf{false};              // Whether the instrument is an ETF
    bool is_marginable{true};        // Whether the stock can be margined
    double margin_requirement{0.5};   // Initial margin requirement (typically 50%)
    std::string sector;              // Industry sector
    std::string industry;            // Specific industry
    std::string trading_hours{"09:30-16:00"}; // Default NYSE hours
    std::vector<DividendInfo> dividends; // Upcoming dividends
};

/**
 * @brief Equity instrument implementation
 */
class EquityInstrument : public Instrument {
public:
    /**
     * @brief Constructor
     * @param symbol Stock symbol
     * @param spec Stock specification
     */
    EquityInstrument(std::string symbol, EquitySpec spec);

    // Implement Instrument interface
    const std::string& get_symbol() const override { return symbol_; }
    AssetType get_type() const override { return AssetType::EQUITY; }
    const std::string& get_exchange() const override { return spec_.exchange; }
    const std::string& get_currency() const override { return spec_.currency; }
    double get_multiplier() const override { return 1.0; }  // Stocks always 1:1
    double get_tick_size() const override { return spec_.tick_size; }
    double get_commission_per_contract() const override {
        return spec_.commission_per_share * spec_.lot_size;
    }
    double get_point_value() const override { return 1.0; }  // $1 per point for stocks
    
    bool is_tradeable() const override;
    double get_margin_requirement() const override { 
        return spec_.margin_requirement; 
    }
    std::string get_trading_hours() const override { 
        return spec_.trading_hours; 
    }
    bool is_market_open(const Timestamp& timestamp) const override;
    double round_price(double price) const override;
    double get_notional_value(double quantity, double price) const override;
    double calculate_commission(double quantity) const override;

    /**
     * @brief Get lot size
     */
    double get_lot_size() const { return spec_.lot_size; }

    /**
     * @brief Check if instrument is an ETF
     */
    bool is_etf() const { return spec_.is_etf; }

    /**
     * @brief Check if stock can be margined
     */
    bool is_marginable() const { return spec_.is_marginable; }

    /**
     * @brief Get sector classification
     */
    const std::string& get_sector() const { return spec_.sector; }

    /**
     * @brief Get industry classification
     */
    const std::string& get_industry() const { return spec_.industry; }

    /**
     * @brief Get upcoming dividends
     */
    const std::vector<DividendInfo>& get_dividends() const { return spec_.dividends; }

    /**
     * @brief Get next dividend
     * @param from Reference timestamp
     * @return Next dividend or nullopt if none scheduled
     */
    std::optional<DividendInfo> get_next_dividend(const Timestamp& from) const;

private:
    std::string symbol_;
    EquitySpec spec_;
};

} // namespace trade_ngin