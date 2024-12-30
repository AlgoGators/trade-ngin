#pragma once
#include "dataframe.hpp"
#include <cmath>

class MarketImpact {
public:
    struct ImpactConfig {
        double permanent_impact_factor;
        double temporary_impact_factor;
        double market_participation_limit;
        double volatility_adjustment;
    };

    MarketImpact(ImpactConfig config) : config_(config) {}

    double calculateImpact(double order_size, double adv, double volatility, double price) {
        double participation_rate = std::abs(order_size) / adv;
        if (participation_rate > config_.market_participation_limit) {
            order_size *= config_.market_participation_limit / participation_rate;
        }

        double permanent_impact = config_.permanent_impact_factor * 
                                std::sqrt(std::abs(order_size) / adv) * 
                                volatility * price;
                                
        double temporary_impact = config_.temporary_impact_factor * 
                                std::pow(std::abs(order_size) / adv, 0.6) * 
                                volatility * price;

        return permanent_impact + temporary_impact;
    }

private:
    ImpactConfig config_;
}; 