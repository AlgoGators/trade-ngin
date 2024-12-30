// Header gaurds
#ifndef SIGNALS_H
#define SIGNALS_H

#include "dataframe.hpp"
#include <vector>
#include <memory>

namespace signals {

class Signal {
public:
    virtual ~Signal() = default;
    virtual std::vector<double> calculate(const DataFrame& market_data) = 0;
    virtual void configure(const std::unordered_map<std::string, double>& params) {}
};

// Technical indicators moved to separate implementation files
double calculateEMA(double price, double prevEMA, double alpha);
std::vector<double> calculateEMAC(const std::vector<double>& prices, int shortSpan, int longSpan);
void calculateShortAndDynamicLongStdDev(const std::vector<double>& prices, 
                                      size_t shortWindow, 
                                      size_t longWindow,
                                      std::vector<double>& combinedStdDev);

} // namespace signals

#endif
