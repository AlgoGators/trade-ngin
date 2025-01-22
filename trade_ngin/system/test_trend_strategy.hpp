#pragma once
#include <memory>
#include <vector>
#include <unordered_map>
#include <string>
#include <cmath>
#include <algorithm>
#include <iostream>
#include <limits>
#include "market_data.hpp"
#include <numeric>

class TrendStrategy {
public:
    TrendStrategy(double initial_capital, double vol_target, double min_vol, double max_vol, double max_leverage)
        : initial_capital_(initial_capital), vol_target_(vol_target), min_vol_(min_vol), max_vol_(max_vol), max_leverage_(max_leverage) {}

    // Main signal generator
    std::vector<double> generateSignals(const std::vector<MarketData>& data);
    void runBacktest(const std::string& connection_string);

private:
    double initial_capital_;
    double vol_target_;
    double min_vol_;
    double max_vol_;
    double max_leverage_;

    std::vector<std::pair<int, int>> initializeEMAWindows() {
        return {{2, 8}, {4, 16}, {8, 32}, {16, 64}, {32, 128}, {64, 256}};
    }

    void calculateShortAndDynamicLongStdDev(const std::vector<double>& prices, 
                                          int shortWindow, int longWindow,
                                          std::vector<double>& blendedStdDev) {
        blendedStdDev.resize(prices.size(), 0.0);
        
        // Calculate returns
        std::vector<double> returns(prices.size());
        for (size_t i = 1; i < prices.size(); ++i) {
            returns[i] = std::log(prices[i] / prices[i-1]);
        }

        // Calculate rolling standard deviations
        for (size_t i = shortWindow; i < prices.size(); ++i) {
            // Short-term volatility
            double shortVar = 0.0;
            for (int j = 0; j < shortWindow; ++j) {
                shortVar += returns[i-j] * returns[i-j];
            }
            shortVar /= (shortWindow - 1);

            // Long-term volatility
            double longVar = 0.0;
            int effectiveLongWindow = std::min(longWindow, static_cast<int>(i));
            for (int j = 0; j < effectiveLongWindow; ++j) {
                longVar += returns[i-j] * returns[i-j];
            }
            longVar /= (effectiveLongWindow - 1);

            // Blend volatilities
            blendedStdDev[i] = std::sqrt((shortVar + longVar) / 2.0 * 252.0);
        }
    }

    std::vector<std::vector<double>> computeEMACrossovers(
        const std::vector<double>& prices,
        const std::vector<std::pair<int, int>>& emaWindows) {
        
        std::vector<std::vector<double>> emaCrossovers;
        for (const auto& [shortWindow, longWindow] : emaWindows) {
            std::vector<double> shortEMA = calculateEMA(prices, shortWindow);
            std::vector<double> longEMA = calculateEMA(prices, longWindow);
            
            std::vector<double> crossover(prices.size());
            for (size_t i = 0; i < prices.size(); ++i) {
                crossover[i] = shortEMA[i] - longEMA[i];
            }
            emaCrossovers.push_back(crossover);
        }
        return emaCrossovers;
    }

    std::vector<std::vector<double>> computeRawForecasts(
        const std::vector<double>& prices,
        const std::vector<std::pair<int, int>>& emaWindows,
        const std::vector<std::vector<double>>& emaCrossovers,
        const std::vector<double>& blendedStdDev) {
        
        std::vector<std::vector<double>> rawForecasts;
        for (size_t i = 0; i < emaWindows.size(); ++i) {
            const auto& emac = emaCrossovers[i];
            std::vector<double> forecast(emac.size(), std::numeric_limits<double>::quiet_NaN());
            
            for (size_t j = 0; j < emac.size(); ++j) {
                if (!std::isnan(blendedStdDev[j]) && !std::isnan(emac[j]) && blendedStdDev[j] != 0.0) {
                    // Scale by price volatility as per strategy.h
                    double sigmaP = prices[j] * blendedStdDev[j] / 16.0;
                    forecast[j] = emac[j] / sigmaP;
                }
            }
            rawForecasts.push_back(forecast);
        }
        return rawForecasts;
    }

    void normalizeAndCapForecasts(std::vector<std::vector<double>>& rawForecasts) {
        for (auto& forecast : rawForecasts) {
            double sumAbs = 0.0;
            int count = 0;
            
            for (double value : forecast) {
                if (!std::isnan(value)) {
                    sumAbs += std::abs(value);
                    ++count;
                }
            }
            
            double absAverage = (count > 0) ? (sumAbs / count) : 1.0;
            double scalingFactor = 10.0 / absAverage;
            
            for (double& value : forecast) {
                if (!std::isnan(value)) {
                    value *= scalingFactor;
                    value = std::max(-20.0, std::min(20.0, value));
                }
            }
        }
    }

    std::vector<double> combineForecasts(size_t size, 
                                       const std::vector<std::vector<double>>& rawForecasts) {
        std::vector<double> combined(size, std::numeric_limits<double>::quiet_NaN());
        
        for (size_t i = 255; i < size; ++i) {
            double sum = 0.0;
            int count = 0;
            
            for (const auto& forecast : rawForecasts) {
                if (!std::isnan(forecast[i])) {
                    sum += forecast[i];
                    ++count;
                }
            }
            
            if (count > 0) {
                double value = (sum / count) * 1.26;
                combined[i] = std::max(-20.0, std::min(20.0, value));
            }
        }
        return combined;
    }

    std::vector<double> calculatePositionsFromForecast(
        const std::vector<double>& forecast,
        const std::vector<double>& prices,
        const std::vector<double>& blendedStdDev,
        std::vector<double>& lowerBuffer,
        std::vector<double>& upperBuffer) {
        
        std::vector<double> positions(prices.size(), std::numeric_limits<double>::quiet_NaN());
        lowerBuffer.resize(prices.size(), std::numeric_limits<double>::quiet_NaN());
        upperBuffer.resize(prices.size(), std::numeric_limits<double>::quiet_NaN());
        
        for (size_t i = 255; i < prices.size(); ++i) {
            if (!std::isnan(forecast[i]) && !std::isnan(prices[i])) {
                positions[i] = (forecast[i] * initial_capital_ * vol_target_) /
                             (10.0 * prices[i] * blendedStdDev[i]);
                
                // Calculate buffer width
                double Bw = (0.1 * initial_capital_ * vol_target_) /
                          (prices[i] * blendedStdDev[i]);
                
                lowerBuffer[i] = std::round(positions[i] - Bw);
                upperBuffer[i] = std::round(positions[i] + Bw);
            }
        }
        return positions;
    }

    std::vector<double> bufferPositions(
        const std::vector<double>& rawPositions,
        const std::vector<double>& lowerBuffer,
        const std::vector<double>& upperBuffer) {
        
        std::vector<double> buffered(rawPositions.size(), std::numeric_limits<double>::quiet_NaN());
        
        size_t startIndex = 255;
        if (startIndex < rawPositions.size() && !std::isnan(rawPositions[startIndex])) {
            buffered[startIndex] = std::round(rawPositions[startIndex]);
            
            for (size_t i = startIndex + 1; i < rawPositions.size(); ++i) {
                if (std::isnan(rawPositions[i]) || std::isnan(lowerBuffer[i]) || 
                    std::isnan(upperBuffer[i])) {
                    buffered[i] = std::numeric_limits<double>::quiet_NaN();
                } else {
                    double currentPosition = buffered[i - 1];
                    
                    if (currentPosition < lowerBuffer[i]) {
                        buffered[i] = lowerBuffer[i];
                    } else if (currentPosition > upperBuffer[i]) {
                        buffered[i] = upperBuffer[i];
                    } else {
                        buffered[i] = currentPosition;
                    }
                }
            }
        }
        return buffered;
    }

    std::vector<double> calculateEMA(const std::vector<double>& data, int window) {
        std::vector<double> ema(data.size());
        double alpha = 2.0 / (window + 1.0);
        
        ema[0] = data[0];
        for (size_t i = 1; i < data.size(); ++i) {
            ema[i] = alpha * data[i] + (1.0 - alpha) * ema[i - 1];
        }
        return ema;
    }
};