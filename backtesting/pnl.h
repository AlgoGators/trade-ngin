// Header gaurds
#ifndef PNL_H
#define PNL_H

#include <vector>
#include <numeric>
#include <cmath>
#include <iostream>
#include <vector>


class PNL {
private:
    std::vector<double> profits;   
    double initialCapital; 
    std::vector<std::pair<double, double>> trades;

public:
    explicit PNL(double capital) : initialCapital(capital) {}

    // Method to calculate PNL from full position and price data
    void calculate(const std::vector<double>& positions, const std::vector<double>& prices) {
        if (positions.size() + 1 != prices.size()) {
            std::cerr << "Error: Positions size must be one less than prices size." << std::endl;
            return;
        }

        profits.clear(); // Clear previous profits in case this is called multiple times
        for (size_t i = 1; i < prices.size(); ++i) {
            double pnl = positions[i - 1] * (prices[i] - prices[i - 1]);
            profits.push_back(pnl);

            // Show each trade's PNL; will be cutting this out
            std::cout << "Trade " << i << ": Position = " << positions[i - 1]
                      << ", Price Change = " << (prices[i] - prices[i - 1])
                      << ", Profit = " << pnl << std::endl;
        }
    }

    double cumulativeProfit() const {
        return std::accumulate(profits.begin(), profits.end(), 0.0);
    }

    double sharpeRatio() const {
        double mean = std::accumulate(profits.begin(), profits.end(), 0.0) / profits.size();

        double variance = 0.0;
        for (const auto& profit : profits) {
            variance += (profit - mean) * (profit - mean);
        }
        variance /= profits.size();
        double stdDev = std::sqrt(variance);

        return mean / stdDev;
    }

    // Obviously this isn't a plot, so this is some infrastructure to be built out
    void plotCumulativeProfit() const {
        double runningTotal = 0.0;
        std::cout << "Cumulative Profit (%):" << std::endl;
        for (const auto& profit : profits) {
            runningTotal += profit;
            double percentage = (runningTotal / initialCapital) * 100.0;
            std::cout << "  Running Total: " << runningTotal
                      << " | Percentage of Initial Capital: " << percentage << "%" << std::endl;
        }
    }

    // Plot % returns

    // Store trades into trades (I suggest storing the profit/loss and the capital at the time)

};

#endif