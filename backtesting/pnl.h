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
    explicit PNL(double capital, double contractSize) : initialCapital(capital), contractSize(contractSize) {}

    // Method to calculate PNL from full position and price data
    void calculate(const std::vector<double>& inputPositions, const std::vector<double>& prices) {
        if (inputPositions.size() != prices.size()) {
            std::cerr << "Error: Positions size must match prices size." << std::endl;
            return;
        }

        profits.clear(); // Clear previous profits

        for (size_t i = 1; i < prices.size(); ++i) {
            if (!std::isnan(inputPositions[i - 1]) && !std::isnan(prices[i]) && !std::isnan(prices[i - 1])) {
                double pnl = inputPositions[i] * (prices[i] - prices[i - 1]) * contractSize;
                profits.push_back(pnl);
            } else {
                profits.push_back(std::numeric_limits<double>::quiet_NaN());
            }
        }
    }

    // Calculate cumulative returns as percentages
    std::vector<double> calculateCumulativeReturns() const {
        std::vector<double> cumulativeReturns;
        double runningTotal = 0.0;

        for (const auto& profit : profits) {
            if (!std::isnan(profit)) {
                runningTotal += profit;
            }
            cumulativeReturns.push_back((runningTotal / initialCapital) * 100.0); // Convert to percentage
        }

        return cumulativeReturns;
    }

    // Calculate cumulative profit
    double cumulativeProfit() const {
        return std::accumulate(profits.begin(), profits.end(), 0.0, [](double sum, double profit) {
            return std::isnan(profit) ? sum : sum + profit;
        });
    }

    // Calculate Sharpe Ratio
    double sharpeRatio() const {
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
};

#endif
