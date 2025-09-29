#include <iostream>
#include <unordered_map>
#include "trade_ngin/core/types.hpp"

using namespace trade_ngin;

int main() {
    std::cout << "Testing PnL Calculations..." << std::endl;
    
    // Test case 1: Long position with profit
    Position long_position;
    long_position.symbol = "NQ.v.0";
    long_position.quantity = Quantity(1.0);
    long_position.average_price = Decimal(24726.50);
    long_position.unrealized_pnl = Decimal(0.0);
    long_position.realized_pnl = Decimal(0.0);
    
    // Current market price (higher than entry)
    double current_price = 24800.0;
    
    // Calculate unrealized PnL: quantity * (current_price - avg_price)
    double unrealized_pnl = static_cast<double>(long_position.quantity) * 
                           (current_price - static_cast<double>(long_position.average_price));
    
    std::cout << "Test 1 - Long Position:" << std::endl;
    std::cout << "  Symbol: " << long_position.symbol << std::endl;
    std::cout << "  Quantity: " << static_cast<double>(long_position.quantity) << std::endl;
    std::cout << "  Average Price: $" << static_cast<double>(long_position.average_price) << std::endl;
    std::cout << "  Current Price: $" << current_price << std::endl;
    std::cout << "  Unrealized PnL: $" << unrealized_pnl << std::endl;
    std::cout << "  Expected: $73.50" << std::endl;
    
    // Test case 2: Short position with profit
    Position short_position;
    short_position.symbol = "ZR.v.0";
    short_position.quantity = Quantity(-1.0);
    short_position.average_price = Decimal(11.28);
    short_position.unrealized_pnl = Decimal(0.0);
    short_position.realized_pnl = Decimal(0.0);
    
    // Current market price (lower than entry - good for short)
    double current_price_short = 10.50;
    
    // Calculate unrealized PnL: quantity * (current_price - avg_price)
    double unrealized_pnl_short = static_cast<double>(short_position.quantity) * 
                                 (current_price_short - static_cast<double>(short_position.average_price));
    
    std::cout << "\nTest 2 - Short Position:" << std::endl;
    std::cout << "  Symbol: " << short_position.symbol << std::endl;
    std::cout << "  Quantity: " << static_cast<double>(short_position.quantity) << std::endl;
    std::cout << "  Average Price: $" << static_cast<double>(short_position.average_price) << std::endl;
    std::cout << "  Current Price: $" << current_price_short << std::endl;
    std::cout << "  Unrealized PnL: $" << unrealized_pnl_short << std::endl;
    std::cout << "  Expected: $0.78" << std::endl;
    
    // Test case 3: Portfolio value calculation
    double initial_capital = 100000.0;
    double portfolio_value = initial_capital + unrealized_pnl + unrealized_pnl_short;
    
    std::cout << "\nTest 3 - Portfolio Value:" << std::endl;
    std::cout << "  Initial Capital: $" << initial_capital << std::endl;
    std::cout << "  Total Unrealized PnL: $" << (unrealized_pnl + unrealized_pnl_short) << std::endl;
    std::cout << "  Portfolio Value: $" << portfolio_value << std::endl;
    std::cout << "  Expected: $100074.28" << std::endl;
    
    std::cout << "\nPnL calculation tests completed!" << std::endl;
    return 0;
}
