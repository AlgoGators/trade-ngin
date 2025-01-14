#pragma once
#include "contract.hpp"

// Forward declare to avoid circular dependency
enum class AdaptiveOrderPriority;
enum class OrderSide;
enum class OrderType;
enum class OrderStatus;

class Order {
public:
    Order(const Contract& contract, OrderSide side, double quantity, 
          OrderType type, AdaptiveOrderPriority priority);

    // Getters
    const Contract& getContract() const { return contract_; }
    OrderSide getSide() const { return side_; }
    double getQuantity() const { return quantity_; }
    OrderType getType() const { return type_; }
    AdaptiveOrderPriority getPriority() const { return priority_; }
    OrderStatus getStatus() const { return status_; }

    // Setters
    void setStatus(OrderStatus status) { status_ = status; }
    void setFilledQuantity(double qty) { filled_quantity_ = qty; }

private:
    Contract contract_;
    OrderSide side_;
    double quantity_;
    OrderType type_;
    AdaptiveOrderPriority priority_;
    OrderStatus status_;
    double filled_quantity_ = 0.0;
    double limit_price_ = 0.0;
    double stop_price_ = 0.0;
}; 