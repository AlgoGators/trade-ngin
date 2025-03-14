// Header Gaurds
#ifndef SIGNALS_H
#define SIGNALS_H

#include <vector>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <iostream>

// Function to calculate EMA (Exponential Moving Average)
double calculateEMA(double price, double prevEMA, double alpha);

// Function to calculate EMAC with EMA initialization matching Pandas
std::vector<double> calculateEMAC(const std::vector<double>& prices, int shortSpan, int longSpan);

// Function to calculate the standard deviation over the past `n` days
double calculateStdDev(const std::vector<double>& prices, size_t end, size_t n);

// Function to calculate short and long window standard deviations with dynamic long window
void calculateShortAndDynamicLongStdDev(
    const std::vector<double>& prices,
    size_t shortWindow,
    size_t longWindow,
    std::vector<double>& combinedStdDev
);

#endif
