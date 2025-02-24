#include "strategy.h"

// Base Strategy Class
Strategy::Strategy(double initialCapital) : capital(initialCapital) {}

trendFollowing::trendFollowing(double capital, double contract_size, double risk_target, double FX, double idm)
    : Strategy(capital), multiplier(contract_size), risk_target(risk_target), FX(FX), idm(idm) {}

std::vector<double> trendFollowing::generatePositions(const std::vector<double>& prices) {
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

std::vector<std::pair<int, int>> trendFollowing::initializeEMAWIndows() {
    return {{2, 8}, {4, 16}, {8, 32}, {16, 64}, {32, 128}, {64, 256}};
}

std::vector<std::vector<double>> trendFollowing::computeEMACrossovers(const std::vector<double>& prices, const std::vector<std::pair<int, int>>& emaWindows) {
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

std::vector<std::vector<double>> trendFollowing::computeRawForecasts(const std::vector<double>& prices,
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

void trendFollowing::normalizeAndCapForecasts(std::vector<std::vector<double>>& rawForecasts) {
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

std::vector<double> trendFollowing::combineForecasts(size_t priceSize, const std::vector<std::vector<double>>& rawForecasts) {
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
