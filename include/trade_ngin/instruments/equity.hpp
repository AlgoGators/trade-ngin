// include/trade_ngin/instruments/equity.hpp
#pragma once

#include <memory>
#include <string>
#include <vector>
#include "trade_ngin/core/holiday_checker.hpp"
#include "trade_ngin/instruments/instrument.hpp"

namespace trade_ngin {

/**
 * @brief Dividend event information
 *
 * Note: The current equity strategy implementation relies on adjusted close prices
 * (closeadj) from the database to account for dividends and splits. This struct
 * and the associated get_next_dividend() method are available for future use by
 * strategies that need explicit dividend awareness (e.g., dividend capture strategies).
 */
struct DividendInfo {
    Timestamp ex_date;
    Timestamp payment_date;
    double amount;
    bool is_special;
};

/**
 * @brief Account model for equity margin and shorting.
 *
 * CASH: full position notional is posted as margin (cash deployed). No
 * shorting. Standard retail cash-account behavior.
 *
 * REG_T: Reg T margin. Long positions post 50% of notional; shorts post
 * 150% (100% proceeds + 50% maintenance margin). Shorting allowed only if
 * EquitySpec::short_selling_allowed is also true.
 */
enum class EquityAccountMode { CASH, REG_T };

/**
 * @brief Stock specification
 */
struct EquitySpec {
    std::string exchange;                      // Exchange code
    std::string currency;                      // Trading currency
    double lot_size{100.0};                    // Standard lot size
    double tick_size{0.01};                    // Minimum price increment
    double commission_per_share{0.0};          // Commission per share
    bool is_etf{false};                        // Whether the instrument is an ETF
    bool is_marginable{true};                  // Whether the stock can be margined
    std::string sector;                        // Industry sector
    std::string industry;                      // Specific industry
    std::string trading_hours{"09:30-16:00"};  // NYSE and NASDAQ regular session hours
    std::vector<DividendInfo> dividends;       // Upcoming dividends

    // Account model. Default CASH preserves safe long-only behavior; opt in
    // to REG_T per-symbol for leveraged/short capability.
    EquityAccountMode account_mode{EquityAccountMode::CASH};

    // Shorting gate. Only meaningful when account_mode == REG_T; with CASH
    // mode, shorts are rejected by the strategy clamp regardless of this flag.
    bool short_selling_allowed{false};

    // Borrow fee configuration.
    // borrow_rate_override: if >= 0, use this annual rate instead of the
    //   formula in TransactionCostManager::calculate_overnight_borrow_fees.
    //   Use to inject broker-provided rates from a live locate API.
    // is_easy_to_borrow: when false, force HTB tier regardless of ADV/price
    //   signals (e.g., recent IPOs with low float despite high ADV).
    // (Per-trade flat locate fee deliberately omitted: the cost-path code
    // does not yet model short-open events distinctly, so a field with no
    // consumer would silently swallow any value the user configured.
    // Add when the short-open cost path lands.)
    double borrow_rate_override{-1.0};
    bool is_easy_to_borrow{true};
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
    const std::string& get_symbol() const override {
        return symbol_;
    }
    AssetType get_type() const override {
        return AssetType::EQUITY;
    }
    const std::string& get_exchange() const override {
        return spec_.exchange;
    }
    const std::string& get_currency() const override {
        return spec_.currency;
    }
    double get_multiplier() const override {
        return 1.0;
    }  // Stocks always 1:1
    double get_tick_size() const override {
        return spec_.tick_size;
    }
    double get_commission_per_contract() const override {
        return spec_.commission_per_share * spec_.lot_size;
    }
    double get_point_value() const override {
        return 1.0;
    }  // $1 per point for stocks

    bool is_tradeable() const override;

    // Legacy no-arg margin: 0.0 sentinel. Equity margin is account-mode and
    // position-dependent; callers must use the price/quantity overload below.
    // Returning 0 (rather than a stale 0.5) ensures any unmigrated caller
    // surfaces an obvious bug instead of silently using nonsense margin.
    double get_margin_requirement() const override {
        return 0.0;
    }
    double get_margin_requirement(double price, double quantity) const override;

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
    double get_lot_size() const {
        return spec_.lot_size;
    }

    /**
     * @brief Check if instrument is an ETF
     */
    bool is_etf() const {
        return spec_.is_etf;
    }

    /**
     * @brief Check if stock can be margined
     */
    bool is_marginable() const {
        return spec_.is_marginable;
    }

    /**
     * @brief Get the account mode (CASH or REG_T).
     */
    EquityAccountMode get_account_mode() const {
        return spec_.account_mode;
    }

    /**
     * @brief Whether shorting is permitted for this instrument.
     *
     * True only when account_mode is REG_T AND short_selling_allowed is set.
     * Strategies must clamp short signals to zero when this returns false.
     */
    bool is_short_allowed() const {
        return spec_.account_mode == EquityAccountMode::REG_T &&
               spec_.short_selling_allowed;
    }

    /**
     * @brief Access the underlying spec (for borrow-fee config etc.).
     */
    const EquitySpec& get_spec() const {
        return spec_;
    }

    /**
     * @brief Get sector classification
     */
    const std::string& get_sector() const {
        return spec_.sector;
    }

    /**
     * @brief Get industry classification
     */
    const std::string& get_industry() const {
        return spec_.industry;
    }

    /**
     * @brief Get upcoming dividends
     */
    const std::vector<DividendInfo>& get_dividends() const {
        return spec_.dividends;
    }

    /**
     * @brief Get next dividend
     * @param from Reference timestamp
     * @return Next dividend or nullopt if none scheduled
     */
    std::optional<DividendInfo> get_next_dividend(const Timestamp& from) const;

    /**
     * @brief Set a shared holiday checker for all equity instruments
     * @param checker Shared pointer to HolidayChecker instance
     */
    static void set_holiday_checker(std::shared_ptr<HolidayChecker> checker);

private:
    std::string symbol_;
    EquitySpec spec_;

    static std::shared_ptr<HolidayChecker> holiday_checker_;
};

}  // namespace trade_ngin