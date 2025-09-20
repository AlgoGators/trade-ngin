// include/trade_ngin/instruments/option.hpp
#pragma once

#include <string>
#include "trade_ngin/instruments/instrument.hpp"

namespace trade_ngin {

enum class OptionType { CALL, PUT };

enum class ExerciseStyle { AMERICAN, EUROPEAN };

struct Greeks {
    double delta{0.0};
    double gamma{0.0};
    double theta{0.0};
    double vega{0.0};
    double rho{0.0};
};

/**
 * @brief Option contract specification
 */
struct OptionSpec {
    std::string underlying_symbol;             // Underlying instrument symbol
    OptionType type;                           // Call or Put
    ExerciseStyle style;                       // American or European
    double strike;                             // Strike price
    Timestamp expiry;                          // Expiration timestamp
    std::string exchange;                      // Exchange code
    std::string currency;                      // Trading currency
    double multiplier{100.0};                  // Contract multiplier (usually 100 for stocks)
    double tick_size{0.01};                    // Minimum price increment
    double commission_per_contract{0.65};      // Commission per contract
    std::string trading_hours{"09:30-16:00"};  // Default trading hours
    double margin_requirement{1.0};            // Initial margin requirement
    bool is_weekly{false};                     // Whether it's a weekly option
    bool is_adjusted{false};                   // Whether contract is adjusted for corporate actions
};

/**
 * @brief Option instrument implementation
 */
class OptionInstrument : public Instrument {
public:
    /**
     * @brief Constructor
     * @param symbol Option symbol
     * @param spec Option specification
     */
    OptionInstrument(std::string symbol, OptionSpec spec);

    // Implement Instrument interface
    const std::string& get_symbol() const override {
        return symbol_;
    }
    AssetType get_type() const override {
        return AssetType::OPTION;
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

    bool is_tradeable() const override;
    double get_margin_requirement() const override;
    std::string get_trading_hours() const override {
        return spec_.trading_hours;
    }
    bool is_market_open(const Timestamp& timestamp) const override;
    double round_price(double price) const override;
    double get_notional_value(double quantity, double price) const override;
    double calculate_commission(double quantity) const override;

    /**
     * @brief Get option type (Call/Put)
     */
    OptionType get_option_type() const {
        return spec_.type;
    }

    /**
     * @brief Get exercise style
     */
    ExerciseStyle get_exercise_style() const {
        return spec_.style;
    }

    /**
     * @brief Get strike price
     */
    double get_strike() const {
        return spec_.strike;
    }

    /**
     * @brief Get expiration timestamp
     */
    const Timestamp& get_expiry() const {
        return spec_.expiry;
    }

    /**
     * @brief Get underlying symbol
     */
    const std::string& get_underlying() const {
        return spec_.underlying_symbol;
    }

    /**
     * @brief Calculate option Greeks
     * @param underlying_price Current underlying price
     * @param volatility Implied volatility
     * @param risk_free_rate Risk-free interest rate
     * @return Greeks structure with calculated values
     */
    Greeks calculate_greeks(double underlying_price, double volatility,
                            double risk_free_rate) const;

    /**
     * @brief Calculate implied volatility
     * @param option_price Option market price
     * @param underlying_price Current underlying price
     * @param risk_free_rate Risk-free interest rate
     * @return Implied volatility or nullopt if calculation fails
     */
    std::optional<double> calculate_implied_volatility(double option_price, double underlying_price,
                                                       double risk_free_rate) const;

    /**
     * @brief Calculate days to expiry
     * @param from Reference timestamp
     * @return Number of days to expiration
     */
    int days_to_expiry(const Timestamp& from) const;

    /**
     * @brief Check if option is in the money
     * @param underlying_price Current underlying price
     * @return true if ITM
     */
    bool is_in_the_money(double underlying_price) const;

    /**
     * @brief Get moneyness (ratio of underlying price to strike)
     * @param underlying_price Current underlying price
     * @return Moneyness ratio
     */
    double get_moneyness(double underlying_price) const;

    /**
     * @brief Check if option is weekly
     */
    bool is_weekly() const {
        return spec_.is_weekly;
    }

    /**
     * @brief Check if option is adjusted
     */
    bool is_adjusted() const {
        return spec_.is_adjusted;
    }

private:
    std::string symbol_;
    OptionSpec spec_;

    /**
     * @brief Calculate theoretical option price using Black-Scholes
     */
    double calculate_theoretical_price(double underlying_price, double volatility,
                                       double risk_free_rate) const;
};

}  // namespace trade_ngin
