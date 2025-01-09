#pragma once
#include "order.hpp"
#include <unordered_map>

class TransactionCosts {
public:
    struct CostComponents {
        double commission;
        double spread;
        double slippage;
        double market_impact;
        double exchange_fees;
        
        double total() const {
            return commission + spread + slippage + market_impact + exchange_fees;
        }
    };

    void setCommissionRate(const std::string& instrument, double rate) {
        commission_rates_[instrument] = rate;
    }

    void setSpreadEstimate(const std::string& instrument, double spread) {
        spread_estimates_[instrument] = spread;
    }

    CostComponents estimateCosts(const Order& order, double price) {
        CostComponents costs;
        const std::string& instrument = order.getContract().symbol();
        
        // Commission
        costs.commission = order.getQuantity() * price * 
                          commission_rates_[instrument];
        
        // Spread
        costs.spread = order.getQuantity() * spread_estimates_[instrument];
        
        // Other components calculated based on market conditions
        return costs;
    }

private:
    std::unordered_map<std::string, double> commission_rates_;
    std::unordered_map<std::string, double> spread_estimates_;
}; 