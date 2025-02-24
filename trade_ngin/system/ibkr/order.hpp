#pragma once

#include <string>
#include "contract.hpp"

namespace ibkr {

enum class OrderType {
    MKT,     // Market
    LMT,     // Limit
    STP,     // Stop
    STP_LMT, // Stop Limit
    TRAIL,   // Trailing Stop
    MOC,     // Market on Close
    LOC,     // Limit on Close
    MIT,     // Market if Touched
    LIT,     // Limit if Touched
    MTL,     // Market to Limit
    REL,     // Relative
    TRAIL_LIMIT, // Trailing Stop Limit
    VOL,     // Volume
    PEG_MKT, // Pegged to Market
    PEG_STK, // Pegged to Stock
    PEG_MID  // Pegged to Midpoint
};

enum class OrderStatus {
    PENDING,    // Order sent but not confirmed
    SUBMITTED,  // Order confirmed by IB
    FILLED,     // Order fully executed
    CANCELLED,  // Order cancelled
    REJECTED,   // Order rejected by IB
    PARTIAL,    // Order partially filled
    API_PENDING,// Order sent from API
    API_CANCELLED, // Order cancelled via API
    INACTIVE    // Order inactive (e.g., daily futures rollover)
};

struct Order {
    std::string orderId;
    Contract contract;
    OrderType orderType;
    std::string action;  // "BUY" or "SELL"
    double totalQuantity;
    double filledQuantity;
    double limitPrice;
    double stopPrice;
    double avgFillPrice;
    OrderStatus status;
    
    // Time in Force
    bool outsideRth;     // Allow outside regular trading hours
    std::string tif;     // Time in Force: DAY, GTC, IOC, GTD
    std::string goodTillDate; // Used with GTD orders
    
    // Optional fields
    double trailingPercent;
    double trailingStopPrice;
    double lmtPriceOffset;  // For trailing stop limit orders
    bool transmit;          // If false, order is created but not transmitted
    
    // Futures specific
    std::string openClose;  // O=Open, C=Close
    int origin;            // 0=Customer, 1=Firm
    std::string account;    // Account number
    std::string settlingFirm; // Clearing firm
    std::string clearingAccount; // Clearing account
    std::string clearingIntent; // "" = Default, "IB", "Away", "PTA"
    
    // Algo/Smart routing
    std::string algoStrategy;
    std::string algoParams;
    bool smartRouting;
    
    // Regulatory
    std::string orderRef;   // User-defined order reference
    bool discretionary;     // Discretionary order
    bool hidden;           // Hidden order
    bool sweepToFill;     // Sweep-to-fill
    bool allOrNone;       // All-or-none
    bool blockOrder;      // Block order
    int minQty;          // Minimum quantity
    double percentOffset; // Percent offset for relative orders
    
    // Risk Management
    bool overridePercentageConstraints;
    std::string rule80A;  // Individual = 'I', Agency = 'A', AgentOtherMember = 'W'
    bool firmQuoteOnly;
    bool eTradeOnly;
    bool notHeld;        // Not held order
    
    Order() : totalQuantity(0), filledQuantity(0), limitPrice(0), 
              stopPrice(0), avgFillPrice(0), status(OrderStatus::PENDING),
              outsideRth(false), trailingPercent(0), trailingStopPrice(0),
              lmtPriceOffset(0), transmit(true), origin(0), smartRouting(true),
              discretionary(false), hidden(false), sweepToFill(false),
              allOrNone(false), blockOrder(false), minQty(0), percentOffset(0),
              overridePercentageConstraints(false), firmQuoteOnly(false),
              eTradeOnly(false), notHeld(false) {}
              
    // Helper methods for common order types
    static Order MarketOrder(const std::string& action, double quantity) {
        Order order;
        order.orderType = OrderType::MKT;
        order.action = action;
        order.totalQuantity = quantity;
        return order;
    }
    
    static Order LimitOrder(const std::string& action, double quantity, double price) {
        Order order;
        order.orderType = OrderType::LMT;
        order.action = action;
        order.totalQuantity = quantity;
        order.limitPrice = price;
        return order;
    }
    
    static Order StopOrder(const std::string& action, double quantity, double stopPrice) {
        Order order;
        order.orderType = OrderType::STP;
        order.action = action;
        order.totalQuantity = quantity;
        order.stopPrice = stopPrice;
        return order;
    }
    
    static Order StopLimitOrder(const std::string& action, double quantity, 
                              double stopPrice, double limitPrice) {
        Order order;
        order.orderType = OrderType::STP_LMT;
        order.action = action;
        order.totalQuantity = quantity;
        order.stopPrice = stopPrice;
        order.limitPrice = limitPrice;
        return order;
    }
    
    // Futures specific order types
    static Order FuturesMarketOrder(const std::string& action, double quantity,
                                  const std::string& openClose = "O") {
        Order order = MarketOrder(action, quantity);
        order.openClose = openClose;
        order.origin = 0;  // Customer
        return order;
    }
    
    static Order FuturesLimitOrder(const std::string& action, double quantity,
                                 double price, const std::string& openClose = "O") {
        Order order = LimitOrder(action, quantity, price);
        order.openClose = openClose;
        order.origin = 0;  // Customer
        return order;
    }
};
