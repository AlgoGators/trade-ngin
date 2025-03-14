// Header Gaurds
#ifndef STRATEGY_H
#define STRATEGY_H

#include <vector>
#include <iostream>
#include <cmath>
#include "signals.h"

// Base Strategy Class
class Strategy {
protected:
    double capital;

public:
    Strategy(double initialCapital);

    virtual std::vector<double> generatePositions(const std::vector<double>& prices) = 0;

    virtual ~Strategy() = default;
};

class trendFollowing : public Strategy {
public:
    explicit trendFollowing(double capital, double contract_size, double risk_target = 0.2, double FX = 1, double idm = 2.5);

    std::vector<double> generatePositions(const std::vector<double>& prices) override;

private:
    double multiplier;
    double risk_target;
    double FX;
    double idm;

    std::vector<std::pair<int, int>> initializeEMAWIndows();

    std::vector<std::vector<double>> computeEMACrossovers(const std::vector<double>& prices, const std::vector<std::pair<int, int>>& emaWindows);

    std::vector<std::vector<double>> computeRawForecasts(const std::vector<double>& prices,
                                                         const std::vector<std::pair<int, int>>& emaWindows,
                                                         const std::vector<std::vector<double>>& emaCrossovers,
                                                         const std::vector<double>& blendedStdDev);

    void normalizeAndCapForecasts(std::vector<std::vector<double>>& rawForecasts);

    std::vector<double> combineForecasts(size_t priceSize, const std::vector<std::vector<double>>& rawForecasts);

    std::vector<double> calculatePositionsFromForecast(const std::vector<double>& combinedForecast, const std::vector<double>& prices,
                                                       const std::vector<double>& blendedStdDev, std::vector<double>& lowerBuffer, std::vector<double>& upperBuffer);

    std::vector<double> bufferPositions(const std::vector<double>& rawPositions, const std::vector<double>& lowerBuffer, const std::vector<double>& upperBuffer);
};

#endif
