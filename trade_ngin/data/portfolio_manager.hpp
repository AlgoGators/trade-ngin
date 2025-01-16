#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

struct Position {
    std::string symbol;
    double quantity;
    double entry_price;
    double current_price;
    double unrealized_pnl;
    double realized_pnl;
    std::string side;  // "LONG" or "SHORT"
    std::string status; // "OPEN" or "CLOSED"
};

class PortfolioManager {
public:
    PortfolioManager(double initial_capital = 500000.0)
        : total_capital_(initial_capital),
          available_capital_(initial_capital) {}

    // Portfolio state
    double getTotalCapital() const { return total_capital_; }
    double getAvailableCapital() const { return available_capital_; }
    const std::unordered_map<std::string, Position>& getPositions() const { return positions_; }
    
    // Position management
    bool openPosition(const std::string& symbol, double quantity, double price, const std::string& side);
    bool closePosition(const std::string& symbol, double price);
    void updatePosition(const std::string& symbol, double current_price);
    
    // Risk management
    double getPositionSize(const std::string& symbol) const;
    double getPortfolioExposure() const;
    bool checkRiskLimits(const std::string& symbol, double quantity, double price) const;
    
    // Portfolio metrics
    double getUnrealizedPnL() const;
    double getRealizedPnL() const;
    double getPortfolioValue() const;
    
    // Position sizing and weighting
    double calculatePositionSize(const std::string& symbol, double signal_strength) const;
    void rebalancePortfolio(const std::unordered_map<std::string, double>& target_weights);

private:
    double total_capital_;
    double available_capital_;
    std::unordered_map<std::string, Position> positions_;
    
    // Risk parameters (placeholders)
    const double MAX_POSITION_SIZE = 0.20;  // 20% of capital
    const double MAX_PORTFOLIO_EXPOSURE = 1.0;  // 100% of capital
    const double RISK_PER_TRADE = 0.02;  // 2% risk per trade
}; 