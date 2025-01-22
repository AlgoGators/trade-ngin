#include <iostream>
#include <vector>
#include <cmath>
#include <numeric>
#include <algorithm>

// Function to calculate the rolling standard deviation
std::vector<double> calculateRollingStdDev(const std::vector<double>& prices, size_t window) {
    std::vector<double> stdDev(prices.size(), 0.0);

    for (size_t i = window - 1; i < prices.size(); ++i) {
        double sum = 0.0, mean = 0.0, variance = 0.0;

        // Calculate the mean of the window
        for (size_t j = i - window + 1; j <= i; ++j) {
            sum += prices[j];
        }
        mean = sum / window;

        // Calculate variance
        for (size_t j = i - window + 1; j <= i; ++j) {
            variance += (prices[j] - mean) * (prices[j] - mean);
        }
        variance /= window;

        // Calculate standard deviation
        stdDev[i] = std::sqrt(variance);
    }

    return stdDev;
}

std::vector<int> momentumStrategyWithRisk(const std::vector<double>& prices, double initialCapital, double riskTarget, size_t stdDevWindow) {
    if (prices.size() < stdDevWindow) {
        throw std::invalid_argument("Not enough data to apply the strategy.");
    }

    // Calculate rolling standard deviation
    std::vector<double> rollingStdDev = calculateRollingStdDev(prices, stdDevWindow);

    std::vector<int> positions(prices.size(), 0); // Initialize positions (number of contracts)
    double capital = initialCapital; // Remaining capital
    int currentPosition = 0; // Tracks the current position (number of contracts, positive for long, negative for short)

    for (size_t i = stdDevWindow; i < prices.size(); ++i) {
        int newPosition = 0;

        // Determine position based on the previous three days
        if (prices[i - 1] > prices[i - 2] && prices[i - 2] > prices[i - 3]) {
            newPosition = 1; // Long
        } else if (prices[i - 1] < prices[i - 2] && prices[i - 2] < prices[i - 3]) {
            newPosition = -1; // Short
        } else {
            newPosition = 0; // Neutral
        }

        // Close current position if switching or going neutral
        if (currentPosition != 0 && newPosition != currentPosition) {
            capital += std::abs(currentPosition) * prices[i]; // Sell or cover the position
            currentPosition = 0; // Reset position
        }

        // Enter new position if not neutral
        if (newPosition != 0) {
            // Adjust capital allocation based on rolling standard deviation
            double allocationFactor = 1.0;
            if (rollingStdDev[i - 1] > riskTarget) {
                allocationFactor = riskTarget / rollingStdDev[i - 1];
            }

            // Calculate number of contracts based on available capital and price
            double allocatedCapital = allocationFactor * capital;
            int numContracts = static_cast<int>(std::floor(allocatedCapital / prices[i]));

            // Update capital and position
            capital -= numContracts * prices[i];
            currentPosition = newPosition * numContracts;
        }

        // Store the current position
        positions[i] = currentPosition;
    }

    return positions;
}

int main() {
    // Example price data
    std::vector<double> prices = {100.0, 102.0, 104.0, 103.0, 101.0, 102.0, 100.0, 98.0, 96.0,
                                  95.0, 94.0, 93.0, 92.0, 91.0, 90.0, 89.0, 88.0, 87.0, 86.0,
                                  85.0, 84.0, 83.0, 82.0, 81.0, 80.0, 79.0, 78.0, 77.0, 76.0, 75.0};
    double initialCapital = 10000.0; // Example initial capital
    double riskTarget = 0.2;         // 20% risk target
    size_t stdDevWindow = 5;        // Changed from 30 to 5 days for the example

    try {
        // Get positions from the momentum strategy with risk adjustment
        std::vector<int> positions = momentumStrategyWithRisk(prices, initialCapital, riskTarget, stdDevWindow);

        // Print the positions
        std::cout << "Positions vector:\n";
        for (size_t i = 0; i < positions.size(); ++i) {
            std::cout << "Day " << i << ": Position = " << positions[i] << "\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }

    return 0;
} 