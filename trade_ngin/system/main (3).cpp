#include <iostream>
#include <vector>
#include <iomanip>
#include <cmath>
#include <limits>
#include <random>
#include "strategy.h"
#include "pnl.h"

// Function to generate random price data
std::vector<double> generatePriceData(size_t size, double startPrice = 100.0, double volatility = 1.0) {
    std::vector<double> prices(size);
    std::default_random_engine generator;
    std::normal_distribution<double> distribution(0.0, volatility);

    prices[0] = startPrice;
    for (size_t i = 1; i < size; ++i) {
        // Generate price changes with random noise
        double change = distribution(generator);
        prices[i] = prices[i - 1] + change;
    }

    return prices;
}

int main() {
    // Generate 500 price points
    size_t numPrices = 500;
    std::vector<double> prices = generatePriceData(numPrices);

    // Instantiate the trendFollowing strategy
    double initialCapital = 100000.0; // Example starting capital
    double contractSize = 100;
    trendFollowing strategy(initialCapital, contractSize);

    // Generate positions (combined forecast) using the strategy
    std::vector<double> combinedForecast = strategy.generatePositions(prices);

    // Create PNL object
    PNL pnl(initialCapital, contractSize);

    // Calculate PNL using positions and prices
    pnl.calculate(combinedForecast, prices);

    // Output cumulative profit
    std::cout << "\nCumulative Profit: " << pnl.cumulativeProfit() << "\n";

    // Output Sharpe Ratio
    std::cout << "Sharpe Ratio: " << pnl.sharpeRatio() << "\n";

    // Plot cumulative profit
    pnl.plotCumulativeProfit();

    return 0;
}
