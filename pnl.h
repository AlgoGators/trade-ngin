#ifndef PNL_H
#define PNL_H

#include <vector>
#include <numeric>
#include <cmath>
#include <iostream>
#include <limits>

class PNL {
private:
    std::vector<double> profits;
    double initialCapital;
    double contractSize;

public:
    explicit PNL(double capital, double contractSize) : initialCapital(capital), contractSize(contractSize)  {}

    // Method to calculate PNL from full position and price data
    void calculate(const std::vector<double>& positions, const std::vector<double>& prices) {
        if (positions.size() != prices.size()) {
            std::cerr << "Error: Positions size must match prices size." << std::endl;
            return;
        }

        profits.clear(); // Clear previous profits in case this is called multiple times

        for (size_t i = 1; i < prices.size(); ++i) {
            // Check for NaN in positions or prices
            if (std::isnan(positions[i - 1]) || std::isnan(prices[i]) || std::isnan(prices[i - 1])) {
                profits.push_back(std::numeric_limits<double>::quiet_NaN());
                continue;
            }

            // Calculate PNL
            double pnl = positions[i] * (prices[i] - prices[i - 1]) * contractSize;
            profits.push_back(pnl);
        }
    }

    double cumulativeProfit() const {
        if (profits.empty()) return 0.0;
        return std::accumulate(profits.begin(), profits.end(), 0.0, [](double sum, double profit) {
            return std::isnan(profit) ? sum : sum + profit;
        });
    }

    double sharpeRatio() const {
        if (profits.empty()) return 0.0;

        // Filter out NaN profits
        std::vector<double> validProfits;
        for (const auto& profit : profits) {
            if (!std::isnan(profit)) validProfits.push_back(profit);
        }

        if (validProfits.empty()) return 0.0;

        double mean = std::accumulate(validProfits.begin(), validProfits.end(), 0.0) / validProfits.size();

        double variance = 0.0;
        for (const auto& profit : validProfits) {
            variance += (profit - mean) * (profit - mean);
        }
        variance /= validProfits.size();
        double stdDev = std::sqrt(variance);

        return stdDev == 0.0 ? 0.0 : mean / stdDev;
    }

    // Placeholder plotting function
    void plotCumulativeProfit() const {
        if (profits.empty()) {
            std::cerr << "Error: No profits available to plot." << std::endl;
            return;
        }

        double runningTotal = 0.0;
        std::cout << "Cumulative Profit (%):" << std::endl;
        for (const auto& profit : profits) {
            if (!std::isnan(profit)) {
                runningTotal += profit;
                double percentage = (runningTotal / initialCapital) * 100.0;
                std::cout << "  Running Total: " << runningTotal
                          << " | Percentage of Initial Capital: " << percentage << "%" << std::endl;
            }
        }
    }
};

#endif
