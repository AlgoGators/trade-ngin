#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <unordered_map>
#include "contract.hpp"
#include "order.hpp"

namespace ibkr {

struct Position {
    Contract contract;
    double position;
    double marketPrice;
    double marketValue;
    double averageCost;
    double unrealizedPNL;
    double realizedPNL;
};

struct AccountSummary {
    std::string accountId;
    double netLiquidation;
    double equity;
    double cash;
    double buyingPower;
    double margin;
    std::vector<Position> positions;
};

class AccountHandler {
public:
    using AccountUpdateCallback = std::function<void(const AccountSummary&)>;
    using PositionUpdateCallback = std::function<void(const Position&)>;
    using OrderUpdateCallback = std::function<void(const Order&)>;
    
    virtual ~AccountHandler() = default;
    
    // Account operations
    virtual AccountSummary requestAccountSummary() = 0;
    virtual std::vector<Position> requestPositions() = 0;
    virtual std::vector<Order> requestOpenOrders() = 0;
    
    // Order operations
    virtual std::string placeOrder(const Order& order) = 0;
    virtual void cancelOrder(const std::string& orderId) = 0;
    virtual void modifyOrder(const std::string& orderId, const Order& newOrder) = 0;
    
    // Real-time updates
    virtual void subscribeAccountUpdates(bool subscribe = true) = 0;
    virtual void subscribePositions(bool subscribe = true) = 0;
    
    // Callbacks
    void setAccountUpdateCallback(AccountUpdateCallback callback) {
        accountCallback_ = callback;
    }
    
    void setPositionUpdateCallback(PositionUpdateCallback callback) {
        positionCallback_ = callback;
    }
    
    void setOrderUpdateCallback(OrderUpdateCallback callback) {
        orderCallback_ = callback;
    }
    
protected:
    AccountUpdateCallback accountCallback_;
    PositionUpdateCallback positionCallback_;
    OrderUpdateCallback orderCallback_;
};
