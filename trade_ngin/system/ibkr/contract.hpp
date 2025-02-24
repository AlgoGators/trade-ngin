#pragma once

#include <string>
#include <vector>

namespace ibkr {

enum class SecType {
    STK,    // Stock
    FUT,    // Future
    OPT,    // Option
    FOP,    // Future Option
    CASH,   // Forex
    IND,    // Index
    BOND,   // Bond
    CMDTY   // Commodity
};

struct Contract {
    int conId;              // Contract identifier
    std::string symbol;     // Underlying symbol
    SecType secType;        // Security type
    std::string exchange;   // Exchange
    std::string currency;   // Currency
    double multiplier;      // Contract multiplier
    
    // Future/Option specific
    std::string lastTradeDateOrContractMonth;  // YYYYMM or YYYYMMDD
    double strike;          // Option strike price
    std::string right;      // Put/Call for options
    std::string localSymbol;// Local symbol at exchange
    std::string tradingClass;// Trading class name
    
    // For futures/commodities
    std::string marketName; // Market name for futures
    std::string minTick;    // Minimum price increment
    std::string orderTypes; // Valid order types
    std::string validExchanges; // Valid exchanges
    int priceMagnifier;     // Magnitude of prices
    int underConId;         // Underlying contract ID
    std::string longName;   // Descriptive name
    std::string contractMonth; // Contract month for futures
    std::string industry;   // Industry classification
    std::string category;   // Category
    std::string subcategory;// Subcategory
    std::string timeZoneId; // Time zone for trading hours
    std::string tradingHours;// Trading hours
    std::string liquidHours;// Liquid trading hours
    
    // For derivatives
    std::string underSymbol;// Underlying symbol
    std::string underSecType;// Underlying security type
    std::string underExchange;// Underlying exchange
    std::string underCurrency;// Underlying currency
    
    Contract(const std::string& sym = "", 
             SecType type = SecType::STK,
             const std::string& exch = "SMART",
             const std::string& curr = "USD")
        : conId(0), symbol(sym), secType(type), exchange(exch), currency(curr),
          multiplier(0), strike(0.0), priceMagnifier(1), underConId(0) {}
          
    // Helper methods for common contract types
    static Contract Stock(const std::string& symbol, 
                        const std::string& exchange = "SMART",
                        const std::string& currency = "USD") {
        return Contract(symbol, SecType::STK, exchange, currency);
    }
    
    static Contract Future(const std::string& symbol,
                         const std::string& contractMonth,
                         const std::string& exchange,
                         const std::string& currency = "USD") {
        Contract contract(symbol, SecType::FUT, exchange, currency);
        contract.lastTradeDateOrContractMonth = contractMonth;
        return contract;
    }
    
    static Contract FutureOption(const std::string& symbol,
                               const std::string& contractMonth,
                               double strike,
                               const std::string& right,
                               const std::string& exchange,
                               const std::string& currency = "USD") {
        Contract contract(symbol, SecType::FOP, exchange, currency);
        contract.lastTradeDateOrContractMonth = contractMonth;
        contract.strike = strike;
        contract.right = right;
        return contract;
    }
    
    // Helper for continuous futures
    static Contract ContinuousFuture(const std::string& symbol,
                                   const std::string& exchange,
                                   const std::string& currency = "USD") {
        Contract contract(symbol, SecType::FUT, exchange, currency);
        contract.contractMonth = "0"; // Continuous contract
        return contract;
    }
};
}
