#include <iostream>
#include <vector>
#include <iomanip>
#include <cmath>
#include <limits>
#include <random>
#include <fstream>
#include <ctime>
#include "strategy.h"
#include "pnl.h"

// Function to generate random price data
std::vector<double> generatePriceData(size_t size, double startPrice = 100.0, double volatility = 1.0) {
    std::vector<double> prices(size);
    std::default_random_engine generator;
    std::normal_distribution<double> distribution(0.0, volatility);

    prices[0] = startPrice;
    for (size_t i = 1; i < size; ++i) {
        double change = distribution(generator);
        prices[i] = prices[i - 1] + change;
    }

    return prices;
}

// Function to generate sequential dates
std::vector<std::string> generateDates(size_t size) {
    std::vector<std::string> dates(size);
    std::time_t t = std::time(nullptr);
    std::tm* now = std::localtime(&t);

    for (size_t i = 0; i < size; ++i) {
        now->tm_mday += 1; // Increment day
        std::mktime(now);  // Normalize date
        char buffer[11];
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", now);
        dates[i] = buffer;
    }

    return dates;
}

// Function to write cumulative returns to CSV
void writeToCSV(const std::string& filename, const std::vector<std::string>& dates, const std::vector<double>& cumulativeReturns) {
    std::ofstream file(filename);
    if (file.is_open()) {
        std::cout << "Writing cumulative returns to " << filename << "...\n";
        file << "Date,Cumulative Return (%)\n"; // Write header
        for (size_t i = 0; i < dates.size(); ++i) {
            if (!std::isnan(cumulativeReturns[i])) {
                file << dates[i] << "," << std::fixed << std::setprecision(2) << cumulativeReturns[i] << "\n";
            }
        }
        file.close();
        std::cout << "Cumulative returns saved to " << filename << std::endl;
    } else {
        std::cerr << "Error: Unable to open file " << filename << std::endl;
    }
}

int main() {
    // Generate 500 price points
    size_t numPrices = 500;
    std::vector<double> prices = generatePriceData(numPrices);

    // Instantiate the trendFollowing strategy
    double initialCapital = 100000.0;
    double contractSize = 100;
    trendFollowing strategy(initialCapital, contractSize);

    // Generate positions using the strategy
    std::vector<double> combinedForecast = strategy.generatePositions(prices);

    // Create PNL object
    PNL pnl(initialCapital, contractSize);

    // Calculate PNL using positions and prices
    pnl.calculate(combinedForecast, prices);

    // Generate cumulative returns
    std::vector<double> cumulativeReturns = pnl.calculateCumulativeReturns();

    // Generate dates
    std::vector<std::string> dates = generateDates(prices.size());

    // Write cumulative returns to CSV
    writeToCSV("cumulative_returns.csv", dates, cumulativeReturns);

    // Output cumulative profit
    std::cout << "\nCumulative Profit: " << pnl.cumulativeProfit() << "\n";
    std::cout << "Sharpe Ratio: " << pnl.sharpeRatio() << "\n";

    return 0;
}
