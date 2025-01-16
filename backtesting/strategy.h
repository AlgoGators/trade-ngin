// Header gaurds
#ifndef STRATEGY_H
#define STRATEGY_H

#include <vector>
#include <iostream>
#include "signals.h"

// Base Strategy Class
class Strategy {
protected:
    double capital;

public:
    Strategy(double initialCapital) : capital(initialCapital) {}

    virtual std::vector<double> generatePositions(const std::vector<double>& prices) = 0;

    virtual ~Strategy() = default;
};

// Buy and Hold Strategy Class for example
class BuyAndHoldStrategy : public Strategy {
public:
    explicit BuyAndHoldStrategy(double capital) : Strategy(capital) {}

    std::vector<double> generatePositions(const std::vector<double>& prices) override {
        if (prices.empty()) {
            std::cerr << "Error: Price data is empty." << std::endl;
            return {};
        }

        std::vector<double> positions;
        double remainingCapital = capital;

        // Calculate the max number of shares affordable at the initial price
        double position = static_cast<int>(remainingCapital / prices[0]); // Automatically floors the division
        remainingCapital -= position * prices[0];

        // Hold the same position for all time steps
        positions.resize(prices.size() - 1, position); // Going to want to work on the modularity of disparity between price and position vectors. Right now positions has to be one less than price in size

        return positions;
    }
};


class trendFollowing : public Strategy {
public:
    explicit trendFollowing(double capital, double contract_size, double risk_target = 0.2, double FX = 1, double idm = 2.5)
        : Strategy(capital), multiplier(contract_size), risk_target(risk_target), FX(FX), idm(idm) {}

    std::vector<double> generatePositions(const std::vector<double>& prices) override {
        if (prices.empty()) {
            std::cerr << "Error: Price data is empty." << std::endl;
            return {};
        }

        std::vector<std::pair<int, int>> emaWindows = initializeEMAWIndows();

        // Compute standard deviations once
        std::vector<double> blendedStdDev;
        calculateShortAndDynamicLongStdDev(prices, 22, 2520, blendedStdDev);

        // Calculate EMACs for all window pairs
        std::vector<std::vector<double>> emaCrossovers = computeEMACrossovers(prices, emaWindows);

        // Compute raw forecasts
        std::vector<std::vector<double>> rawForecasts = computeRawForecasts(prices, emaWindows, emaCrossovers, blendedStdDev);

        // Normalize raw forecasts and cap values between -20 and 20
        normalizeAndCapForecasts(rawForecasts);

        // Combine forecasts into a single vector
        std::vector<double> combinedForecast = combineForecasts(prices.size(), rawForecasts);

        std::vector<double> lowerBuffer, upperBuffer;
        std::vector<double> rawPositions = calculatePositionsFromForecast(combinedForecast, prices, blendedStdDev, lowerBuffer, upperBuffer);
        return bufferPositions(rawPositions, lowerBuffer, upperBuffer);
    }

private:
    double multiplier;
    double risk_target;
    double FX;
    double idm;

    std::vector<std::pair<int, int>> initializeEMAWIndows() {
        return {{2, 8}, {4, 16}, {8, 32}, {16, 64}, {32, 128}, {64, 256}};
    }

    std::vector<std::vector<double>> computeEMACrossovers(const std::vector<double>& prices, const std::vector<std::pair<int, int>>& emaWindows) {
        std::vector<std::vector<double>> emaCrossovers;
        for (size_t idx = 0; idx < emaWindows.size(); ++idx) {
            int shortWindow = emaWindows[idx].first;
            int longWindow = emaWindows[idx].second;

            // Compute EMAC for the current pair
            std::vector<double> emac = calculateEMAC(prices, shortWindow, longWindow);
            emaCrossovers.push_back(emac);
        }
        return emaCrossovers;
    }

    std::vector<std::vector<double>> computeRawForecasts(const std::vector<double>& prices,
                                                         const std::vector<std::pair<int, int>>& emaWindows,
                                                         const std::vector<std::vector<double>>& emaCrossovers,
                                                         const std::vector<double>& blendedStdDev) {
        std::vector<std::vector<double>> rawForecasts;
        for (size_t i = 0; i < emaWindows.size(); ++i) {
            const auto& emac = emaCrossovers[i];

            std::vector<double> rawForecast(emac.size(), std::numeric_limits<double>::quiet_NaN());
            for (size_t j = 0; j < emac.size(); ++j) {
                if (!std::isnan(blendedStdDev[j]) && !std::isnan(emac[j])) {
                    if (blendedStdDev[j] != 0.0) {
                        double sigmaP = prices[j] * blendedStdDev[j] / 16;
                        rawForecast[j] = emac[j] / sigmaP;
                    }
                }
            }
            rawForecasts.push_back(rawForecast);
        }
        return rawForecasts;
    }

    void normalizeAndCapForecasts(std::vector<std::vector<double>>& rawForecasts) {
        for (auto& rawForecast : rawForecasts) {
            double sumAbs = 0.0;
            int count = 0;

            // Calculate the absolute average value
            for (double value : rawForecast) {
                if (!std::isnan(value)) {
                    sumAbs += std::abs(value);
                    ++count;
                }
            }

            double absAverage = (count > 0) ? (sumAbs / count) : 1.0; // Avoid division by zero
            double scalingFactor = 10.0 / absAverage;

            for (double& value : rawForecast) {
                if (!std::isnan(value)) {
                    value *= scalingFactor;
                    value = std::max(-20.0, std::min(20.0, value)); // Cap values between -20 and 20
                }
            }
        }
    }

    std::vector<double> combineForecasts(size_t priceSize, const std::vector<std::vector<double>>& rawForecasts) {
        std::vector<double> combinedForecast(priceSize, std::numeric_limits<double>::quiet_NaN());
        for (size_t i = 255; i < priceSize; ++i) { // Start at day 256
            double sum = 0.0;
            int count = 0;

            for (const auto& rawForecast : rawForecasts) {
                if (!std::isnan(rawForecast[i])) {
                    sum += rawForecast[i];
                    ++count;
                }
            }

            if (count > 0) {
                double value = (sum / count) * 1.26; // Equal weighting and scale by 1.26
                value = std::max(-20.0, std::min(20.0, value)); // Cap values between -20 and 20
                combinedForecast[i] = value;
            }
        }
        return combinedForecast;
    }

std::vector<double> calculatePositionsFromForecast(const std::vector<double>& combinedForecast, const std::vector<double>& prices, const std::vector<double>& blendedStdDev, std::vector<double>& lowerBuffer, std::vector<double>& upperBuffer) {
    std::vector<double> positions(prices.size(), std::numeric_limits<double>::quiet_NaN());
    lowerBuffer.resize(prices.size(), std::numeric_limits<double>::quiet_NaN());
    upperBuffer.resize(prices.size(), std::numeric_limits<double>::quiet_NaN());

    for (size_t i = 255; i < prices.size(); ++i) {
        if (!std::isnan(combinedForecast[i]) && !std::isnan(prices[i])) {
            positions[i] = (combinedForecast[i] * capital * idm * risk_target) /
                           (10.0 * multiplier * prices[i] * FX * blendedStdDev[i]);

            // Calculate buffer width
            double Bw = (0.1 * capital * idm * risk_target) /
                        (multiplier * prices[i] * FX * blendedStdDev[i]);

            // Update lower and upper buffers
            lowerBuffer[i] = std::round(positions[i] - Bw);
            upperBuffer[i] = std::round(positions[i] + Bw);
        }
    }

    return positions;
}

std::vector<double> bufferPositions(const std::vector<double>& rawPositions, const std::vector<double>& lowerBuffer, const std::vector<double>& upperBuffer) {
    std::vector<double> bufferedPositions(rawPositions.size(), std::numeric_limits<double>::quiet_NaN());

    // Start from index 255 where rawPositions stops being NaN
    size_t startIndex = 255;

    if (startIndex < rawPositions.size() && !std::isnan(rawPositions[startIndex])) {
        // Set the first valid position to the rounded value of the first valid raw position
        bufferedPositions[startIndex] = std::round(rawPositions[startIndex]);

        // Iterate through the rest of the positions starting from index 256
        for (size_t i = startIndex + 1; i < rawPositions.size(); ++i) {
            if (std::isnan(rawPositions[i]) || std::isnan(lowerBuffer[i]) || std::isnan(upperBuffer[i])) {
                // Keep position as NaN if any required value is NaN
                bufferedPositions[i] = std::numeric_limits<double>::quiet_NaN();
            } else {
                double currentPosition = bufferedPositions[i - 1]; // Use the previous position

                if (currentPosition < lowerBuffer[i]) {
                    // Buy contracts to reach the lower buffer
                    bufferedPositions[i] = lowerBuffer[i];
                } else if (currentPosition > upperBuffer[i]) {
                    // Sell contracts to reach the upper buffer
                    bufferedPositions[i] = upperBuffer[i];
                } else {
                    // No trading required, maintain the current position
                    bufferedPositions[i] = currentPosition;
                }
            }
        }
    }

    return bufferedPositions;
}
};


#endif
